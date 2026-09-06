#define _GNU_SOURCE
#include "cnet_capsule_core.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
static double elapsed(void){struct timespec t;assert(!clock_gettime(CLOCK_MONOTONIC,&t));return t.tv_sec+t.tv_nsec*1e-9;}
static int order(const void *a,const void *b){double x=*(const double*)a,y=*(const double*)b;return (x>y)-(x<y);}
int main(int argc,char **argv){
    assert(argc==3);CnetCoreCell cell;FILE *file=fopen(argv[1],"rb");assert(file);assert(fread(&cell,sizeof cell,1,file)==1&&fgetc(file)==EOF);assert(!fclose(file));
    char error[160];double started=elapsed();CnetCapsuleCore *core=cnet_capsule_core_open(argv[2],error,sizeof error);assert(core);double open_seconds=elapsed()-started;
    for(unsigned repeat=0;repeat<3;repeat++)for(unsigned position=0;position<2;position++){
        unsigned neural=(repeat+position)%2;double samples[9000];unsigned verified=0,refused=0;
        for(unsigned i=0;i<9000;i++){
            unsigned input=i%9;char request[96];snprintf(request,sizeof request,"capsule LOCAL_STATE REACH_FLAG %u",input);CnetCapsuleCoreReply reply;
            double begin=elapsed();int rc=neural?cnet_capsule_core_ask_cell(core,request,&cell,1,&reply):cnet_capsule_core_ask(core,request,&reply);samples[i]=elapsed()-begin;
            if(input<8){assert(!rc&&reply.verified&&reply.value==(input!=0));verified++;}else{assert(rc&&!reply.verified);refused++;}
        }
        qsort(samples,9000,sizeof *samples,order);
        printf("{\"kind\":\"cell_serving_benchmark\",\"neural\":%u,\"repeat\":%u,\"requests\":9000,\"verified_correct\":%u,\"ood_refused\":%u,\"p50_us\":%.3f,\"p95_us\":%.3f,\"p99_us\":%.3f,\"registry_open_seconds\":%.6f,\"registry_capsules\":1,\"shared_machine\":true}\n",neural,repeat,verified,refused,samples[4500]*1e6,samples[8550]*1e6,samples[8910]*1e6,open_seconds);
    }
    cnet_capsule_core_close(core);
}
