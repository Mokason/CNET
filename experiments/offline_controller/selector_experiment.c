#define _GNU_SOURCE
#include "selector_fixture.h"
#include "cell_train.h"
#include "offline.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
static SelectorFixture fixtures[SELECTOR_EVAL_CASES];
static float samples[2048*3],targets[2048];
static double seconds(void){struct timespec t;assert(!clock_gettime(CLOCK_MONOTONIC,&t));return t.tv_sec+t.tv_nsec*1e-9;}
static void freeze(int stress){
    const unsigned sizes[]={8,16,32,64};unsigned used=0;
    for(unsigned f=0;f<5;f++)for(unsigned n=0;n<4;n++)for(unsigned i=0;i<64;i++){
        SelectorFixture *s=fixtures+used;s->family=f;s->index=i;
        if(stress)selector_stress_generate(&s->graph,f,sizes[n],UINT32_C(79062026)+used*7919,i%2==0);
        else selector_generate(&s->graph,f,sizes[n],UINT32_C(69062026)+used*7919,i%2==0);
        int distance[64];selector_teacher(&s->graph,distance);assert((distance[s->graph.start]>=0)==(i%2==0));used++;
    }
    FILE *file=fopen(stress?"selector-stress.native":"selector-fixtures.native","wx");assert(file);
    assert(fwrite(fixtures,sizeof fixtures,1,file)==1);assert(!fclose(file));
    printf("SELECTOR_FIXTURES_FROZEN stress=%d cases=%u families=5 sizes=8,16,32,64 threshold=0.9 completion_floor=0.95 abstention_floor=0.95\n",stress,used);
}
static void collect(const CnetCoreCell *m,int on_policy,uint32_t seed){
    unsigned used=0;
    for(unsigned c=0;c<32;c++){
        CnetSelectorGraph g;selector_generate(&g,0,8,selector_random(&seed),c%2==0);
        int distance[64];selector_teacher(&g,distance);float previous[64]={0},next[64];previous[g.goal]=1;
        for(unsigned t=1;t<=8;t++){
            for(unsigned u=0;u<8;u++){
                float *x=samples+used*3;x[0]=u==g.goal;x[1]=previous[u];x[2]=0;
                for(unsigned v=0;v<8;v++)if(((g.edge[u]>>v)&1)&&g.node[u].output_type==g.node[v].input_type&&previous[v]>x[2])x[2]=previous[v];
                targets[used]=(distance[u]>=0&&(unsigned)distance[u]<=t);
                if(on_policy)assert(!cnet_core_cell_predict(m,x,next+u));else next[u]=targets[used];
                used++;
            }
            memcpy(previous,next,8*sizeof(float));
        }
    }
    assert(used==2048);
}
static void train(unsigned seed,int on_policy){
    assert(!close_range(3,~0u,0));
    CnetCoreCell m;uint32_t rng=997*seed;for(int i=0;i<41;i++)m.weight[i]=(float)(int)(selector_random(&rng)%20001)/10000-1;
    CellGpu *gpu=cell_gpu_open(&m,(int)(seed%2));assert(gpu);assert(!offline_seal(-1)&&!offline_negative_test());
    for(int b=0;b<8;b++){for(int i=0;i<3;i++)samples[b*3+i]=(float)((b>>(2-i))&1);targets[b]=b!=0;}
    float loss;double start=seconds();assert(!cell_gpu_fit(gpu,samples,targets,8,2000,1,&loss));
    for(unsigned round=0;round<4;round++){
        assert(!cell_gpu_snapshot(gpu,&m));collect(&m,on_policy,seed*87101+round*7111);
        assert(!cell_gpu_fit(gpu,samples,targets,2048,512,1,&loss));
    }
    assert(!cell_gpu_snapshot(gpu,&m));cell_gpu_close(gpu);
    char name[80];snprintf(name,sizeof name,"cell-%s-%u.native-weights",on_policy?"onpolicy":"ideal",seed);
    FILE *f=fopen(name,"wx");assert(f);assert(fwrite(&m,sizeof m,1,f)==1);assert(!fclose(f));
    printf("{\"kind\":\"selector_training\",\"seed\":%u,\"variant\":\"%s\",\"parameters\":41,\"training_nodes\":8,\"warmup_teacher\":\"explicit_Boolean_OR_specification\",\"correction_teacher\":\"independent_BFS\",\"warmup_rows\":8,\"warmup_epochs\":2000,\"correction_rounds\":4,\"rows_per_round\":2048,\"epochs_per_round\":512,\"last_loss\":%.9g,\"seconds\":%.6f,\"offline_sealed\":true}\n",seed,on_policy?"onpolicy":"ideal",loss,seconds()-start);
}
static int evaluate(const char *weights,const char *dataset,unsigned iterations){
    assert(!close_range(3,~0u,0));
    CnetCoreCell m;FILE *f=fopen(weights,"rb");assert(f);assert(fread(&m,sizeof m,1,f)==1&&fgetc(f)==EOF);assert(!fclose(f));
    f=fopen(dataset,"rb");assert(f);assert(fread(fixtures,sizeof fixtures,1,f)==1&&fgetc(f)==EOF);assert(!fclose(f));
    assert(!cnet_core_cell_validate(&m));assert(!offline_seal(-1)&&!offline_negative_test());
    int all_pass=1;
    for(unsigned group=0;group<20;group++){
        unsigned reachable=0,complete=0,abstain=0,safe_refusals=0,hops=0,relabel=0;double brier=0,elapsed=0,teacher_time=0;
        for(unsigned i=0;i<64;i++){
            SelectorFixture *fixture=fixtures+group*64+i;CnetSelectorGraph *g=&fixture->graph;CnetSelectorProposal p,q;
            const unsigned sizes[]={8,16,32,64};
            assert(fixture->family==group/4&&fixture->index==i&&g->n==sizes[group%4]);assert(!cnet_core_selector_validate(g));
            int distances[64];double begin=seconds();selector_teacher(g,distances);teacher_time+=seconds()-begin;
            int positive=distances[g->start]>=0;reachable+=positive;
            begin=seconds();assert(!cnet_core_selector_propose(&m,g,iterations,&p));elapsed+=seconds()-begin;
            brier+=(p.score-positive)*(p.score-positive);abstain+=!positive&&p.score<.9f;safe_refusals+=!positive&&!p.count;
            unsigned current=g->start;
            for(unsigned h=0;h<p.hops;h++){
                unsigned v=p.path[h];assert(v<g->n&&((g->edge[current]>>v)&1)&&g->node[current].output_type==g->node[v].input_type);
                assert(distances[current]>0&&distances[v]>=0);current=v;hops++;
            }
            if(p.count){assert(current==g->goal&&positive);complete++;}
            CnetSelectorGraph changed;selector_permute(&changed,g,1781+i);
            assert(!cnet_core_selector_propose(&m,&changed,iterations,&q));
            assert(p.score==q.score&&p.count==q.count&&p.hops==q.hops);
            for(unsigned j=0;j<p.count;j++)assert(g->node[p.ranked[j]].identity==changed.node[q.ranked[j]].identity);
            for(unsigned j=0;j<p.hops;j++)assert(g->node[p.path[j]].identity==changed.node[q.path[j]].identity);
            relabel++;
        }
        int pass=complete>=.95*reachable&&abstain>=.95*(64-reachable);all_pass&=pass;
        printf("{\"kind\":\"selector_evaluation\",\"family\":\"%s\",\"nodes\":%u,\"iterations\":%u,\"cases\":64,\"reachable\":%u,\"completed\":%u,\"unreachable_abstained\":%u,\"verifier_safe_refusals\":%u,\"checked_hops\":%u,\"relabel_pass\":%u,\"brier\":%.8f,\"proposal_seconds\":%.8f,\"deterministic_teacher_seconds\":%.8f,\"task_gate_pass\":%s}\n",selector_family(fixtures[group*64].family),fixtures[group*64].graph.n,iterations,reachable,complete,abstain,safe_refusals,hops,relabel,brier/64,elapsed,teacher_time,pass?"true":"false");
    }
    printf("SELECTOR_EVALUATION_%s iterations=%u groups=20\n",all_pass?"PASS":"WITHHELD",iterations);
    return all_pass?0:2;
}
int main(int argc,char **argv){
    if(argc==2&&!strcmp(argv[1],"freeze")){freeze(0);return 0;}
    if(argc==2&&!strcmp(argv[1],"freeze-stress")){freeze(1);return 0;}
    if(argc==4&&!strcmp(argv[1],"train")){char *end;unsigned seed=(unsigned)strtoul(argv[2],&end,10);assert(!*end&&seed>=1&&seed<=3);assert(!strcmp(argv[3],"ideal")||!strcmp(argv[3],"onpolicy"));train(seed,!strcmp(argv[3],"onpolicy"));return 0;}
    assert(argc==5&&!strcmp(argv[1],"evaluate"));char *end;unsigned iterations=(unsigned)strtoul(argv[4],&end,10);assert(!*end&&(iterations==1||iterations==64));return evaluate(argv[2],argv[3],iterations);
}
