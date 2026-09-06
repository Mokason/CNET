#include "cnet_core_allocator.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(void){
    /* Fabricated algebra fixtures test the gate only. They are deliberately
     * NOT evidence of independently acquired outcomes or learned benefit. */
    CnetAllocatorEpisode *e=calloc(256,sizeof *e);assert(e);
    CnetCoreCell m={{0}};m.weight[2]=8;m.weight[3]=-4;m.weight[32]=8;m.weight[40]=-4;
    for(unsigned i=0;i<256;i++){
        e[i].id=i+1;e[i].family=i%4;e[i].n=2;e[i].population=64;e[i].jobs=1;e[i].work=1;e[i].wait_limit=4;
        e[i].task[0]=(CnetAllocatorTask){.id=1,.features={1,1,0},.cost=1};
        e[i].task[1]=(CnetAllocatorTask){.id=2,.features={0,0,1},.cost=1};e[i].coverage[1]=UINT64_MAX;
        memset(e[i].receipt[0],'a',64);memset(e[i].receipt[1],'b',64);
    }
    CnetAllocatorGateReport r;
    assert(!cnet_core_allocator_evaluate(&m,e,32,&r)&&r.passed&&r.gain[0]==1);
    assert(cnet_core_allocator_evaluate_against(&m,&m,e,32,&r)==1&&!r.passed&&r.comparators==4&&r.gain[3]==0);
    assert(cnet_core_allocator_evaluate(&m,e,31,&r)==-1);
    for(unsigned i=0;i<32;i++)e[i].coverage[1]=1;
    assert(cnet_core_allocator_evaluate(&m,e,32,&r)==1&&!r.passed);
    for(unsigned i=0;i<32;i++)e[i].coverage[1]=UINT64_MAX;
    for(unsigned i=0;i<32;i+=4){e[i].coverage[1]=0;e[i].coverage[0]=UINT64_MAX;}
    assert(cnet_core_allocator_evaluate(&m,e,32,&r)==1&&!r.passed&&r.family_gain[0][0]<0);
    for(unsigned i=0;i<256;i++){
        e[i].n=9;
        for(unsigned j=2;j<9;j++){e[i].task[j]=(CnetAllocatorTask){.id=j+1,.cost=1};memset(e[i].receipt[j],'c',64);}
    }
    assert(cnet_core_allocator_evaluate(&m,e,256,&r)==-1);
    free(e);puts("CORE_ALLOCATOR_GATE_ALGEBRA_PASS actual_outcome_gain=WITHHELD");
}
