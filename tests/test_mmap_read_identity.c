/*
 * Hop-1 invariant test: mmap-read == fread-read for the same tensor.
 *
 * CNET_GGUF_MMAP changes exactly ONE thing in cce_gguf_load_f32 — the source
 * of the quantized bytes: fread from a FILE* vs a fault from the whole-file
 * mmap (via fmemopen). data_offset is computed identically (from the real
 * file, before the swap). So the only thing to prove is that reading a
 * tensor's bytes at abs_off = data_start + tensor.offset yields IDENTICAL
 * bytes both ways — after which every dequant branch and everything
 * downstream is identical by construction.
 *
 * This reads a real Gemma Q4_K tensor via (a) fopen+fseeko+fread and
 * (b) mmap+pointer at the SAME offset, and memcmps raw bytes AND dequant
 * output. Seconds, no model load, no CNET link.
 *
 * Build: cc -O2 -D_FILE_OFFSET_BITS=64 -x c++ -o bin/test_mmap_read_identity tests/test_mmap_read_identity.c -lm
 *   (-x c++: the minimal GGUF parser uses C++ reference-param cursors, as in spike/)
 * Result: blk.0.attn_k.weight raw + dequant byte-identical (30720 blocks), ~9 ms.
 */
#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#if CNET_HAVE_MMAP
#include <sys/mman.h>
#endif
#include <sys/stat.h>

#define QK_K 256
#define GGML_Q4_K 12

static float f16(uint16_t h){uint32_t s=(uint32_t)(h&0x8000)<<16,e=(h>>10)&0x1F,m=h&0x3FF,b;
    if(e==0){if(m==0)b=s;else{e=113;while((m&0x400)==0){m<<=1;e--;}m&=0x3FF;b=s|(e<<23)|(m<<13);}}
    else if(e==0x1F)b=s|0x7F800000|(m<<13); else b=s|((e+112)<<23)|(m<<13);
    float f;memcpy(&f,&b,4);return f;}
static void dq4k(const uint8_t*blk,float*out){uint16_t d16,dm16;memcpy(&d16,blk,2);memcpy(&dm16,blk+2,2);
    float d=f16(d16),dmin=f16(dm16);const uint8_t*sc=blk+4,*q=blk+16;int is=0,oi=0;
    for(int j=0;j<QK_K;j+=64){uint8_t s,m;int jj=is;
        if(jj<4){s=sc[jj]&63;m=sc[jj+4]&63;}else{s=(sc[jj+4]&0xF)|((sc[jj-4]>>6)<<4);m=(sc[jj+4]>>4)|((sc[jj]>>6)<<4);}
        float d1=d*s,m1=dmin*m;jj=is+1;
        if(jj<4){s=sc[jj]&63;m=sc[jj+4]&63;}else{s=(sc[jj+4]&0xF)|((sc[jj-4]>>6)<<4);m=(sc[jj+4]>>4)|((sc[jj]>>6)<<4);}
        float d2=d*s,m2=dmin*m;
        for(int l=0;l<32;l++)out[oi++]=d1*(q[l]&0xF)-m1;
        for(int l=0;l<32;l++)out[oi++]=d2*(q[l]>>4)-m2;q+=32;is+=2;}}

static uint64_t u64(const uint8_t*&p){uint64_t v;memcpy(&v,p,8);p+=8;return v;}
static uint32_t u32(const uint8_t*&p){uint32_t v;memcpy(&v,p,4);p+=4;return v;}
static int skipkv(const uint8_t*&p){uint32_t t=u32(p);
    switch(t){case 0:case 1:case 7:p+=1;break;case 2:case 3:p+=2;break;case 4:case 5:case 6:p+=4;break;
    case 10:case 11:case 12:p+=8;break;case 8:{uint64_t n=u64(p);p+=n;}break;
    case 9:{uint32_t et=u32(p);uint64_t n=u64(p);for(uint64_t i=0;i<n;i++){if(et==8){uint64_t sn=u64(p);p+=sn;}
        else{int z=(et==0||et==1||et==7)?1:(et==2||et==3)?2:(et==4||et==5||et==6)?4:8;p+=z;}}}break;
    default:return 0;}return 1;}
#if CNET_HAVE_MMAP

int main(int argc,char**argv){
    const char* path=argc>1?argv[1]:"/home/marble/AI/Models/gemma4-v2-Q4_K_M.gguf";
    /* A model that is not on this box is a SKIP, not a failure. Saying so here,
       rather than masking a nonzero exit with `|| true` in the recipe, is what
       keeps recipe_gate satisfied and keeps a real crash visible. */
    int fd=open(path,O_RDONLY);
    if(fd<0){printf("HOP1 SKIP: no model at %s\n", path); return 0;}
    struct stat st; fstat(fd,&st);
    uint8_t* base=(uint8_t*)mmap(NULL,st.st_size,PROT_READ,MAP_SHARED,fd,0);
    if(base==MAP_FAILED){perror("mmap");return 1;}
    const uint8_t*p=base; if(memcmp(p,"GGUF",4)){fprintf(stderr,"not GGUF\n");return 1;} p+=4;
    u32(p); uint64_t nt=u64(p),nk=u64(p),align=32;
    for(uint64_t i=0;i<nk;i++){uint64_t kn=u64(p);const char*k=(const char*)p;size_t kl=kn;p+=kn;
        const uint8_t*vp=p; if(kl==17&&!memcmp(k,"general.alignment",17)){uint32_t t=u32(vp);if(t==4)align=u32(vp);}
        if(!skipkv(p)){fprintf(stderr,"kv fail\n");return 1;}}
    char nm[256]={0}; uint64_t off=0,ne=0; int found=0; const uint8_t*tp=p;
    for(uint64_t i=0;i<nt;i++){uint64_t nn=u64(tp);const char*n=(const char*)tp;size_t nl=nn;tp+=nn;
        uint32_t nd=u32(tp);uint64_t e=1;for(uint32_t d=0;d<nd;d++)e*=u64(tp);uint32_t ty=u32(tp);uint64_t o=u64(tp);
        if(!found&&ty==GGML_Q4_K&&e%QK_K==0){found=1;off=o;ne=e;size_t c=nl<255?nl:255;memcpy(nm,n,c);nm[c]=0;}}
    if(!found){fprintf(stderr,"no Q4_K tensor\n");return 1;}
    uint64_t hdr=(uint64_t)(tp-base), data_start=(hdr+align-1)/align*align, abs_off=data_start+off;
    uint64_t nblk=ne/QK_K, nbytes=nblk*144;
    printf("tensor %s Q4_K, abs_off=%llu, %.2f MB quantized\n", nm,(unsigned long long)abs_off,nbytes/1e6);

    // (a) FILE* path — exactly cce_gguf_load_f32's fseeko+fread
    uint8_t* via_fread=(uint8_t*)malloc(nbytes);
    FILE* f=fopen(path,"rb"); fseeko(f,(off_t)abs_off,SEEK_SET);
    if(fread(via_fread,1,nbytes,f)!=nbytes){fprintf(stderr,"fread short\n");return 1;} fclose(f);
    // (b) mmap path — hop 1's direct source
    const uint8_t* via_mmap=base+abs_off;

    size_t raw_diff = memcmp(via_fread, via_mmap, nbytes);
    // dequant both, compare fp32 output
    float *da=(float*)malloc(QK_K*sizeof(float)),*db=(float*)malloc(QK_K*sizeof(float));
    size_t dq_diff=0;
    for(uint64_t b=0;b<nblk;b++){dq4k(via_fread+b*144,da);dq4k(via_mmap+b*144,db);
        if(memcmp(da,db,QK_K*sizeof(float))) dq_diff++;}
    printf("raw bytes: %s; dequant fp32: %s (%llu blocks)\n",
        raw_diff==0?"IDENTICAL":"DIFFER", dq_diff==0?"IDENTICAL":"DIFFER",(unsigned long long)nblk);
    free(via_fread);free(da);free(db); munmap(base,st.st_size); close(fd);
    int ok = (raw_diff==0 && dq_diff==0);
    printf("%s\n", ok?"HOP1 INVARIANT OK: mmap read == fread read, byte-identical dequant":"HOP1 FAIL");
    return ok?0:1;
}
#else /* !CNET_HAVE_MMAP */

/* This invariant is about mmap vs fread returning the same bytes. Without
   mmap there is no second path to compare, so the test reports a skip rather
   than a pass -- a platform that cannot run a proof must not print one. */
int main(void) {
    printf("HOP1 SKIP: no mmap on this platform; nothing to compare\n");
    return 0;
}

#endif /* CNET_HAVE_MMAP */
