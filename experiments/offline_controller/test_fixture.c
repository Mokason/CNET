#include "fixture.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void) {
    Task t = { .start=0, .goal=3 };
    memset(t.compatible, 1, sizeof t.compatible);
    t.edge[1]=t.edge[2]=t.edge[11]=t.edge[19]=1;
    float labels[ACTIONS], x[INPUTS];
    assert(task_teacher(&t,0,labels)==2);
    assert(labels[1]==.5f && labels[2]==.5f);
    assert(task_check(&t,0,1)==1 && task_check(&t,1,3)==2);
    assert(!task_check(&t,0,3) && !task_check(&t,0,8));
    assert(!task_check(&t,-1,0) && !task_check(&t,0,-1));
    t.compatible[1]=0;
    assert(!task_check(&t,0,1));
    assert(task_teacher(&t,0,labels)==2 && labels[2]==1);
    t.compatible[2]=0;
    assert(task_teacher(&t,0,labels)==-1 && labels[8]==1);
    task_features(&t,0,x);
    assert(x[1]==1 && x[65]==0 && x[128]==1 && x[139]==1);
    puts("CONTROLLER_FIXTURE_PASS ties=1 incompatible=1 unreachable=1 invalid=1");
}
