# Compounding Retrieval Memory — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the generative layer *compound* — a growing retrieval memory keyed on generation context, so learning a new book is O(book) (index it) instead of retraining O(Σ), with measured reuse and a bridge to certified contracts.

**Architecture:** A new `RetrievalStore` (context→continuation counts, kNN-LM datastore) is fed by the existing PDF→corpus pipeline. A frozen `cce_wordlm` provides smoothing; generation is retrieval-primary with LM backoff. A `compound_demo` measures the compounding (identical/related/unrelated ingest), persists the store, runs on a real PDF, and flags certifiable contexts. Zero core/router/contract/CCE edits.

**Tech Stack:** C11 (gcc, `-mno-avx`), stdlib + libm. Reuses `src/corpus/corpus_split.c`, `src/corpus/corpus_store.c`, `src/pdf/*`, `src/cce/cce_wordlm.c`.

**Project note — NO GIT:** `.git` was removed. Run no git commands. "Commit" steps → **checkpoint**: re-run `make compound` and confirm assertions still pass.

---

## File Structure

- **Create `include/corpus/retrieval.h`, `src/corpus/retrieval.c`** — the `RetrievalStore` (the compounding memory).
- **Create `tests/compound_demo.c`** — self-checking demo: store unit checks, the compounding proof, retrieval-augmented generation, persistence, real-PDF run, contract-candidate scan. `CHECK` macro + nonzero exit on failure.
- **Modify `Makefile`** — `RETRIEVAL_SRC`/`COMPOUND_DEMO` vars, `compound` target, `.PHONY`, `clean`.

`CHECK` macro for `compound_demo.c`:
```c
static int g_fail=0, g_checks=0;
#define CHECK(c,m) do{ g_checks++; if(!(c)){ g_fail++; printf("  FAIL: %s (%s:%d)\n",m,__FILE__,__LINE__);} }while(0)
```

---

## Task 1: `RetrievalStore` module + unit checks

**Files:**
- Create: `include/corpus/retrieval.h`, `src/corpus/retrieval.c`, `tests/compound_demo.c`
- Modify: `Makefile`

- [ ] **Step 1: Header**

`include/corpus/retrieval.h`:
```c
#ifndef CORPUS_RETRIEVAL_H
#define CORPUS_RETRIEVAL_H
#include <stddef.h>
#define RETRIEVAL_MAX_CTX 4

typedef struct { size_t new_keys, reused_keys, new_pairs, total_contexts; } IngestStats;
typedef struct RetrievalStore RetrievalStore;

RetrievalStore *retrieval_create(int ctx);      /* ctx context words, 1..RETRIEVAL_MAX_CTX */
void            retrieval_free(RetrievalStore *r);

/* Index (last-ctx-words -> next) over a token stream. Returns measured stats. */
IngestStats retrieval_ingest(RetrievalStore *r, const int *tokens, size_t n);
/* Continuation counts for a context (ctx tokens). Returns #distinct written (0=unseen). */
int retrieval_lookup(const RetrievalStore *r, const int *ctx_tokens,
                     int *out_words, int *out_counts, int cap);
size_t retrieval_keys(const RetrievalStore *r);
size_t retrieval_pairs(const RetrievalStore *r);
int             retrieval_save(const RetrievalStore *r, const char *path);
RetrievalStore *retrieval_load(const char *path);
/* Count near-deterministic contexts (contract-distillation candidates): top
   continuation has count >= min_count and share >= min_share. */
size_t retrieval_certifiable_contexts(const RetrievalStore *r, int min_count, double min_share);
#endif
```

- [ ] **Step 2: Implementation `src/corpus/retrieval.c`**

```c
#include "../../include/corpus/retrieval.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef struct { int next; int count; } Cont;
typedef struct { int used; int ctx[RETRIEVAL_MAX_CTX]; Cont *conts; int n_conts, cap_conts; } Entry;
struct RetrievalStore { int ctx; Entry *tab; size_t cap, mask, n_keys, n_pairs; };

static unsigned long ctx_hash(const int *c, int len){
    unsigned long h=1469598103934665603UL;
    for(int i=0;i<len;i++){ h ^= (unsigned long)(unsigned int)c[i]; h *= 1099511628211UL; }
    return h;
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
        if(fread(c,sizeof(int),ctx,f)!=(size_t)ctx) break; if(fread(&nc,sizeof(int),1,f)!=1) break;
        int found; size_t h=find_slot(r,c,&found); Entry *e=&r->tab[h];
        e->used=1; memcpy(e->ctx,c,ctx*sizeof(int)); e->conts=(Cont*)malloc((nc>0?nc:1)*sizeof(Cont)); e->cap_conts=nc>0?nc:1; e->n_conts=nc;
        if(nc>0 && fread(e->conts,sizeof(Cont),nc,f)!=(size_t)nc) break;
        r->n_keys++; r->n_pairs+=(size_t)nc;
    }
    fclose(f); return r;
}
```

- [ ] **Step 3: `tests/compound_demo.c` scaffold with store unit checks**

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../include/corpus/retrieval.h"

static int g_fail=0, g_checks=0;
#define CHECK(c,m) do{ g_checks++; if(!(c)){ g_fail++; printf("  FAIL: %s (%s:%d)\n",m,__FILE__,__LINE__);} }while(0)

static void test_store(void){
    RetrievalStore *r=retrieval_create(2);
    int A[]={1,2,3,2,3,4};                       /* (1,2)->3 (2,3)->2 (3,2)->3 (2,3)->4 */
    IngestStats s1=retrieval_ingest(r,A,6);
    CHECK(s1.new_keys==3, "first ingest: 3 distinct contexts");
    CHECK(retrieval_keys(r)==3, "store has 3 keys");
    IngestStats s2=retrieval_ingest(r,A,6);       /* identical re-ingest */
    CHECK(s2.new_keys==0, "re-ingest identical: 0 new keys (reuse)");
    CHECK(s2.reused_keys==s1.total_contexts, "re-ingest: all contexts reused");
    int w[8],c[8]; int ctx23[2]={2,3};
    int n=retrieval_lookup(r,ctx23,w,c,8);
    CHECK(n==2, "(2,3) has 2 continuations {3,4}");
    retrieval_free(r);
}

int main(void){
    printf("=== compound_demo ===\n");
    test_store();
    printf("%d/%d checks passed\n", g_checks-g_fail, g_checks);
    return g_fail?1:0;
}
```

- [ ] **Step 4: Makefile**

Add vars near other demo vars:
```make
RETRIEVAL_SRC := src/corpus/retrieval.c
COMPOUND_DEMO := tests/compound_demo.c
```
Add target (links everything later tasks need, so the recipe never changes):
```make
compound: $(RETRIEVAL_SRC) $(CORPUS_SRC) $(PDF_SRC) $(CCE_WORDLM) $(COMPOUND_DEMO) include/corpus/retrieval.h include/corpus/corpus_split.h include/corpus/corpus_store.h include/pdf/pdf_extract.h include/cce/cce_wordlm.h
	$(CC) $(CFLAGS) -Wno-unused-function -o $@ $(RETRIEVAL_SRC) $(CORPUS_SRC) $(PDF_SRC) $(CCE_WORDLM) $(COMPOUND_DEMO) $(LDFLAGS)
	./compound
```
Add `compound` to `.PHONY` (line 117) and `compound retrieval_store.bin` to the `clean` rm list (~line 570).

- [ ] **Step 5: Build and run**

Run: `make compound`
Expected: `test_store` passes; `4/4 checks passed`; exit 0.

- [ ] **Step 6: Checkpoint (no git)** — re-run `make compound`.

---

## Task 2: the compounding proof (identical / related / unrelated)

**Files:**
- Modify: `tests/compound_demo.c`

- [ ] **Step 1: Add a hashed vocab + tokenizer + controlled corpora (above `main`)**

```c
/* --- hashed string->id vocab (same pattern as pdflearn) --- */
typedef struct { char **w; size_t n, cap; int *ht; size_t htmask; } Vocab;
static unsigned long djb2(const char *s){ unsigned long h=5381; int c; while((c=(unsigned char)*s++)) h=((h<<5)+h)^c; return h; }
static void vocab_init(Vocab *v){ v->w=NULL; v->n=0; v->cap=0; v->ht=NULL; v->htmask=0; }
static void ht_put(Vocab *v,int i){ size_t h=djb2(v->w[i])&v->htmask; while(v->ht[h]!=-1) h=(h+1)&v->htmask; v->ht[h]=i; }
static void vgrow(Vocab *v){ size_t nc=v->htmask?(v->htmask+1)*2:1024; v->ht=realloc(v->ht,nc*sizeof(int)); for(size_t i=0;i<nc;i++)v->ht[i]=-1; v->htmask=nc-1; for(size_t i=0;i<v->n;i++) ht_put(v,(int)i); }
static int vid(Vocab *v,const char*s,int add){
    if(v->htmask==0){ if(!add) return -1; vgrow(v);} else if(add&&v->n*10>=(v->htmask+1)*7) vgrow(v);
    size_t h=djb2(s)&v->htmask; while(v->ht[h]!=-1){ if(strcmp(v->w[v->ht[h]],s)==0) return v->ht[h]; h=(h+1)&v->htmask; }
    if(!add) return -1; if(v->n==v->cap){ v->cap=v->cap?v->cap*2:256; v->w=realloc(v->w,v->cap*sizeof(char*)); }
    int idx=(int)v->n; v->w[idx]=strdup(s); v->n++; ht_put(v,idx); return idx;
}
static int vtok(Vocab *v,const char*s,int*out,int cap,int add){ int n=0;char cur[64];int cl=0;
    for(const char*p=s;;++p){ char c=*p; int al=(c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='\'';
        if(al){ if(cl<63) cur[cl++]=(c>='A'&&c<='Z')?(char)(c+32):c; }
        else{ if(cl>0){cur[cl]=0;int id=vid(v,cur,add);if(id>=0&&n<cap)out[n++]=id;cl=0;}
              if(c=='.'||c=='!'||c=='?'){int id=vid(v,".",add);if(id>=0&&n<cap)out[n++]=id;} if(c==0)break; } }
    return n; }

/* Controlled corpora with KNOWN overlap. ctx=3 keys. */
static const char *BOOK_A[] = {
    "the team aligns on the goal and the value of the work.",
    "data and the kind of teams you need to build the system.",
    "change is the focus and the value of the investment grows.",
    "the leader shares the story and builds trust with the team."
};
static const char *BOOK_A2[] = {   /* identical to A */
    "the team aligns on the goal and the value of the work.",
    "data and the kind of teams you need to build the system.",
    "change is the focus and the value of the investment grows.",
    "the leader shares the story and builds trust with the team."
};
static const char *BOOK_B[] = {    /* related: 2 shared sentences + 2 new (shared vocab) */
    "the team aligns on the goal and the value of the work.",
    "change is the focus and the value of the investment grows.",
    "the team builds the system and shares the value of data.",
    "the leader aligns the goal and grows the trust of the team."
};
static const char *BOOK_C[] = {    /* unrelated: disjoint vocabulary */
    "quantum particles entangle across vast cosmic distances instantly.",
    "photosynthesis converts sunlight into chemical energy within leaves.",
    "volcanic eruptions reshape landscapes over geological timescales.",
    "migratory birds navigate using magnetic fields and starlight."
};
#define NA ((int)(sizeof(BOOK_A)/sizeof(*BOOK_A)))

/* tokenize a whole book (shared vocab) into one flat stream; returns length. */
static size_t book_tokens(Vocab *v, const char **book, int nsent, int *out, int cap){
    size_t n=0; for(int s=0;s<nsent && (int)n<cap;s++){ int t[256]; int nt=vtok(v,book[s],t,256,1);
        for(int i=0;i<nt && (int)n<cap;i++) out[n++]=t[i]; }
    return n;
}

static double reuse_rate(IngestStats s){ return s.total_contexts? (double)s.reused_keys/s.total_contexts : 0.0; }
```

- [ ] **Step 2: Add the proof and call it from `main`**

```c
static void test_compounding(void){
    Vocab v; vocab_init(&v);
    int A[1024],A2[1024],B[1024],C[1024];
    size_t na=book_tokens(&v,BOOK_A,NA,A,1024);
    size_t na2=book_tokens(&v,BOOK_A2,NA,A2,1024);
    size_t nb=book_tokens(&v,BOOK_B,NA,B,1024);
    size_t nc=book_tokens(&v,BOOK_C,NA,C,1024);

    RetrievalStore *r=retrieval_create(3);
    IngestStats sa  = retrieval_ingest(r,A,na);
    IngestStats sa2 = retrieval_ingest(r,A2,na2);   /* identical */
    IngestStats sb  = retrieval_ingest(r,B,nb);     /* related   */
    IngestStats sc  = retrieval_ingest(r,C,nc);     /* unrelated */

    printf("[compound] A  : new_keys=%zu reuse=%.0f%%\n", sa.new_keys, 100*reuse_rate(sa));
    printf("[compound] A2 : new_keys=%zu reuse=%.0f%%  (identical re-ingest)\n", sa2.new_keys, 100*reuse_rate(sa2));
    printf("[compound] B  : new_keys=%zu reuse=%.0f%%  (related)\n", sb.new_keys, 100*reuse_rate(sb));
    printf("[compound] C  : new_keys=%zu reuse=%.0f%%  (unrelated)\n", sc.new_keys, 100*reuse_rate(sc));

    CHECK(sa2.new_keys==0, "identical re-ingest adds 0 new keys (100% reuse)");
    CHECK(sb.new_keys>0 && sb.new_keys<sb.total_contexts, "related book: partial reuse (some new, some reused)");
    CHECK(sb.reused_keys>0, "related book reuses prior keys");
    CHECK((double)sc.new_keys/sc.total_contexts > reuse_rate(sb), "unrelated book has less reuse than related");

    retrieval_free(r);
    for(size_t i=0;i<v.n;i++) free(v.w[i]); free(v.w); free(v.ht);
}
```
Add `test_compounding();` to `main` after `test_store();`.

- [ ] **Step 3: Build and run**

Run: `make compound`
Expected: `[compound] A2 : new_keys=0 ... (identical re-ingest)`, `B` shows partial reuse, `C` shows low reuse; checks pass.
If `B` shows 0 reuse, the shared sentences aren't producing shared 3-grams — verify `BOOK_B` reuses whole sentences from `BOOK_A` verbatim (it does: sentences 1 and 3).

- [ ] **Step 4: Checkpoint (no git)** — re-run `make compound`.

---

## Task 3: retrieval-augmented generation + flat-vs-retrain table

**Files:**
- Modify: `tests/compound_demo.c`

- [ ] **Step 1: Add backoff generation + the cost table (above `main`), include cce_wordlm**

Add `#include "../include/cce/cce_wordlm.h"` at the top.

```c
/* Retrieval-primary generation with frozen-LM backoff. Seeds from the first
   ctx tokens of `seed`; uses retrieval counts when the context is known, else
   the frozen LM's prediction. Real words by construction. */
static void generate(RetrievalStore *r, cce_wordlm *lm, Vocab *v, const int *seed, int seedlen, char *out, size_t outsz){
    int ctx[3]; for(int k=0;k<3;k++) ctx[k]= (seedlen>k)? seed[k] : 0;
    out[0]=0; for(int k=0;k<3;k++){ if(ctx[k]>=0&&ctx[k]<(int)v->n){ strcat(out,v->w[ctx[k]]); strcat(out," "); } }
    int dot=vid(v,".",0);
    for(int step=0; step<30 && strlen(out)<700; step++){
        int w[32],c[32]; int n=retrieval_lookup(r,ctx,w,c,32);
        int nx;
        if(n>0){ int best=0; for(int i=1;i<n;i++) if(c[i]>c[best]) best=i; nx=w[best]; }   /* memory */
        else { float pen[4096]; int V=(int)v->n; for(int i=0;i<V&&i<4096;i++) pen[i]=0; nx=cce_wordlm_predict(lm,ctx,pen); }  /* LM backoff */
        if(nx<0||nx==dot) break;
        if(nx<(int)v->n){ strcat(out,v->w[nx]); strcat(out," "); }
        ctx[0]=ctx[1]; ctx[1]=ctx[2]; ctx[2]=nx;
    }
}

/* Cost contrast: retrieval ingest (flat O(book)) vs retrain a fresh LM on the
   cumulative corpus (O(sum), grows). Prints a table; asserts the trend. */
static void test_cost_curve(void){
    Vocab v; vocab_init(&v);
    const char **books[3]={BOOK_A,BOOK_B,BOOK_C}; const char *names[3]={"A","B","C"};
    int cum[4096]; size_t cumn=0;
    RetrievalStore *r=retrieval_create(3);
    double retr_ms[3], retrain_ms[3];
    printf("[cost] book | retrieval_ingest_ms | retrain_baseline_ms (cumulative)\n");
    for(int b=0;b<3;b++){
        int tok[1024]; size_t nt=book_tokens(&v,books[b],NA,tok,1024);
        clock_t a=clock(); retrieval_ingest(r,tok,nt); clock_t e=clock();
        retr_ms[b]=1000.0*(e-a)/CLOCKS_PER_SEC;
        for(size_t i=0;i<nt && cumn<4096;i++) cum[cumn++]=tok[i];     /* grow cumulative corpus */
        cce_wordlm *lm=cce_wordlm_create((int)v.n,16,3,32,7u);
        a=clock();
        for(int ep=0;ep<5;ep++) for(size_t i=3;i<cumn;i++) cce_wordlm_train_step(lm,&cum[i-3],cum[i],0.02f);
        e=clock(); retrain_ms[b]=1000.0*(e-a)/CLOCKS_PER_SEC; cce_wordlm_free(lm);
        printf("[cost]  %s   |        %7.2f      |        %7.2f\n", names[b], retr_ms[b], retrain_ms[b]);
    }
    CHECK(retrain_ms[2] > retrain_ms[0], "retrain baseline cost GROWS with cumulative corpus");
    retrieval_free(r);
    for(size_t i=0;i<v.n;i++) free(v.w[i]); free(v.w); free(v.ht);
}

/* Generation reuses memory across books (frozen LM trained once on A only). */
static void test_generation(void){
    Vocab v; vocab_init(&v);
    int A[1024],B[1024]; size_t na=book_tokens(&v,BOOK_A,NA,A,1024); size_t nb=book_tokens(&v,BOOK_B,NA,B,1024);
    cce_wordlm *lm=cce_wordlm_create((int)v.n,16,3,32,7u);          /* frozen: trained once on A */
    for(int ep=0;ep<40;ep++) for(size_t i=3;i<na;i++) cce_wordlm_train_step(lm,&A[i-3],A[i],0.02f);
    RetrievalStore *r=retrieval_create(3);
    retrieval_ingest(r,A,na); retrieval_ingest(r,B,nb);            /* memory grows; LM NOT retrained */
    char out[1024]; int seed[3]={A[0],A[1],A[2]};
    generate(r,lm,&v,seed,3,out,sizeof(out));
    printf("[generate] %s\n", out);
    CHECK(strlen(out)>12, "generation produces real-word text from memory");
    cce_wordlm_free(lm); retrieval_free(r);
    for(size_t i=0;i<v.n;i++) free(v.w[i]); free(v.w); free(v.ht);
}
```
Add `test_cost_curve();` and `test_generation();` to `main`.

- [ ] **Step 2: Build and run**

Run: `make compound`
Expected: a `[cost]` table where `retrain_baseline_ms` rises A→C while `retrieval_ingest_ms` stays tiny/flat; a `[generate]` real-word line; checks pass.

- [ ] **Step 3: Checkpoint (no git)** — re-run `make compound`.

---

## Task 4: persistence (save / load round-trip)

**Files:**
- Modify: `tests/compound_demo.c`

- [ ] **Step 1: Add the persistence check**

```c
static void test_persist(void){
    Vocab v; vocab_init(&v); int A[1024]; size_t na=book_tokens(&v,BOOK_A,NA,A,1024);
    RetrievalStore *r=retrieval_create(3); retrieval_ingest(r,A,na);
    size_t keys=retrieval_keys(r), pairs=retrieval_pairs(r);
    CHECK(retrieval_save(r,"retrieval_store.bin")==0, "save store");
    retrieval_free(r);
    RetrievalStore *r2=retrieval_load("retrieval_store.bin");
    CHECK(r2!=NULL, "load store");
    CHECK(retrieval_keys(r2)==keys && retrieval_pairs(r2)==pairs, "round-trip preserves keys+pairs");
    int w[8],c[8]; int ctx[3]={A[0],A[1],A[2]};
    CHECK(retrieval_lookup(r2,ctx,w,c,8) > 0, "loaded store answers a known context");
    retrieval_free(r2); remove("retrieval_store.bin");
    for(size_t i=0;i<v.n;i++) free(v.w[i]); free(v.w); free(v.ht);
}
```
Add `test_persist();` to `main`.

- [ ] **Step 2: Build and run**

Run: `make compound`
Expected: persistence checks pass (save, load, round-trip, loaded-lookup).

- [ ] **Step 3: Checkpoint (no git)** — re-run `make compound`.

---

## Task 5: real PDF (two halves) + contract-candidate scan + acceptance

**Files:**
- Modify: `tests/compound_demo.c`

- [ ] **Step 1: Add the real-PDF run (uses the existing PDF pipeline)**

Add includes: `#include "../include/pdf/pdf_extract.h"`, `#include "../include/corpus/corpus_split.h"`.

```c
static unsigned char *readfile(const char *p, size_t *len){ FILE *f=fopen(p,"rb"); if(!f) return NULL;
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET); if(sz<=0){fclose(f);return NULL;}
    unsigned char *b=malloc(sz); *len=fread(b,1,sz,f); fclose(f); return b; }

/* Ingest a sentence range [lo,hi) of a PDF into the store; returns IngestStats. */
static IngestStats ingest_pdf_range(RetrievalStore *r, Vocab *v, const char *path, int lo, int hi, int *valid){
    IngestStats acc={0,0,0,0}; *valid=0;
    size_t len=0; unsigned char *buf=readfile(path,&len); if(!buf) return acc;
    size_t tcap=len*4+16; char *text=malloc(tcap); size_t tl=0;
    PdfStatus st=pdf_extract_text(buf,len,text,tcap,&tl);
    if(st==PDF_OK){ *valid=1; StrList s; strlist_init(&s); corpus_split(text,&s);
        for(size_t i=(size_t)lo;i<s.count && (int)i<hi;i++){ int t[512]; int nt=vtok(v,s.lines[i],t,512,1);
            IngestStats x=retrieval_ingest(r,t,nt);
            acc.new_keys+=x.new_keys; acc.reused_keys+=x.reused_keys; acc.new_pairs+=x.new_pairs; acc.total_contexts+=x.total_contexts; }
        strlist_free(&s); }
    free(text); free(buf); return acc;
}

static void test_real_pdf(void){
    const char *path="C:/Users/Mokason/Downloads/aivalueplaybook.pdf";
    FILE *probe=fopen(path,"rb"); if(!probe){ printf("[realpdf] %s not found - skipping\n", path); return; }
    fclose(probe);
    Vocab v; vocab_init(&v); RetrievalStore *r=retrieval_create(3);
    int ok1,ok2;
    IngestStats h1=ingest_pdf_range(r,&v,path,0,200,&ok1);       /* first 200 sentences */
    IngestStats h2=ingest_pdf_range(r,&v,path,200,400,&ok2);     /* next 200 sentences */
    printf("[realpdf] half1 new_keys=%zu reuse=%.0f%% ; half2 new_keys=%zu reuse=%.0f%%\n",
           h1.new_keys, 100*reuse_rate(h1), h2.new_keys, 100*reuse_rate(h2));
    CHECK(ok1 && ok2, "real PDF extracted OK");
    CHECK(h2.reused_keys>0, "second half reuses context keys from the first (cross-section compounding)");
    size_t cand=retrieval_certifiable_contexts(r,3,0.95);
    printf("[realpdf] contract-distillation candidates (near-deterministic contexts): %zu\n", cand);
    retrieval_free(r); for(size_t i=0;i<v.n;i++) free(v.w[i]); free(v.w); free(v.ht);
}
```
Add `test_real_pdf();` to `main`.

- [ ] **Step 2: Build and run (final acceptance)**

Run: `make compound; echo "exit=$?"`
Confirm all present and exit 0:
- `[compound] A2 : new_keys=0 ... (identical re-ingest)`
- `[compound] B  : ... (related)` with partial reuse
- `[cost]` table: `retrain_baseline_ms` rises, `retrieval_ingest_ms` flat
- `[generate] ...` real-word line
- persistence checks pass
- `[realpdf] half2 ... reuse>0` and a candidate count
- `N/N checks passed`, `exit=0`

This is the acceptance: learning compounds (identical=0 new, related=partial, flat ingest vs growing retrain), generation reuses cross-book memory, it persists, runs on a real PDF, and surfaces certifiable contexts.

- [ ] **Step 3: Checkpoint (no git)** — re-run `make compound`.

---

## Self-Review (against the spec)

**Spec coverage:**
- §1.1 flat learning cost → Task 3 `test_cost_curve` (retrieval flat vs retrain growing). ✓
- §1.2 measured reuse (identical/related/unrelated) → Task 2 `test_compounding`. ✓
- §1.3 generation reuses cross-book memory → Task 3 `test_generation`. ✓
- §1.4 persists + real PDF → Task 4 `test_persist`, Task 5 `test_real_pdf`. ✓
- §1.5 contract bridge → Task 5 `retrieval_certifiable_contexts`. ✓
- §3.1 RetrievalStore (ingest stats, lookup, save/load, certifiable) → Task 1. ✓
- §3.2 retrieval-primary + LM backoff decode → Task 3 `generate`. ✓
- §5 three-point proof + flat-vs-retrain → Tasks 2–3. ✓
- §6 usable persistent multi-book → Tasks 4–5. ✓
- §9 limits respected (exact match; frozen LM; no parametric memory). ✓

**Placeholder scan:** none — every step has complete code and exact commands. The interpolation refinement (§3.2 Phase 2) is intentionally deferred in favor of backoff (noted in the spec); no placeholder.

**Type consistency:** `IngestStats{new_keys,reused_keys,new_pairs,total_contexts}` used identically everywhere; `RetrievalStore` opaque; `retrieval_*` signatures match the header across tasks; `Vocab`/`vid`/`vtok`/`book_tokens`/`reuse_rate` consistent; `generate(r,lm,v,seed,seedlen,out,sz)` matches its call. `BOOK_A/A2/B/C` + `NA` consistent.

---

## Execution note

Recommended: inline execution via `superpowers:executing-plans`. One new module + one growing demo file; deterministic checks. The real-PDF task auto-skips if the file is absent, so the suite stays green anywhere.

---

## Implementation notes (executed inline — 2026-06-26)

All 5 tasks landed; `make compound` = **17/17 checks**, exit 0; `make pdftest` still 19/19
(additive). Measured results:
- **Compounding proof:** identical re-ingest → **0 new keys / 100% reuse**; related →
  **55% reuse** (partial); unrelated → **0% reuse** (honest, no free lunch).
- **Cost contrast:** retrieval ingest **flat (0.00 ms)** vs retrain baseline **2→5→10 ms**
  (grows) — O(book) vs O(Σ), the headline.
- **Generation:** retrieval-primary + frozen-LM-backoff → coherent text from memory.
- **Persistence:** save/load round-trip preserves keys+pairs.
- **Real PDF (`aivalueplaybook.pdf`, two 200-sentence halves):** cross-section reuse is
  **modest (~4–5%)** — real prose rarely repeats 3-grams verbatim, stated honestly — but
  real (reuse > 0); **14 near-deterministic contexts** surfaced as contract-distillation
  candidates.

Deviations / notes:
- **Backoff generation** used (not full `λ`-interpolation) to keep Phase 1 core-change-free,
  exactly as the spec scoped; interpolation (needs a small `cce_wordlm` dist helper) deferred.
- **Warning cleanups:** the FNV-1a hash needed `unsigned long long`/`ULL` constants — on
  MinGW `unsigned long` is 32-bit and truncated the 64-bit constant; plus misleading-
  indentation splits and using the `outsz` param. New files compile warning-clean.
- Purely additive: new `src/corpus/retrieval.c`, `tests/compound_demo.c`, `compound` target.
  No existing source modified.
