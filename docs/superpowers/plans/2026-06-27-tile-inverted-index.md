# Unified Inverted Index for Tile Memory (Lever 5) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `tilemem_search` consult a `term → tids` inverted index and score only candidate tiles (HOT in array order, WARM by byte offset), producing byte-identical results to today's linear scan while skipping every non-matching tile.

**Architecture:** Grow `TermDF` with per-term posting lists; add a per-tile integer `tid`, a tid-indexed location array (HOT slot / WARM offset / liveness), and a tid-indexed epoch `seen[]`. Build the index in the existing load pass; keep it consistent on ingest/spill/evict. Keep today's algorithm as `tilemem_search_linear` (the test oracle); make `tilemem_search` the index version. Identity vs the oracle is the regression gate.

**Tech Stack:** C11, MinGW gcc, `make` (Windows). No git on CNET (verify via filesystem + tests; never run git). Spec: `docs/superpowers/specs/2026-06-27-tile-inverted-index-design.md`.

**Note on "Commit"/"checkpoint":** No git in this repo. A passing build/test is the checkpoint. Do not run `git`.

---

## File Structure

- **Modify** `include/corpus/tile_memory.h` — add `unsigned tid;` to `Tile`; declare `tilemem_search_linear`.
- **Modify** `src/corpus/tile_memory.c` — postings in `TermDF`; `Loc` array + `seen[]` + `next_tid` in `TileMemory`; offset-returning `warm_append`; index population in `tilemem_load`/`tilemem_ingest`/`spill_coldest`/`tilemem_evict_containing`; shared `build_weighted_query`/`score_tile` helpers; `tilemem_search_linear` (oracle) + index-based `tilemem_search`; frees in `tilemem_close`.
- **Create** `tests/test_tile_index.c` + a `make tileindex` target — identity property test + forced-WARM benchmark.
- **Modify** `Makefile` — `TILEINDEX_TEST` var, `tileindex` target, `.PHONY` + `clean` entries.

---

## Task 1: Oracle + shared helpers (no behavior change)

**Files:**
- Modify: `include/corpus/tile_memory.h`
- Modify: `src/corpus/tile_memory.c`
- Create: `tests/test_tile_index.c`
- Modify: `Makefile`

- [ ] **Step 1: Declare the oracle in the header**

In `include/corpus/tile_memory.h`, after the `tilemem_search` prototype, add:

```c
/* Reference linear scan (scores every HOT tile + streams all WARM). Identical results to
   tilemem_search; kept as the inverted-index test oracle and for benchmarking. */
int    tilemem_search_linear(TileMemory *m, const char *query, int topK, TileHit *out, int cap);
```

- [ ] **Step 2: Add the shared helpers and split search into oracle + passthrough**

In `src/corpus/tile_memory.c`, add these two helpers immediately **above** the current `tilemem_search` function:

```c
/* Build the weighted query-term set (hard idf terms + optional PPMI soft terms).
   qt[] (caller-owned) backs the hard-term strings; soft terms point into the synonym map.
   Returns the number of weighted terms written to wq/ww (<= cap). Shared by both searchers. */
static int build_weighted_query(TileMemory *m, const char *query, char qt[][32],
                                const char **wq, double *ww, int cap){
    int nq=tokenize_terms(query,qt,64);
    int qidx[64]; int nqd=0;
    for(int a=0;a<nq && nqd<64;a++){ int dup=0; for(int b=0;b<nqd;b++) if(strcmp(qt[a],qt[qidx[b]])==0){ dup=1; break; }
        if(!dup) qidx[nqd++]=a; }
    int nw=0;
    for(int q=0;q<nqd && nw<cap;q++){ unsigned df=0; unsigned *d=tdf_slot(&m->tdf,qt[qidx[q]],0); if(d) df=*d;
        wq[nw]=qt[qidx[q]]; ww[nw]=log(((double)m->doc_count+1.0)/((double)df+1.0))+1.0; nw++; }
    if(m->syn_alpha>0.0 && m->syn){
        if(m->syn_dirty) tilemem_build_synonyms(m);
        int base=nw;
        for(int q=0;q<nqd && nw<cap;q++){
            const char *nb[16]; float pp[16]; int kk=m->syn_k<16? m->syn_k:16;
            int got=syn_neighbors(m->syn,qt[qidx[q]],nb,pp,kk);
            for(int z=0;z<got && nw<cap;z++){
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
    return nw;
}
/* TF-IDF overlap score for one tile key over the weighted query terms, length-normalized.
   Pure (no side effects) so HOT and WARM, oracle and index, all score identically. */
static float score_tile(const char *key, const char **wq, const double *ww, int nw){
    char tt[256][32]; int nt=tokenize_terms(key,tt,256);
    double s=0; char hit[128]={0};
    for(int k=0;k<nt;k++) for(int w=0;w<nw;w++) if(!hit[w] && strcmp(tt[k],wq[w])==0){ s+=ww[w]; hit[w]=1; break; }
    return (float)(s/sqrt((double)nt+1.0));
}
```

Now **replace the entire current `tilemem_search` function** with the oracle plus a passthrough:

```c
int tilemem_search_linear(TileMemory *m, const char *query, int topK, TileHit *out, int cap){
    for(int i=0;i<m->n_res;i++) free(m->res[i]);
    m->n_res=0;
    char qt[64][32]; const char *wq[128]; double ww[128];
    int nw=build_weighted_query(m,query,qt,wq,ww,128);
    int lim=topK<cap?topK:cap; int n=0;
    for(int i=0;i<m->n_hot;i++){
        float sc=score_tile(m->hot[i].key,wq,ww,nw);
        if(sc>0){ topk_insert(m,out,&n,lim,sc,m->hot[i].id,m->hot[i].label,m->hot[i].source,m->hot[i].value); m->hot[i].heat++; }
    }
    char wp[300]; warm_path(m,wp,sizeof(wp)); FILE *f=fopen(wp,"rb");
    if(f){ Tile t; while(tile_read(f,&t,m->dim)==0){
        float sc=score_tile(t.key,wq,ww,nw);
        topk_insert(m,out,&n,lim,sc,t.id,t.label,t.source,t.value);
        tile_free(&t); } fclose(f); }
    return n;
}
int tilemem_search(TileMemory *m, const char *query, int topK, TileHit *out, int cap){
    return tilemem_search_linear(m,query,topK,out,cap);   /* index version lands in Task 3 */
}
```

This is behavior-identical to before (the scoring/expansion logic is the same, now factored).

- [ ] **Step 3: Create the identity test + Makefile target**

Create `tests/test_tile_index.c`:

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../include/corpus/tile_memory.h"

static int g_fail=0, g_checks=0;
#define CHECK(c,m) do{ g_checks++; if(!(c)){ g_fail++; printf("  FAIL: %s (%s:%d)\n",m,__FILE__,__LINE__);} }while(0)
static void reset_store(const char *dir){ char p[300];
    snprintf(p,sizeof(p),"%s/hot.bin",dir); remove(p);
    snprintf(p,sizeof(p),"%s/warm.bin",dir); remove(p);
    snprintf(p,sizeof(p),"%s/idf.bin",dir); remove(p);
    snprintf(p,sizeof(p),"%s/synonyms.bin",dir); remove(p); }

/* assert tilemem_search == tilemem_search_linear for one query (ids, scores, order) */
static int same_results(TileMemory *m, const char *q){
    TileHit a[16], b[16];
    int na=tilemem_search(m,q,16,a,16);
    int nb=tilemem_search_linear(m,q,16,b,16);
    if(na!=nb) return 0;
    for(int i=0;i<na;i++){
        if(a[i].score!=b[i].score) return 0;
        if(strcmp(a[i].id,b[i].id)!=0) return 0;
        if(strcmp(a[i].value?a[i].value:"",b[i].value?b[i].value:"")!=0) return 0;
    }
    return 1;
}

static void test_identity_small(void){
    reset_store("tix_small");
    TileMemory *m=tilemem_open("tix_small",256,1000,0.99);
    const char *docs[]={"alpha beta gamma","beta delta","gamma epsilon zeta",
                        "alpha epsilon","delta zeta eta","theta iota"};
    for(int i=0;i<6;i++) tilemem_ingest(m,docs[i],docs[i],"","c");
    const char *qs[]={"alpha","beta gamma","epsilon zeta","nomatch","alpha beta delta eta"};
    for(int i=0;i<5;i++) CHECK(same_results(m,qs[i]),"index == linear (small, all HOT)");
    tilemem_close(m);
}

int main(void){
    printf("=== test_tile_index ===\n");
    test_identity_small();
    printf("\n%d/%d checks passed\n", g_checks-g_fail, g_checks);
    return g_fail?1:0;
}
```

In `Makefile`: after the `SYNONYMS_TEST` line add `TILEINDEX_TEST := tests/test_tile_index.c`. Add `tileindex` to `.PHONY`. After the `synonyms:` target add:

```make
# Lever 5: unified inverted index over tiles. Identity (index==linear) + forced-WARM bench.
tileindex: $(TILEMEM_SRC) $(SYNONYMS_SRC) $(CORPUS_SRC) $(PDF_SRC) $(TILEINDEX_TEST) include/corpus/tile_memory.h include/corpus/synonyms.h include/corpus/corpus_split.h include/pdf/pdf_extract.h
	$(CC) $(CFLAGS) -Wno-unused-function -o $@ $(TILEMEM_SRC) $(SYNONYMS_SRC) $(CORPUS_SRC) $(PDF_SRC) $(TILEINDEX_TEST) $(LDFLAGS)
	./tileindex
```

Add `tileindex` to the `clean` `rm -f` list and `tix_small tix_warm tix_book` to the `rm -rf` list.

- [ ] **Step 4: Run — verify identical + warning-clean**

Run: `make tileindex`
Expected: `5/5 checks passed` (search just calls linear, so trivially identical), warning-clean.

- [ ] **Step 5: Confirm no regressions (helpers refactor)**

Run: `make synonyms` (expect 25/25), `make tiermem_test` (18/18), `make tfidf` (3/3).
Expected: all green — the refactor into `build_weighted_query`/`score_tile` preserved behavior.

---

## Task 2: Index data structures + build + mutation (search still passthrough)

**Files:**
- Modify: `include/corpus/tile_memory.h`
- Modify: `src/corpus/tile_memory.c`

- [ ] **Step 1: Add `tid` to the Tile struct**

In `include/corpus/tile_memory.h`, add to the `Tile` struct (after `int count;`):

```c
    unsigned tid;        /* inverted-index id (ephemeral; not persisted) */
```

- [ ] **Step 2: Grow `TermDF` with posting lists**

In `src/corpus/tile_memory.c`, replace the `TermDF` typedef:

```c
typedef struct { char **key; unsigned *df; size_t cap, mask, n; } TermDF;
```

with:

```c
typedef struct { char **key; unsigned *df; unsigned **post; int *postn, *postcap; size_t cap, mask, n; } TermDF;
```

Replace `tdf_init`, `tdf_free`, `tdf_grow`, `tdf_slot` with this set (factoring a shared `tdf_bucket`, adding `tdf_post`/`tdf_postings`):

```c
static void tdf_init(TermDF *t){ memset(t,0,sizeof(*t)); }
static void tdf_free(TermDF *t){ for(size_t i=0;i<t->cap;i++){ free(t->key[i]); free(t->post[i]); }
    free(t->key); free(t->df); free(t->post); free(t->postn); free(t->postcap); tdf_init(t); }
static void tdf_grow(TermDF *t){
    size_t nc=t->cap?t->cap*2:1024, nm=nc-1;
    char **nk=(char**)calloc(nc,sizeof(char*)); unsigned *nd=(unsigned*)calloc(nc,sizeof(unsigned));
    unsigned **np=(unsigned**)calloc(nc,sizeof(unsigned*));
    int *npn=(int*)calloc(nc,sizeof(int)); int *npc=(int*)calloc(nc,sizeof(int));
    for(size_t i=0;i<t->cap;i++) if(t->key[i]){ size_t h=tdf_hash(t->key[i])&nm; while(nk[h]) h=(h+1)&nm;
        nk[h]=t->key[i]; nd[h]=t->df[i]; np[h]=t->post[i]; npn[h]=t->postn[i]; npc[h]=t->postcap[i]; }
    free(t->key); free(t->df); free(t->post); free(t->postn); free(t->postcap);
    t->key=nk; t->df=nd; t->post=np; t->postn=npn; t->postcap=npc; t->cap=nc; t->mask=nm;
}
static int tdf_bucket(TermDF *t, const char *s, int add){
    if(t->cap==0){ if(!add) return -1; tdf_grow(t); } else if(add && t->n*10>=t->cap*7) tdf_grow(t);
    size_t h=tdf_hash(s)&t->mask;
    while(t->key[h]){ if(strcmp(t->key[h],s)==0) return (int)h; h=(h+1)&t->mask; }
    if(!add) return -1;
    t->key[h]=strdup(s); t->df[h]=0; t->post[h]=NULL; t->postn[h]=0; t->postcap[h]=0; t->n++; return (int)h;
}
static unsigned *tdf_slot(TermDF *t, const char *s, int add){ int h=tdf_bucket(t,s,add); return h<0?NULL:&t->df[h]; }
static void tdf_post(TermDF *t, const char *s, unsigned tid){
    int h=tdf_bucket(t,s,1); if(h<0) return;
    if(t->postn[h]==t->postcap[h]){ t->postcap[h]=t->postcap[h]?t->postcap[h]*2:4;
        t->post[h]=(unsigned*)realloc(t->post[h],(size_t)t->postcap[h]*sizeof(unsigned)); }
    t->post[h][t->postn[h]++]=tid;
}
static unsigned *tdf_postings(TermDF *t, const char *s, int *n){ int h=tdf_bucket(t,s,0);
    if(h<0){ *n=0; return NULL; } *n=t->postn[h]; return t->post[h]; }
```

(`tdf_hash` already exists above this block; keep it.)

- [ ] **Step 3: Add the location array + seen epoch + tid counter to TileMemory**

In `src/corpus/tile_memory.c`, add this typedef just above `struct TileMemory`:

```c
typedef struct { size_t where; int tier; char live; } Loc;  /* tier 0=HOT(where=hot idx) 1=WARM(where=offset) */
```

Add these fields to `struct TileMemory` (after the synonym fields):

```c
    Loc *loc; size_t loc_cap; unsigned next_tid;   /* tid -> location (presence/live = liveness) */
    unsigned *seen; size_t seen_cap, seen_epoch;   /* per-search candidate marker */
```

Add these helpers after the `Loc` typedef / before `tilemem_open` (anywhere among the statics):

```c
static void loc_ensure(TileMemory *m, unsigned tid){
    if(tid<m->loc_cap) return;
    size_t nc=m->loc_cap?m->loc_cap:256; while(tid>=nc) nc*=2;
    m->loc=(Loc*)realloc(m->loc,nc*sizeof(Loc));
    for(size_t i=m->loc_cap;i<nc;i++){ m->loc[i].where=0; m->loc[i].tier=0; m->loc[i].live=0; }
    m->loc_cap=nc;
}
static void loc_set(TileMemory *m, unsigned tid, int tier, size_t where){
    loc_ensure(m,tid); m->loc[tid].tier=tier; m->loc[tid].where=where; m->loc[tid].live=1; }
static void loc_kill(TileMemory *m, unsigned tid){ if(tid<m->loc_cap) m->loc[tid].live=0; }
static int  loc_live(TileMemory *m, unsigned tid){ return tid<m->loc_cap && m->loc[tid].live; }
static void seen_ensure(TileMemory *m){
    if(m->next_tid<=m->seen_cap) return;
    size_t nc=m->seen_cap?m->seen_cap:256; while(m->next_tid>nc) nc*=2;
    m->seen=(unsigned*)realloc(m->seen,nc*sizeof(unsigned));
    for(size_t i=m->seen_cap;i<nc;i++) m->seen[i]=0;
    m->seen_cap=nc;
}
static int cmp_size(const void *a, const void *b){ size_t x=*(const size_t*)a, y=*(const size_t*)b; return (x>y)-(x<y); }
```

- [ ] **Step 4: Make `warm_append` return the record offset**

Replace `warm_append`:

```c
static void warm_append(TileMemory *m, const Tile *t){
    MKDIR(m->store_dir);
    char wp[300]; warm_path(m,wp,sizeof(wp));
    FILE *f=fopen(wp,"ab"); if(!f) return; tile_write(f,t); fclose(f); m->warm_count++;
}
```

with:

```c
static size_t warm_append(TileMemory *m, const Tile *t){
    MKDIR(m->store_dir);
    char wp[300]; warm_path(m,wp,sizeof(wp));
    FILE *f=fopen(wp,"ab"); if(!f) return (size_t)-1;
    fseek(f,0,SEEK_END); long off=ftell(f);
    tile_write(f,t); fclose(f); m->warm_count++;
    return (size_t)off;
}
```

- [ ] **Step 5: Keep the index consistent in `spill_coldest`**

Replace `spill_coldest`:

```c
static void spill_coldest(TileMemory *m){
    if(m->n_hot==0) return;
    int b=0; for(int i=1;i<m->n_hot;i++)
        if(m->hot[i].heat<m->hot[b].heat || (m->hot[i].heat==m->hot[b].heat && m->hot[i].count<m->hot[b].count)) b=i;
    size_t off=warm_append(m,&m->hot[b]);
    loc_set(m, m->hot[b].tid, 1, off);          /* spilled tile now lives in WARM */
    tile_free(&m->hot[b]);
    int last=--m->n_hot; m->hot[b]=m->hot[last];
    if(b!=last) loc_set(m, m->hot[b].tid, 0, (size_t)b);   /* moved tile's new HOT index */
}
```

- [ ] **Step 6: Assign tids + postings + locations in `tilemem_load`**

In `tilemem_load`, the HOT read loop currently is:

```c
    if(f){ Tile t; while(tile_read(f,&t,m->dim)==0){
        if(m->n_hot==m->cap_hot){ m->cap_hot=m->cap_hot?m->cap_hot*2:64; m->hot=(Tile*)realloc(m->hot,(size_t)m->cap_hot*sizeof(Tile)); }
        m->hot[m->n_hot++]=t; } fclose(f); }
```

Replace it with:

```c
    if(f){ Tile t; while(tile_read(f,&t,m->dim)==0){
        if(m->n_hot==m->cap_hot){ m->cap_hot=m->cap_hot?m->cap_hot*2:64; m->hot=(Tile*)realloc(m->hot,(size_t)m->cap_hot*sizeof(Tile)); }
        unsigned tid=m->next_tid++; t.tid=tid; int idx=m->n_hot; m->hot[m->n_hot++]=t;
        loc_set(m,tid,0,(size_t)idx);
        char tt[256][32]; const char *dt[256]; int nd=tile_distinct_terms(t.key,tt,dt,256);
        for(int a=0;a<nd;a++) tdf_post(&m->tdf,dt[a],tid);
    } fclose(f); }
```

The WARM read loop currently is:

```c
    char wp[300]; warm_path(m,wp,sizeof(wp)); FILE *g=fopen(wp,"rb");
    if(g){ Tile t; while(tile_read(g,&t,m->dim)==0){ m->warm_count++; tile_free(&t); } fclose(g); }
```

Replace it with:

```c
    char wp[300]; warm_path(m,wp,sizeof(wp)); FILE *g=fopen(wp,"rb");
    if(g){ Tile t; for(;;){ long off=ftell(g); if(tile_read(g,&t,m->dim)!=0) break;
        m->warm_count++; unsigned tid=m->next_tid++; loc_set(m,tid,1,(size_t)off);
        char tt[256][32]; const char *dt[256]; int nd=tile_distinct_terms(t.key,tt,dt,256);
        for(int a=0;a<nd;a++) tdf_post(&m->tdf,dt[a],tid);
        tile_free(&t); } fclose(g); }
```

(The `idf.bin` load below stays unchanged — `df` is restored from disk; postings come from the tile scan above. `tile_distinct_terms` is already defined earlier in the file.)

- [ ] **Step 7: Assign tid + postings + location in `tilemem_ingest`**

In `tilemem_ingest`, the new-tile section currently ends:

```c
    t->heat=1; t->count=1;
    /* TF-IDF: bump document frequency once per distinct term in this tile. */
    { char terms[256][32]; int nt=tokenize_terms(key,terms,256);
      for(int a=0;a<nt;a++){ int dup=0; for(int b=0;b<a;b++) if(strcmp(terms[a],terms[b])==0){ dup=1; break; }
        if(!dup){ unsigned *d=tdf_slot(&m->tdf,terms[a],1); if(d)(*d)++; } } }
    m->doc_count++;
    m->syn_dirty=1;
    return 1;
```

Replace that block with:

```c
    t->heat=1; t->count=1;
    unsigned tid=m->next_tid++; t->tid=tid;
    loc_set(m,tid,0,(size_t)(m->n_hot-1));
    /* TF-IDF df bump + inverted-index posting, once per distinct term. */
    { char terms[256][32]; int nt=tokenize_terms(key,terms,256);
      for(int a=0;a<nt;a++){ int dup=0; for(int b=0;b<a;b++) if(strcmp(terms[a],terms[b])==0){ dup=1; break; }
        if(!dup){ unsigned *d=tdf_slot(&m->tdf,terms[a],1); if(d)(*d)++; tdf_post(&m->tdf,terms[a],tid); } } }
    m->doc_count++;
    m->syn_dirty=1;
    return 1;
```

- [ ] **Step 8: Drop locations in `tilemem_evict_containing`**

The current loop body is:

```c
    for(int i=0;i<m->n_hot;){
        if(strstr(m->hot[i].key, needle)){ tile_free(&m->hot[i]); m->hot[i]=m->hot[--m->n_hot]; ev++; }
        else i++;
    }
```

Replace it with:

```c
    for(int i=0;i<m->n_hot;){
        if(strstr(m->hot[i].key, needle)){ loc_kill(m,m->hot[i].tid); tile_free(&m->hot[i]);
            int last=--m->n_hot; m->hot[i]=m->hot[last]; if(i!=last) loc_set(m,m->hot[i].tid,0,(size_t)i); ev++; }
        else i++;
    }
```

- [ ] **Step 9: Free the new structures in `tilemem_close`**

In `tilemem_close`, before `free(m);`, add:

```c
    free(m->loc); free(m->seen);
```

- [ ] **Step 10: Build and verify no regressions (index built but unused)**

Run: `make tileindex` (expect 5/5 — search still passthrough), `make tiermem_test` (18/18), `make synonyms` (25/25), `make tfidf` (3/3), `make graduate` (9/9).
Expected: all green and warning-clean. The index is now populated on every ingest/load/spill/evict but not yet read by search, so behavior is unchanged.

---

## Task 3: Index-based search + identity gate

**Files:**
- Modify: `src/corpus/tile_memory.c`
- Modify: `tests/test_tile_index.c`

- [ ] **Step 1: Add the forced-spill identity test (it will currently pass via passthrough, then must stay green after the rewrite)**

In `tests/test_tile_index.c`, add above `main`:

```c
static void test_identity_warm(void){
    reset_store("tix_warm");
    TileMemory *m=tilemem_open("tix_warm",256,4,0.99);   /* hot_cap=4 forces spill to WARM */
    const char *docs[]={"alpha beta","gamma delta","alpha gamma","beta epsilon",
                        "delta zeta","alpha epsilon zeta","gamma eta","beta delta theta"};
    for(int i=0;i<8;i++) tilemem_ingest(m,docs[i],docs[i],"","c");   /* >4 -> WARM populated */
    const char *qs[]={"alpha","beta delta","gamma eta","zeta","nomatch","alpha beta gamma delta"};
    for(int i=0;i<6;i++) CHECK(same_results(m,qs[i]),"index == linear (forced WARM)");
    /* after an eviction, identity still holds */
    tilemem_evict_containing(m,"alpha");
    for(int i=0;i<6;i++) CHECK(same_results(m,qs[i]),"index == linear (after evict)");
    tilemem_close(m);
}

static void test_identity_expansion(void){
    reset_store("tix_exp");
    TileMemory *m=tilemem_open("tix_exp",256,3,0.99);
    const char *docs[]={"alpha xray one","alpha xray two","beta yankee three",
                        "beta yankee four","xray yankee five","delta echo six"};
    for(int i=0;i<6;i++) tilemem_ingest(m,docs[i],docs[i],"","c");
    tilemem_set_expansion(m,0.35,5,2,1,0.9,64);   /* PPMI soft terms participate in candidates */
    const char *qs[]={"alpha beta","xray","delta","beta yankee"};
    for(int i=0;i<4;i++) CHECK(same_results(m,qs[i]),"index == linear (with PPMI expansion)");
    tilemem_close(m);
}
```

Add `test_identity_warm();` and `test_identity_expansion();` in `main` after `test_identity_small();`. Also add a store dir `tix_exp` to the Makefile `clean` `rm -rf` list.

- [ ] **Step 2: Run to confirm the new tests pass via passthrough (baseline before the rewrite)**

Run: `make tileindex`
Expected: `15/15 checks passed` (5 + 12). They pass trivially now (search == linear); the point is they must still pass after Step 3 swaps in the index.

- [ ] **Step 3: Replace the `tilemem_search` passthrough with the index version**

In `src/corpus/tile_memory.c`, replace:

```c
int tilemem_search(TileMemory *m, const char *query, int topK, TileHit *out, int cap){
    return tilemem_search_linear(m,query,topK,out,cap);   /* index version lands in Task 3 */
}
```

with:

```c
int tilemem_search(TileMemory *m, const char *query, int topK, TileHit *out, int cap){
    for(int i=0;i<m->n_res;i++) free(m->res[i]);
    m->n_res=0;
    char qt[64][32]; const char *wq[128]; double ww[128];
    int nw=build_weighted_query(m,query,qt,wq,ww,128);
    int lim=topK<cap?topK:cap; int n=0;
    /* gather candidate tids from postings; collect WARM candidates' offsets */
    m->seen_epoch++; seen_ensure(m);
    size_t *wc=NULL; int wcn=0, wccap=0;
    for(int w=0;w<nw;w++){ int pn; unsigned *pl=tdf_postings(&m->tdf,wq[w],&pn);
        for(int p=0;p<pn;p++){ unsigned tid=pl[p];
            if(!loc_live(m,tid)) continue;
            if(m->seen[tid]==m->seen_epoch) continue;
            m->seen[tid]=m->seen_epoch;
            if(m->loc[tid].tier==1){ if(wcn==wccap){ wccap=wccap?wccap*2:16;
                    wc=(size_t*)realloc(wc,(size_t)wccap*sizeof(size_t)); } wc[wcn++]=m->loc[tid].where; }
        } }
    /* HOT pass in array order (preserves tie-break order); score candidates only */
    for(int i=0;i<m->n_hot;i++){
        if(m->seen[m->hot[i].tid]!=m->seen_epoch) continue;
        float sc=score_tile(m->hot[i].key,wq,ww,nw);
        if(sc>0){ topk_insert(m,out,&n,lim,sc,m->hot[i].id,m->hot[i].label,m->hot[i].source,m->hot[i].value); m->hot[i].heat++; }
    }
    /* WARM pass by ascending offset (= file order); read only candidate tiles */
    if(wcn>0){ qsort(wc,(size_t)wcn,sizeof(size_t),cmp_size);
        char wp[300]; warm_path(m,wp,sizeof(wp)); FILE *f=fopen(wp,"rb");
        if(f){ for(int c=0;c<wcn;c++){ if(fseek(f,(long)wc[c],SEEK_SET)!=0) break; Tile t;
            if(tile_read(f,&t,m->dim)!=0) break;
            float sc=score_tile(t.key,wq,ww,nw);
            topk_insert(m,out,&n,lim,sc,t.id,t.label,t.source,t.value);
            tile_free(&t); } fclose(f); } }
    free(wc);
    return n;
}
```

- [ ] **Step 4: Run the identity gate**

Run: `make tileindex`
Expected: `15/15 checks passed`, warning-clean. This proves the index search is byte-identical to the linear oracle across HOT-only, forced-WARM, post-eviction, no-match, and PPMI-expansion cases.

- [ ] **Step 5: Full identity-adjacent regressions**

Run: `make tiermem_test` (18/18), `make synonyms` (25/25), `make tfidf` (3/3), `make graduate` (9/9), `make fontdecode` (20/20), `make pdftest` (19/19).
Expected: all green — these suites call `tilemem_search` (now the index version) and must be unaffected. `tiermem_test` exercises real spill/persistence, so it is a strong end-to-end identity check.

---

## Task 4: Forced-WARM benchmark + final sweep

**Files:**
- Modify: `tests/test_tile_index.c`

- [ ] **Step 1: Add the benchmark (auto-skips if the book is absent)**

In `tests/test_tile_index.c`, add the corpus/pdf includes after the tile_memory include:

```c
#include "../include/corpus/corpus_split.h"
#include "../include/pdf/pdf_extract.h"
```

Add above `main`:

```c
static unsigned char *readfile(const char *p,size_t*len){ FILE*f=fopen(p,"rb"); if(!f)return NULL;
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET); if(sz<=0){fclose(f);return NULL;}
    unsigned char*b=malloc((size_t)sz); *len=fread(b,1,(size_t)sz,f); fclose(f); return b; }

static double bench_ms(TileMemory *m, const char *q, int linear, int reps){
    TileHit h[8]; clock_t t0=clock();
    for(int r=0;r<reps;r++){ if(linear) tilemem_search_linear(m,q,8,h,8); else tilemem_search(m,q,8,h,8); }
    return 1000.0*(double)(clock()-t0)/CLOCKS_PER_SEC/(double)reps;
}

static void test_book_bench(void){
    const char *path="C:/Users/Mokason/Downloads/aivalueplaybook.pdf";
    FILE *pr=fopen(path,"rb"); if(!pr){ printf("[book] not found - skipping benchmark\n"); return; } fclose(pr);
    size_t len=0; unsigned char *buf=readfile(path,&len); if(!buf) return;
    char *text=malloc(len*4+16); size_t tl=0;
    if(pdf_extract_text(buf,len,text,len*4+16,&tl)!=PDF_OK){ free(text); free(buf); return; }
    StrList s; strlist_init(&s); corpus_split(text,&s);
    reset_store("tix_book");
    TileMemory *m=tilemem_open("tix_book",256,500,0.6);   /* small HOT -> most tiles spill to WARM */
    for(size_t i=0;i<s.count;i++) if(corpus_quality_keep(s.lines[i])) tilemem_ingest(m,s.lines[i],s.lines[i],"","book");
    printf("\n[bench] tiles=%zu  hot=%zu  warm=%zu\n", tilemem_total(m), tilemem_hot_count(m), tilemem_warm_count(m));
    const char *qs[]={"last mile problem","storytelling narrative","the and of"};
    for(int i=0;i<3;i++){
        double lin=bench_ms(m,qs[i],1,20), idx=bench_ms(m,qs[i],0,20);
        CHECK(same_results(m,qs[i]),"book: index == linear");
        printf("[bench] q=\"%-22s\" linear %.3f ms  index %.3f ms  (%.1fx)\n", qs[i], lin, idx, lin/(idx>0?idx:1e-9));
    }
    tilemem_close(m);
    free(text); free(buf); strlist_free(&s);
}
```

Add `test_book_bench();` in `main` after the identity tests.

- [ ] **Step 2: Run and capture the benchmark**

Run: `make tileindex`
Expected: all checks pass (`18/18` with book — 15 + 3 identity, or `15/15` + skip). Capture the `[bench]` lines verbatim: for selective queries (`last mile problem`, `storytelling narrative`) the index should be markedly faster than linear; for a common-word query (`the and of`) the gap is smaller (long postings) — report both honestly.

- [ ] **Step 3: Full regression sweep**

Run: `make tileindex`, `make tiermem_test`, `make synonyms`, `make tfidf`, `make graduate`, `make fontdecode`, `make pdftest`.
Expected: all green (`tiermem_test` 18/18, `synonyms` 25/25, `tfidf` 3/3, `graduate` 9/9, `fontdecode` 20/20, `pdftest` 19/19). `graduate`'s pre-existing `src/router/*` warnings are expected. If any regress, stop and debug (superpowers:systematic-debugging).

---

## Self-Review

**Spec coverage:**
- §1.1 index-driven search → Task 3 Step 3. ✓
- §1.2 byte-identical results → Task 1 (oracle) + Task 3 identity tests (Steps 1–4). ✓
- §1.3 WARM random access by offset → Task 2 Step 4 (`warm_append` offset) + Task 3 WARM pass. ✓
- §1.4 no persisted index, rebuilt on load → Task 2 Step 6. ✓
- §1.5 no regressions, zero core edits → Task 3 Step 5 + Task 4 Step 3. ✓
- §3 structures (tid, postings, loc, seen) → Task 2 Steps 1–3. ✓
- §4 build on open → Task 2 Step 6. ✓
- §5 search algorithm → Task 3 Step 3. ✓
- §6 mutation (ingest/spill/evict/swap) → Task 2 Steps 5,7,8. ✓
- §9 identity + edge tests + benchmark → Tasks 1,3,4. ✓

**Placeholder scan:** No TBD/TODO; complete code in every code step. ✓

**Type consistency:** `tilemem_search_linear(TileMemory*,const char*,int,TileHit*,int)` matches header + impl + test calls. `tdf_post(TermDF*,const char*,unsigned)`, `tdf_postings(TermDF*,const char*,int*)`, `loc_set(TileMemory*,unsigned,int,size_t)`, `loc_live(TileMemory*,unsigned)`, `score_tile(const char*,const char**,const double*,int)`, `build_weighted_query(TileMemory*,const char*,char[][32],const char**,double*,int)` — all used consistently across tasks. `Loc{size_t where;int tier;char live;}` and `Tile.tid (unsigned)` consistent. ✓

---

## Project notes

- **No git on CNET** — never run git; verify via filesystem + `make`.
- Largest single `tile_memory` change to date; the index-vs-linear identity gate is the safety net.
- Touches only `tile_memory.{c,h}`, new `tests/test_tile_index.c`, `Makefile`. **Zero core/router/contract edits.** PPMI semantic-dedup follows as its own cycle.
