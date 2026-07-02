/* Corpus PPMI synonym map: same-tile co-occurrence -> Positive PMI -> top-k per term.
   Interpretable, corpus-grown, no dense weights. See include/corpus/synonyms.h. */
#include "../../include/corpus/synonyms.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <stdint.h>

/* ---- sparse pair-count map: key=(i<<32)|j with i<j (i,j term ids); 0 = empty ---- */
typedef struct { uint64_t *key; uint32_t *cnt; size_t cap, mask, n; } PairMap;
static void pm_grow(PairMap *p){
    size_t nc=p->cap? p->cap*2:4096, nm=nc-1;
    uint64_t *nk=(uint64_t*)calloc(nc,sizeof(uint64_t));
    uint32_t *ncnt=(uint32_t*)calloc(nc,sizeof(uint32_t));
    for(size_t i=0;i<p->cap;i++) if(p->key[i]){ size_t h=(size_t)(p->key[i]*1099511628211ULL)&nm;
        while(nk[h]) h=(h+1)&nm;
        nk[h]=p->key[i]; ncnt[h]=p->cnt[i]; }
    free(p->key); free(p->cnt); p->key=nk; p->cnt=ncnt; p->cap=nc; p->mask=nm;
}
static void pm_inc(PairMap *p, uint64_t k){
    if(p->cap==0 || p->n*10>=p->cap*7) pm_grow(p);
    size_t h=(size_t)(k*1099511628211ULL)&p->mask;
    while(p->key[h]){ if(p->key[h]==k){ p->cnt[h]++; return; } h=(h+1)&p->mask; }
    p->key[h]=k; p->cnt[h]=1; p->n++;
}

/* ---- vocab (term -> id) + finalized top-k neighbor store (parallel arrays by id) ---- */
struct Synonyms {
    char   **term;  size_t nterm, cap_term;   /* id -> term string (owned) */
    int     *hslot; size_t hcap, hmask;        /* hash term -> id+1 (0 empty) */
    PairMap  pairs;                            /* raw co-occurrence (freed at finalize) */
    char  ***nbr;   float **wt; int *nn;       /* per id: neighbor term strings + ppmi + count */
    int      finalized;
};

static unsigned long sh(const char *s){ unsigned long h=5381; int c; while((c=(unsigned char)*s++)) h=((h<<5)+h)^c; return h; }

static void vocab_grow_terms(Synonyms *s){
    size_t nc=s->cap_term? s->cap_term*2:256;
    s->term=(char**)realloc(s->term,nc*sizeof(char*));
    s->nbr =(char***)realloc(s->nbr ,nc*sizeof(char**));
    s->wt  =(float**)realloc(s->wt  ,nc*sizeof(float*));
    s->nn  =(int*)   realloc(s->nn  ,nc*sizeof(int));
    for(size_t i=s->cap_term;i<nc;i++){ s->term[i]=NULL; s->nbr[i]=NULL; s->wt[i]=NULL; s->nn[i]=0; }
    s->cap_term=nc;
}
static int vocab_find(const Synonyms *s, const char *t){
    if(s->hcap==0) return -1;
    size_t h=sh(t)&s->hmask;
    while(s->hslot[h]){ int id=s->hslot[h]-1; if(strcmp(s->term[id],t)==0) return id; h=(h+1)&s->hmask; }
    return -1;
}
static void vocab_rehash(Synonyms *s){
    size_t nc=s->hcap? s->hcap*2:512, nm=nc-1;
    int *ns=(int*)calloc(nc,sizeof(int));
    for(size_t id=0;id<s->nterm;id++){ size_t h=sh(s->term[id])&nm; while(ns[h]) h=(h+1)&nm; ns[h]=(int)id+1; }
    free(s->hslot); s->hslot=ns; s->hcap=nc; s->hmask=nm;
}
static int vocab_id(Synonyms *s, const char *t){
    int id=vocab_find(s,t); if(id>=0) return id;
    if(s->nterm+1>=s->cap_term) vocab_grow_terms(s);
    if(s->hcap==0 || (s->nterm+1)*10>=s->hcap*7) vocab_rehash(s);
    id=(int)s->nterm++; s->term[id]=strdup(t);
    size_t h=sh(t)&s->hmask; while(s->hslot[h]) h=(h+1)&s->hmask; s->hslot[h]=id+1;
    return id;
}

Synonyms *syn_new(void){ Synonyms *s=(Synonyms*)calloc(1,sizeof(*s)); return s; }

static void free_topk(Synonyms *s){
    if(!s->nbr) return;
    for(size_t id=0;id<s->cap_term;id++){
        if(s->nbr[id]){ for(int i=0;i<s->nn[id];i++) free(s->nbr[id][i]); free(s->nbr[id]); s->nbr[id]=NULL; }
        if(s->wt[id]){ free(s->wt[id]); s->wt[id]=NULL; }
        s->nn[id]=0;
    }
}
void syn_free(Synonyms *s){
    if(!s) return;
    free_topk(s);
    for(size_t id=0;id<s->nterm;id++) free(s->term[id]);
    free(s->term); free(s->nbr); free(s->wt); free(s->nn); free(s->hslot);
    free(s->pairs.key); free(s->pairs.cnt);
    free(s);
}

void syn_observe_tile(Synonyms *s, const char *const *terms, int n){
    if(n<2) return;
    int ids[256]; int m=0;
    for(int i=0;i<n && m<256;i++){ int id=vocab_id(s,terms[i]);
        int dup=0; for(int j=0;j<m;j++) if(ids[j]==id){ dup=1; break; } if(!dup) ids[m++]=id; }
    for(int i=1;i<m;i++){ int v=ids[i],j=i-1; while(j>=0&&ids[j]>v){ ids[j+1]=ids[j]; j--; } ids[j+1]=v; }
    for(int i=0;i<m;i++) for(int j=i+1;j<m;j++){
        uint64_t key=((uint64_t)(uint32_t)ids[i]<<32)|(uint32_t)ids[j];
        pm_inc(&s->pairs,key);
    }
}

/* insert (nbr,w) into id's top-k list, kept sorted descending by weight */
static void topk_insert(Synonyms *s, int id, const char *nbr, float w, int k){
    if(!s->nbr[id]){ s->nbr[id]=(char**)calloc((size_t)k,sizeof(char*));
                     s->wt[id]=(float*)calloc((size_t)k,sizeof(float)); s->nn[id]=0; }
    char **N=s->nbr[id]; float *W=s->wt[id]; int n=s->nn[id];
    if(n>=k && w<=W[k-1]) return;
    int pos;
    if(n<k){ pos=n; s->nn[id]=n+1; }
    else   { pos=k-1; free(N[k-1]); }
    int p=pos; while(p>0 && W[p-1]<w){ N[p]=N[p-1]; W[p]=W[p-1]; p--; }
    N[p]=strdup(nbr); W[p]=w;
}

void syn_finalize(Synonyms *s, unsigned (*df_of)(void*,const char*), void *ctx,
                  size_t N, int k, int min_cooc, double df_frac, int min_df, int discount){
    if(k<1) k=1;
    free_topk(s);
    /* precompute df + eligibility per id */
    unsigned *df=(unsigned*)calloc(s->nterm?s->nterm:1,sizeof(unsigned));
    char     *elig=(char*)calloc(s->nterm?s->nterm:1,sizeof(char));
    double hi=df_frac*(double)N;
    for(size_t id=0;id<s->nterm;id++){ unsigned d=df_of?df_of(ctx,s->term[id]):0; df[id]=d;
        elig[id]=(d>=(unsigned)min_df && (double)d<=hi); }
    double Nd=(double)N;
    for(size_t h=0;h<s->pairs.cap;h++){
        uint64_t key=s->pairs.key[h]; if(!key) continue;
        uint32_t c=s->pairs.cnt[h]; if((int)c<min_cooc) continue;
        int i=(int)(key>>32), j=(int)(key&0xffffffffu);
        if(!elig[i]||!elig[j]) continue;
        double pmi=log((double)c*Nd/((double)df[i]*(double)df[j]));
        if(pmi<=0.0) continue;
        double score=pmi;
        if(discount){
            double mn=(df[i]<df[j])?(double)df[i]:(double)df[j];
            score = pmi * ((double)c/((double)c+1.0)) * (mn/(mn+1.0));
        }
        topk_insert(s,i,s->term[j],(float)score,k);
        topk_insert(s,j,s->term[i],(float)score,k);
    }
    free(df); free(elig);
    free(s->pairs.key); free(s->pairs.cnt); memset(&s->pairs,0,sizeof(s->pairs));
    s->finalized=1;
}

int syn_neighbors(const Synonyms *s, const char *term, const char **out_terms, float *out_ppmi, int k){
    int id=vocab_find(s,term);
    if(id<0 || !s->nbr || !s->nbr[id]) return 0;
    int n=s->nn[id]; if(n>k) n=k;
    for(int i=0;i<n;i++){ out_terms[i]=s->nbr[id][i]; out_ppmi[i]=s->wt[id][i]; }
    return n;
}

int syn_save(const Synonyms *s, const char *path, size_t stamp){
    FILE *f=fopen(path,"w"); if(!f) return -1;
    fprintf(f,"%zu\n",stamp);
    for(size_t id=0;id<s->nterm;id++){
        if(!s->nbr || !s->nbr[id] || s->nn[id]<=0) continue;
        fprintf(f,"%s %d",s->term[id],s->nn[id]);
        for(int i=0;i<s->nn[id];i++) fprintf(f," %s %.6f",s->nbr[id][i],s->wt[id][i]);
        fprintf(f,"\n");
    }
    fclose(f); return 0;
}

int syn_load(Synonyms *s, const char *path, size_t *stamp_out){
    FILE *f=fopen(path,"r"); if(!f){ if(stamp_out)*stamp_out=0; return -1; }
    size_t stamp=0; if(fscanf(f,"%zu\n",&stamp)!=1){ fclose(f); if(stamp_out)*stamp_out=0; return -1; }
    if(stamp_out)*stamp_out=stamp;
    char term[64]; int m;
    while(fscanf(f,"%63s %d",term,&m)==2){
        if(m<0||m>1024){ fclose(f); return -1; }
        int id=vocab_id(s,term);
        s->nbr[id]=(char**)calloc((size_t)(m?m:1),sizeof(char*));
        s->wt[id]=(float*)calloc((size_t)(m?m:1),sizeof(float)); s->nn[id]=m;
        for(int i=0;i<m;i++){ char nb[64]; float w;
            if(fscanf(f,"%63s %f",nb,&w)!=2){ fclose(f); return -1; }
            s->nbr[id][i]=strdup(nb); s->wt[id][i]=w; }
    }
    s->finalized=1; fclose(f); return 0;
}

size_t syn_term_count(const Synonyms *s){ return s?s->nterm:0; }
size_t syn_neighbor_edges(const Synonyms *s){ if(!s||!s->nbr) return 0;
    size_t e=0; for(size_t id=0;id<s->nterm;id++) if(s->nbr[id]) e+=(size_t)s->nn[id]; return e; }
