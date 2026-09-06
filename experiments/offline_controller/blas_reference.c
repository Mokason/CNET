/* Benchmark-only instantiation: preserve the reference training algorithm and
 * substitute its two matrix operations, rather than maintain a copied network. */
#include "net.h"
#include <cblas.h>
static int blas_linear(cce_amdmath *unused,const float *x,const float *w,float *y,size_t e,size_t n,size_t in,size_t out){
    (void)unused;if(e!=1)return -1;
    cblas_sgemm(CblasRowMajor,CblasNoTrans,CblasTrans,(int)n,(int)out,(int)in,1,x,(int)in,w,(int)in,0,y,(int)out);return 0;
}
static int blas_update(cce_amdmath *unused,float *w,const float *x,const float *dy,size_t e,size_t n,size_t in,size_t out,float lr){
    (void)unused;if(e!=1)return -1;
    cblas_sgemm(CblasRowMajor,CblasTrans,CblasNoTrans,(int)out,(int)in,(int)n,-lr,dy,(int)out,x,(int)in,1,w,(int)in);return 0;
}
#define cce_amdmath_linear_f32 blas_linear
#define cce_amdmath_sgd_f32 blas_update
#define net_step blas_net_step
#define net_init blas_net_init
#define net_parameters blas_net_parameters
#define net_forward_flops blas_net_forward_flops
#include "net.c"
float optimized_step(Net *m,Scratch *s,const float *x,const float *y,int n,float lr){
    static char backend_tag;
    return blas_net_step(m,s,x,y,n,lr,(cce_amdmath*)(void*)&backend_tag);
}
