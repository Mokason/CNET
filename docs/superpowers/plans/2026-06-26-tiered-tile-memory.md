# Tiered Tile Memory (AICIMO-adapted) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A production tiered, fuzzy, decaying tile-memory subsystem: book passages become tiles, fuzzy-deduped on ingest (compounding reuse), kept in a capacity-bounded HOT tier that spills to a WARM on-disk tier (bounded RAM, unbounded disk), persistent across runs, with stable tiles eligible to graduate into contracts.

**Architecture:** `src/corpus/tile_memory.c` ports AICIMO's `TiledMemoryIndex` (FNV-1a hashed dense vector + cosine recall) and `GraphMemoryHead` decay into C, adds HOT/WARM residency + persistence. A real test suite `tests/test_tile_memory.c` (`make tiermem_test`) covers the API and three proofs (fuzzy-reuse > exact-reuse; HOT ≤ cap while total grows; durable idempotent persistence). Reuses `corpus_split`/`pdf_extract`. Zero core edits.

**Tech Stack:** C11 (gcc, `-mno-avx`), stdlib + libm. **Production module, not a demo:** clean documented API, persistent store under a canonical dir, proper test file.

**Project note — NO GIT:** `.git` removed. Run no git commands. "Commit" → **checkpoint**: re-run `make tiermem_test`, confirm green.

---

## File Structure

- **Create `include/corpus/tile_memory.h`, `src/corpus/tile_memory.c`** — the library.
- **Create `tests/test_tile_memory.c`** — real test suite (`CHECK` macro, nonzero exit on fail).
- **Modify `Makefile`** — `TILEMEM_SRC`/`TILEMEM_TEST` vars, `tiermem_test` target, `.PHONY`, `clean`.

`CHECK` macro:
```c
static int g_fail=0, g_checks=0;
#define CHECK(c,m) do{ g_checks++; if(!(c)){ g_fail++; printf("  FAIL: %s (%s:%d)\n",m,__FILE__,__LINE__);} }while(0)
```

---

## Task 1: tile + vectorize + cosine + open/close

**Files:**
- Create: `include/corpus/tile_memory.h`, `src/corpus/tile_memory.c`, `tests/test_tile_memory.c`
- Modify: `Makefile`

- [ ] **Step 1: Header**

`include/corpus/tile_memory.h`:
```c
#ifndef CORPUS_TILE_MEMORY_H
#define CORPUS_TILE_MEMORY_H
#include <stddef.h>

typedef struct {
    char   id[64];
    char  *key;          /* trigger text (owned) */
    char  *value;        /* content/continuation (owned) */
    char   label[32];
    char   source[64];
    int    heat;
    int    count;
    float *vec;          /* dim floats, L2-normalized (owned) */
} Tile;

typedef struct { char id[64]; char label[32]; char source[64]; const char *value; float score; } TileHit;
typedef struct TileMemory TileMemory;

/* Open/create the persistent store at store_dir (hot.bin + warm.bin). Loads prior state. */
TileMemory *tilemem_open(const char *store_dir, int dim, int hot_cap, double dedup_tau);
void        tilemem_close(TileMemory *m);   /* persists HOT (+ keeps WARM file) */

int    tilemem_ingest(TileMemory *m, const char *key, const char *value,
                      const char *label, const char *source);   /* 1=new, 0=reused */
int    tilemem_search(TileMemory *m, const char *query, int topK, TileHit *out, int cap);
void   tilemem_decay(TileMemory *m);

size_t tilemem_hot_count(const TileMemory *m);
size_t tilemem_warm_count(const TileMemory *m);
size_t tilemem_total(const TileMemory *m);
size_t tilemem_certifiable(const TileMemory *m, int min_count, double min_share);
#endif
```

- [ ] **Step 2: Implementation skeleton `src/corpus/tile_memory.c` (vectorize, cosine, open/close)**

```c
#include "../../include/corpus/tile_memory.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

struct TileMemory {
    int dim, hot_cap; double tau;
    char store_dir[256];
    Tile *hot; int n_hot, cap_hot;
    size_t warm_count;
    /* scratch for search results' value strings (valid until next search) */
};

/* --- AICIMO-ported tokenization + hashed vector --- */
static unsigned fnv1a(const char *s){ unsigned h=2166136261u; for(;*s;s++){ h^=(unsigned char)*s; h*=16777619u; } return h; }
static int is_stop(const char *t){
    static const char *S[]={"the","and","for","from","with","that","this","into","should","would","could",
        "what","when","where","which","note","are","was","its","has","have","not","you","your","our",0};
    for(int i=0;S[i];i++) if(strcmp(S[i],t)==0) return 1; return 0;
}
static void stem(char *t){ size_t n=strlen(t);
    if(n>4 && strcmp(t+n-3,"ies")==0){ t[n-3]='y'; t[n-2]=0; }
    else if(n>3 && t[n-1]=='s'){ t[n-1]=0; }
}
static void vectorize(const char *text, float *vec, int dim){
    memset(vec,0,(size_t)dim*sizeof(float));
    char tok[64]; int tl=0;
    for(const char *p=text;;p++){ char c=*p;
        int al=(c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_';
        if(al){ if(tl<63) tok[tl++]=(c>='A'&&c<='Z')?(char)(c+32):c; }
        else { if(tl>0){ tok[tl]=0; stem(tok); if(strlen(tok)>1 && !is_stop(tok)) vec[fnv1a(tok)%(unsigned)dim]+=1.0f; tl=0; }
               if(c==0) break; } }
    double s=0; for(int i=0;i<dim;i++) s+=(double)vec[i]*vec[i];
    if(s>0){ float inv=1.0f/(float)sqrt(s); for(int i=0;i<dim;i++) vec[i]*=inv; }
}
static float cosine(const float *a, const float *b, int dim){ float d=0; for(int i=0;i<dim;i++) d+=a[i]*b[i]; return d; }

static void slugify(const char *text, char *out, size_t cap){
    size_t o=0; int prev_us=0;
    for(const char *p=text; *p && o+1<cap; p++){ char c=*p;
        if((c>='a'&&c<='z')||(c>='0'&&c<='9')){ out[o++]=c; prev_us=0; }
        else if((c>='A'&&c<='Z')){ out[o++]=(char)(c+32); prev_us=0; }
        else if(!prev_us && o>0){ out[o++]='_'; prev_us=1; } }
    while(o>0 && out[o-1]=='_') o--;
    out[o]=0; if(o==0) snprintf(out,cap,"tile");
}

TileMemory *tilemem_open(const char *store_dir, int dim, int hot_cap, double dedup_tau){
    if(dim<=0||hot_cap<=0) return NULL;
    TileMemory *m=(TileMemory*)calloc(1,sizeof(*m));
    m->dim=dim; m->hot_cap=hot_cap; m->tau=dedup_tau;
    snprintf(m->store_dir,sizeof(m->store_dir),"%s",store_dir);
    /* load is added in Task 3; for now start empty */
    return m;
}
static void tile_free(Tile *t){ free(t->key); free(t->value); free(t->vec); }
void tilemem_close(TileMemory *m){
    if(!m) return;
    /* save is added in Task 3 */
    for(int i=0;i<m->n_hot;i++) tile_free(&m->hot[i]);
    free(m->hot); free(m);
}
size_t tilemem_hot_count(const TileMemory *m){ return (size_t)m->n_hot; }
size_t tilemem_warm_count(const TileMemory *m){ return m->warm_count; }
size_t tilemem_total(const TileMemory *m){ return (size_t)m->n_hot + m->warm_count; }
```

- [ ] **Step 3: Test scaffold `tests/test_tile_memory.c` (vectorize/cosine sanity)**

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/corpus/tile_memory.h"

static int g_fail=0, g_checks=0;
#define CHECK(c,m) do{ g_checks++; if(!(c)){ g_fail++; printf("  FAIL: %s (%s:%d)\n",m,__FILE__,__LINE__);} }while(0)

static void test_open(void){
    TileMemory *m=tilemem_open("tile_store_test",256,1000,0.5);
    CHECK(m!=NULL,"open store");
    CHECK(tilemem_total(m)==0,"new store empty");
    tilemem_close(m);
}

int main(void){
    printf("=== test_tile_memory ===\n");
    test_open();
    printf("%d/%d checks passed\n", g_checks-g_fail, g_checks);
    return g_fail?1:0;
}
```

- [ ] **Step 4: Makefile**

Add vars near other corpus vars:
```make
TILEMEM_SRC := src/corpus/tile_memory.c
TILEMEM_TEST := tests/test_tile_memory.c
```
Add target:
```make
tiermem_test: $(TILEMEM_SRC) $(CORPUS_SRC) $(PDF_SRC) $(TILEMEM_TEST) include/corpus/tile_memory.h include/corpus/corpus_split.h include/pdf/pdf_extract.h
	$(CC) $(CFLAGS) -Wno-unused-function -o $@ $(TILEMEM_SRC) $(CORPUS_SRC) $(PDF_SRC) $(TILEMEM_TEST) $(LDFLAGS)
	./tiermem_test
```
Add `tiermem_test` to `.PHONY` (line 117); add `tiermem_test` and `tile_store_test tile_mem_pdf` to the `clean` rm list (the latter two are store dirs; use `rm -rf` form — add to the recipe as separate `rm -rf tile_store_test tile_mem_pdf` line under `clean`).

- [ ] **Step 5: Build and run**

Run: `make tiermem_test`
Expected: `test_open` passes; `2/2 checks passed`; exit 0.

- [ ] **Step 6: Checkpoint (no git)** — re-run `make tiermem_test`.

---

## Task 2: ingest (fuzzy-dedup) + search (HOT) + fuzzy-reuse proof

**Files:**
- Modify: `src/corpus/tile_memory.c`, `tests/test_tile_memory.c`

- [ ] **Step 1: Implement `tilemem_ingest` + `tilemem_search` (HOT-only)**

Add to `tile_memory.c`:
```c
/* best cosine match in HOT; returns index or -1, sets *best_cos. */
static int hot_best(TileMemory *m, const float *qv, float *best_cos){
    int best=-1; float bc=-1;
    for(int i=0;i<m->n_hot;i++){ float c=cosine(qv,m->hot[i].vec,m->dim); if(c>bc){ bc=c; best=i; } }
    *best_cos=bc; return best;
}
int tilemem_ingest(TileMemory *m, const char *key, const char *value, const char *label, const char *source){
    float *qv=(float*)malloc((size_t)m->dim*sizeof(float)); vectorize(key,qv,m->dim);
    float bc; int b=hot_best(m,qv,&bc);
    if(b>=0 && bc>=(float)m->tau){ m->hot[b].count++; m->hot[b].heat++; free(qv); return 0; }  /* fuzzy reuse */
    if(m->n_hot==m->cap_hot){ m->cap_hot=m->cap_hot?m->cap_hot*2:64; m->hot=(Tile*)realloc(m->hot,(size_t)m->cap_hot*sizeof(Tile)); }
    /* Task 3 adds: if n_hot >= hot_cap, spill coldest to WARM first. */
    Tile *t=&m->hot[m->n_hot++];
    memset(t,0,sizeof(*t));
    t->key=strdup(key); t->value=strdup(value?value:key); t->vec=qv;
    snprintf(t->label,sizeof(t->label),"%s",label?label:""); snprintf(t->source,sizeof(t->source),"%s",source?source:"");
    slugify(label&&*label?label:key, t->id, sizeof(t->id));
    t->heat=1; t->count=1;
    return 1;
}
int tilemem_search(TileMemory *m, const char *query, int topK, TileHit *out, int cap){
    float *qv=(float*)malloc((size_t)m->dim*sizeof(float)); vectorize(query,qv,m->dim);
    int lim = topK<cap?topK:cap; int n=0;
    /* simple top-K by insertion over HOT */
    for(int i=0;i<m->n_hot;i++){ float c=cosine(qv,m->hot[i].vec,m->dim); if(c<=0) continue;
        int pos=n<lim?n:-1;
        if(pos<0){ if(c>out[n-1].score) pos=n-1; else continue; }
        else n++;
        int j=pos; while(j>0 && out[j-1].score<c){ out[j]=out[j-1]; j--; }
        snprintf(out[j].id,sizeof(out[j].id),"%s",m->hot[i].id);
        snprintf(out[j].label,sizeof(out[j].label),"%s",m->hot[i].label);
        snprintf(out[j].source,sizeof(out[j].source),"%s",m->hot[i].source);
        out[j].value=m->hot[i].value; out[j].score=c;
        m->hot[i].heat++;   /* promote-on-hit */
    }
    free(qv); return n;
}
size_t tilemem_certifiable(const TileMemory *m, int min_count, double min_share){
    (void)min_share; size_t c=0; for(int i=0;i<m->n_hot;i++) if(m->hot[i].count>=min_count) c++; return c;
}
void tilemem_decay(TileMemory *m){ for(int i=0;i<m->n_hot;i++) if(m->hot[i].heat>0) m->hot[i].heat--; }
```

- [ ] **Step 2: Failing tests — fuzzy-dedup + the fuzzy-reuse proof**

Add to `test_tile_memory.c` (corpora + tests), call from `main`:
```c
static const char *BOOK_A[]={
    "the team aligns on the goal and the value of the work",
    "data and the kind of teams you need to build the system",
    "change is the focus and the value of the investment grows",
    "the leader shares the story and builds trust with the team" };
static const char *BOOK_B[]={
    "the team aligns on the goal and the value of the work",          /* verbatim */
    "change is the focus and the value of the investment grows",      /* verbatim */
    "the team builds the system and shares the value of the data",    /* paraphrase, shared vocab */
    "the leader aligns the goal and grows the trust of the team" };   /* paraphrase, shared vocab */
#define NB 4

static int ingest_book(TileMemory *m, const char **bk, int n){ int reused=0;
    for(int i=0;i<n;i++) reused += (tilemem_ingest(m,bk[i],bk[i],"","A")==0);
    return reused;
}

static void test_dedup(void){
    TileMemory *m=tilemem_open("tile_store_test",256,1000,0.99);
    tilemem_ingest(m,BOOK_A[0],BOOK_A[0],"","A");
    int r=tilemem_ingest(m,BOOK_A[0],BOOK_A[0],"","A");   /* identical */
    CHECK(r==0,"identical sentence is fuzzy-deduped (reused)");
    CHECK(tilemem_total(m)==1,"identical ingest keeps 1 tile");
    tilemem_close(m);
}

static void test_fuzzy_vs_exact(void){
    /* exact (tau=0.999): only verbatim matches reuse */
    TileMemory *me=tilemem_open("tile_store_test",256,1000,0.999);
    ingest_book(me,BOOK_A,NB); int reuse_exact=ingest_book(me,BOOK_B,NB);
    tilemem_close(me);
    /* fuzzy (tau=0.5): paraphrases with shared vocab also reuse */
    TileMemory *mf=tilemem_open("tile_store_test",256,1000,0.5);
    ingest_book(mf,BOOK_A,NB); int reuse_fuzzy=ingest_book(mf,BOOK_B,NB);
    tilemem_close(mf);
    printf("[fuzzy] reuse exact=%d fuzzy=%d (of %d)\n", reuse_exact, reuse_fuzzy, NB);
    CHECK(reuse_exact>=2,"exact reuse catches the 2 verbatim sentences");
    CHECK(reuse_fuzzy>reuse_exact,"fuzzy reuse exceeds exact (AICIMO upgrade)");
}

static void test_search(void){
    TileMemory *m=tilemem_open("tile_store_test",256,1000,0.5);
    ingest_book(m,BOOK_A,NB);
    TileHit hits[4]; int n=tilemem_search(m,"what is the value of the team goal",3,hits,4);
    CHECK(n>0,"search returns hits");
    CHECK(n>0 && hits[0].score>0,"top hit has positive score");
    tilemem_close(m);
}
```
Add `test_dedup(); test_fuzzy_vs_exact(); test_search();` to `main`.

- [ ] **Step 3: Build and run**

Run: `make tiermem_test`
Expected: `[fuzzy] reuse exact=2 fuzzy=N` with `N>2`; all checks pass.
If `reuse_fuzzy==reuse_exact`, lower the fuzzy `tau` (0.5 → 0.4) so the shared-vocab paraphrases clear it; if `reuse_exact>2`, raise exact `tau` toward 1.0.

- [ ] **Step 4: Checkpoint (no git)** — re-run `make tiermem_test`.

---

## Task 3: HOT cap + WARM spill + persistence (the residency proof)

**Files:**
- Modify: `src/corpus/tile_memory.c`, `tests/test_tile_memory.c`

- [ ] **Step 1: Add tile (de)serialization + WARM spill + load/save in `tile_memory.c`**

```c
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#define MKDIR(p) mkdir(p,0755)
#endif

static void warm_path(const TileMemory *m, char *out, size_t cap){ snprintf(out,cap,"%s/warm.bin",m->store_dir); }
static void hot_path (const TileMemory *m, char *out, size_t cap){ snprintf(out,cap,"%s/hot.bin", m->store_dir); }

static void wstr(FILE *f, const char *s){ int n=(int)strlen(s); fwrite(&n,sizeof(int),1,f); fwrite(s,1,(size_t)n,f); }
static char *rstr(FILE *f){ int n; if(fread(&n,sizeof(int),1,f)!=1||n<0||n>100000) return NULL;
    char *s=(char*)malloc((size_t)n+1); if(fread(s,1,(size_t)n,f)!=(size_t)n){ free(s); return NULL; } s[n]=0; return s; }

static void tile_write(FILE *f, const Tile *t){
    fwrite(t->id,1,64,f); fwrite(t->label,1,32,f); fwrite(t->source,1,64,f);
    fwrite(&t->heat,sizeof(int),1,f); fwrite(&t->count,sizeof(int),1,f);
    wstr(f,t->key); wstr(f,t->value);
}
/* read a tile (vec recomputed from key); returns 0 on clean read, -1 on EOF/err. */
static int tile_read(FILE *f, Tile *t, int dim){
    memset(t,0,sizeof(*t));
    if(fread(t->id,1,64,f)!=64) return -1;
    if(fread(t->label,1,32,f)!=32) return -1;
    if(fread(t->source,1,64,f)!=64) return -1;
    if(fread(&t->heat,sizeof(int),1,f)!=1) return -1;
    if(fread(&t->count,sizeof(int),1,f)!=1) return -1;
    t->key=rstr(f); t->value=rstr(f); if(!t->key||!t->value){ free(t->key); free(t->value); return -1; }
    t->vec=(float*)malloc((size_t)dim*sizeof(float)); vectorize(t->key,t->vec,dim);
    return 0;
}

/* append one tile to warm.bin (creating store dir if needed). */
static void warm_append(TileMemory *m, const Tile *t){
    MKDIR(m->store_dir);
    char wp[300]; warm_path(m,wp,sizeof(wp));
    FILE *f=fopen(wp,"ab"); if(!f) return; tile_write(f,t); fclose(f); m->warm_count++;
}
/* evict the coldest (min heat, then min count) HOT tile to WARM. */
static void spill_coldest(TileMemory *m){
    if(m->n_hot==0) return;
    int b=0; for(int i=1;i<m->n_hot;i++){
        if(m->hot[i].heat<m->hot[b].heat || (m->hot[i].heat==m->hot[b].heat && m->hot[i].count<m->hot[b].count)) b=i; }
    warm_append(m,&m->hot[b]); tile_free(&m->hot[b]);
    m->hot[b]=m->hot[--m->n_hot];
}
```
Then in `tilemem_ingest`, **before** appending the new tile, enforce the cap:
```c
    while(m->n_hot >= m->hot_cap) spill_coldest(m);
```
(insert just before the `if(m->n_hot==m->cap_hot)` grow line). And implement load/save:
```c
static void tilemem_load(TileMemory *m){
    char hp[300]; hot_path(m,hp,sizeof(hp)); FILE *f=fopen(hp,"rb");
    if(f){ Tile t; while(tile_read(f,&t,m->dim)==0){
        if(m->n_hot==m->cap_hot){ m->cap_hot=m->cap_hot?m->cap_hot*2:64; m->hot=(Tile*)realloc(m->hot,(size_t)m->cap_hot*sizeof(Tile)); }
        m->hot[m->n_hot++]=t; } fclose(f); }
    char wp[300]; warm_path(m,wp,sizeof(wp)); FILE *g=fopen(wp,"rb");
    if(g){ Tile t; while(tile_read(g,&t,m->dim)==0){ m->warm_count++; tile_free(&t); } fclose(g); }
}
static void tilemem_save(TileMemory *m){
    MKDIR(m->store_dir);
    char hp[300]; hot_path(m,hp,sizeof(hp)); FILE *f=fopen(hp,"wb");
    if(f){ for(int i=0;i<m->n_hot;i++) tile_write(f,&m->hot[i]); fclose(f); }
}
```
Wire them: in `tilemem_open` replace the "start empty" comment with `tilemem_load(m);`; in `tilemem_close` replace the "save is added in Task 3" comment with `tilemem_save(m);` (before freeing).

- [ ] **Step 2: Failing tests — residency + persistence**

```c
static void test_residency(void){
    system("rm -rf tile_store_res 2>nul || rmdir /s /q tile_store_res 2>nul");
    TileMemory *m=tilemem_open("tile_store_res",256,3,0.99);   /* tiny HOT cap = 3 */
    const char *S[]={"alpha one two three","beta four five six","gamma seven eight nine",
                     "delta ten eleven twelve","epsilon thirteen fourteen fifteen"};
    for(int i=0;i<5;i++) tilemem_ingest(m,S[i],S[i],"","src");
    CHECK(tilemem_hot_count(m)<=3,"HOT stays within cap (bounded RAM)");
    CHECK(tilemem_total(m)==5,"total grows past cap (capacity on disk)");
    CHECK(tilemem_warm_count(m)>=2,"overflow spilled to WARM");
    tilemem_close(m);
}

static void test_persist(void){
    system("rm -rf tile_store_per 2>nul || rmdir /s /q tile_store_per 2>nul");
    TileMemory *m=tilemem_open("tile_store_per",256,1000,0.5);
    ingest_book(m,BOOK_A,NB); size_t total=tilemem_total(m);
    tilemem_close(m);
    TileMemory *m2=tilemem_open("tile_store_per",256,1000,0.5);   /* reload */
    CHECK(tilemem_total(m2)==total,"store persists across open/close");
    int reused=ingest_book(m2,BOOK_A,NB);   /* re-ingest same content */
    CHECK(reused==NB,"re-ingesting prior content is fully deduped (durable + idempotent)");
    tilemem_close(m2);
}
```
Add `test_residency(); test_persist();` to `main`. (The `system(rm -rf ...)` resets the store dirs for a clean run.)

- [ ] **Step 3: Build and run**

Run: `make tiermem_test`
Expected: residency — HOT ≤ 3, total 5, WARM ≥ 2; persistence — reload total matches, re-ingest fully deduped; all checks pass.

- [ ] **Step 4: Checkpoint (no git)** — re-run `make tiermem_test`.

---

## Task 4: WARM-aware search + real-PDF integration

**Files:**
- Modify: `src/corpus/tile_memory.c`, `tests/test_tile_memory.c`

- [ ] **Step 1: Extend `tilemem_search` to stream WARM on a weak HOT result, promoting hits**

Add a WARM scan helper and call it from `tilemem_search` after the HOT pass:
```c
/* Stream warm.bin, score each tile; promote any tile beating the current weakest
   returned hit into HOT (so its value pointer is stable), re-running the cap. */
static void search_warm(TileMemory *m, const float *qv, int topK, TileHit *out, int *n, int lim){
    char wp[300]; warm_path(m,wp,sizeof(wp)); FILE *f=fopen(wp,"rb"); if(!f) return;
    (void)topK;
    Tile t;
    while(tile_read(f,&t,m->dim)==0){
        float c=cosine(qv,t.vec,m->dim);
        float weakest = (*n>=lim)? out[*n-1].score : 0.0f;
        if(c>0 && (*n<lim || c>weakest)){
            /* promote into HOT */
            while(m->n_hot>=m->hot_cap) spill_coldest(m);
            if(m->n_hot==m->cap_hot){ m->cap_hot=m->cap_hot?m->cap_hot*2:64; m->hot=(Tile*)realloc(m->hot,(size_t)m->cap_hot*sizeof(Tile)); }
            t.heat++; m->hot[m->n_hot++]=t; m->warm_count = m->warm_count? m->warm_count-1:0;
            int idx=m->n_hot-1; int pos=(*n<lim)? (*n)++ : (*n)-1; int j=pos;
            while(j>0 && out[j-1].score<c){ out[j]=out[j-1]; j--; }
            snprintf(out[j].id,sizeof(out[j].id),"%s",m->hot[idx].id);
            snprintf(out[j].label,sizeof(out[j].label),"%s",m->hot[idx].label);
            snprintf(out[j].source,sizeof(out[j].source),"%s",m->hot[idx].source);
            out[j].value=m->hot[idx].value; out[j].score=c;
        } else { tile_free(&t); }
    }
    fclose(f);
}
```
In `tilemem_search`, after the HOT loop and before `free(qv)`, add: `search_warm(m,qv,topK,out,&n,lim);`
(Note: promoting removes the tile from the WARM file logically but not physically; `warm_count` is decremented and the next `tilemem_save`/compaction would rewrite. For this scope, duplicates across HOT+WARM after promotion are tolerated; a compaction pass is a later refinement — log it.)

- [ ] **Step 2: Real-PDF integration test (auto-skips if absent)**

```c
#include "../include/pdf/pdf_extract.h"
#include "../include/corpus/corpus_split.h"

static unsigned char *readfile(const char *p,size_t*len){ FILE*f=fopen(p,"rb"); if(!f)return NULL;
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET); if(sz<=0){fclose(f);return NULL;}
    unsigned char*b=malloc(sz); *len=fread(b,1,sz,f); fclose(f); return b; }

static void test_real_pdf(void){
    const char *path="C:/Users/Mokason/Downloads/aivalueplaybook.pdf";
    FILE *pr=fopen(path,"rb"); if(!pr){ printf("[realpdf] not found - skipping\n"); return; } fclose(pr);
    system("rm -rf tile_mem_pdf 2>nul || rmdir /s /q tile_mem_pdf 2>nul");
    TileMemory *m=tilemem_open("tile_mem_pdf",256,500,0.5);   /* HOT cap 500 vs a ~6900-sentence book */
    size_t len=0; unsigned char *buf=readfile(path,&len);
    char *text=malloc(len*4+16); size_t tl=0;
    PdfStatus st=pdf_extract_text(buf,len,text,len*4+16,&tl);
    int reused=0, total_in=0;
    if(st==PDF_OK){ StrList s; strlist_init(&s); corpus_split(text,&s);
        for(size_t i=0;i<s.count;i++){ total_in++; reused += (tilemem_ingest(m,s.lines[i],s.lines[i],"","aivalueplaybook")==0); }
        strlist_free(&s); }
    printf("[realpdf] ingested=%d reused=%d ; HOT=%zu WARM=%zu total=%zu\n",
           total_in, reused, tilemem_hot_count(m), tilemem_warm_count(m), tilemem_total(m));
    CHECK(st==PDF_OK,"real PDF extracted");
    CHECK(tilemem_hot_count(m)<=500,"HOT stayed within cap on a whole book (bounded RAM)");
    CHECK(tilemem_total(m)>1000,"memory grew well past HOT cap (unbounded on disk)");
    TileHit hits[5]; int n=tilemem_search(m,"value of data and teams",5,hits,5);
    printf("[realpdf] recall top-%d for 'value of data and teams':\n",n);
    for(int i=0;i<n;i++) printf("    %.3f  %.70s\n", hits[i].score, hits[i].value?hits[i].value:"");
    CHECK(n>0,"recall returns hits from the persisted book memory");
    free(text); free(buf); tilemem_close(m);
}
```
Add `test_real_pdf();` to `main`.

- [ ] **Step 3: Build and run (acceptance)**

Run: `make tiermem_test; echo "exit=$?"`
Expected: all prior checks; `[realpdf] ingested=~6900 reused=K ; HOT=...(≤500) WARM=... total=>1000`; a recall list of relevant sentences; `N/N checks passed`, `exit=0`.

- [ ] **Step 4: Checkpoint (no git)** — re-run `make tiermem_test`; confirm green and that re-running keeps the persisted store (idempotent reuse rises).

---

## Task 5 (stretch): contract-graduation hook

**Files:**
- Modify: `src/corpus/tile_memory.c`, `tests/test_tile_memory.c`

- [ ] **Step 1: Make `tilemem_certifiable` count tiles whose `count >= min_count`**

(Already implemented minimally in Task 2; this task validates and documents it as the bridge.) Add a test:
```c
static void test_certifiable(void){
    TileMemory *m=tilemem_open("tile_store_cert",256,1000,0.5);
    for(int i=0;i<5;i++) tilemem_ingest(m,BOOK_A[0],BOOK_A[0],"","A");  /* same tile 5x -> count 5 */
    CHECK(tilemem_certifiable(m,3,0.95)>=1,"a high-count stable tile is a contract candidate");
    tilemem_close(m);
    system("rm -rf tile_store_cert 2>nul || rmdir /s /q tile_store_cert 2>nul");
}
```
Add `test_certifiable();` to `main`. The full `btn_certify` graduation (link `$(CONTRACT)` etc.) is deferred — the candidate count is the bridge surface; promoting a candidate into a frozen certified primitive reuses the `endgate`/jsonstory pattern and is its own follow-up.

- [ ] **Step 2: Build and run**

Run: `make tiermem_test`
Expected: `test_certifiable` passes; suite green.

- [ ] **Step 3: Checkpoint (no git)** — re-run `make tiermem_test`.

---

## Self-Review (against the spec)

**Spec coverage:**
- §1.1 production library + API → Tasks 1–4 (header in Task 1). ✓
- §1.2 fuzzy-dedup compounding → Task 2 (`test_fuzzy_vs_exact`). ✓
- §1.3 bounded RAM / unbounded disk → Task 3 (`test_residency`), Task 4 real-PDF. ✓
- §1.4 persists across runs → Task 3 (`test_persist`). ✓
- §1.5 decay/forgetting → Task 2 `tilemem_decay` + Task 3 spill-coldest. ✓
- §1.6 contract graduation → Task 5 (candidate count; full certify deferred, noted). ✓
- §1.7 real tests → `tests/test_tile_memory.c`, `make tiermem_test`. ✓
- §2 Tile struct → Task 1. ✓
- §3 HOT/WARM tiers → Tasks 3–4. ✓
- §4 AICIMO logic (hashed vec, fuzzy-dedup, decay) → Tasks 1–3. ✓
- §5 API → Task 1 header, implemented across 1–4. ✓
- §8 sentence→passage tile (key=value=sentence) → Task 2/4 ingestion. ✓
- §10 limits respected (hashed cosine; WARM file scan; tunable τ/cap). ✓

**Placeholder scan:** none — complete code + exact commands. The promotion-leaves-WARM-duplicate and full-`btn_certify` graduation are explicitly scoped as later refinements, not placeholders.

**Type consistency:** `Tile`/`TileHit`/`TileMemory` consistent; `tilemem_open/close/ingest/search/decay/hot_count/warm_count/total/certifiable` signatures match the header across tasks; `vectorize`/`cosine`/`fnv1a`/`is_stop`/`stem`/`slugify`/`spill_coldest`/`tile_read`/`tile_write` used consistently; `BOOK_A`/`BOOK_B`/`NB`/`ingest_book` consistent.

---

## Execution note

Recommended: inline execution via `superpowers:executing-plans`. One production module + one real test file; deterministic checks; the real-PDF task auto-skips if the file is absent. Tune `τ` (fuzzy) and `hot_cap` per the in-task notes if a proof's numbers are off.

---

## Implementation notes (executed inline — 2026-06-26)

`make tiermem_test` = **18/18**, no warnings; `make pdftest` still 19/19 (additive,
zero core edits). Measured:
- **Fuzzy upgrade:** `reuse exact=2 fuzzy=4` — fuzzy cosine catches the 2 paraphrases the
  exact match misses (the AICIMO win).
- **Residency on a real book:** ingest `aivalueplaybook.pdf` → **HOT pinned at 500**,
  **WARM=5548 on disk, total=6048** (899 in-book fuzzy-reuses). "Not all in memory," proven.
- **Semantic recall:** query *"value of data and teams"* returned relevant book sentences
  (*"importance of data engineering teams…"*, *"…executive team helped drive the data…"*).
- **Persistence:** durable across open/close; re-ingest fully deduped (idempotent).

Deviations / notes:
- Wrote the **complete module** in one pass (not skeleton-then-extend) — the test CHECKs
  localize failures; faster inline.
- **Search value lifetime** via an internal `res` arena (strdup'd, freed each search/close)
  instead of the plan's promote-WARM-into-HOT — simpler and correct; **WARM promotion-on-hit
  is deferred** (read-only WARM scan returns hits without mutating HOT).
- `reset_store()` uses portable `remove()` of `hot.bin`/`warm.bin` (not `system("rm -rf")`).
- Warning cleanups: 3 misleading-indentation splits.
- Deferred (noted, not placeholders): true **mmap** WARM (CNET's WARM cascade view is the
  production path); full **`btn_certify` graduation** (the `tilemem_certifiable` count is the
  bridge surface).
- Purely additive: new `src/corpus/tile_memory.c`, `tests/test_tile_memory.c`, `tiermem_test`
  target. No existing source modified.
