#define _GNU_SOURCE
#include "worker.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
static int finish(WorkerPool *p,int slot,WorkerResult *r){int rc;do{rc=worker_poll(p,slot,r);if(!rc){struct timespec t={0,1000000};nanosleep(&t,NULL);}}while(!rc);return rc;}
int main(int argc,char **argv){
    assert(argc==3||(argc==4&&!strcmp(argv[3],"production")));int faults=argc==3;
    WorkerPool *p=worker_pool_open(argv[1]);assert(p);CnetCoreCell zero={{0}};WorkerResult a,b;
    int one=worker_start(p,WORKER_TRAIN,0,&zero,30000),two=worker_start(p,WORKER_TRAIN,1,&zero,30000);assert(one>=0&&two>=0&&one!=two);
    assert(worker_start(p,WORKER_TRAIN,0,&zero,30000)<0);assert(finish(p,one,&a)==1&&finish(p,two,&b)==1);
    assert(a.sandboxed&&b.sandboxed&&a.device==0&&b.device==1);
    one=worker_start(p,WORKER_EVALUATE,1,&a.cell,30000);assert(one>=0);CnetCoreCell frozen=a.cell;memset(&a.cell,0,sizeof a.cell);
    assert(finish(p,one,&b)==1&&b.max_error<1e-6f&&!memcmp(&b.cell,&frozen,sizeof frozen));
    FILE *file=fopen(argv[2],"wx");assert(file);assert(fwrite(&frozen,sizeof frozen,1,file)==1);assert(!fclose(file));
    one=worker_start(p,WORKER_TRAIN,0,&zero,30000);assert(one>=0&&!worker_cancel(p,one));assert(worker_poll(p,one,&a)<0);
    if(faults){
        one=worker_start_fault(p,0,1,30000);assert(one>=0&&finish(p,one,&a)<0);
        one=worker_start_fault(p,0,2,30000);assert(one>=0&&finish(p,one,&a)<0);
        one=worker_start_fault(p,0,3,500);assert(one>=0&&finish(p,one,&a)<0);
    }
    one=worker_start(p,WORKER_EVALUATE,0,&frozen,30000);assert(one>=0&&finish(p,one,&a)==1);
    worker_pool_close(p);
    printf("DUAL_GPU_WORKER_PASS devices=0,1 max_jobs=2 frozen_transfer=1 cancellation_restart=1 fault_injections=%d eval_max_abs=%.9g output=%s\n",faults,a.max_error,argv[2]);
}
