#include "cnet_core_selector.h"
#include <string.h>
int cnet_core_selector_validate(const CnetSelectorGraph *g){
    if(!g||g->feature_version!=CNET_SELECTOR_FEATURE_VERSION||!g->generation||
       g->n<2||g->n>CNET_SELECTOR_MAX_NODES||g->start>=g->n||g->goal>=g->n)return -1;
    for(unsigned u=0;u<g->n;u++){
        const CnetSelectorNode *p=g->node+u;
        if(!p->identity||!p->version||!p->input_type||!p->output_type||p->available>1||
           (g->n<64&&(g->edge[u]>>g->n)))return -1;
        for(unsigned v=0;v<u;v++)if(p->identity==g->node[v].identity)return -1;
    }
    return g->node[g->start].available&&g->node[g->goal].available?0:-1;
}
static int legal(const CnetSelectorGraph *g,unsigned u,unsigned v){
    return ((g->edge[u]>>v)&1)&&g->node[u].available&&g->node[v].available&&g->node[u].output_type==g->node[v].input_type;
}
static int ahead(const CnetSelectorGraph *g,const int *first,const float *score,unsigned v,unsigned before){
    return first[v]<first[before]||(first[v]==first[before]&&
        (score[v]>score[before]||(score[v]==score[before]&&g->node[v].identity<g->node[before].identity)));
}
int cnet_core_selector_propose(const CnetCoreCell *m,const CnetSelectorGraph *g,unsigned iterations,CnetSelectorProposal *out){
    if(!out)return -1;
    memset(out,0,sizeof *out);out->distance=-1;
    if(cnet_core_cell_validate(m)||cnet_core_selector_validate(g)||iterations<1||iterations>64)return -1;
    CnetSelectorProposal p={0};p.distance=-1;p.generation=g->generation;
    float previous[64]={0},next[64];int first[64];
    for(unsigned u=0;u<g->n;u++)first[u]=-1;
    previous[g->goal]=1;first[g->goal]=0;
    for(unsigned t=1;t<=iterations;t++){
        for(unsigned u=0;u<g->n;u++){
            float x[3]={(float)(u==g->goal),previous[u],0};
            for(unsigned v=0;v<g->n;v++)if(legal(g,u,v)&&previous[v]>x[2])x[2]=previous[v];
            next[u]=0;if(g->node[u].available&&cnet_core_cell_predict(m,x,next+u))return -1;
            if(next[u]>=.9f&&first[u]<0)first[u]=(int)t;
        }
        memcpy(previous,next,g->n*sizeof(float));
    }
    p.iterations=iterations;p.score=previous[g->start];p.distance=first[g->start];
    if(g->start!=g->goal&&p.score>=.9f&&p.distance>0){
        for(unsigned v=0;v<g->n;v++)if(legal(g,g->start,v)&&previous[v]>=.9f&&first[v]>=0&&first[v]<p.distance){
            unsigned i=p.count++;
            while(i){
                unsigned before=p.ranked[i-1];
                if(!ahead(g,first,previous,v,before))break;
                p.ranked[i]=before;i--;
            }
            p.ranked[i]=v;
        }
    }
    if(p.count){
        unsigned cur=g->start;
        while(cur!=g->goal&&p.hops<g->n){
            unsigned best=g->n;
            for(unsigned v=0;v<g->n;v++)if(legal(g,cur,v)&&previous[v]>=.9f&&first[v]>=0&&first[v]<first[cur]&&
                (best==g->n||ahead(g,first,previous,v,best)))best=v;
            if(best==g->n)break;
            p.path[p.hops++]=best;cur=best;
        }
        if(cur!=g->goal){p.count=0;p.hops=0;memset(p.path,0,sizeof p.path);}
    }
    *out=p;return 0;
}
