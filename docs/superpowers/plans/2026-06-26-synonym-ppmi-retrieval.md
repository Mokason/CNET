# Synonym Layer via Corpus PPMI (Lever 3) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close the synonym gap in tile-memory retrieval — a query that shares zero words with the answer can now find it — via a corpus-derived Positive-PMI co-occurrence map used for opt-in, reversible query-time expansion.

**Architecture:** A new self-contained `src/corpus/synonyms.c` builds a sparse `term → top-k related terms` map from same-tile co-occurrence (PPMI; reuses the `df`/`N` tile-memory already tracks). `tile_memory` opt-in (`alpha>0`) expands each query term with its down-weighted neighbors *before* the existing TF-IDF overlap scorer runs. Default `alpha=0` ⇒ byte-identical to today. Spec: `docs/superpowers/specs/2026-06-26-synonym-ppmi-retrieval-design.md`.

**Tech Stack:** C11, MinGW gcc, `make` (Windows). No external libraries. No git on CNET (verify via filesystem; do **not** run git commands).

**Note on "Commit" steps:** This repo has **no git**. Wherever a generic TDD flow would commit, instead **re-run the full regression set** (Task 5) as the durability checkpoint. Do not run `git` commands.

---

## File Structure

- **Create** `include/corpus/synonyms.h` — public interface for the PPMI map (new/free/observe/finalize/neighbors/save/load + size accessors).
- **Create** `src/corpus/synonyms.c` — vocab (term→id hash), sparse pair-count map, PPMI finalize + top-k prune, neighbor lookup, text persistence. ~280 lines, one responsibility.
- **Create** `tests/test_synonyms.c` — unit tests (PPMI math, twins, stopword/hapax exclusion, save/load round-trip) + integration tests via `tile_memory` (gap-closer, alpha=0 reversibility) + real-book benchmark.
- **Modify** `include/corpus/tile_memory.h` — include `synonyms.h`; declare `tilemem_set_expansion`, `tilemem_build_synonyms`, `tilemem_synonyms`.
- **Modify** `src/corpus/tile_memory.c` — synonym fields + defaults; helpers (`syn_path`, `tile_distinct_terms`, `tm_df_of`); `tilemem_build_synonyms`; `syn_load` in `tilemem_load`; `syn_free` in `tilemem_close`; `syn_dirty=1` in `tilemem_ingest`; expansion block in `tilemem_search`; `tilemem_set_expansion`.
- **Modify** `Makefile` — `SYNONYMS_SRC`/`SYNONYMS_TEST` vars, `synonyms` target, `.PHONY` + `clean` entries. **Also (discovered during Task 3):** because `tile_memory.c` now calls `syn_*`, every target that links `tile_memory.c` must also link `$(SYNONYMS_SRC)` — add it (and the `synonyms.h` header dep) to the `tfidf`, `tiermem_test`, `fontdecode`, and `graduate` targets, or they fail with `undefined reference to 'syn_observe_tile'`.

---

## Task 1: Synonyms module — in-memory PPMI map

**Files:**
- Create: `include/corpus/synonyms.h`
- Create: `src/corpus/synonyms.c`
- Create: `tests/test_synonyms.c`
- Modify: `Makefile`

- [ ] **Step 1: Write the header**

Create `include/corpus/synonyms.h`:

```c
#ifndef CORPUS_SYNONYMS_H
#define CORPUS_SYNONYMS_H
#include <stddef.h>

/* Corpus-derived synonym/relatedness map via Positive PMI over same-tile
   co-occurrence. Interpretable, grows with the corpus, no dense weights, no
   external data. See docs/superpowers/specs/2026-06-26-synonym-ppmi-retrieval-design.md */

typedef struct Synonyms Synonyms;

Synonyms *syn_new(void);
void      syn_free(Synonyms *s);

/* Accumulate same-tile co-occurrence for one tile's DISTINCT terms. */
void      syn_observe_tile(Synonyms *s, const char *const *terms, int n_distinct);

/* Raw counts -> PPMI -> keep top-k neighbors per term, then freeze.
   df_of(ctx,term) = # tiles containing term (global doc frequency); N = tile count.
   Eligible terms: pair count >= min_cooc AND df in [min_df, df_frac*N]. */
void      syn_finalize(Synonyms *s,
                       unsigned (*df_of)(void *ctx, const char *term), void *ctx,
                       size_t N, int k, int min_cooc, double df_frac, int min_df);

/* Top neighbors of `term` (post-finalize). Writes up to k (term,ppmi) into out_*,
   returns count. out_terms point into the map (valid until syn_free/rebuild). */
int       syn_neighbors(const Synonyms *s, const char *term,
                        const char **out_terms, float *out_ppmi, int k);

int       syn_save(const Synonyms *s, const char *path, size_t stamp); /* text; 0 ok */
int       syn_load(Synonyms *s, const char *path, size_t *stamp_out);  /* 0 ok; *stamp_out=saved stamp */

size_t    syn_term_count(const Synonyms *s);      /* vocab size */
size_t    syn_neighbor_edges(const Synonyms *s);  /* total top-k list entries */

#endif
```

- [ ] **Step 2: Add the Makefile target and variables**

In `Makefile`, after the `TFIDF_TEST := tests/test_tfidf.c` line (near line 128), add:

```make
SYNONYMS_SRC := src/corpus/synonyms.c
SYNONYMS_TEST := tests/test_synonyms.c
```

Add `synonyms` to the `.PHONY` list (the long line near line 130) — insert ` synonyms` before ` clean`.

After the `tfidf:` target block (near line 530), add:

```make
# Lever 3: corpus PPMI synonym map + opt-in query expansion. Unit tests + book bench.
synonyms: $(SYNONYMS_SRC) $(TILEMEM_SRC) $(CORPUS_SRC) $(PDF_SRC) $(SYNONYMS_TEST) include/corpus/synonyms.h include/corpus/tile_memory.h include/corpus/corpus_split.h include/pdf/pdf_extract.h
	$(CC) $(CFLAGS) -Wno-unused-function -o $@ $(SYNONYMS_SRC) $(TILEMEM_SRC) $(CORPUS_SRC) $(PDF_SRC) $(SYNONYMS_TEST) $(LDFLAGS)
	./synonyms
```

In the `clean:` target, add `synonyms` to the `rm -f` binary list (line ~627) and add `syn_ctl syn_rev syn_book` to the `rm -rf` store-dir list (line ~628).

- [ ] **Step 3: Write the failing unit test (PPMI math + twins + exclusion)**

Create `tests/test_synonyms.c`:

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../include/corpus/synonyms.h"

static int g_fail=0, g_checks=0;
#define CHECK(c,m) do{ g_checks++; if(!(c)){ g_fail++; printf("  FAIL: %s (%s:%d)\n",m,__FILE__,__LINE__);} }while(0)

/* in-test document-frequency source */
typedef struct { const char *t; unsigned df; } DF;
static DF g_df[64]; static int g_ndf=0;
static void df_set(const char *t, unsigned df){ g_df[g_ndf].t=t; g_df[g_ndf].df=df; g_ndf++; }
static unsigned df_lookup(void *ctx,const char*term){ (void)ctx;
    for(int i=0;i<g_ndf;i++) if(!strcmp(g_df[i].t,term)) return g_df[i].df; return 0; }

static float nbr_weight(Synonyms *s, const char *term, const char *want){
    const char *nb[8]; float pp[8]; int n=syn_neighbors(s,term,nb,pp,8);
    for(int i=0;i<n;i++) if(!strcmp(nb[i],want)) return pp[i];
    return -1.0f;
}

static void test_ppmi_math(void){
    /* 4 tiles: {a,b,x,h?}, {a,b,x}, {c,d,x}, {c,d,x}. x in all (hub), h once (hapax). */
    g_ndf=0;
    df_set("a",2); df_set("b",2); df_set("c",2); df_set("d",2); df_set("x",4); df_set("h",1);
    Synonyms *s=syn_new();
    const char *t1[]={"a","b","x","h"}; syn_observe_tile(s,t1,4);
    const char *t2[]={"a","b","x"};     syn_observe_tile(s,t2,3);
    const char *t3[]={"c","d","x"};     syn_observe_tile(s,t3,3);
    const char *t4[]={"c","d","x"};     syn_observe_tile(s,t4,3);
    /* N=4, k=5, min_cooc=2, df_frac=0.5 (x df4 > 2 -> excluded), min_df=2 (h df1 -> excluded) */
    syn_finalize(s, df_lookup, NULL, 4, 5, 2, 0.5, 2);

    /* PMI(a,b)=log(c*N/(df_a*df_b))=log(2*4/(2*2))=log(2)=0.6931 */
    float w=nbr_weight(s,"a","b");
    CHECK(w>0.0f,"a has neighbor b");
    CHECK(fabsf(w-0.6931f)<0.01f,"PPMI(a,b)=ln(2) as hand-computed");
    CHECK(nbr_weight(s,"b","a")>0.0f,"twins are mutual (b<->a)");
    CHECK(nbr_weight(s,"a","x")<0.0f,"hub term x excluded by df_frac (not a neighbor)");
    CHECK(nbr_weight(s,"a","h")<0.0f,"hapax term h excluded by min_df (not a neighbor)");
    CHECK(nbr_weight(s,"a","c")<0.0f,"a and c never co-occur -> no edge");
    syn_free(s);
}

int main(void){
    printf("=== test_synonyms ===\n");
    test_ppmi_math();
    printf("\n%d/%d checks passed\n", g_checks-g_fail, g_checks);
    return g_fail?1:0;
}
```

- [ ] **Step 4: Run it to verify it fails (no implementation yet)**

Run: `make synonyms`
Expected: **link/compile error** — `undefined reference to 'syn_new'` (and the other `syn_*` symbols), because `src/corpus/synonyms.c` does not exist yet.

- [ ] **Step 5: Implement `src/corpus/synonyms.c`**

Create `src/corpus/synonyms.c`:

```c
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
        while(nk[h]) h=(h+1)&nm; nk[h]=p->key[i]; ncnt[h]=p->cnt[i]; }
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
                  size_t N, int k, int min_cooc, double df_frac, int min_df){
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
        topk_insert(s,i,s->term[j],(float)pmi,k);
        topk_insert(s,j,s->term[i],(float)pmi,k);
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
```

- [ ] **Step 6: Run the unit test to verify it passes**

Run: `make synonyms`
Expected: compiles clean (no warnings under `-Wall -Wextra -pedantic`), runs `./synonyms`, prints `6/6 checks passed`, exit 0.

- [ ] **Step 7: Durability checkpoint (no git)**

Confirm the binary built and the 6 checks pass. Do **not** run git. Leave Task 5 (full regression) for the end.

---

## Task 2: Persistence round-trip

**Files:**
- Modify: `tests/test_synonyms.c`

(The `syn_save`/`syn_load` implementations already landed in Task 1 Step 5; this task adds their test.)

- [ ] **Step 1: Add the failing round-trip test**

In `tests/test_synonyms.c`, add this function above `main`:

```c
static void test_roundtrip(void){
    g_ndf=0;
    df_set("a",2); df_set("b",2); df_set("c",2); df_set("d",2);
    Synonyms *s=syn_new();
    const char *t1[]={"a","b"}; syn_observe_tile(s,t1,2);
    const char *t2[]={"a","b"}; syn_observe_tile(s,t2,2);
    const char *t3[]={"c","d"}; syn_observe_tile(s,t3,2);
    const char *t4[]={"c","d"}; syn_observe_tile(s,t4,2);
    syn_finalize(s, df_lookup, NULL, 4, 5, 2, 1.0, 1);
    float before=nbr_weight(s,"a","b");
    CHECK(before>0.0f,"pre-save: a->b edge exists");
    CHECK(syn_save(s,"syn_rt.txt",4)==0,"save ok");
    syn_free(s);

    Synonyms *s2=syn_new(); size_t stamp=0;
    CHECK(syn_load(s2,"syn_rt.txt",&stamp)==0,"load ok");
    CHECK(stamp==4,"stamp round-trips");
    float after=nbr_weight(s2,"a","b");
    CHECK(after>0.0f,"post-load: a->b edge survives");
    CHECK(fabsf(after-before)<0.001f,"weight round-trips exactly");
    syn_free(s2);
    remove("syn_rt.txt");
}
```

Add `test_roundtrip();` in `main` after `test_ppmi_math();`.

- [ ] **Step 2: Run to verify it passes**

Run: `make synonyms`
Expected: `./synonyms` prints `12/12 checks passed` (6 from Task 1 + 6 here), exit 0.

- [ ] **Step 3: Durability checkpoint (no git)**

Confirm 12/12. Do not run git.

---

## Task 3: tile_memory integration + query-time expansion

**Files:**
- Modify: `include/corpus/tile_memory.h`
- Modify: `src/corpus/tile_memory.c`
- Modify: `tests/test_synonyms.c`

- [ ] **Step 1: Write the failing integration tests (gap-closer + reversibility)**

In `tests/test_synonyms.c`, add the tile_memory include at the top (after the synonyms include):

```c
#include "../include/corpus/tile_memory.h"
```

Add these helpers + tests above `main`:

```c
static void reset_syn_store(const char *dir){ char p[300];
    snprintf(p,sizeof(p),"%s/hot.bin",dir);      remove(p);
    snprintf(p,sizeof(p),"%s/warm.bin",dir);     remove(p);
    snprintf(p,sizeof(p),"%s/idf.bin",dir);      remove(p);
    snprintf(p,sizeof(p),"%s/synonyms.bin",dir); remove(p); }

static int hits_contain(TileHit *h, int n, const char *needle){
    for(int i=0;i<n;i++) if(h[i].value && strstr(h[i].value,needle)) return 1; return 0;
}

static void test_gap_closer(void){
    reset_syn_store("syn_ctl");
    TileMemory *m=tilemem_open("syn_ctl",256,1000,0.99);
    /* alpha co-occurs with xray; beta co-occurs with yankee. The answer tile shares
       NO words with the query "alpha beta" but contains xray + yankee. */
    tilemem_ingest(m,"alpha xray one","alpha xray one","","c");
    tilemem_ingest(m,"alpha xray two","alpha xray two","","c");
    tilemem_ingest(m,"beta yankee three","beta yankee three","","c");
    tilemem_ingest(m,"beta yankee four","beta yankee four","","c");
    tilemem_ingest(m,"xray yankee zzanswer","xray yankee zzanswer","","c");
    tilemem_ingest(m,"delta echo six","delta echo six","","c");
    tilemem_ingest(m,"foxtrot golf seven","foxtrot golf seven","","c");

    /* expansion OFF (alpha=0): the answer tile is missed (no shared words). */
    TileHit h[10]; int n=tilemem_search(m,"alpha beta",10,h,10);
    CHECK(n>0,"baseline returns the alpha/beta tiles");
    CHECK(!hits_contain(h,n,"zzanswer"),"baseline MISSES the synonym-only answer tile");

    /* expansion ON: alpha->xray, beta->yankee surfaces the answer tile. */
    tilemem_set_expansion(m, 0.35, 5, 2, 1, 0.9, 64);
    n=tilemem_search(m,"alpha beta",10,h,10);
    CHECK(hits_contain(h,n,"zzanswer"),"expansion FINDS the zero-overlap answer tile (gap closed)");

    /* normal lexical retrieval still works with expansion on. */
    n=tilemem_search(m,"delta",10,h,10);
    CHECK(n>0 && hits_contain(h,n,"delta echo"),"expansion-on still returns direct lexical hit");
    tilemem_close(m);
}

static void test_alpha0_reversible(void){
    reset_syn_store("syn_rev");
    TileMemory *m=tilemem_open("syn_rev",256,1000,0.99);
    tilemem_ingest(m,"alpha xray one","alpha xray one","","c");
    tilemem_ingest(m,"alpha xray two","alpha xray two","","c");
    tilemem_ingest(m,"xray yankee zzanswer","xray yankee zzanswer","","c");
    /* explicitly enable then disable: alpha=0 must behave exactly like never-enabled. */
    tilemem_set_expansion(m, 0.0, 5, 2, 1, 0.9, 64);
    TileHit h[10]; int n=tilemem_search(m,"alpha beta",10,h,10);
    CHECK(!hits_contain(h,n,"zzanswer"),"alpha=0 reproduces baseline (no expansion)");
    tilemem_close(m);
}
```

Add `test_gap_closer();` and `test_alpha0_reversible();` in `main` after `test_roundtrip();`.

- [ ] **Step 2: Run to verify it fails (no integration yet)**

Run: `make synonyms`
Expected: **compile error** — `tilemem_set_expansion` / implicit declaration, because the function and fields don't exist yet.

- [ ] **Step 3: Extend the tile_memory header**

In `include/corpus/tile_memory.h`, after the existing `#include <stddef.h>` (line 3), add:

```c
#include "synonyms.h"
```

Before the final `#endif`, add:

```c
/* ---- Lever 3: opt-in query-time synonym expansion (PPMI co-occurrence) ----
   alpha=0 (default) disables expansion => search is byte-identical to TF-IDF.
   Pass 0/negative for any tuning knob to keep its current default. */
void tilemem_set_expansion(TileMemory *m, double alpha, int k, int min_cooc,
                           int min_df, double df_frac, int max_expand);
/* Rebuild the PPMI synonym map over the full HOT+WARM corpus and persist it. */
void tilemem_build_synonyms(TileMemory *m);
/* Read-only view of the current synonym map (NULL/empty before a build). */
const Synonyms *tilemem_synonyms(const TileMemory *m);
```

- [ ] **Step 4: Add synonym fields + include to tile_memory.c**

In `src/corpus/tile_memory.c`, after `#include "../../include/corpus/tile_memory.h"` (line 4), add:

```c
#include "../../include/corpus/synonyms.h"
```

Replace the `struct TileMemory { ... };` block (lines 21-28) with:

```c
struct TileMemory {
    int dim, hot_cap; double tau;
    char store_dir[256];
    Tile *hot; int n_hot, cap_hot;
    size_t warm_count;
    TermDF tdf; size_t doc_count;     /* term->df dictionary + tile count (TF-IDF) */
    char **res; int n_res, cap_res;   /* search-result value arena (freed each search) */
    Synonyms *syn; int syn_dirty;     /* Lever 3: PPMI synonym map + stale flag */
    double syn_alpha; int syn_k, syn_min_cooc, syn_min_df, syn_max_expand;
    double syn_df_frac, syn_pmi_scale;
};
```

- [ ] **Step 5: Add helpers (syn_path, tile_distinct_terms, tm_df_of)**

In `src/corpus/tile_memory.c`, after the `idf_path` helper (line 99), add:

```c
static void syn_path (const TileMemory *m, char *out, size_t cap){ snprintf(out,cap,"%s/synonyms.bin",m->store_dir); }
```

After `tokenize_terms` (ends line 82), add:

```c
/* distinct (deduped) terms of a tile key into `out` (pointers into `store`). Returns count. */
static int tile_distinct_terms(const char *key, char store[][32], const char **out, int cap){
    int nt=tokenize_terms(key,store,cap), nd=0;
    for(int a=0;a<nt;a++){ int dup=0; for(int b=0;b<nd;b++) if(strcmp(store[a],out[b])==0){ dup=1; break; }
        if(!dup) out[nd++]=store[a]; }
    return nd;
}
/* document-frequency callback for syn_finalize (reads the term->df dictionary). */
static unsigned tm_df_of(void *ctx, const char *term){
    TileMemory *m=(TileMemory*)ctx; unsigned *d=tdf_slot(&m->tdf,(char*)term,0); return d? *d:0;
}
```

- [ ] **Step 6: Build the map over the full corpus**

In `src/corpus/tile_memory.c`, add `tilemem_build_synonyms` immediately after `tilemem_ingest` (after line 198):

```c
void tilemem_build_synonyms(TileMemory *m){
    if(m->syn) syn_free(m->syn);
    m->syn=syn_new();
    char tt[256][32]; const char *dt[256];
    for(int i=0;i<m->n_hot;i++){ int nd=tile_distinct_terms(m->hot[i].key,tt,dt,256);
        syn_observe_tile(m->syn,dt,nd); }
    char wp[300]; warm_path(m,wp,sizeof(wp)); FILE *f=fopen(wp,"rb");
    if(f){ Tile t; while(tile_read(f,&t,m->dim)==0){
        int nd=tile_distinct_terms(t.key,tt,dt,256); syn_observe_tile(m->syn,dt,nd);
        tile_free(&t); } fclose(f); }
    syn_finalize(m->syn, tm_df_of, m, m->doc_count,
                 m->syn_k, m->syn_min_cooc, m->syn_df_frac, m->syn_min_df);
    char sp[300]; syn_path(m,sp,sizeof(sp)); syn_save(m->syn,sp,m->doc_count);
    m->syn_dirty=0;
}
const Synonyms *tilemem_synonyms(const TileMemory *m){ return m->syn; }
```

- [ ] **Step 7: Initialize fields + load map (open/load), free (close), mark dirty (ingest)**

Replace `tilemem_open` (lines 154-162) with:

```c
TileMemory *tilemem_open(const char *store_dir, int dim, int hot_cap, double dedup_tau){
    if(dim<=0||hot_cap<=0) return NULL;
    TileMemory *m=(TileMemory*)calloc(1,sizeof(*m));
    m->dim=dim; m->hot_cap=hot_cap; m->tau=dedup_tau;
    snprintf(m->store_dir,sizeof(m->store_dir),"%s",store_dir);
    tdf_init(&m->tdf);
    m->syn=syn_new();
    m->syn_alpha=0.0; m->syn_k=5; m->syn_min_cooc=2; m->syn_min_df=2;
    m->syn_df_frac=0.5; m->syn_max_expand=64; m->syn_pmi_scale=5.0; m->syn_dirty=0;
    tilemem_load(m);
    return m;
}
```

At the end of `tilemem_load` (after the `idf.bin` block closes, line 143), add:

```c
    char sp[300]; syn_path(m,sp,sizeof(sp)); size_t stamp=0;
    if(syn_load(m->syn,sp,&stamp)==0){ if(stamp!=m->doc_count) m->syn_dirty=1; }
    else if(m->doc_count>0) m->syn_dirty=1;   /* tiles exist but no/stale map */
```

In `tilemem_close` (lines 163-172), add before `free(m);`:

```c
    syn_free(m->syn);
```

In `tilemem_ingest`, before `return 1;` (line 197), add:

```c
    m->syn_dirty=1;
```

- [ ] **Step 8: Add the expansion block to tilemem_search**

Replace the whole `tilemem_search` function (lines 217-244) with:

```c
int tilemem_search(TileMemory *m, const char *query, int topK, TileHit *out, int cap){
    for(int i=0;i<m->n_res;i++) free(m->res[i]);
    m->n_res=0;
    /* distinct query terms (by index into qt) */
    char qt[64][32]; int nq=tokenize_terms(query,qt,64);
    int qidx[64]; int nqd=0;
    for(int a=0;a<nq && nqd<64;a++){ int dup=0; for(int b=0;b<nqd;b++) if(strcmp(qt[a],qt[qidx[b]])==0){ dup=1; break; }
        if(!dup) qidx[nqd++]=a; }
    /* unified weighted query terms: hard (idf) + optional soft (PPMI expansion) */
    const char *wq[128]; double ww[128]; int nw=0;
    for(int q=0;q<nqd;q++){ unsigned df=0; unsigned *d=tdf_slot(&m->tdf,qt[qidx[q]],0); if(d) df=*d;
        wq[nw]=qt[qidx[q]]; ww[nw]=log(((double)m->doc_count+1.0)/((double)df+1.0))+1.0; nw++; }
    if(m->syn_alpha>0.0 && m->syn){
        if(m->syn_dirty) tilemem_build_synonyms(m);
        int base=nw;
        for(int q=0;q<nqd && nw<128;q++){
            const char *nb[16]; float pp[16]; int kk=m->syn_k<16? m->syn_k:16;
            int got=syn_neighbors(m->syn,qt[qidx[q]],nb,pp,kk);
            for(int z=0;z<got && nw<128;z++){
                if(nw-base>=m->syn_max_expand) break;
                int dup=0; for(int w=0;w<nw;w++) if(strcmp(wq[w],nb[z])==0){ dup=1; break; }
                if(dup) continue;
                unsigned df=0; unsigned *d=tdf_slot(&m->tdf,(char*)nb[z],0); if(d) df=*d;
                double idf=log(((double)m->doc_count+1.0)/((double)df+1.0))+1.0;
                double scl=(double)pp[z]/m->syn_pmi_scale; if(scl>1.0) scl=1.0;
                wq[nw]=nb[z]; ww[nw]=idf*scl*m->syn_alpha; nw++;
            }
        }
    }
    int lim=topK<cap?topK:cap; int n=0;
    /* TF-IDF overlap over hard+soft terms, length-normalized. */
    for(int i=0;i<m->n_hot;i++){
        char tt[256][32]; int nt=tokenize_terms(m->hot[i].key,tt,256);
        double s=0; char hit[128]={0};
        for(int k=0;k<nt;k++) for(int w=0;w<nw;w++) if(!hit[w] && strcmp(tt[k],wq[w])==0){ s+=ww[w]; hit[w]=1; break; }
        float sc=(float)(s/sqrt((double)nt+1.0));
        if(sc>0){ topk_insert(m,out,&n,lim,sc,m->hot[i].id,m->hot[i].label,m->hot[i].source,m->hot[i].value); m->hot[i].heat++; }
    }
    char wp[300]; warm_path(m,wp,sizeof(wp)); FILE *f=fopen(wp,"rb");
    if(f){ Tile t; while(tile_read(f,&t,m->dim)==0){
        char tt[256][32]; int nt=tokenize_terms(t.key,tt,256);
        double s=0; char hit[128]={0};
        for(int k=0;k<nt;k++) for(int w=0;w<nw;w++) if(!hit[w] && strcmp(tt[k],wq[w])==0){ s+=ww[w]; hit[w]=1; break; }
        float sc=(float)(s/sqrt((double)nt+1.0));
        topk_insert(m,out,&n,lim,sc,t.id,t.label,t.source,t.value);
        tile_free(&t); } fclose(f); }
    return n;
}
```

(With `alpha=0`, `nw==nqd`, `wq`/`ww` equal the old `qt[qidx]`/`qidf` — scores and ordering are byte-identical to the previous implementation.)

- [ ] **Step 9: Add the setter**

In `src/corpus/tile_memory.c`, after `tilemem_search` add:

```c
void tilemem_set_expansion(TileMemory *m, double alpha, int k, int min_cooc,
                           int min_df, double df_frac, int max_expand){
    m->syn_alpha=alpha;
    if(k>0)          m->syn_k=k;
    if(min_cooc>0)   m->syn_min_cooc=min_cooc;
    if(min_df>0)     m->syn_min_df=min_df;
    if(df_frac>0.0)  m->syn_df_frac=df_frac;
    if(max_expand>0) m->syn_max_expand=max_expand;
    m->syn_dirty=1;   /* params changed -> rebuild on next expanded search */
}
```

- [ ] **Step 10: Run the integration tests to verify they pass**

Run: `make synonyms`
Expected: compiles clean, `./synonyms` prints `18/18 checks passed` (12 + 6 here), exit 0.

- [ ] **Step 11: Durability checkpoint (no git)**

Confirm 18/18 and warning-clean build. Do not run git.

---

## Task 4: Real-book benchmark (required deliverable)

**Files:**
- Modify: `tests/test_synonyms.c`

- [ ] **Step 1: Add the benchmark (auto-skips if the book is absent)**

In `tests/test_synonyms.c`, add the corpus/pdf includes after the tile_memory include:

```c
#include "../include/corpus/corpus_split.h"
#include "../include/pdf/pdf_extract.h"
#include <time.h>
```

Add this function above `main` (the file/extract pattern mirrors `tests/test_tfidf.c`):

```c
static unsigned char *readfile(const char *p,size_t*len){ FILE*f=fopen(p,"rb"); if(!f)return NULL;
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET); if(sz<=0){fclose(f);return NULL;}
    unsigned char*b=malloc((size_t)sz); *len=fread(b,1,(size_t)sz,f); fclose(f); return b; }

static void test_book_bench(void){
    const char *path="C:/Users/Mokason/Downloads/aivalueplaybook.pdf";
    FILE *pr=fopen(path,"rb"); if(!pr){ printf("[book] not found - skipping benchmark\n"); return; } fclose(pr);
    size_t len=0; unsigned char *buf=readfile(path,&len); if(!buf) return;
    char *text=malloc(len*4+16); size_t tl=0;
    if(pdf_extract_text(buf,len,text,len*4+16,&tl)!=PDF_OK){ free(text); free(buf); return; }
    StrList s; strlist_init(&s); corpus_split(text,&s);
    reset_syn_store("syn_book");
    TileMemory *m=tilemem_open("syn_book",256,9000,0.6);
    for(size_t i=0;i<s.count;i++) if(corpus_quality_keep(s.lines[i]))
        tilemem_ingest(m,s.lines[i],s.lines[i],"","book");
    tilemem_set_expansion(m, 0.35, 5, 2, 2, 0.5, 64);

    clock_t t0=clock(); tilemem_build_synonyms(m);
    double build=1000.0*(double)(clock()-t0)/CLOCKS_PER_SEC;
    const Synonyms *syn=tilemem_synonyms(m);
    size_t terms=syn_term_count(syn), edges=syn_neighbor_edges(syn);
    printf("\n[bench] map build %.0f ms over %zu tiles ; vocab=%zu terms ; edges=%zu\n",
           build, tilemem_total(m), terms, edges);

    /* show learned neighbors for a few seed terms (honest: may be noisy/topical) */
    const char *seeds[]={"story","data","mile","customer"};
    for(int q=0;q<4;q++){ const char *nb[5]; float pp[5]; int got=syn_neighbors(syn,seeds[q],nb,pp,5);
        printf("[neighbors] %-9s ->", seeds[q]);
        for(int i=0;i<got;i++) printf(" %s(%.2f)", nb[i], pp[i]);
        printf("%s\n", got? "":" (none)"); }

    /* per-query overhead: baseline (alpha=0) vs expanded (alpha=0.35) */
    TileHit h[5];
    tilemem_set_expansion(m, 0.0, 5, 2, 2, 0.5, 64);
    t0=clock(); int nb0=tilemem_search(m,"storytelling narrative",5,h,5);
    double q0=1000.0*(double)(clock()-t0)/CLOCKS_PER_SEC;
    tilemem_set_expansion(m, 0.35, 5, 2, 2, 0.5, 64);
    t0=clock(); int nb1=tilemem_search(m,"storytelling narrative",5,h,5);
    double q1=1000.0*(double)(clock()-t0)/CLOCKS_PER_SEC;
    printf("[bench] query baseline(alpha=0) %.3f ms (%d hits) ; expanded(alpha=0.35) %.3f ms (%d hits)\n",
           q0, nb0, q1, nb1);

    CHECK(terms>0,"book produced a non-empty synonym vocabulary");
    CHECK(edges>0,"book produced at least one PPMI neighbor edge");
    tilemem_close(m);
    free(text); free(buf); strlist_free(&s);
}
```

Add `test_book_bench();` in `main` after `test_alpha0_reversible();`.

- [ ] **Step 2: Run and capture the benchmark output**

Run: `make synonyms`
Expected: all checks pass. **Note:** the pre-Task-4 base is **17** checks (the plan's earlier prose miscounted as 18 — `test_gap_closer` has 4 + `test_alpha0_reversible` has 1 = 5 new in Task 3, so 12+5=17). This task adds 2 (`terms>0`, `edges>0`), so expect **`19/19`** if the book is present, or **`17/17`** + the `[book] not found - skipping` line if absent. Capture the `[bench]` and `[neighbors]` lines for the final report — these are the required benchmark deliverable.

- [ ] **Step 3: Durability checkpoint (no git)**

Confirm checks pass and benchmark lines printed. Do not run git.

---

## Task 5: Full regression sweep

**Files:** none (verification only)

- [ ] **Step 1: Run every retrieval/corpus suite and confirm green**

Run each and confirm the stated result:

```
make tfidf          # expect: ./tfidf  -> all checks pass (TF-IDF unchanged)
make tiermem_test   # expect: ./tiermem_test 18/18
make graduate       # expect: ./graduate 9/9
make fontdecode     # expect: ./fontdecode 20/20
make pdftest        # expect: ./pdftest 19/19
make synonyms       # expect: ./synonyms 19/19 (or 17/17 + book-skip)
```

Expected: every suite green. `make tfidf` passing is the key proof that the `alpha=0` default left TF-IDF byte-identical. If any regress, **stop and debug** (use superpowers:systematic-debugging) before claiming completion.

- [ ] **Step 2: Confirm warning-clean**

The `synonyms` target compiles under `-Wall -Wextra -pedantic` (the shared `CFLAGS`). Confirm no warnings from `synonyms.c` or the `tile_memory.c` edits in the build log (the `-Wno-unused-function` flag only suppresses unused-static warnings, as for the sibling targets).

- [ ] **Step 3: Report results**

Summarize: the gap-closer proof (zero-overlap query now retrieves the answer), the `alpha=0` reversibility/regression result, and the captured book benchmark numbers (build ms, vocab/edge counts, per-query baseline-vs-expanded ms, sample learned neighbors).

---

## Self-Review

**Spec coverage:**
- §1.1 corpus-grown PPMI map → Task 1 (`syn_*`). ✓
- §1.2 closes the gap (headline) → Task 3 `test_gap_closer`. ✓
- §1.3 reversible/opt-in default-off → Task 3 `test_alpha0_reversible` + Task 5 `make tfidf`. ✓
- §1.4 zero core edits, suites green → Task 5. ✓
- §3 PPMI math + df reuse + filters → Task 1 `syn_finalize`, `tm_df_of`; `test_ppmi_math` asserts the value + both filters. ✓
- §4 module boundary (tile_memory → synonyms) → Task 1 + Task 3 include direction. ✓
- §5 build over full HOT+WARM, stamp-based staleness → Task 3 `tilemem_build_synonyms` + `tilemem_load` stamp check. ✓
- §6 query-time soft expansion, capped → Task 3 Step 8. ✓
- §7 text sidecar persistence → Task 1 `syn_save/load` + Task 2. ✓
- §9 tests incl. benchmark → Tasks 1–4. ✓
- §10 knobs + defaults (alpha=0) → Task 3 Steps 7,9. ✓

**Placeholder scan:** No TBD/TODO; every code step shows complete code. PMI_SCALE is a concrete default (`5.0`). ✓

**Type consistency:** `syn_save(...,size_t stamp)` / `syn_load(...,size_t *stamp_out)` match between header, impl, and callers. `tilemem_set_expansion(double,int,int,int,double,int)` identical in header, impl, and all test call sites. `tile_distinct_terms(key,store[][32],out,cap)` and `tm_df_of(void*,const char*)` match `syn_finalize`'s `df_of` signature. `wq[128]/ww[128]/hit[128]` sizes consistent across both scoring loops. ✓

---

## Project notes

- **No git on CNET** — never run git commands; verify via filesystem and test runs. "Commit" is replaced by re-running tests.
- Zero core/router/contract edits: only `src/corpus/synonyms.c` (new), `src/corpus/tile_memory.c` + its header (thin), `tests/test_synonyms.c` (new), `Makefile`.
