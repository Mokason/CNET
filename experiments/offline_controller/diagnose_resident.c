#include "resident.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
static Net cpu,host_gpu,gpu_weights;
static Scratch scratch;
static float x[BATCH*INPUTS],y[BATCH*ACTIONS];
int main(void) {
    uint32_t rng=711;
    for(int b=0;b<BATCH;b++){Task t;task_generate(&t,&rng);task_features(&t,t.start,x+b*INPUTS);task_teacher(&t,t.start,y+b*ACTIONS);}
    assert(!setenv("CNET_AMDMATH_MIN_FLOPS","0",1));
    cce_amdmath *g=cce_amdmath_open_device(0,NULL,0);assert(g);
    net_init(&cpu,4,1,997);host_gpu=cpu;Resident *r=resident_open(&cpu,0);assert(r);
    for(int i=0;i<128;i++) {
        float a=net_step(&cpu,&scratch,x,y,BATCH,.15f,NULL),b=net_step(&host_gpu,&scratch,x,y,BATCH,.15f,g),c;
        assert(!resident_step(r,x,y,BATCH,.15f,&c));assert(!resident_snapshot(r,&gpu_weights));
        float ch=0,cr=0,hr=0;
        for(int t=0;t<DEPTH;t++)for(int j=0;j<HIDDEN*JOINED;j++){
            ch=fmaxf(ch,fabsf(cpu.w[t][j]-host_gpu.w[t][j]));cr=fmaxf(cr,fabsf(cpu.w[t][j]-gpu_weights.w[t][j]));hr=fmaxf(hr,fabsf(host_gpu.w[t][j]-gpu_weights.w[t][j]));
        }
        if(i<3||i%10==9||i==36||i==127)printf("step=%d cpu_loss=%.9g host_gpu_loss=%.9g resident_loss=%.9g cpu_host_weight=%.9g cpu_resident_weight=%.9g host_resident_weight=%.9g\n",i+1,a,b,c,ch,cr,hr);
    }
    resident_close(r);cce_amdmath_close(g);return 0;
}
