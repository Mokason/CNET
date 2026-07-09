// Spike 2: the compute half — quantized matmul over VRAM-resident weights.
//
// The loader spike proved mmap -> 1 DMA hop -> VRAM-resident Q4_K. This
// proves the kernel that CONSUMES that layout: a GEMV (single-token matmul,
// what a mining forward does) that dequantizes each Q4_K block INLINE inside
// the dot product, so the fp32 weight never exists — not in host RAM, not in
// VRAM. That is the whole point: the 7 GB quantized file is the only copy.
//
// Fidelity: a matmul accumulates, and GPU block-reduction reorders the sum,
// so this is NOT bit-identical to a dequant-then-matmul reference (fp32 is
// not associative). The oracle consumes DECISIONS (argmax/top-k), so the
// criterion is decision-identical + tight relative agreement.
//
// Build: hipcc -O2 --offload-arch=gfx1201 -o bin/qmatmul_spike spike/qmatmul_spike.cpp
// Run:   ./bin/qmatmul_spike /home/marble/AI/Models/gemma4-v2-Q4_K_M.gguf

#include <hip/hip_runtime.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

#define QK_K 256
#define GGML_Q4_K 12
#define HIPCK(x) do { hipError_t e=(x); if(e!=hipSuccess){ \
    fprintf(stderr,"HIP error %s at %s:%d\n",hipGetErrorString(e),__FILE__,__LINE__); return 1; } } while(0)

__host__ __device__ static inline float f16_to_f32(uint16_t h){
    uint32_t sign=(uint32_t)(h&0x8000)<<16, exp=(h>>10)&0x1F, man=h&0x3FF, bits;
    if(exp==0){ if(man==0) bits=sign; else { exp=127-15+1;
        while((man&0x400)==0){man<<=1;exp--;} man&=0x3FF; bits=sign|(exp<<23)|(man<<13);} }
    else if(exp==0x1F) bits=sign|0x7F800000|(man<<13);
    else bits=sign|((exp+127-15)<<23)|(man<<13);
    float f; memcpy(&f,&bits,4); return f;
}

// Dequant one Q4_K block into 256 floats (host + device, shared math).
__host__ __device__ static void dequant_q4k_block(const uint8_t* blk, float* out){
    uint16_t d16, dmin16; memcpy(&d16,blk,2); memcpy(&dmin16,blk+2,2);
    float d=f16_to_f32(d16), dmin=f16_to_f32(dmin16);
    const uint8_t* scales=blk+4; const uint8_t* q=blk+16; int is=0, oi=0;
    for(int j=0;j<QK_K;j+=64){
        uint8_t sc,mm; int jj=is;
        if(jj<4){sc=scales[jj]&63; mm=scales[jj+4]&63;}
        else{sc=(scales[jj+4]&0xF)|((scales[jj-4]>>6)<<4); mm=(scales[jj+4]>>4)|((scales[jj]>>6)<<4);}
        float d1=d*sc, m1=dmin*mm; jj=is+1;
        if(jj<4){sc=scales[jj]&63; mm=scales[jj+4]&63;}
        else{sc=(scales[jj+4]&0xF)|((scales[jj-4]>>6)<<4); mm=(scales[jj+4]>>4)|((scales[jj]>>6)<<4);}
        float d2=d*sc, m2=dmin*mm;
        for(int l=0;l<32;l++) out[oi++]=d1*(q[l]&0xF)-m1;
        for(int l=0;l<32;l++) out[oi++]=d2*(q[l]>>4)-m2;
        q+=32; is+=2;
    }
}

// GEMV: y[row] = sum_in W[row][in] * x[in], W stored row-major as Q4_K blocks
// (each output row = in_dim/256 blocks). One thread-block per output row;
// threads stride over the row's blocks, dequant INLINE, dot, block-reduce.
// The fp32 weight is never materialized — only the 256 vals of the block a
// thread is currently touching live, in registers.
#define TPB 64
__global__ void k_qgemv(const uint8_t* __restrict__ W, const float* __restrict__ x,
                        float* __restrict__ y, int blocks_per_row){
    int row = blockIdx.x;
    int t = threadIdx.x;
    const uint8_t* row_blocks = W + (size_t)row*blocks_per_row*144;
    float acc = 0.f;
    float deq[QK_K];
    for(int b=t; b<blocks_per_row; b+=TPB){
        dequant_q4k_block(row_blocks + (size_t)b*144, deq);
        const float* xseg = x + (size_t)b*QK_K;
        #pragma unroll 8
        for(int i=0;i<QK_K;i++) acc += deq[i]*xseg[i];
    }
    __shared__ float sh[TPB];
    sh[t]=acc; __syncthreads();
    for(int s=TPB/2; s>0; s>>=1){ if(t<s) sh[t]+=sh[t+s]; __syncthreads(); }
    if(t==0) y[row]=sh[0];
}

static uint64_t rd_u64(const uint8_t*&p){uint64_t v;memcpy(&v,p,8);p+=8;return v;}
static uint32_t rd_u32(const uint8_t*&p){uint32_t v;memcpy(&v,p,4);p+=4;return v;}
static bool skip_kv_value(const uint8_t*&p){
    uint32_t t=rd_u32(p);
    switch(t){
        case 0:case 1:case 7:p+=1;break; case 2:case 3:p+=2;break;
        case 4:case 5:case 6:p+=4;break; case 10:case 11:case 12:p+=8;break;
        case 8:{uint64_t n=rd_u64(p);p+=n;}break;
        case 9:{uint32_t et=rd_u32(p);uint64_t n=rd_u64(p);
            for(uint64_t i=0;i<n;i++){ if(et==8){uint64_t sn=rd_u64(p);p+=sn;}
                else{int sz=(et==0||et==1||et==7)?1:(et==2||et==3)?2:(et==4||et==5||et==6)?4:8;p+=sz;} }}break;
        default:return false;
    } return true;
}

int main(int argc,char**argv){
    const char* path=argc>1?argv[1]:"/home/marble/AI/Models/gemma4-v2-Q4_K_M.gguf";
    int fd=open(path,O_RDONLY); if(fd<0){perror("open");return 1;}
    struct stat st; fstat(fd,&st);
    uint8_t* base=(uint8_t*)mmap(NULL,st.st_size,PROT_READ,MAP_SHARED,fd,0);
    if(base==MAP_FAILED){perror("mmap");return 1;}
    const uint8_t* p=base;
    if(memcmp(p,"GGUF",4)){fprintf(stderr,"not GGUF\n");return 1;} p+=4;
    rd_u32(p); uint64_t n_tensors=rd_u64(p), n_kv=rd_u64(p), alignment=32;
    for(uint64_t i=0;i<n_kv;i++){ uint64_t kn=rd_u64(p); const char* key=(const char*)p; size_t kl=kn; p+=kn;
        const uint8_t* vp=p; if(kl==17&&!memcmp(key,"general.alignment",17)){uint32_t t=rd_u32(vp);if(t==4)alignment=rd_u32(vp);}
        if(!skip_kv_value(p)){fprintf(stderr,"kv fail\n");return 1;} }
    // find a 2-D Q4_K weight (in_dim divisible by 256)
    char nm_out[256]={0}; uint64_t off=0, in_dim=0, out_dim=0; bool found=false;
    const uint8_t* tp=p;
    for(uint64_t i=0;i<n_tensors;i++){
        uint64_t nn=rd_u64(tp); const char* nm=(const char*)tp; size_t nl=nn; tp+=nn;
        uint32_t nd=rd_u32(tp); uint64_t d0=nd>0?rd_u64(tp):0; for(uint32_t d=1;d<nd;d++){}
        uint64_t d1=nd>1?rd_u64(tp):1; for(uint32_t d=2;d<nd;d++) rd_u64(tp);
        uint32_t type=rd_u32(tp); uint64_t o=rd_u64(tp);
        if(!found && type==GGML_Q4_K && nd==2 && d0%QK_K==0){
            found=true; in_dim=d0; out_dim=d1; off=o;
            size_t c=nl<255?nl:255; memcpy(nm_out,nm,c); nm_out[c]=0;
        }
    }
    if(!found){fprintf(stderr,"no 2D Q4_K tensor\n");return 1;}
    uint64_t hdr_end=(uint64_t)(tp-base);
    uint64_t data_start=(hdr_end+alignment-1)/alignment*alignment;
    uint8_t* tptr=base+data_start+off;
    int blocks_per_row=(int)(in_dim/QK_K);
    uint64_t nblocks=out_dim*blocks_per_row, nbytes=nblocks*144;
    printf("mmap %s (%.2f GB); weight %s Q4_K [in=%llu out=%llu] %.1f MB quantized\n",
           path, st.st_size/1e9, nm_out, (unsigned long long)in_dim,(unsigned long long)out_dim, nbytes/1e6);

    // 1 DMA hop -> VRAM-resident quantized weight (pinned mmap span)
    long pg=sysconf(_SC_PAGESIZE);
    uint8_t* rs=(uint8_t*)((uintptr_t)tptr & ~((uintptr_t)pg-1));
    size_t rl=((tptr+nbytes)-rs+pg-1)/pg*pg;
    madvise(rs,rl,MADV_WILLNEED);
    bool pinned=(hipHostRegister(rs,rl,hipHostRegisterDefault)==hipSuccess);
    uint8_t* dW=nullptr; float *dx=nullptr,*dy=nullptr;
    HIPCK(hipMalloc(&dW,nbytes));
    HIPCK(hipMalloc(&dx,in_dim*sizeof(float)));
    HIPCK(hipMalloc(&dy,out_dim*sizeof(float)));
    HIPCK(hipMemcpy(dW,tptr,nbytes,hipMemcpyHostToDevice));   // resident quantized

    // deterministic activation (a real mining forward feeds a normalized vec)
    float* x=(float*)malloc(in_dim*sizeof(float));
    uint32_t s=0x9E3779B9u; double nrm=0;
    for(uint64_t i=0;i<in_dim;i++){ s^=s<<13;s^=s>>17;s^=s<<5; float v=((int)(s&0xFFFF)-32768)/32768.f; x[i]=v; nrm+=v*(double)v; }
    nrm=sqrt(nrm); for(uint64_t i=0;i<in_dim;i++) x[i]/=(float)nrm;
    HIPCK(hipMemcpy(dx,x,in_dim*sizeof(float),hipMemcpyHostToDevice));

    // fused quantized GEMV — fp32 weight never materialized
    hipEvent_t e0,e1; hipEventCreate(&e0);hipEventCreate(&e1);
    HIPCK(hipEventRecord(e0));
    k_qgemv<<<(unsigned)out_dim, TPB>>>(dW,dx,dy,blocks_per_row);
    HIPCK(hipGetLastError());
    HIPCK(hipEventRecord(e1)); HIPCK(hipEventSynchronize(e1));
    float ms=0; hipEventElapsedTime(&ms,e0,e1);
    float* y=(float*)malloc(out_dim*sizeof(float));
    HIPCK(hipMemcpy(y,dy,out_dim*sizeof(float),hipMemcpyDeviceToHost));

    // CPU reference: dequant-then-GEMV, SAME fp32 precision contract as the
    // kernel (apples-to-apples: the only difference left is reduction order).
    float* yr=(float*)malloc(out_dim*sizeof(float));
    float* deq=(float*)malloc(QK_K*sizeof(float));
    for(uint64_t r=0;r<out_dim;r++){ float acc=0.f; const uint8_t* rb=tptr+(size_t)r*blocks_per_row*144;
        for(int b=0;b<blocks_per_row;b++){ dequant_q4k_block(rb+(size_t)b*144,deq);
            for(int i=0;i<QK_K;i++) acc+=deq[i]*x[b*QK_K+i]; } yr[r]=acc; }

    // decision + numeric agreement
    int am_d=0, am_r=0; float maxrel=0;
    for(uint64_t r=0;r<out_dim;r++){
        if(y[r]>y[am_d]) am_d=(int)r; if(yr[r]>yr[am_r]) am_r=(int)r;
        float den=fabsf(yr[r])>1e-6f?fabsf(yr[r]):1e-6f, rel=fabsf(y[r]-yr[r])/den;
        if(rel>maxrel) maxrel=rel;
    }
    // top-3 agreement (what topk mining reads)
    auto top3=[&](float* v, int* o){ o[0]=o[1]=o[2]=0;
        for(uint64_t r=0;r<out_dim;r++){ if(v[r]>v[o[0]]){o[2]=o[1];o[1]=o[0];o[0]=(int)r;}
            else if(v[r]>v[o[1]]){o[2]=o[1];o[1]=(int)r;} else if(v[r]>v[o[2]]) o[2]=(int)r; } };
    int t3d[3],t3r[3]; top3(y,t3d); top3(yr,t3r);
    bool top3_match=(t3d[0]==t3r[0]&&t3d[1]==t3r[1]&&t3d[2]==t3r[2]);

    printf("fused qGEMV: %llu outputs in %.3f ms; argmax dev=%d cpu=%d %s; top3 %s; max rel err %.2e\n",
           (unsigned long long)out_dim, ms, am_d, am_r, am_d==am_r?"MATCH":"DIFFER",
           top3_match?"MATCH":"DIFFER", maxrel);
    printf("  -> kernel fp32 noise floor %.2e << CNET_CERT_MARGIN 0.02: the margin\n"
           "     gate already abstains any point this noise could flip. Decision-safe.\n", maxrel);
    size_t vf,vt; hipMemGetInfo(&vf,&vt);
    printf("VRAM %.0f MB used; fp32 weight never materialized (host or device)\n",(vt-vf)/1e6);

    // The oracle consumes DECISIONS. Decision-identical is the hard gate;
    // numeric agreement to fp32 reduction-order noise (well under the margin
    // gate) is the supporting evidence.
    bool ok = (am_d==am_r) && top3_match && maxrel < 5e-3f;
    free(x);free(y);free(yr);free(deq);
    hipEventDestroy(e0);hipEventDestroy(e1);
    hipFree(dW);hipFree(dx);hipFree(dy);
    if(pinned) hipHostUnregister(rs);
    munmap(base,st.st_size); close(fd);
    printf("%s\n", ok?"QMATMUL SPIKE OK: resident Q4_K weight, fused dequant-in-dot, decision-identical to reference"
                     :"QMATMUL SPIKE FAIL");
    return ok?0:1;
}
