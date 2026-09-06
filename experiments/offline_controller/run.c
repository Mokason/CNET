#define _GNU_SOURCE
#include "net.h"
#include "offline.h"
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#define TRAIN 8192
#define HELD 512
static Task train[TRAIN], validation[HELD], test[HELD];
static uint64_t seen[TRAIN+2*HELD];
static Net model, frozen;
static Scratch scratch;
static float x[BATCH*INPUTS], y[BATCH*ACTIONS];
static const char *names[]={"single","recurrent4","feedforward4"};
static double now(void) { struct timespec t; assert(!clock_gettime(CLOCK_MONOTONIC,&t)); return t.tv_sec+t.tv_nsec*1e-9; }
static void save_split(const char *path,int count,uint32_t seed,int *used) {
    FILE *f=fopen(path,"wx"); assert(f);
    for (int i=0;i<count;i++) {
        Task t; uint64_t key; int duplicate;
        do {
            task_generate(&t,&seed); key=task_topology(&t); duplicate=0;
            for (int j=0;j<*used;j++) if (seen[j]==key) { duplicate=1; break; }
        } while (duplicate);
        seen[(*used)++]=key;
        uint64_t edges=0,types=0;
        for (int j=0;j<64;j++) { edges|=(uint64_t)t.edge[j]<<j; types|=(uint64_t)t.compatible[j]<<j; }
        assert(fprintf(f,"%016" PRIx64 " %016" PRIx64 " %d %d\n",edges,types,t.start,t.goal)>0);
    }
    assert(!fclose(f));
}
static void read_split(const char *path,Task *tasks,int count) {
    FILE *f=fopen(path,"r"); assert(f);
    for (int i=0;i<count;i++) {
        uint64_t edges,types; Task *t=tasks+i;
        assert(fscanf(f,"%" SCNx64 " %" SCNx64 " %d %d",&edges,&types,&t->start,&t->goal)==4);
        assert(t->start>=0 && t->start<8 && t->goal>=0 && t->goal<8 && t->start!=t->goal);
        for (int j=0;j<64;j++) { t->edge[j]=(edges>>j)&1; t->compatible[j]=(types>>j)&1; }
    }
    char extra; assert(fscanf(f," %c",&extra)==EOF); assert(!fclose(f));
}
static void evaluate(Task *tasks,int count,const char *split,unsigned seed,int mode,int injected,cce_amdmath *gpu) {
    int reachable=0, success=0, optimal=0, invalid=0, attempts=0, abstain_unreachable=0, false_arrival=0;
    int buckets[8]={0}, solved[8]={0}, subset_reachable=0, subset_success=0, subset_optimal=0;
    double start=now();
    for (int offset=0;offset<count;offset+=BATCH) {
        int cur[BATCH], done[BATCH]={0}, dist[BATCH];
        uint8_t banned[BATCH][8][9]={0};
        assert(count-offset>=BATCH);
        for (int b=0;b<BATCH;b++) {
            Task *t=tasks+offset+b; cur[b]=t->start;
            dist[b]=task_teacher(t,cur[b],y+b*ACTIONS);
            if (dist[b]>0) { reachable++; buckets[dist[b]]++; if (offset+b<32) subset_reachable++; }
        }
        for (int step=0;step<8;step++) {
            for (int b=0;b<BATCH;b++) task_features(tasks+offset+b,cur[b],x+b*INPUTS);
            assert(net_step(&frozen,&scratch,x,NULL,BATCH,0,gpu)==0);
            for (int b=0;b<BATCH;b++) if (!done[b]) {
                Task *t=tasks+offset+b; float *p=scratch.p+b*OUTPUTS;
                int a=8; float best=-1;
                for (int j=0;j<9;j++) if (!banned[b][cur[b]][j] && p[j]>best) { best=p[j]; a=j; }
                if (!step) {
                    optimal+=y[b*ACTIONS+a]>0;
                    if (offset+b<32) subset_optimal+=y[b*ACTIONS+a]>0;
                    if (injected && dist[b]>0) a=cur[b]; /* impossible self-edge */
                }
                attempts++;
                int checked=task_check(t,cur[b],a);
                printf("{\"kind\":\"action\",\"split\":\"%s\",\"seed\":%u,\"mode\":\"%s\",\"injected\":%d,\"case\":%d,\"step\":%d,\"current\":%d,\"proposal\":%d,\"check\":%d}\n",split,seed,names[mode],injected,offset+b,step,cur[b],a,checked);
                if (a==8) { done[b]=1; abstain_unreachable+=dist[b]<0; }
                else if (!checked) { invalid++; false_arrival+=a==t->goal; banned[b][cur[b]][a]=1; }
                else {
                    cur[b]=a;
                    if (checked==2) { assert(dist[b]>0); done[b]=1; success++; solved[dist[b]]++; if (offset+b<32) subset_success++; }
                }
            }
        }
    }
    assert(!memcmp(&frozen,&model,sizeof model));
    double elapsed=now()-start;
    printf("{\"kind\":\"evaluation\",\"split\":\"%s\",\"seed\":%u,\"mode\":\"%s\",\"injected\":%d,\"cases\":%d,\"reachable\":%d,\"completed\":%d,\"optimal_first\":%d,\"attempts\":%d,\"illegal\":%d,\"unreachable_abstained\":%d,\"false_arrival_proposals\":%d,\"subset32_reachable\":%d,\"subset32_completed\":%d,\"subset32_optimal\":%d,\"batch_amortized_ms\":%.6f,\"seconds\":%.6f,\"forward_flops_per_action\":%lu,\"active_parameters\":%lu}\n",split,seed,names[mode],injected,count,reachable,success,optimal,attempts,invalid,abstain_unreachable,false_arrival,subset_reachable,subset_success,subset_optimal,1000*elapsed/count,elapsed,net_forward_flops(&model),net_parameters(&model));
    for (int d=1;d<8;d++) printf("{\"kind\":\"length_bucket\",\"split\":\"%s\",\"seed\":%u,\"mode\":\"%s\",\"injected\":%d,\"distance\":%d,\"cases\":%d,\"completed\":%d}\n",split,seed,names[mode],injected,d,buckets[d],solved[d]);
}
int main(int argc,char **argv) {
    assert(argc==2);
    if (!strcmp(argv[1],"freeze")) {
        int used=0;
        save_split("train.tsv",TRAIN,101,&used);
        save_split("validation.tsv",HELD,202,&used);
        save_split("test.tsv",HELD,303,&used);
        printf("CONTROLLER_SPLITS_FROZEN unique_topologies=%d\n",used); return 0;
    }
    assert(!strcmp(argv[1],"run"));
    assert(!close_range(3,~0u,0));
    read_split("train.tsv",train,TRAIN); read_split("validation.tsv",validation,HELD); read_split("test.tsv",test,HELD);
    assert(!setenv("CNET_AMDMATH_MIN_FLOPS","0",1));
    char name[256]; cce_amdmath *inference=cce_amdmath_open_device(0,name,sizeof name); assert(inference);
    fprintf(stderr,"CONTROLLER_INFERENCE_DEVICE %s\n",name);
    cce_amdmath *training=cce_amdmath_open_device(1,name,sizeof name); assert(training);
    fprintf(stderr,"CONTROLLER_TRAINING_DEVICE %s\n",name);
    assert(!offline_seal(-1) && !offline_negative_test());
    for (unsigned seed=1;seed<=3;seed++) for (int mode=0;mode<3;mode++) {
        net_init(&model,mode?4:1,mode!=2,seed*997);
        uint32_t samples=seed*1777;
        double start=now(); float initial=0,last=0;
        for (int step=0;step<600;step++) {
            for (int b=0;b<BATCH;b++) {
                Task *t=train+next_random(&samples)%TRAIN;
                task_features(t,t->start,x+b*INPUTS); task_teacher(t,t->start,y+b*ACTIONS);
            }
            last=net_step(&model,&scratch,x,y,BATCH,.15f,training);
            if (!(last>0)) { fprintf(stderr,"CONTROLLER_TRAIN_RED %s\n",cce_amdmath_last_error(training)); return 1; }
            if (!step) initial=last;
        }
        printf("{\"kind\":\"training\",\"seed\":%u,\"mode\":\"%s\",\"examples\":76800,\"initial_batch_loss\":%.6f,\"last_batch_loss\":%.6f,\"seconds\":%.6f,\"train_device\":1,\"eval_device\":0,\"network_denied\":true}\n",seed,names[mode],initial,last,now()-start);
        frozen=model;
        evaluate(validation,HELD,"validation",seed,mode,0,inference);
        evaluate(test,HELD,"test",seed,mode,0,inference);
        evaluate(test,HELD,"test",seed,mode,1,inference);
        char file[80]; snprintf(file,sizeof file,"%s-seed%u.native-weights",names[mode],seed);
        FILE *f=fopen(file,"wx"); assert(f); assert(fwrite(&model,sizeof model,1,f)==1); assert(!fclose(f));
        fprintf(stderr,"CONTROLLER_RUN_PROGRESS seed=%u mode=%s\n",seed,names[mode]); fflush(stdout);
    }
    cce_amdmath_close(training); cce_amdmath_close(inference);
    fprintf(stderr,"CONTROLLER_EXPERIMENT_COMPLETE broader_claims=WITHHELD\n");
}
