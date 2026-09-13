#include "cnet_vsa_text.h"
#include <immintrin.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <assert.h>
static float fast(const int8_t *a,float an,const int8_t *b,float bn) {
    __m512i d0=_mm512_setzero_si512(),d1=d0,d2=d0,d3=d0,s0=d0,s1=d0,s2=d0,s3=d0;
    __m512i bias=_mm512_set1_epi8((char)128),ones=_mm512_set1_epi8(1);
    for(int i=0;i<CNET_VSA_TOPICAL_DIM;i+=256) {
        __m512i a0=_mm512_xor_si512(_mm512_loadu_si512(a+i+0),bias),b0=_mm512_loadu_si512(b+i+0);
        d0=_mm512_dpbusd_epi32(d0,a0,b0);s0=_mm512_dpbusd_epi32(s0,ones,b0);
        __m512i a1=_mm512_xor_si512(_mm512_loadu_si512(a+i+64),bias),b1=_mm512_loadu_si512(b+i+64);
        d1=_mm512_dpbusd_epi32(d1,a1,b1);s1=_mm512_dpbusd_epi32(s1,ones,b1);
        __m512i a2=_mm512_xor_si512(_mm512_loadu_si512(a+i+128),bias),b2=_mm512_loadu_si512(b+i+128);
        d2=_mm512_dpbusd_epi32(d2,a2,b2);s2=_mm512_dpbusd_epi32(s2,ones,b2);
        __m512i a3=_mm512_xor_si512(_mm512_loadu_si512(a+i+192),bias),b3=_mm512_loadu_si512(b+i+192);
        d3=_mm512_dpbusd_epi32(d3,a3,b3);s3=_mm512_dpbusd_epi32(s3,ones,b3);
    }
    __m512i dot=_mm512_add_epi32(_mm512_add_epi32(d0,d1),_mm512_add_epi32(d2,d3));
    __m512i sum=_mm512_add_epi32(_mm512_add_epi32(s0,s1),_mm512_add_epi32(s2,s3));
    int32_t v=_mm512_reduce_add_epi32(dot)-128*_mm512_reduce_add_epi32(sum);
    return (float)v/(an*bn);
}
__attribute__((noinline)) static void batch(const int8_t *a,const int8_t *b,float *out) {
    const __m512i bias=_mm512_set1_epi8((char)128),ones=_mm512_set1_epi8(1);
    for(int c=0;c<812;c+=4) {
        __m512i d0=_mm512_setzero_si512(),d1=d0,d2=d0,d3=d0,s0=d0,s1=d0,s2=d0,s3=d0;
        for(int i=0;i<CNET_VSA_TOPICAL_DIM;i+=64) {
            __m512i av=_mm512_xor_si512(_mm512_loadu_si512(a+i),bias);
            __m512i b0=_mm512_loadu_si512(b+(c+0)*CNET_VSA_TOPICAL_DIM+i);d0=_mm512_dpbusd_epi32(d0,av,b0);s0=_mm512_dpbusd_epi32(s0,ones,b0);
            __m512i b1=_mm512_loadu_si512(b+(c+1)*CNET_VSA_TOPICAL_DIM+i);d1=_mm512_dpbusd_epi32(d1,av,b1);s1=_mm512_dpbusd_epi32(s1,ones,b1);
            __m512i b2=_mm512_loadu_si512(b+(c+2)*CNET_VSA_TOPICAL_DIM+i);d2=_mm512_dpbusd_epi32(d2,av,b2);s2=_mm512_dpbusd_epi32(s2,ones,b2);
            __m512i b3=_mm512_loadu_si512(b+(c+3)*CNET_VSA_TOPICAL_DIM+i);d3=_mm512_dpbusd_epi32(d3,av,b3);s3=_mm512_dpbusd_epi32(s3,ones,b3);
        }
        out[c+0]=(float)(_mm512_reduce_add_epi32(d0)-128*_mm512_reduce_add_epi32(s0))/(4000.f*4000.f);
        out[c+1]=(float)(_mm512_reduce_add_epi32(d1)-128*_mm512_reduce_add_epi32(s1))/(4000.f*4000.f);
        out[c+2]=(float)(_mm512_reduce_add_epi32(d2)-128*_mm512_reduce_add_epi32(s2))/(4000.f*4000.f);
        out[c+3]=(float)(_mm512_reduce_add_epi32(d3)-128*_mm512_reduce_add_epi32(s3))/(4000.f*4000.f);
    }
}
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1e6+t.tv_nsec/1e3;}
int main(void) {
    int8_t *all=malloc(812*CNET_VSA_TOPICAL_DIM),query[CNET_VSA_TOPICAL_DIM];
    uint32_t state=13;
    for(int i=0;i<812*CNET_VSA_TOPICAL_DIM;++i){state=state*1664525+1013904223;all[i]=(int8_t)(state>>24);}
    for(int i=0;i<CNET_VSA_TOPICAL_DIM;++i)query[i]=(int8_t)i;
    for(int i=0;i<812;++i)assert(fast(query,4000,all+i*CNET_VSA_TOPICAL_DIM,4000)==cnet_vsa_text_q8_similarity_n(query,4000,all+i*CNET_VSA_TOPICAL_DIM,4000));
    volatile float total=0;
    for(int mode=0;mode<2;++mode){double start=now();for(int r=0;r<1000;++r)for(int i=0;i<812;++i)total+=mode?fast(query,4000,all+i*CNET_VSA_TOPICAL_DIM,4000):cnet_vsa_text_q8_similarity_n(query,4000,all+i*CNET_VSA_TOPICAL_DIM,4000);printf("mode%d scan_us %.3f\n",mode,(now()-start)/1000);}
    float out[812];batch(query,all,out);
    for(int i=0;i<812;++i)assert(out[i]==cnet_vsa_text_q8_similarity_n(query,4000,all+i*CNET_VSA_TOPICAL_DIM,4000));
    double start=now();for(int r=0;r<1000;++r){batch(query,all,out);total+=out[r%812];}printf("batch scan_us %.3f\n",(now()-start)/1000);
    free(all);return total==0;
}
