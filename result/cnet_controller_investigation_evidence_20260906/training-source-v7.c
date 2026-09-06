#define _GNU_SOURCE
#include "net.h"
#include "offline.h"
#include <assert.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
static Task training_tasks[8192], validation_tasks[512];
static Net candidate;
static Scratch state;
static float input[BATCH*INPUTS], target[BATCH*ACTIONS];
static const char *variants[]={"raw","typed","aligned","raw_states","aligned_states","raw_resampled","aligned_stream","aligned_structural_stream"};
static uint64_t effective_graph(const Task *t){uint64_t key=0;for(int j=0;j<64;j++)key|=(uint64_t)(t->edge[j]&&t->compatible[j])<<j;return key;}
/* Old raw-ID split retained only to reproduce rejected development variant 6.
 * It is NOT sufficient to protect aligned inputs under node relabeling. */
static unsigned partition(uint64_t key){key^=key>>30;key*=UINT64_C(0xbf58476d1ce4e5b9);key^=key>>27;key*=UINT64_C(0x94d049bb133111eb);key^=key>>31;return key%5;}
/* Permutation-invariant, deliberately coarse structural split. This is not a
 * graph canonicalizer. Isomorphic graphs always share a signature/partition.
 * Only data assignment uses it: it never enters features or labels. */
static uint64_t degree_signature(uint64_t graph) {
    unsigned pairs[8];
    for(int u=0;u<8;u++) {
        unsigned in=0,out=0;
        for(int v=0;v<8;v++){in+=(graph>>(v*8+u))&1;out+=(graph>>(u*8+v))&1;}
        pairs[u]=out*9+in;
    }
    for(int u=1;u<8;u++){unsigned value=pairs[u];int v=u;while(v&&pairs[v-1]>value){pairs[v]=pairs[v-1];v--;}pairs[v]=value;}
    uint64_t hash=UINT64_C(14695981039346656037);
    for(int u=0;u<8;u++){hash^=pairs[u];hash*=UINT64_C(1099511628211);}
    return hash;
}
static int key_compare(const void *a,const void *b){uint64_t x=*(const uint64_t*)a,y=*(const uint64_t*)b;return (x>y)-(x<y);}
static double seconds(void) { struct timespec t; assert(!clock_gettime(CLOCK_MONOTONIC,&t)); return t.tv_sec+t.tv_nsec*1e-9; }
static void read_tasks(const char *path,Task *out,int count) {
    FILE *f=fopen(path,"r"); assert(f);
    for (int n=0;n<count;n++) {
        uint64_t edge,compatible; Task *t=out+n;
        assert(fscanf(f,"%" SCNx64 " %" SCNx64 " %d %d",&edge,&compatible,&t->start,&t->goal)==4);
        assert(t->start>=0 && t->start<8 && t->goal>=0 && t->goal<8 && t->start!=t->goal);
        for (int i=0;i<64;i++) {t->edge[i]=(edge>>i)&1;t->compatible[i]=(compatible>>i)&1;}
    }
    char extra; assert(fscanf(f," %c",&extra)==EOF); assert(!fclose(f));
}
/* Permutation and immediate type compatibility only. No path/teacher output
 * enters x. order maps neural action coordinates back to original node IDs. */
static void encode(const Task *t,int cur,int format,float *x,float *y,int order[8]) {
    assert(cur!=t->goal);
    Task mapped=*t;
    for (int i=0;i<8;i++) order[i]=i;
    if (format==2) {
        int n=1;order[0]=cur;order[7]=t->goal;
        for (int i=0;i<8;i++) if (i!=cur && i!=t->goal) order[n++]=i;
        for (int u=0;u<8;u++) for (int v=0;v<8;v++) {
            mapped.edge[u*8+v]=t->edge[order[u]*8+order[v]];
            mapped.compatible[u*8+v]=t->compatible[order[u]*8+order[v]];
        }
        mapped.goal=7;cur=0;
    }
    task_features(&mapped,cur,x);
    if (format) for (int i=0;i<64;i++) {x[i]*=x[64+i];x[64+i]=0;}
    if (y) task_teacher(&mapped,cur,y);
}
static int select_action(const Task *t,int cur,const float *p,const int order[8],const uint8_t banned[9],int mask) {
    int coordinate=8;float best=-1;
    for(int j=0;j<9;j++) {
        int original=j==8?8:order[j];
        if(banned[original] || (mask && original!=8 && !task_check(t,cur,original)))continue;
        if(p[j]>best){best=p[j];coordinate=j;}
    }
    return coordinate;
}
static void probe(Task *tasks,int count,const char *split,int format,unsigned seed,int variant,int step,int mask,FILE *trace,cce_amdmath *gpu) {
    assert(count>0 && count%BATCH==0);
    int reachable=0,complete=0,abstain=0,immediate=0,illegal=0,optimal=0,attempts=0;
    int buckets[8]={0},solved[8]={0},timeouts=0,cycles=0,transitions=0,raw_illegal=0;
    double loss=0;
    for (int offset=0;offset<count;offset+=BATCH) {
        int cur[BATCH],done[BATCH]={0},distance[BATCH],order[BATCH][8];
        uint8_t banned[BATCH][8][9]={0};
        unsigned visited[BATCH];
        for (int b=0;b<BATCH;b++) {
            Task *t=tasks+offset+b;float labels[9];cur[b]=t->start;
            distance[b]=task_teacher(t,cur[b],labels);reachable+=distance[b]>0;
            if(distance[b]>0)buckets[distance[b]]++;
            visited[b]=1u<<cur[b];
        }
        for (int hop=0;hop<8;hop++) {
            for (int b=0;b<BATCH;b++) {
                Task *t=tasks+offset+b;
                encode(t,done[b]?t->start:cur[b],format,input+b*INPUTS,hop?NULL:target+b*ACTIONS,order[b]);
            }
            float batch_loss=net_step(&candidate,&state,input,hop?NULL:target,BATCH,0,gpu);
            assert(batch_loss>=0); if(!hop)loss+=batch_loss*BATCH;
            for (int b=0;b<BATCH;b++) if(!done[b]) {
                Task *t=tasks+offset+b;float *p=state.p+b*OUTPUTS;
                int raw=select_action(t,cur[b],p,order[b],banned[b][cur[b]],0);
                int raw_action=raw==8?8:order[b][raw];
                raw_illegal+=raw_action!=8 && !task_check(t,cur[b],raw_action);
                int coordinate=select_action(t,cur[b],p,order[b],banned[b][cur[b]],mask);
                int a=coordinate==8?8:order[b][coordinate];
                if(!hop){optimal+=target[b*ACTIONS+coordinate]>0;immediate+=distance[b]>0&&a==8;}
                attempts++;
                int checked=task_check(t,cur[b],a);
                if(trace)fprintf(trace,"{\"kind\":\"action\",\"split\":\"%s\",\"seed\":%u,\"mode\":\"%s\",\"mask\":%d,\"case\":%d,\"step\":%d,\"current\":%d,\"proposal\":%d,\"check\":%d}\n",split,seed,variants[variant],mask,offset+b,hop,cur[b],a,checked);
                if(a==8){done[b]=1;abstain+=distance[b]<0;continue;}
                if(!checked){illegal++;banned[b][cur[b]][a]=1;continue;}
                transitions++;cycles+=(visited[b]>>a)&1;visited[b]|=1u<<a;
                cur[b]=a;
                if(checked==2){assert(distance[b]>0);done[b]=1;complete++;solved[distance[b]]++;}
            }
        }
        for(int b=0;b<BATCH;b++)timeouts+=!done[b];
    }
    printf("{\"kind\":\"curve\",\"variant\":\"%s\",\"seed\":%u,\"step\":%d,\"split\":\"%s\",\"cases\":%d,\"mask\":%d,\"loss\":%.7f,\"optimal_first\":%d,\"reachable\":%d,\"completed\":%d,\"unreachable\":%d,\"unreachable_abstained\":%d,\"reachable_immediate_abstention\":%d,\"illegal\":%d,\"attempts\":%d,\"raw_illegal\":%d,\"transitions\":%d,\"cycles\":%d,\"timeouts\":%d,\"distance_cases\":[",variants[variant],seed,step,split,count,mask,loss/count,optimal,reachable,complete,count-reachable,abstain,immediate,illegal,attempts,raw_illegal,transitions,cycles,timeouts);
    for(int d=1;d<8;d++)printf("%s%d",d==1?"":",",buckets[d]);
    printf("],\"distance_completed\":[");
    for(int d=1;d<8;d++)printf("%s%d",d==1?"":",",solved[d]);
    puts("]}");
    fflush(stdout);
}
int main(int argc,char **argv) {
    assert(argc==4);char *end;
    long variant=strtol(argv[1],&end,10);assert(!*end&&variant>=0&&variant<8);
    unsigned seed=(unsigned)strtoul(argv[2],&end,10);assert(!*end&&seed>0&&seed<=3);
    long steps=strtol(argv[3],&end,10);assert(!*end&&steps>0&&steps<=30000);
    assert(!close_range(3,~0u,0));
    read_tasks("train.tsv",training_tasks,8192);read_tasks("validation.tsv",validation_tasks,512);
    cce_amdmath *gpu=NULL;
#ifndef CONTROLLER_CPU_ONLY
    assert(!setenv("CNET_AMDMATH_MIN_FLOPS","0",1));
    char name[256];gpu=cce_amdmath_open_device(1,name,sizeof name);assert(gpu);
#endif
    assert(!offline_seal(-1)&&!offline_negative_test());
    net_init(&candidate,4,1,seed*997);
    uint32_t sampler=seed*1777,states=seed*4111,stream=seed*87101;
    int format=(variant==3||variant==5)?0:variant>=4?2:(int)variant;
    uint64_t held_keys[512];for(int i=0;i<512;i++){uint64_t key=effective_graph(validation_tasks+i);held_keys[i]=variant==7?degree_signature(key):key;}
    qsort(held_keys,512,sizeof *held_keys,key_compare);
    int pools[2][8192],pool_count[2]={0};
    for(int i=0;i<8192;i++){float y[9];int r=task_teacher(training_tasks+i,training_tasks[i].start,y)>0;pools[r][pool_count[r]++]=i;}
    printf("{\"kind\":\"class_distribution\",\"reachable\":%d,\"unreachable\":%d,\"resampled_abstain_fraction\":%.8f}\n",pool_count[1],pool_count[0],variant==5?1.0/9:(double)pool_count[0]/8192);
    uint8_t exposed[8192][8]={0};int distinct=0;unsigned long draws_ood=0;
    probe(training_tasks,512,"training_probe",format,seed,(int)variant,0,0,NULL,gpu);
    probe(validation_tasks,512,"validation",format,seed,(int)variant,0,0,NULL,gpu);
    double elapsed=0;
    for(int step=1;step<=steps;step++) {
        for(int b=0;b<BATCH;b++) {
            int idx=next_random(&sampler)%8192;
            if(variant==5){int r=next_random(&states)%9!=0;idx=pools[r][next_random(&sampler)%pool_count[r]];}
            Task generated,*t=training_tasks+idx;
            if(variant>=6) {
                uint64_t key;
                do{task_generate(&generated,&stream);key=effective_graph(&generated);}
                while(variant==7?(key=degree_signature(key),key%5==0||bsearch(&key,held_keys,512,sizeof key,key_compare)):
                      !partition(key)||bsearch(&key,held_keys,512,sizeof key,key_compare));
                t=&generated;
            }
            int cur=t->start,order[8];
            if(variant==3||variant==4){cur=(int)(next_random(&states)%7);if(cur>=t->goal)cur++;}
            encode(t,cur,format,input+b*INPUTS,target+b*ACTIONS,order);
            draws_ood+=target[b*ACTIONS+8]>0;
            if(variant<6&&!exposed[idx][cur]){exposed[idx][cur]=1;distinct++;}
        }
        double before=seconds();
        assert(net_step(&candidate,&state,input,target,BATCH,.15f,gpu)>0);
        elapsed+=seconds()-before;
        if(step==600||step==2000||step==6000||step==12000||step==steps) {
            probe(training_tasks,512,"training_probe",format,seed,(int)variant,step,0,NULL,gpu);
            probe(validation_tasks,512,"validation",format,seed,(int)variant,step,0,NULL,gpu);
        }
    }
    printf("{\"kind\":\"exposure\",\"distinct_graph_states\":%d,\"streamed\":%s,\"draws\":%ld,\"unreachable_draws\":%lu}\n",variant>=6?-1:distinct,variant>=6?"true":"false",steps*BATCH,draws_ood);
    printf("{\"kind\":\"diagnostic_training\",\"variant\":\"%s\",\"seed\":%u,\"steps\":%ld,\"update_seconds\":%.6f,\"device\":%d}\n",variants[variant],seed,steps,elapsed,gpu?1:-1);
    char file[100];snprintf(file,sizeof file,"diagnostic-%s-%u-%ld.weights",variants[variant],seed,steps);
    FILE *f=fopen(file,"wx");assert(f);assert(fwrite(&candidate,sizeof candidate,1,f)==1);assert(!fclose(f));
#ifndef CONTROLLER_CPU_ONLY
    cce_amdmath_close(gpu);
#endif
    return 0;
}
