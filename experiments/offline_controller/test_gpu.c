#define _GNU_SOURCE
#include "net.h"
#include "offline.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static Net reference, candidate;
static Scratch scratch;
static float x[BATCH*INPUTS], y[BATCH*ACTIONS], p[BATCH*OUTPUTS];
int main(void) {
    assert(!close_range(3,~0u,0));
    assert(!setenv("CNET_AMDMATH_MIN_FLOPS","0",1));
    cce_amdmath *gpu[2]; char name[256];
    for (int d=0;d<2;d++) {
        gpu[d]=cce_amdmath_open_device(d,name,sizeof name);
        assert(gpu[d]); printf("CONTROLLER_DEVICE index=%d name=%s\n",d,name);
    }
    assert(!offline_seal(-1) && !offline_negative_test());
    uint32_t seed=745;
    for (int b=0;b<BATCH;b++) {
        Task t; task_generate(&t,&seed); task_features(&t,t.start,x+b*INPUTS);
        task_teacher(&t,t.start,y+b*ACTIONS);
    }
    for (int d=0;d<2;d++) for (int mode=0;mode<3;mode++) {
        net_init(&reference,mode?4:1,mode!=2,43); candidate=reference;
        assert(net_step(&reference,&scratch,x,y,BATCH,.15f,NULL)>0);
        memcpy(p,scratch.p,sizeof p);
        float gpu_loss=net_step(&candidate,&scratch,x,y,BATCH,.15f,gpu[d]);
        if (!(gpu_loss>0)) fprintf(stderr,"GPU_STEP_RED device=%d mode=%d error=%s\n",d,mode,cce_amdmath_last_error(gpu[d]));
        assert(gpu_loss>0);
        float error=0;
        for (int i=0;i<BATCH*OUTPUTS;i++) if (fabsf(p[i]-scratch.p[i])>error) error=fabsf(p[i]-scratch.p[i]);
        for (int t=0;t<4;t++) for (int i=0;i<HIDDEN*JOINED;i++)
            if (fabsf(reference.w[t][i]-candidate.w[t][i])>error) error=fabsf(reference.w[t][i]-candidate.w[t][i]);
        for (int i=0;i<OUTPUTS*(HIDDEN+1);i++)
            if (fabsf(reference.out[i]-candidate.out[i])>error) error=fabsf(reference.out[i]-candidate.out[i]);
        assert(error<.00002f);
        printf("CONTROLLER_GPU_PARITY_PASS device=%d mode=%d max_abs=%.9g network_denied=1\n",d,mode,error);
    }
    for (int d=0;d<2;d++) cce_amdmath_close(gpu[d]);
}
