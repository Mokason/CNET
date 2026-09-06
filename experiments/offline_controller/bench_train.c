#define _POSIX_C_SOURCE 200809L
#include "net.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
/* Synthetic timing/parity process, not an accuracy evaluator. No socket or
 * service calls. Unlike accuracy runs, leaves profiler descriptors intact. */
#define CHUNKS 8
static float x[CHUNKS][BATCH*INPUTS], y[CHUNKS][BATCH*ACTIONS];
static Net reference,candidate;
static Scratch scratch;
static float probabilities[BATCH*ACTIONS];
static double seconds(void){struct timespec t;assert(!clock_gettime(CLOCK_MONOTONIC,&t));return t.tv_sec+t.tv_nsec*1e-9;}
static double sequence(Net *m,int steps,cce_amdmath *gpu,float *loss) {
    double begin=seconds();
    for(int i=0;i<steps;i++) {
        *loss=net_step(m,&scratch,x[i%CHUNKS],y[i%CHUNKS],BATCH,.15f,gpu);
        assert(isfinite(*loss)&&*loss>0);
    }
    /* Each host API ends in a synchronous D2H copy; all work has completed. */
    return seconds()-begin;
}
static void compare(float a,float b,float *error){assert(isfinite(a)&&isfinite(b));*error=fmaxf(*error,fabsf(a-b));}
int main(int argc,char **argv) {
    assert(argc==4);char *end;
    long device=strtol(argv[1],&end,10);assert(!*end&&device>=0&&device<=1);
    long steps=strtol(argv[2],&end,10);assert(!*end&&steps>=1&&steps<=1024);
    long repeats=strtol(argv[3],&end,10);assert(!*end&&repeats>=1&&repeats<=5);
    uint32_t seed=471;
    for(int c=0;c<CHUNKS;c++)for(int b=0;b<BATCH;b++) {
        Task t;task_generate(&t,&seed);task_features(&t,t.start,x[c]+b*INPUTS);task_teacher(&t,t.start,y[c]+b*ACTIONS);
    }
    assert(!setenv("CNET_AMDMATH_MIN_FLOPS","0",1));char name[256];
    cce_amdmath *gpu=cce_amdmath_open_device((int)device,name,sizeof name);assert(gpu);
    float loss;
    net_init(&candidate,4,1,997);sequence(&candidate,8,gpu,&loss);
    net_init(&reference,4,1,997);sequence(&reference,8,NULL,&loss);
    for(int r=0;r<repeats;r++) {
        net_init(&candidate,4,1,997);reference=candidate;
        double cpu_time,gpu_time;float cpu_loss,gpu_loss;
        if(r%2){gpu_time=sequence(&candidate,(int)steps,gpu,&gpu_loss);cpu_time=sequence(&reference,(int)steps,NULL,&cpu_loss);}
        else {cpu_time=sequence(&reference,(int)steps,NULL,&cpu_loss);gpu_time=sequence(&candidate,(int)steps,gpu,&gpu_loss);}
        float weight_error=0,prob_error=0,loss_error=fabsf(cpu_loss-gpu_loss);
        for(int t=0;t<DEPTH;t++)for(int j=0;j<HIDDEN*JOINED;j++)compare(reference.w[t][j],candidate.w[t][j],&weight_error);
        for(int j=0;j<OUTPUTS*(HIDDEN+1);j++)compare(reference.out[j],candidate.out[j],&weight_error);
        assert(net_step(&reference,&scratch,x[0],NULL,BATCH,0,NULL)==0);
        for(int b=0;b<BATCH;b++)memcpy(probabilities+b*ACTIONS,scratch.p+b*OUTPUTS,ACTIONS*sizeof(float));
        assert(net_step(&candidate,&scratch,x[0],NULL,BATCH,0,gpu)==0);
        for(int b=0;b<BATCH;b++)for(int a=0;a<ACTIONS;a++)compare(probabilities[b*ACTIONS+a],scratch.p[b*OUTPUTS+a],&prob_error);
        if(weight_error>=1e-4f||prob_error>=1e-4f||loss_error>=1e-4f)fprintf(stderr,"CONTROLLER_SUSTAINED_PARITY_RED\n");
        assert(weight_error<1e-4f&&prob_error<1e-4f&&loss_error<1e-4f);
        printf("{\"kind\":\"training_benchmark\",\"device\":%ld,\"repeat\":%d,\"updates\":%ld,\"batch\":%d,\"cpu_seconds\":%.8f,\"gpu_seconds\":%.8f,\"speedup_over_scalar_cpu\":%.5f,\"weight_max_abs\":%.9g,\"prob_max_abs\":%.9g,\"loss_abs\":%.9g,\"shared_machine\":true,\"parity_pass\":true}\n",device,r,steps,BATCH,cpu_time,gpu_time,cpu_time/gpu_time,weight_error,prob_error,loss_error);
    }
    cce_amdmath_close(gpu);return 0;
}
