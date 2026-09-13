/* Read-only passage evidence experiment; never changes a capsule or its seal. */
#include "cnet_vsa_gen_capsule.h"
#include "cnet_vsa_lexicon.h"
#include <stdlib.h>
#include <string.h>
typedef struct { uint64_t key, mask[2]; } Term;
typedef struct { CnetVsaGenCapsule *cap; int8_t *q8; float *norm; Term *terms; int nt; } Probe;
static CnetVsaLexicon lex;
int pr_lex(const char *path) {
    if(cnet_vsa_lexicon_load(&lex,path))return -1;
    cnet_vsa_lexicon_set_active(&lex);return 0;
}
static int cmpterm(const void *a,const void *b) {
    uint64_t x=((const Term*)a)->key,y=((const Term*)b)->key;return (x>y)-(x<y);
}
void pr_free(Probe *p){if(p){free(p->cap);free(p->q8);free(p->norm);free(p->terms);free(p);}}
Probe *pr_load(const char *path) {
    Probe *p=calloc(1,sizeof(*p));if(!p)return NULL;
    p->cap=malloc(sizeof(*p->cap));
    if(!p->cap || cnet_vsa_gencap_load(p->cap,path) || !p->cap->passages.present){pr_free(p);return NULL;}
    int n=p->cap->passages.count;
    p->q8=malloc((size_t)n*CNET_VSA_TOPICAL_DIM);p->norm=malloc((size_t)n*sizeof(float));
    p->terms=calloc((size_t)n*CNET_VSA_MAX_TOKENS,sizeof(Term));
    if(!p->q8 || !p->norm || !p->terms){pr_free(p);return NULL;}
    for(int i=0;i<n;++i) {
        const char *text=p->cap->passages.text+p->cap->passages.offset[i];
        int8_t *v=p->q8+(size_t)i*CNET_VSA_TOPICAL_DIM;
        if(cnet_vsa_gencap_encode_intent_q8(text,v,p->cap->calib.reserved0)){pr_free(p);return NULL;}
        p->norm[i]=cnet_vsa_text_q8_norm(v);
        CnetVsaTokenList tl;if(cnet_vsa_text_tokenize(text,&tl)<0){pr_free(p);return NULL;}
        for(size_t j=0;j<tl.count;++j) {
            if(cnet_vsa_text_is_stopword(tl.tokens[j].token))continue;
            Term *t=&p->terms[p->nt++];t->key=cnet_vsa_lexicon_word_key(tl.tokens[j].token);t->mask[i/64]=1ULL<<(i%64);
        }
    }
    qsort(p->terms,p->nt,sizeof(Term),cmpterm);
    int used=0;
    for(int i=0;i<p->nt;++i) {
        if(used && p->terms[used-1].key==p->terms[i].key) {
            p->terms[used-1].mask[0]|=p->terms[i].mask[0];p->terms[used-1].mask[1]|=p->terms[i].mask[1];
        } else p->terms[used++]=p->terms[i];
    }
    p->nt=used;return p;
}
int pr_count(const Probe *p){return p->cap->passages.count;}
float pr_floor(const Probe *p){return p->cap->passages.z_min;}
const char *pr_text(const Probe *p,int i){return i>=0 && i<pr_count(p)?p->cap->passages.text+p->cap->passages.offset[i]:NULL;}
int pr_scores(const Probe *p,const char *query,float *cosine,float *overlap) {
    int n=pr_count(p);int8_t qv[CNET_VSA_TOPICAL_DIM];
    if(cnet_vsa_gencap_encode_intent_q8(query,qv,p->cap->calib.reserved0))return -1;
    float qn=cnet_vsa_text_q8_norm(qv);
    for(int i=0;i<n;++i)cosine[i]=cnet_vsa_text_q8_similarity_n(qv,qn,p->q8+(size_t)i*CNET_VSA_TOPICAL_DIM,p->norm[i]);
    memset(overlap,0,(size_t)n*sizeof(float));
    CnetVsaTokenList tl;if(cnet_vsa_text_tokenize(query,&tl)<0)return -1;
    uint64_t seen[CNET_VSA_MAX_TOKENS];int ns=0;float total=0;
    for(size_t i=0;i<tl.count;++i) {
        if(cnet_vsa_text_is_stopword(tl.tokens[i].token))continue;
        uint64_t key=cnet_vsa_lexicon_word_key(tl.tokens[i].token);int duplicate=0;
        for(int j=0;j<ns;++j)if(seen[j]==key){duplicate=1;break;}
        if(duplicate)continue;
        seen[ns++]=key;
        const CnetVsaLexiconEntry *e=cnet_vsa_lexicon_find_hash(&lex,key);
        float idf=e?e->idf:1.0f;total+=idf;
        Term needle={.key=key};Term *t=bsearch(&needle,p->terms,p->nt,sizeof(Term),cmpterm);
        if(t)for(int h=0;h<2;++h){uint64_t bits=t->mask[h];while(bits){int bit=__builtin_ctzll(bits);overlap[h*64+bit]+=idf;bits&=bits-1;}}
    }
    if(total>0)for(int i=0;i<n;++i)overlap[i]/=total;
    return ns;
}
void pr_sample(const char *name,int size,int *out,int k) {
    uint64_t seed=1469598103934665603ULL;
    for(const unsigned char *p=(const unsigned char*)name;*p;++p){seed^=*p;seed*=1099511628211ULL;}
    if(!seed)seed=0x9E3779B97F4A7C15ULL;
    for(int i=0;i<size;++i) {
        if(i<k)out[i]=i;
        else{seed^=seed<<13;seed^=seed>>7;seed^=seed<<17;uint64_t j=seed%(uint64_t)(i+1);if(j<(uint64_t)k)out[j]=i;}
    }
}
int pr_vocab(uint64_t *keys,int8_t *vectors) {
    int n=(int)lex.hdr.count;
    if(keys && vectors)for(int i=0;i<n;++i){keys[i]=lex.entries[i].key;memcpy(vectors+(size_t)i*CNET_VSA_TOPICAL_DIM,lex.entries[i].q8,CNET_VSA_TOPICAL_DIM);}
    return n;
}
int pr_terms(const char *text,uint64_t *keys,float *weights) {
    CnetVsaTokenList tl;if(cnet_vsa_text_tokenize(text,&tl)<0)return -1;int n=0;
    for(size_t i=0;i<tl.count;++i){if(cnet_vsa_text_is_stopword(tl.tokens[i].token))continue;
        uint64_t key=cnet_vsa_lexicon_word_key(tl.tokens[i].token);int seen=0;
        for(int j=0;j<n;++j)if(keys[j]==key){seen=1;break;}
        if(seen)continue;
        const CnetVsaLexiconEntry *e=cnet_vsa_lexicon_find_hash(&lex,key);keys[n]=key;weights[n++]=e?e->idf:1.f;
    }return n;
}
