#include "net.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static Net original, updated, plus, minus;
static Scratch scratch;
static float x[2*INPUTS], y[2*ACTIONS];
int main(void) {
    uint32_t seed=123;
    for (int i=0;i<2;i++) {
        Task t; task_generate(&t,&seed); task_features(&t,t.start,x+i*INPUTS);
        task_teacher(&t,t.start,y+i*ACTIONS);
    }
    for (int mode=0;mode<3;mode++) {
        net_init(&original,mode?4:1,mode!=2,919);
        updated=original;
        float initial=net_step(&updated,&scratch,x,y,2,.1f,NULL);
        assert(initial>0 && memcmp(&updated,&original,sizeof original));
        float max_error=0;
        int layers=original.tied?1:original.depth;
        for (int layer=0;layer<=layers;layer++) for (int k=0;k<8;k++) {
            int offset=layer==layers ? k*61 : (k*1799)%(HIDDEN*JOINED);
            float *o=layer==layers?original.out:original.w[layer];
            float *u=layer==layers?updated.out:updated.w[layer];
            plus=minus=original;
            float *p=layer==layers?plus.out:plus.w[layer];
            float *m=layer==layers?minus.out:minus.w[layer];
            p[offset]+=.002f; m[offset]-=.002f;
            float a=net_step(&plus,&scratch,x,y,2,0,NULL);
            float b=net_step(&minus,&scratch,x,y,2,0,NULL);
            float error=fabsf((a-b)/.004f-(o[offset]-u[offset])/.1f);
            if (error>max_error) max_error=error;
            assert(error<.0015f);
        }
        for (int i=0;i<80;i++) assert(net_step(&updated,&scratch,x,y,2,.15f,NULL)>0);
        float final=net_step(&updated,&scratch,x,y,2,0,NULL);
        assert(final<initial*.3f);
        plus=updated; (void)net_step(&updated,&scratch,x,y,2,0,NULL);
        assert(!memcmp(&plus,&updated,sizeof plus));
        printf("CONTROLLER_NET_PASS mode=%d gradient_max_abs=%.7g initial=%.6f final=%.6f frozen=1\n",mode,max_error,initial,final);
    }
    x[0]=NAN;
    assert(net_step(&updated,&scratch,x,y,2,0,NULL)<0);
}
