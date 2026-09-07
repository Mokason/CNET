#define _POSIX_C_SOURCE 200809L
#include "worker.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static int finish(WorkerPool *p,int slot,WorkerResult *r){
    int rc;do{rc=worker_poll(p,slot,r);if(!rc){struct timespec t={0,1000000};nanosleep(&t,NULL);}}while(!rc);return rc;
}
int main(int argc,char **argv){
    assert(argc==2);WorkerPool *p=worker_pool_open(argv[1]);assert(p);
    WorkerBatch *b=calloc(1,sizeof *b);assert(b);
    b->magic=WORKER_BATCH_MAGIC;b->feature_version=1;b->rows=16;b->epochs=2000;b->lr=1;
    for(unsigned i=0;i<41;i++)b->cell.weight[i]=(float)((int)(i*37%101)-50)/100;
    /* Numerical fixture only, NOT independent acquisition/gain evidence. */
    for(unsigned i=0;i<16;i++){b->x[i*3]=(float)i/15;b->x[i*3+1]=.25f;b->x[i*3+2]=.5f;b->y[i]=.1f+.8f*b->x[i*3];}
    b->x[0]=NAN;assert(worker_start_batch(p,WORKER_BATCH_TRAIN,0,b,30000)<0);b->x[0]=0;
    b->rows=2049;assert(worker_batch_validate(b));b->rows=16;
    b->epochs=4097;assert(worker_batch_validate(b));b->epochs=2000;
    b->y[0]=1.1f;assert(worker_batch_validate(b));b->y[0]=.1f;
    b->lr=INFINITY;assert(worker_batch_validate(b));b->lr=1;
    assert(worker_start_batch(p,WORKER_BATCH_TRAIN,2,b,30000)<0);
    int slots[2];WorkerResult r[2];
    for(unsigned d=0;d<2;d++){slots[d]=worker_start_batch(p,WORKER_BATCH_TRAIN,(int)d,b,30000);assert(slots[d]>=0);}
    for(unsigned d=0;d<2;d++){
        assert(finish(p,slots[d],r+d)==1&&r[d].rows==16&&memcmp(&r[d].cell,&b->cell,sizeof b->cell));
        float low,high;assert(!cnet_core_cell_predict(&r[d].cell,b->x,&low));assert(!cnet_core_cell_predict(&r[d].cell,b->x+45,&high));
        assert(high-low>.5f);printf("CORE_ALLOCATOR_WORKER_NUMERIC_PASS device=%u rows=16 parity=%.9g gain_claim=WITHHELD\n",d,r[d].max_error);
    }
    b->cell=r[0].cell;int slot=worker_start_batch(p,WORKER_BATCH_EVALUATE,1,b,30000);assert(slot>=0);
    CnetCoreCell frozen=b->cell;memset(&b->cell,0,sizeof b->cell);
    assert(finish(p,slot,r)==1&&!memcmp(&r[0].cell,&frozen,sizeof frozen));
    worker_pool_close(p);free(b);
}
