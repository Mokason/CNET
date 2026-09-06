#include "cnet_core_allocator.h"
#include <math.h>
#include <string.h>
static unsigned cursor_rank(unsigned i,unsigned cursor,unsigned n){return (i+n-cursor)%n;}
int cnet_core_allocator_choose(const CnetCoreCell *cell,const CnetAllocatorTask *t,
    size_t n,unsigned cursor,unsigned jobs,unsigned work,unsigned wait_limit,
    unsigned policy,CnetAllocatorChoice *out){
    if(!out)return -1;
    memset(out,0,sizeof *out);
    if(!t||!n||n>32||cursor>=n||!jobs||jobs>8||!work||work>2048||!wait_limit||wait_limit>65535||policy>3||
       (policy==0&&cnet_core_cell_validate(cell)))return -1;
    float scores[32]={0};
    for(unsigned i=0;i<n;i++){
        if(!t[i].id||!t[i].cost||t[i].cost>work||t[i].age>65535)return -1;
        for(unsigned j=0;j<i;j++)if(t[i].id==t[j].id)return -1;
        for(unsigned j=0;j<3;j++)if(!isfinite(t[i].features[j])||t[i].features[j]<0||t[i].features[j]>1)return -1;
        if(!policy&&cnet_core_cell_predict(cell,t[i].features,scores+i))return -1;
        scores[i]=(policy==2||policy==3?t[i].features[0]:scores[i])/t[i].cost;
    }
    CnetAllocatorChoice p={0};uint32_t used=0;
    while(p.count<jobs){
        unsigned best=(unsigned)n;
        for(unsigned i=0;i<n;i++){
            if((used>>i&1)||t[i].cost>work-p.work)continue;
            if(best==n){best=i;continue;}
            int overdue=t[i].age>=wait_limit,prior=t[best].age>=wait_limit,better;
            if(overdue!=prior)better=overdue;
            else if(overdue&&t[i].age!=t[best].age)better=t[i].age>t[best].age;
            else if(!overdue&&policy==3&&t[i].features[1]!=t[best].features[1])better=t[i].features[1]>t[best].features[1];
            else if(!overdue&&policy!=1&&scores[i]!=scores[best])better=scores[i]>scores[best];
            else better=cursor_rank(i,cursor,(unsigned)n)<cursor_rank(best,cursor,(unsigned)n);
            if(better)best=i;
        }
        if(best==n)break;
        used|=UINT32_C(1)<<best;p.index[p.count++]=best;p.work+=t[best].cost;
    }
    *out=p;return 0;
}
static int digest(const char *s){
    int nonzero=0;
    for(unsigned i=0;i<64;i++){if(!((s[i]>='0'&&s[i]<='9')||(s[i]>='a'&&s[i]<='f')))return -1;nonzero|=s[i]!='0';}
    return s[64]||!nonzero?-1:0;
}
int cnet_core_allocator_episode_validate(const CnetAllocatorEpisode *e){
    if(!e||!e->id||e->family>=4||!e->population||e->population>64)return -1;
    CnetAllocatorChoice p;
    if(cnet_core_allocator_choose(NULL,e->task,e->n,e->cursor,e->jobs,e->work,e->wait_limit,1,&p))return -1;
    for(unsigned i=0;i<e->n;i++)if((e->population<64&&(e->coverage[i]>>e->population))||digest(e->receipt[i]))return -1;
    return 0;
}
int cnet_core_allocator_evaluate_against(const CnetCoreCell *cell,const CnetCoreCell *active,const CnetAllocatorEpisode *e,size_t n,CnetAllocatorGateReport *out){
    if(!out)return -1;
    memset(out,0,sizeof *out);
    if(cnet_core_cell_validate(cell)||(active&&cnet_core_cell_validate(active))||!e||n<32||n>256)return -1;
    CnetAllocatorGateReport r={0};r.comparators=active?4:3;double square[4]={0};size_t rows=0;
    for(size_t i=0;i<n;i++){
        if(cnet_core_allocator_episode_validate(e+i))return -1;
        rows+=e[i].n;if(rows>2048)return -1;
        for(size_t j=0;j<i;j++)if(e[i].id==e[j].id)return -1;
        double value[5];
        for(unsigned policy=0;policy<=r.comparators;policy++){
            CnetAllocatorChoice p;
            if(cnet_core_allocator_choose(policy==4?active:cell,e[i].task,e[i].n,e[i].cursor,e[i].jobs,e[i].work,e[i].wait_limit,policy==4?0:policy,&p))return -1;
            uint64_t mask=0;for(unsigned j=0;j<p.count;j++)mask|=e[i].coverage[p.index[j]];
            value[policy]=(double)__builtin_popcountll(mask)/e[i].population;r.coverage[policy]+=value[policy];
        }
        for(unsigned j=0;j<r.comparators;j++){double d=value[0]-value[j+1];r.gain[j]+=d;square[j]+=d*d;r.family_gain[e[i].family][j]+=d;}
        r.family_episodes[e[i].family]++;r.episodes++;
    }
    r.passed=1;
    for(unsigned j=0;j<=r.comparators;j++)r.coverage[j]/=n;
    for(unsigned j=0;j<r.comparators;j++){
        double sum=r.gain[j];r.gain[j]/=n;
        double variance=fmax(0,(square[j]-sum*sum/n)/(n-1));
        r.paired95_lower[j]=r.gain[j]-2.040*sqrt(variance/n);
        if(r.gain[j]<.05||r.paired95_lower[j]<=0)r.passed=0;
    }
    for(unsigned f=0;f<4;f++){
        if(r.family_episodes[f]<4)r.passed=0;
        for(unsigned j=0;j<r.comparators;j++){
            if(r.family_episodes[f])r.family_gain[f][j]/=r.family_episodes[f];
            if(r.family_gain[f][j]<0)r.passed=0;
        }
    }
    *out=r;return r.passed?0:1;
}
int cnet_core_allocator_evaluate(const CnetCoreCell *cell,const CnetAllocatorEpisode *e,size_t n,CnetAllocatorGateReport *out){return cnet_core_allocator_evaluate_against(cell,NULL,e,n,out);}
