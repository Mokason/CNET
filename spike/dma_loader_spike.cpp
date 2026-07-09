// Spike: the CNET loader endgame in one file.
//
// Proves the whole loader can be rebuilt as "mmap the quantized GGUF, one
// direct async DMA hop of the quantized bytes into VRAM, dequant ON THE
// DEVICE" — replacing the 140 GB host fp32 explosion. Steps:
//
//   1. mmap the real Gemma Q4_K_M GGUF (no read(), no copy).
//   2. Locate one real Q4_K weight tensor (minimal GGUF directory parse).
//   3. hipHostRegister the tensor's page-aligned span — PIN the mmap so the
//      transfer is a true single DMA hop, not a staged pageable copy.
//   4. hipMemcpyAsync the quantized bytes straight to VRAM (THE ONE HOP).
//   5. A device Q4_K dequant kernel reads the quantized weight resident in
//      VRAM and writes fp32 — the model never exists as host fp32.
//   6. Cross-check the device dequant against the CPU scalar reference
//      (today's dequant xcheck, extended to the kernel) — bit-faithful or
//      the whole idea is unsafe (the Q4_K-placeholder lesson).
//
// Build: hipcc -O2 --offload-arch=gfx1201 -o bin/dma_loader_spike spike/dma_loader_spike.cpp
// Run:   ./bin/dma_loader_spike /home/marble/AI/Models/gemma4-v2-Q4_K_M.gguf

#include <hip/hip_runtime.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

#define QK_K 256
#define GGML_Q4_K 12
#define HIPCK(x) do { hipError_t e=(x); if(e!=hipSuccess){ \
    fprintf(stderr,"HIP error %s at %s:%d\n",hipGetErrorString(e),__FILE__,__LINE__); return 1; } } while(0)

// ---- shared f16->f32 (host + device) ----------------------------------
__host__ __device__ static inline float f16_to_f32(uint16_t h){
    uint32_t sign=(uint32_t)(h&0x8000)<<16, exp=(h>>10)&0x1F, man=h&0x3FF, bits;
    if(exp==0){ if(man==0) bits=sign; else { exp=127-15+1;
        while((man&0x400)==0){man<<=1;exp--;} man&=0x3FF; bits=sign|(exp<<23)|(man<<13);} }
    else if(exp==0x1F) bits=sign|0x7F800000|(man<<13);
    else bits=sign|((exp+127-15)<<23)|(man<<13);
    float f; memcpy(&f,&bits,4); return f;
}

// ---- Q4_K dequant, ONE 144-byte block -> 256 floats (host + device) ----
__host__ __device__ static void dequant_q4k_block(const uint8_t* blk, float* out){
    uint16_t d16, dmin16;
    memcpy(&d16, blk, 2); memcpy(&dmin16, blk+2, 2);
    float d=f16_to_f32(d16), dmin=f16_to_f32(dmin16);
    const uint8_t* scales=blk+4; const uint8_t* q=blk+16;
    int is=0; int oi=0;
    for(int j=0;j<QK_K;j+=64){
        uint8_t sc,mm; int jj=is;
        if(jj<4){sc=scales[jj]&63; mm=scales[jj+4]&63;}
        else{sc=(scales[jj+4]&0xF)|((scales[jj-4]>>6)<<4); mm=(scales[jj+4]>>4)|((scales[jj]>>6)<<4);}
        float d1=d*sc, m1=dmin*mm;
        jj=is+1;
        if(jj<4){sc=scales[jj]&63; mm=scales[jj+4]&63;}
        else{sc=(scales[jj+4]&0xF)|((scales[jj-4]>>6)<<4); mm=(scales[jj+4]>>4)|((scales[jj]>>6)<<4);}
        float d2=d*sc, m2=dmin*mm;
        for(int l=0;l<32;l++) out[oi++]=d1*(q[l]&0xF)-m1;
        for(int l=0;l<32;l++) out[oi++]=d2*(q[l]>>4)-m2;
        q+=32; is+=2;
    }
}

// device kernel: one block per thread-block, resident quantized weight in VRAM
__global__ void k_dequant_q4k(const uint8_t* __restrict__ blks, float* __restrict__ out, int nblocks){
    int b = blockIdx.x;
    if(b>=nblocks) return;
    dequant_q4k_block(blks + (size_t)b*144, out + (size_t)b*QK_K);
}

// ---- minimal GGUF directory parse -------------------------------------
struct GgufTensor { char name[256]; uint32_t type; uint64_t offset; uint64_t nelem; };

static uint64_t rd_u64(const uint8_t*&p){ uint64_t v; memcpy(&v,p,8); p+=8; return v; }
static uint32_t rd_u32(const uint8_t*&p){ uint32_t v; memcpy(&v,p,4); p+=4; return v; }

static bool skip_kv_value(const uint8_t*&p){
    uint32_t t=rd_u32(p);
    auto scalar=[&](int sz){ p+=sz; };
    switch(t){
        case 0: case 1: case 7: scalar(1); break;
        case 2: case 3: scalar(2); break;
        case 4: case 5: case 6: scalar(4); break;
        case 10: case 11: case 12: scalar(8); break;
        case 8: { uint64_t n=rd_u64(p); p+=n; } break;   // string
        case 9: { uint32_t et=rd_u32(p); uint64_t n=rd_u64(p);
                  for(uint64_t i=0;i<n;i++){
                      if(et==8){ uint64_t sn=rd_u64(p); p+=sn; }
                      else{ int sz=(et==0||et==1||et==7)?1:(et==2||et==3)?2:(et==4||et==5||et==6)?4:8; p+=sz; }
                  } } break;
        default: return false;
    }
    return true;
}

int main(int argc, char** argv){
    const char* path = argc>1 ? argv[1] : "/home/marble/AI/Models/gemma4-v2-Q4_K_M.gguf";
    int fd = open(path, O_RDONLY);
    if(fd<0){ perror("open"); return 1; }
    struct stat st; fstat(fd, &st);
    uint8_t* base = (uint8_t*)mmap(NULL, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
    if(base==MAP_FAILED){ perror("mmap"); return 1; }
    printf("mmap: %s (%.2f GB) — no read(), no host copy\n", path, st.st_size/1e9);

    const uint8_t* p = base;
    if(memcmp(p,"GGUF",4)!=0){ fprintf(stderr,"not GGUF\n"); return 1; }
    p+=4;
    uint32_t ver=rd_u32(p); (void)ver;
    uint64_t n_tensors=rd_u64(p), n_kv=rd_u64(p);
    uint64_t alignment=32;
    for(uint64_t i=0;i<n_kv;i++){
        uint64_t kn=rd_u64(p); const char* key=(const char*)p; size_t klen=kn; p+=kn;
        const uint8_t* vp=p;
        if(klen==17 && memcmp(key,"general.alignment",17)==0){ uint32_t t=rd_u32(vp); if(t==4){ alignment=rd_u32(vp);} }
        if(!skip_kv_value(p)){ fprintf(stderr,"kv parse fail\n"); return 1; }
    }
    // tensor directory
    GgufTensor chosen; bool found=false;
    const uint8_t* tp=p;
    for(uint64_t i=0;i<n_tensors;i++){
        uint64_t nn=rd_u64(tp); const char* nm=(const char*)tp; size_t nl=nn; tp+=nn;
        uint32_t ndim=rd_u32(tp); uint64_t ne=1;
        for(uint32_t d=0;d<ndim;d++) ne*=rd_u64(tp);
        uint32_t type=rd_u32(tp); uint64_t off=rd_u64(tp);
        if(!found && type==GGML_Q4_K && ne%QK_K==0){
            found=true; chosen.type=type; chosen.offset=off; chosen.nelem=ne;
            size_t c=nl<255?nl:255; memcpy(chosen.name,nm,c); chosen.name[c]=0;
        }
    }
    if(!found){ fprintf(stderr,"no Q4_K tensor found\n"); return 1; }
    // data section start = align(current pos, alignment)
    uint64_t hdr_end = (uint64_t)(tp - base);
    uint64_t data_start = (hdr_end + alignment - 1) / alignment * alignment;
    uint64_t abs_off = data_start + chosen.offset;
    uint64_t nblocks = chosen.nelem / QK_K;
    uint64_t nbytes = nblocks * 144;
    printf("tensor: %s  Q4_K  %llu elems  %llu blocks  %.2f MB quantized\n",
           chosen.name, (unsigned long long)chosen.nelem, (unsigned long long)nblocks, nbytes/1e6);

    // ---- 3. PIN the mmap'd tensor span (page-aligned) ------------------
    long pg = sysconf(_SC_PAGESIZE);
    uint8_t* tptr = base + abs_off;
    uint8_t* reg_start = (uint8_t*)((uintptr_t)tptr & ~((uintptr_t)pg-1));
    size_t reg_len = ((tptr + nbytes) - reg_start + pg - 1) / pg * pg;
    madvise(reg_start, reg_len, MADV_WILLNEED);
    hipError_t pinrc = hipHostRegister(reg_start, reg_len, hipHostRegisterDefault);
    bool pinned = (pinrc==hipSuccess);
    printf("pin mmap span (%.2f MB): %s%s\n", reg_len/1e6,
           pinned?"OK — true single-hop DMA":"pageable fallback (",
           pinned?"":hipGetErrorString(pinrc));
    if(!pinned) printf(")\n");

    // ---- 4. ONE async DMA hop: quantized bytes -> VRAM ----------------
    uint8_t* d_blks=nullptr; float* d_out=nullptr;
    HIPCK(hipMalloc(&d_blks, nbytes));
    HIPCK(hipMalloc(&d_out, nblocks*QK_K*sizeof(float)));
    hipStream_t s; HIPCK(hipStreamCreate(&s));
    hipEvent_t e0,e1; hipEventCreate(&e0); hipEventCreate(&e1);
    HIPCK(hipEventRecord(e0,s));
    HIPCK(hipMemcpyAsync(d_blks, tptr, nbytes, hipMemcpyHostToDevice, s));
    HIPCK(hipEventRecord(e1,s));

    // ---- 5. device dequant of the VRAM-resident quantized weight ------
    k_dequant_q4k<<<(unsigned)nblocks, 1, 0, s>>>(d_blks, d_out, (int)nblocks);
    HIPCK(hipGetLastError());

    // pull back for the cross-check
    float* h_dev = (float*)malloc(nblocks*QK_K*sizeof(float));
    HIPCK(hipMemcpyAsync(h_dev, d_out, nblocks*QK_K*sizeof(float), hipMemcpyDeviceToHost, s));
    HIPCK(hipStreamSynchronize(s));
    float ms=0; hipEventElapsedTime(&ms,e0,e1);
    printf("DMA hop: %.2f MB in %.3f ms (%.1f GB/s), VRAM resident\n", nbytes/1e6, ms, nbytes/1e6/ms);

    // ---- 6. CPU scalar reference on the SAME mmap bytes, bit-compare ---
    size_t diffs=0; float* ref=(float*)malloc(QK_K*sizeof(float));
    for(uint64_t b=0;b<nblocks;b++){
        dequant_q4k_block(tptr + b*144, ref);
        if(memcmp(ref, h_dev + b*QK_K, QK_K*sizeof(float))!=0) diffs++;
    }
    printf("device-vs-CPU dequant: %s (%llu/%llu blocks%s)\n",
           diffs==0?"BIT-IDENTICAL":"DIVERGE", (unsigned long long)(nblocks-diffs),
           (unsigned long long)nblocks, diffs?" DIFFER":"");

    // VRAM report
    size_t vfree,vtot; hipMemGetInfo(&vfree,&vtot);
    printf("VRAM: %.0f MB used of %.0f MB (one resident quantized tensor)\n",
           (vtot-vfree)/1e6, vtot/1e6);

    // cleanup — no orphaned compute
    free(ref); free(h_dev);
    hipEventDestroy(e0); hipEventDestroy(e1); hipStreamDestroy(s);
    hipFree(d_blks); hipFree(d_out);
    if(pinned) hipHostUnregister(reg_start);
    munmap(base, st.st_size); close(fd);
    printf("%s\n", diffs==0 ? "SPIKE OK: mmap -> 1 async DMA hop -> VRAM-resident quantized -> device dequant, bit-faithful" : "SPIKE FAIL");
    return diffs==0?0:1;
}
