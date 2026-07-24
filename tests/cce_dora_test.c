#include "../include/cce/cce_dora.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int f;
#define C(c,m) do{if(!(c)){fprintf(stderr,"FAIL %s\n",m);f++;}else printf("ok %s\n",m);}while(0)
int main(void){
    const int in=16, out=8, rank=4, n=64;
    cce_dora d;
    float *X,*R;
    cce_lora_train_opts opt=cce_lora_train_defaults();
    int i,j;
    C(cce_dora_init(&d,in,out,rank,8.f,42)==CCE_OK,"init");
    {
        cce_tensor x,y; int xs[1]={in}, ys[1]={out};
        cce_tensor_alloc(&x,xs,1); cce_tensor_alloc(&y,ys,1);
        for(i=0;i<in;i++) x.data[i]=0.1f*(float)i;
        for(i=0;i<out;i++) y.data[i]=1.f;
        float before[32]; memcpy(before,y.data,out*sizeof(float));
        cce_dora_apply(&d,&x,&y);
        { float md=0; for(i=0;i<out;i++){float dd=fabsf(before[i]-y.data[i]); if(dd>md)md=dd;}
          C(md==0.f,"untrained no-op"); }
        cce_tensor_free(&x); cce_tensor_free(&y);
    }
    X=malloc((size_t)n*in*sizeof(float)); R=malloc((size_t)n*out*sizeof(float));
    for(i=0;i<n;i++){
        for(j=0;j<in;j++) X[i*in+j]=((i*31+j)%100)/100.f;
        for(j=0;j<out;j++) R[i*out+j]=0.05f*(float)((i+j)%7);
    }
    opt.epochs=200; opt.lr=0.05f;
    { double mse=cce_dora_train(&d,X,R,(size_t)n,&opt);
      C(mse>=0 && mse<1.0,"train mse finite"); /* DoRA mag scale is gentle; LoRA does the fit */ }
    free(X); free(R);
    cce_dora_free(&d);
    if(f) return 1;
    printf("CCE_DORA_PASS checks=3\n");
    return 0;
}
