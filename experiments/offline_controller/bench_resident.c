#define _POSIX_C_SOURCE 200809L
#include "resident.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
/* Timing/parity fixture only, not held-out task accuracy. */
static Net cpu,host,copy;
#ifdef CONTROLLER_OPENBLAS
#include <cblas.h>
static Net optimized;
float optimized_step(Net *,Scratch *,const float *,const float *,int,float);
#endif
static Scratch scratch;
static float x[8][BATCH*INPUTS],y[8][BATCH*ACTIONS];
static float probabilities[BATCH*OUTPUTS],resident_probabilities[BATCH*OUTPUTS];
static double seconds(void){struct timespec t;assert(!clock_gettime(CLOCK_MONOTONIC,&t));return t.tv_sec+t.tv_nsec*1e-9;}
static double sequence(Net *m,cce_amdmath *g,Resident *r,int steps,float *loss){
    double start=seconds();
    for(int i=0;i<steps;i++){
#ifdef CONTROLLER_OPENBLAS
        if(m==&optimized)*loss=optimized_step(m,&scratch,x[i%8],y[i%8],BATCH,.15f);
        else
#endif
        if(r){
            int count=steps-i<8?steps-i:8;float losses[8];
            assert(!resident_steps(r,&x[0][0],&y[0][0],count,BATCH,.15f,losses));
            *loss=losses[count-1];i+=count-1;
        }
        else *loss=net_step(m,&scratch,x[i%8],y[i%8],BATCH,.15f,g);
        assert(isfinite(*loss)&&*loss>0);
    }
    return seconds()-start;
}
static float error(const Net *a,const Net *b){
    float e=0;
    for(int t=0;t<DEPTH;t++)for(int i=0;i<HIDDEN*JOINED;i++){
        assert(isfinite(a->w[t][i])&&isfinite(b->w[t][i]));e=fmaxf(e,fabsf(a->w[t][i]-b->w[t][i]));
    }
    for(int i=0;i<OUTPUTS*(HIDDEN+1);i++){
        assert(isfinite(a->out[i])&&isfinite(b->out[i]));e=fmaxf(e,fabsf(a->out[i]-b->out[i]));
    }
    return e;
}
int main(int argc,char **argv){
    assert(argc==4);char *end;
    long device=strtol(argv[1],&end,10);assert(!*end&&device>=0&&device<2);
    long steps=strtol(argv[2],&end,10);assert(!*end&&steps>0&&steps<=128);
    long repeats=strtol(argv[3],&end,10);assert(!*end&&repeats>0&&repeats<=5);
    uint32_t rng=471;
    for(int c=0;c<8;c++)for(int b=0;b<BATCH;b++){Task t;task_generate(&t,&rng);task_features(&t,t.start,x[c]+b*INPUTS);task_teacher(&t,t.start,y[c]+b*ACTIONS);}
    assert(!setenv("CNET_AMDMATH_MIN_FLOPS","0",1));
    cce_amdmath *g=cce_amdmath_open_device((int)device,NULL,0);assert(g);
    net_init(&cpu,4,1,997);host=cpu;Resident *r=resident_open(&cpu,(int)device);assert(r);
    printf("{\"kind\":\"resident_backend\",\"name\":\"%s\",\"device\":%ld}\n",resident_backend(r),device);
    float losses[4];sequence(&cpu,NULL,NULL,8,losses);sequence(&host,g,NULL,8,losses+1);sequence(NULL,NULL,r,8,losses+2);resident_close(r);
    int modes=3;
#ifdef CONTROLLER_OPENBLAS
    net_init(&optimized,4,1,997);sequence(&optimized,NULL,NULL,8,losses+3);modes=4;
#endif
    for(int k=0;k<repeats;k++){
        net_init(&cpu,4,1,997);host=cpu;r=resident_open(&cpu,(int)device);assert(r);
        double duration[4];
#ifdef CONTROLLER_OPENBLAS
        optimized=cpu;
#endif
        for(int j=0;j<modes;j++)switch((j+k)%modes){
            case 0:duration[0]=sequence(&cpu,NULL,NULL,(int)steps,losses);break;
            case 1:duration[1]=sequence(&host,g,NULL,(int)steps,losses+1);break;
            case 2:duration[2]=sequence(NULL,NULL,r,(int)steps,losses+2);break;
#ifdef CONTROLLER_OPENBLAS
            case 3:duration[3]=sequence(&optimized,NULL,NULL,(int)steps,losses+3);break;
#endif
        }
        assert(!resident_snapshot(r,&copy));float eh=error(&cpu,&host),er=error(&cpu,&copy);
        assert(eh<1e-4f&&er<1e-4f&&fabsf(losses[0]-losses[1])<1e-4f&&fabsf(losses[0]-losses[2])<1e-4f);
        assert(net_step(&cpu,&scratch,x[0],NULL,BATCH,0,NULL)==0);
        for(int i=0;i<BATCH*OUTPUTS;i++)probabilities[i]=scratch.p[i];
        assert(net_step(&host,&scratch,x[0],NULL,BATCH,0,g)==0);
        assert(!resident_predict(r,x[0],BATCH,resident_probabilities));float ep=0;
        for(int b=0;b<BATCH;b++)for(int a=0;a<ACTIONS;a++){
            int i=b*OUTPUTS+a;assert(isfinite(probabilities[i])&&isfinite(scratch.p[i])&&isfinite(resident_probabilities[i]));
            ep=fmaxf(ep,fabsf(probabilities[i]-scratch.p[i]));ep=fmaxf(ep,fabsf(probabilities[i]-resident_probabilities[i]));
        }
        assert(ep<1e-4f);
#ifdef CONTROLLER_OPENBLAS
        float eo=error(&cpu,&optimized);assert(eo<1e-4f&&fabsf(losses[0]-losses[3])<1e-4f);
        assert(optimized_step(&optimized,&scratch,x[0],NULL,BATCH,0)==0);
        for(int b=0;b<BATCH;b++)for(int a=0;a<ACTIONS;a++)assert(fabsf(probabilities[b*OUTPUTS+a]-scratch.p[b*OUTPUTS+a])<1e-4f);
        printf("{\"kind\":\"optimized_cpu_benchmark\",\"device\":%ld,\"repeat\":%d,\"updates\":%ld,\"batch\":128,\"threads\":%d,\"openblas_seconds\":%.8f,\"resident_seconds\":%.8f,\"weight_max_abs\":%.9g,\"parity_pass\":true,\"shared_machine\":true}\n",device,k,steps,openblas_get_num_threads(),duration[3],duration[2],eo);
#endif
        printf("{\"kind\":\"resident_benchmark\",\"device\":%ld,\"repeat\":%d,\"updates\":%ld,\"batch\":%d,\"enqueue_batches\":8,\"scalar_cpu_seconds\":%.8f,\"host_gpu_seconds\":%.8f,\"resident_seconds\":%.8f,\"cpu_host_weight_abs\":%.9g,\"cpu_resident_weight_abs\":%.9g,\"shared_machine\":true,\"parity_pass\":true}\n",device,k,steps,BATCH,duration[0],duration[1],duration[2],eh,er);
        resident_close(r);
    }
    cce_amdmath_close(g);return 0;
}
