#include "cnet_vsa_evidence.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int evidence_write(char *out, size_t size, const char *text, int status) {
    size_t needed=strlen(text)+1;
    if(needed>size)return CNET_VSA_EVIDENCE_NOSPACE;
    memcpy(out,text,needed);return status;
}
static int evidence_refuse(char *out, size_t size) {
    return evidence_write(out,size,"ABSTAIN: no uniquely supported answer.",CNET_VSA_EVIDENCE_REFUSE);
}
int cnet_vsa_evidence_response(const CnetVsaEvidenceFact *facts, size_t count,
                              int start, const int *relations, size_t hops,
                              int proposed, char *out, size_t out_size) {
    if(!out||!out_size)return CNET_VSA_EVIDENCE_INVALID;
    out[0]=0;
    if((count&&!facts)||count>CNET_VSA_EVIDENCE_FACTS||!relations||!hops||
       hops>CNET_VSA_EVIDENCE_HOPS||start<0||start>=CNET_VSA_EVIDENCE_ENTITIES||
       proposed< -1||proposed>=CNET_VSA_EVIDENCE_ENTITIES)return CNET_VSA_EVIDENCE_INVALID;
    for(size_t i=0;i<hops;++i)
        if(relations[i]<0||relations[i]>=CNET_VSA_EVIDENCE_RELATIONS)return CNET_VSA_EVIDENCE_INVALID;
    for(size_t i=0;i<count;++i) {
        const CnetVsaEvidenceFact *e=&facts[i];
        if(e->subject<0||e->subject>=CNET_VSA_EVIDENCE_ENTITIES||
           e->object<0||e->object>=CNET_VSA_EVIDENCE_ENTITIES||
           e->relation<0||e->relation>=CNET_VSA_EVIDENCE_RELATIONS||
           (e->sign!=1&&e->sign!=-1))return CNET_VSA_EVIDENCE_INVALID;
    }
    unsigned char active[CNET_VSA_EVIDENCE_ENTITIES]={0},next[CNET_VSA_EVIDENCE_ENTITIES];
    int16_t parent[CNET_VSA_EVIDENCE_HOPS][CNET_VSA_EVIDENCE_ENTITIES];
    active[start]=1;
    for(size_t h=0;h<hops;++h) {
        memset(next,0,sizeof next);
        for(size_t i=0;i<count;++i) {
            const CnetVsaEvidenceFact *e=&facts[i];
            if(e->sign<0||!active[e->subject]||e->relation!=relations[h])continue;
            for(size_t j=0;j<count;++j) {
                const CnetVsaEvidenceFact *f=&facts[j];
                if(f->sign<0&&f->subject==e->subject&&f->relation==e->relation&&f->object==e->object)
                    return evidence_refuse(out,out_size);
            }
            if(!next[e->object])parent[h][e->object]=(int16_t)i;
            next[e->object]=1;
        }
        memcpy(active,next,sizeof active);
    }
    int answer=-1;
    for(int n=0;n<CNET_VSA_EVIDENCE_ENTITIES;++n)if(active[n]) {
        if(answer>=0)return evidence_refuse(out,out_size);
        answer=n;
    }
    if(answer<0||(proposed>=0&&proposed!=answer))return evidence_refuse(out,out_size);
    int path[CNET_VSA_EVIDENCE_HOPS],at=answer;
    for(size_t h=hops;h>0;--h) {
        path[h-1]=parent[h-1][at];at=facts[path[h-1]].subject;
    }
    /* Build atomically so a short caller buffer cannot publish a partial proof. */
    char text[CNET_VSA_EVIDENCE_OUTPUT];
    size_t used=(size_t)snprintf(text,sizeof text,"node%03d.",answer);
    for(size_t h=0;h<hops;++h) {
        const CnetVsaEvidenceFact *e=&facts[path[h]];
        int n=snprintf(text+used,sizeof text-used," node%03d rel%02d node%03d.",e->subject,e->relation,e->object);
        if(n<0||(size_t)n>=sizeof text-used)return CNET_VSA_EVIDENCE_NOSPACE;
        used+=(size_t)n;
    }
    return evidence_write(out,out_size,text,CNET_VSA_EVIDENCE_OK);
}
