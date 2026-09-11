/* Experiment adapter around the production generator. No production policy edits. */
#define _POSIX_C_SOURCE 200809L
#include "cnet_vsa_gen_capsule.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static double now_us(void) {
    struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
    return t.tv_sec*1e6+t.tv_nsec/1e3;
}
void *response_create(const char *corpus, double *cost) {
    double t=now_us();
    CnetVsaNgramEngine *cap=calloc(1,sizeof(*cap));if(!cap)return NULL;
    if(cnet_vsa_ngram_init(cap,512,1337))abort();
    const char *p=corpus;
    while(*p) {
        const char *end=strchr(p,'\n');size_t n=end?(size_t)(end-p):strlen(p);
        if(n>=128)abort();
        char sentence[128];memcpy(sentence,p,n);sentence[n]=0;
        if(n)cnet_vsa_ngram_ingest_sentence(cap,sentence);
        p+=n;if(*p)p++;
    }
    cost[0]=now_us()-t;cost[1]=sizeof(*cap);cost[2]=sizeof(*cap);
    return cap;
}
int response_generate(void *ptr,const char *prompt,int answer,char *out,size_t cap,double *us) {
    CnetVsaNgramEngine *model=ptr;float intent[512];char seed[32];
    snprintf(seed,sizeof seed,"node%03d",answer>=0?answer:0);
    double t=now_us();
#ifdef CNET_VSA_ENCODER_DEFAULT
    if(cnet_vsa_gencap_encode_intent_ex(prompt,intent,512,CNET_VSA_ENCODER_DEFAULT))abort();
#else
    if(cnet_vsa_gencap_encode_intent(prompt,intent,512))abort();
#endif
    /* Same generation parameters as registry dispatch. Scope already supplied
     * by the experiment; no claim of a semantic proof check in this baseline. */
    int tokens=0,rc=cnet_vsa_ngram_generate(model,seed,intent,.45f,.85f,28,out,cap,&tokens);
    *us=now_us()-t;return rc;
}
void response_free(void *ptr){free(ptr);}

#include "cnet_vsa_evidence.h"
int response_evidence(const CnetVsaEvidenceFact *facts,size_t count,int start,
                      const int *relations,size_t hops,int proposed,char *out,size_t size,double *us) {
    double t=now_us();
    int rc=cnet_vsa_evidence_response(facts,count,start,relations,hops,proposed,out,size);
    *us=now_us()-t;return rc;
}

/* Record the selected baseline API explicitly when reproducing on older master. */
int response_encoder_profile(void) {
#ifdef CNET_VSA_ENCODER_DEFAULT
    return 1;
#else
    return 0;
#endif
}
