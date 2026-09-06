#include "resident.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
static Net initial,snapshot;
static float x[BATCH*INPUTS],y[BATCH*ACTIONS];
int main(int argc,char **argv){
    assert(argc==2);char *end;long device=strtol(argv[1],&end,10);assert(!*end&&device>=0&&device<2);
    net_init(&initial,4,1,997);uint32_t rng=471;
    for(int b=0;b<BATCH;b++){Task t;task_generate(&t,&rng);task_features(&t,t.start,x+b*INPUTS);task_teacher(&t,t.start,y+b*ACTIONS);}
    Resident *r=resident_open(&initial,(int)device);assert(r);float loss=0;
    for(int i=0;i<24;i++)assert(!resident_step(r,x,y,BATCH,.015f,&loss));
    assert(!resident_snapshot(r,&snapshot));resident_close(r);
    printf("CONTROLLER_RESIDENT_PROFILE_PASS device=%ld updates=24 snapshots=1 loss=%.9g\n",device,loss);
    return 0;
}
