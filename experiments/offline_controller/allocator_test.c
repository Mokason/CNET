#define _GNU_SOURCE
#include "cnet_core_allocator.h"
#include "cnet_core_candidate.h"
#include <assert.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
int main(void) {
    CnetCoreCell zero={{0}};
    CnetAllocatorTask t[3]={{.id=10,.features={.8f,.2f,.25f},.cost=2},
        {.id=20,.features={.2f,.9f,.25f},.cost=2},
        {.id=30,.features={.1f,.1f,.25f},.cost=2,.age=4}};
    CnetAllocatorChoice p;
    assert(!cnet_core_allocator_choose(&zero,t,3,1,2,4,4,CNET_ALLOCATOR_CURSOR,&p));
    assert(p.count==2&&p.index[0]==2&&p.index[1]==1&&p.work==4);
    assert(!cnet_core_allocator_choose(&zero,t,3,1,1,4,4,CNET_ALLOCATOR_DEMAND_COST,&p));
    assert(p.count==1&&p.index[0]==2);
    t[2].age=0;
    assert(!cnet_core_allocator_choose(&zero,t,3,1,1,4,4,CNET_ALLOCATOR_DEMAND_COST,&p)&&p.index[0]==0);
    assert(!cnet_core_allocator_choose(&zero,t,3,1,1,4,4,CNET_ALLOCATOR_COMPLETION,&p)&&p.index[0]==1);
    t[0].features[0]=NAN;
    assert(cnet_core_allocator_choose(&zero,t,3,1,1,4,4,0,&p)&&p.count==0);
    t[0].features[0]=.8f;t[1].id=10;
    assert(cnet_core_allocator_choose(&zero,t,3,1,1,4,4,0,&p));
    char root[]="/tmp/cnet-allocator-candidate-XXXXXX";assert(mkdtemp(root));
    int fd=open(root,O_RDONLY|O_DIRECTORY);assert(fd>=0);
    CnetCoreCandidate a={0},b;memset(a.training_sha256,'a',64);memset(a.evaluation_sha256,'b',64);
    assert(!cnet_core_candidate_save_objective_at(fd,"allocator",&a,CNET_CORE_OBJECTIVE_ALLOCATOR));
    assert(cnet_core_candidate_load_at(fd,"allocator",&b));
    assert(!cnet_core_candidate_load_objective_at(fd,"allocator",&b,CNET_CORE_OBJECTIVE_ALLOCATOR));
    assert(!cnet_core_candidate_save_at(fd,"graph",&a));
    assert(cnet_core_candidate_load_objective_at(fd,"graph",&b,CNET_CORE_OBJECTIVE_ALLOCATOR));
    assert(unlinkat(fd,"allocator",0)==0&&unlinkat(fd,"graph",0)==0);close(fd);assert(!rmdir(root));
    puts("CORE_ALLOCATOR_CPU_PASS fairness=1 budgets=1 comparators=1 objective_confusion_refused=1");
}
