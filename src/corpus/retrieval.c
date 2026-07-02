/* Compounding retrieval memory: a context -> continuation-counts datastore
   (a kNN-LM / interpolated-n-gram store). New corpora are INDEXED into it
   (O(corpus)); contexts already present are reused, not relearned. The growing,
   persistent memory is the LM-layer analog of CNET's frozen-primitive library. */
#include "../../include/corpus/retrieval.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef struct { int next; int count; } Cont;
typedef struct { int used; int ctx[RETRIEVAL_MAX_CTX]; Cont *conts; int n_conts, cap_conts; } Entry;
struct RetrievalStore { int ctx; Entry *tab; size_t cap, mask, n_keys, n_pairs; };

static size_t ctx_hash(const int *c, int len){   /* FNV-1a (64-bit math; ULL avoids
                                                    truncation where unsigned long is 32-bit) */
    unsigned long long h=1469598103934665603ULL;
    for(int i=0;i<len;i++){ h ^= (unsigned long long)(unsigned int)c[i]; h *= 1099511628211ULL; }
    return (size_t)h;
}
static int ctx_eq(const int *a, const int *b, int len){ for(int i=0;i<len;i++) if(a[i]!=b[i]) return 0; return 1; }

static void grow(RetrievalStore *r){
    size_t nc = r->cap? r->cap*2 : 1024, nmask=nc-1;
    Entry *nt=(Entry*)calloc(nc,sizeof(Entry));
    for(size_t i=0;i<r->cap;i++) if(r->tab[i].used){
        size_t h=ctx_hash(r->tab[i].ctx,r->ctx)&nmask; while(nt[h].used) h=(h+1)&nmask; nt[h]=r->tab[i];
    }
    free(r->tab); r->tab=nt; r->cap=nc; r->mask=nmask;
}
RetrievalStore *retrieval_create(int ctx){
    if(ctx<1||ctx>RETRIEVAL_MAX_CTX) return NULL;
    RetrievalStore *r=(RetrievalStore*)calloc(1,sizeof(*r)); r->ctx=ctx; return r;
}
void retrieval_free(RetrievalStore *r){
    if(!r) return;
    for(size_t i=0;i<r->cap;i++) if(r->tab[i].used) free(r->tab[i].conts);
    free(r->tab); free(r);
}
static size_t find_slot(RetrievalStore *r, const int *c, int *found){
    if(r->cap==0 || r->n_keys*10 >= r->cap*7) grow(r);
    size_t h=ctx_hash(c,r->ctx)&r->mask;
    while(r->tab[h].used){ if(ctx_eq(r->tab[h].ctx,c,r->ctx)){ *found=1; return h; } h=(h+1)&r->mask; }
    *found=0; return h;
}
static int add_cont(Entry *e, int next){   /* returns 1 if a NEW (ctx,next) pair */
    for(int i=0;i<e->n_conts;i++) if(e->conts[i].next==next){ e->conts[i].count++; return 0; }
    if(e->n_conts==e->cap_conts){ e->cap_conts=e->cap_conts?e->cap_conts*2:4; e->conts=(Cont*)realloc(e->conts,e->cap_conts*sizeof(Cont)); }
    e->conts[e->n_conts].next=next; e->conts[e->n_conts].count=1; e->n_conts++; return 1;
}
IngestStats retrieval_ingest(RetrievalStore *r, const int *tokens, size_t n){
    IngestStats st={0,0,0,0};
    if((int)n<=r->ctx) return st;
    for(size_t i=(size_t)r->ctx;i<n;i++){
        const int *c=&tokens[i-r->ctx]; int found; size_t h=find_slot(r,c,&found); Entry *e=&r->tab[h];
        if(!found){ e->used=1; memcpy(e->ctx,c,r->ctx*sizeof(int)); e->conts=NULL; e->n_conts=0; e->cap_conts=0; r->n_keys++; st.new_keys++; }
        else st.reused_keys++;
        if(add_cont(e,tokens[i])){ r->n_pairs++; st.new_pairs++; }
        st.total_contexts++;
    }
    return st;
}
int retrieval_lookup(const RetrievalStore *r, const int *ctx_tokens, int *out_words, int *out_counts, int cap){
    if(r->cap==0) return 0;
    size_t h=ctx_hash(ctx_tokens,r->ctx)&r->mask;
    while(r->tab[h].used){
        if(ctx_eq(r->tab[h].ctx,ctx_tokens,r->ctx)){
            const Entry *e=&r->tab[h]; int m=e->n_conts<cap?e->n_conts:cap;
            for(int i=0;i<m;i++){ out_words[i]=e->conts[i].next; out_counts[i]=e->conts[i].count; }
            return m;
        }
        h=(h+1)&r->mask;
    }
    return 0;
}
size_t retrieval_keys(const RetrievalStore *r){ return r->n_keys; }
size_t retrieval_pairs(const RetrievalStore *r){ return r->n_pairs; }
size_t retrieval_certifiable_contexts(const RetrievalStore *r, int min_count, double min_share){
    size_t cnt=0;
    for(size_t i=0;i<r->cap;i++){ if(!r->tab[i].used) continue;
        const Entry *e=&r->tab[i]; int total=0,top=0;
        for(int j=0;j<e->n_conts;j++){ total+=e->conts[j].count; if(e->conts[j].count>top) top=e->conts[j].count; }
        if(top>=min_count && total>0 && (double)top/total>=min_share) cnt++;
    }
    return cnt;
}
int retrieval_save(const RetrievalStore *r, const char *path){
    FILE *f=fopen(path,"wb"); if(!f) return -1;
    fwrite("CNETRET1",1,8,f); fwrite(&r->ctx,sizeof(int),1,f); fwrite(&r->n_keys,sizeof(size_t),1,f);
    for(size_t i=0;i<r->cap;i++){ if(!r->tab[i].used) continue; const Entry *e=&r->tab[i];
        fwrite(e->ctx,sizeof(int),r->ctx,f); fwrite(&e->n_conts,sizeof(int),1,f); fwrite(e->conts,sizeof(Cont),e->n_conts,f); }
    fclose(f); return 0;
}
RetrievalStore *retrieval_load(const char *path){
    FILE *f=fopen(path,"rb"); if(!f) return NULL;
    char m[8]; if(fread(m,1,8,f)!=8||memcmp(m,"CNETRET1",8)){ fclose(f); return NULL; }
    int ctx; size_t nk;
    if(fread(&ctx,sizeof(int),1,f)!=1||fread(&nk,sizeof(size_t),1,f)!=1){ fclose(f); return NULL; }
    RetrievalStore *r=retrieval_create(ctx); if(!r){ fclose(f); return NULL; }
    for(size_t k=0;k<nk;k++){ int c[RETRIEVAL_MAX_CTX]; int nc;
        if(fread(c,sizeof(int),ctx,f)!=(size_t)ctx) break;
        if(fread(&nc,sizeof(int),1,f)!=1) break;
        int found; size_t h=find_slot(r,c,&found); (void)found; Entry *e=&r->tab[h];
        e->used=1; memcpy(e->ctx,c,ctx*sizeof(int)); e->conts=(Cont*)malloc((nc>0?nc:1)*sizeof(Cont)); e->cap_conts=nc>0?nc:1; e->n_conts=nc;
        if(nc>0 && fread(e->conts,sizeof(Cont),nc,f)!=(size_t)nc) break;
        r->n_keys++; r->n_pairs+=(size_t)nc;
    }
    fclose(f); return r;
}
