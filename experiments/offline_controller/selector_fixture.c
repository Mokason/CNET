#include "selector_fixture.h"
#include <assert.h>
#include <string.h>
uint32_t selector_random(uint32_t *s){*s^=*s<<13;*s^=*s>>17;*s^=*s<<5;return *s;}
const char *selector_family(unsigned f){static const char *names[]={"dag","cyclic","chain","tree","typed_distractor"};return f<5?names[f]:"invalid";}
void selector_permute(CnetSelectorGraph *out,const CnetSelectorGraph *in,uint32_t seed){
    CnetSelectorGraph r=*in;unsigned map[64];for(unsigned i=0;i<in->n;i++)map[i]=i;
    for(unsigned i=in->n;i>1;i--){unsigned j=selector_random(&seed)%i,v=map[i-1];map[i-1]=map[j];map[j]=v;}
    memset(r.edge,0,sizeof r.edge);
    for(unsigned u=0;u<in->n;u++){
        r.node[map[u]]=in->node[u];
        for(unsigned v=0;v<in->n;v++)if((in->edge[u]>>v)&1)r.edge[map[u]]|=UINT64_C(1)<<map[v];
    }
    r.start=map[in->start];r.goal=map[in->goal];*out=r;
}
void selector_generate(CnetSelectorGraph *g,unsigned family,unsigned n,uint32_t seed,int reachable){
    assert(family<5&&n>=2&&n<=64&&seed);memset(g,0,sizeof *g);
    g->n=n;g->start=0;g->goal=n-1;g->feature_version=1;g->generation=1;
    for(unsigned u=0;u<n;u++)g->node[u]=(CnetSelectorNode){1000+u,1+selector_random(&seed)%4,1,1,1};
    if(family==3){
        g->start=n-1;g->goal=0;
        for(unsigned u=1;u<n;u++)g->edge[u]=UINT64_C(1)<<(selector_random(&seed)%u);
    }else{
        for(unsigned u=0;u+1<n;u++)g->edge[u]=UINT64_C(1)<<(u+1);
        if(family==1)g->edge[n-1]=1;
        if(family<2)for(unsigned u=0;u<n;u++)for(unsigned v=0;v<n;v++)
            if((family==1||u<v)&&selector_random(&seed)%n==0)g->edge[u]|=UINT64_C(1)<<v;
        if(family==4){
            for(unsigned u=0;u<n;u++){g->node[u].input_type=1+u%7;g->node[u].output_type=1+(u+1)%7;}
            for(unsigned u=0;u<n;u++)for(unsigned v=0;v<n;v++)
                if(g->node[u].output_type!=g->node[v].input_type&&selector_random(&seed)%3==0)g->edge[u]|=UINT64_C(1)<<v;
        }
    }
    if(!reachable)for(unsigned u=0;u<n;u++)if(g->node[u].output_type==g->node[g->goal].input_type)g->edge[u]&=~(UINT64_C(1)<<g->goal);
    CnetSelectorGraph shuffled;selector_permute(&shuffled,g,seed);*g=shuffled;
}
/* Independent exact teacher: reverse breadth-first search. It never imports
 * neural scores, proposals, first-activation distances or execution answers. */
void selector_teacher(const CnetSelectorGraph *g,int distance[64]){
    unsigned queue[64],head=0,tail=0;for(unsigned u=0;u<g->n;u++)distance[u]=-1;
    distance[g->goal]=0;queue[tail++]=g->goal;
    while(head<tail){
        unsigned v=queue[head++];
        for(unsigned u=0;u<g->n;u++)if(distance[u]<0&&g->node[u].available&&g->node[v].available&&
            g->node[u].output_type==g->node[v].input_type&&((g->edge[u]>>v)&1)){
            distance[u]=distance[v]+1;assert(tail<64);queue[tail++]=u;
        }
    }
}
/* Separate post-review stress suite. No training uses these graphs. Every graph
 * has productive and dead-end regions; positives offer a legal dead-end branch.
 * Negatives alternate a dead-end start and an interior cut with reachable suffix. */
void selector_stress_generate(CnetSelectorGraph *g,unsigned family,unsigned n,uint32_t seed,int reachable){
    assert(family<5&&n>=8&&n<=64&&seed);memset(g,0,sizeof *g);
    unsigned half=n/2;g->n=n;g->goal=half-1;g->feature_version=1;g->generation=1;
    for(unsigned u=0;u<n;u++)g->node[u]=(CnetSelectorNode){1000+u,1+selector_random(&seed)%4,1,1,1};
    for(unsigned u=0;u+1<half;u++){
        unsigned v=family==3?u+1+selector_random(&seed)%(half-u-1):u+1;
        g->edge[u]=UINT64_C(1)<<v;
        if(family<2)for(unsigned w=u+1;w<half;w++)if(selector_random(&seed)%4==0)g->edge[u]|=UINT64_C(1)<<w;
        if(family==1&&u)g->edge[u]|=UINT64_C(1)<<(u-1);
    }
    for(unsigned u=half;u+1<n;u++)g->edge[u]=UINT64_C(1)<<(u+1);
    if(family!=0)g->edge[n-1]=UINT64_C(1)<<half;
    g->edge[0]|=UINT64_C(1)<<half;
    if(family==4){g->node[n-1].output_type=2;g->edge[n-1]|=UINT64_C(1)<<g->goal;g->node[n-2].available=0;}
    if(!reachable){
        if(seed&1)g->start=half;
        else {unsigned cut=half/2;for(unsigned u=0;u<cut;u++)for(unsigned v=cut;v<half;v++)g->edge[u]&=~(UINT64_C(1)<<v);}
    }
    int distance[64];selector_teacher(g,distance);unsigned productive=0;
    for(unsigned u=0;u<n;u++)productive+=distance[u]>=0;
    assert(productive>1&&productive<n&&(distance[g->start]>=0)==reachable);
    CnetSelectorGraph shuffled;selector_permute(&shuffled,g,seed);*g=shuffled;
}
