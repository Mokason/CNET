# TF-IDF Retrieval (Lever 2) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make tile-memory retrieval idf-weighted so rare distinctive terms decide the match — fixing the "last mile" miss where common words (`AI`/`problem`/`project`) drowned the rare `mile`.

**Architecture:** `tile_memory` accumulates document frequency `df[dim]` + `doc_count` on ingest (persisted in `idf.bin`); `tilemem_search` scores with idf-weighted cosine (weight query + each tile vec by `idf=log((N+1)/(df+1))+1`, then cosine). Ingest/dedup keep plain cosine. A test proves the last-mile fix + benchmarks. Zero core edits.

**Tech Stack:** C11 (gcc, `-mno-avx`), stdlib + libm.

**Project note — NO GIT:** `.git` removed. "Commit" → checkpoint: re-run `make tfidf`.

---

## File Structure

- **Modify `include/corpus/tile_memory.h`** — no API change (internal df fields added in `.c`); behavior of `tilemem_search` upgraded.
- **Modify `src/corpus/tile_memory.c`** — `df`/`doc_count` fields, alloc/free, ingest update, `idf.bin` persistence, idf-weighted search.
- **Create `tests/test_tfidf.c`** — controlled flip test + real-book last-mile proof + benchmarks.
- **Modify `Makefile`** — `TFIDF_TEST` var, `tfidf` target, `.PHONY`, `clean`.

`CHECK` macro in the test:
```c
static int g_fail=0, g_checks=0;
#define CHECK(c,m) do{ g_checks++; if(!(c)){ g_fail++; printf("  FAIL: %s (%s:%d)\n",m,__FILE__,__LINE__);} }while(0)
```

---

## Task 1: df accumulation + persistence

**Files:**
- Modify: `src/corpus/tile_memory.c`

- [ ] **Step 1: Add `df`/`doc_count` to the struct**

In `struct TileMemory` (after `size_t warm_count;`):
```c
    unsigned *df; size_t doc_count;   /* document frequency per bucket + tile count (idf) */
```

- [ ] **Step 2: `idf.bin` path + save/load + alloc/free**

Add a path helper near `warm_path`/`hot_path`:
```c
static void idf_path(const TileMemory *m, char *out, size_t cap){ snprintf(out,cap,"%s/idf.bin",m->store_dir); }
```
In `tilemem_load` (after the WARM count loop, before the closing `}`):
```c
    char ip[300]; idf_path(m,ip,sizeof(ip)); FILE *h=fopen(ip,"rb");
    if(h){ size_t dc; int d;
        if(fread(&d,sizeof(int),1,h)==1 && d==m->dim && fread(&dc,sizeof(size_t),1,h)==1){
            m->doc_count=dc; if(fread(m->df,sizeof(unsigned),(size_t)m->dim,h)!=(size_t)m->dim){ m->doc_count=0; memset(m->df,0,(size_t)m->dim*sizeof(unsigned)); } }
        fclose(h); }
```
In `tilemem_save` (after the hot.bin write):
```c
    char ip[300]; idf_path(m,ip,sizeof(ip)); FILE *h=fopen(ip,"wb");
    if(h){ fwrite(&m->dim,sizeof(int),1,h); fwrite(&m->doc_count,sizeof(size_t),1,h);
        fwrite(m->df,sizeof(unsigned),(size_t)m->dim,h); fclose(h); }
```
In `tilemem_open` (after `m->dim=dim; ...`, before `tilemem_load(m)`):
```c
    m->df=(unsigned*)calloc((size_t)dim,sizeof(unsigned));
```
In `tilemem_close` (before `free(m);`):
```c
    free(m->df);
```

- [ ] **Step 3: Update df on a NEW tile in `tilemem_ingest`**

After the new tile is populated (`t->heat=1; t->count=1;`), before `return 1;`:
```c
    for(int i=0;i<m->dim;i++) if(qv[i]>0.0f) m->df[i]++;
    m->doc_count++;
```
(qv is the new tile's vec; reuse path returned earlier, so df only counts new tiles.)

- [ ] **Step 4: Build via the existing suite (no behavior change yet)**

Run: `make tiermem_test`
Expected: still `18/18 checks passed` (df is accumulated but search is unchanged so far; the idf.bin write is new but harmless).

- [ ] **Step 5: Checkpoint (no git)** — re-run `make tiermem_test`.

---

## Task 2: idf-weighted search + proof + benchmarks

**Files:**
- Modify: `src/corpus/tile_memory.c`
- Create: `tests/test_tfidf.c`
- Modify: `Makefile`

- [ ] **Step 1: Add an idf-weighted cosine and use it in `tilemem_search`**

Add near `cosine` (after it):
```c
#include <math.h>   /* already included at top; harmless if duplicated via guard */
static float idf_cosine(const float *qw, const float *tv, const float *idf, int dim){
    double dot=0, tn=0;
    for(int i=0;i<dim;i++){ float tw=tv[i]*idf[i]; dot+=(double)qw[i]*tw; tn+=(double)tw*tw; }
    return tn>0.0 ? (float)(dot/sqrt(tn)) : 0.0f;   /* qw is pre-normalized */
}
```
Replace the body of `tilemem_search` from the `float *qv=...` line through `free(qv); return n;` with:
```c
    float *qv=(float*)malloc((size_t)m->dim*sizeof(float)); vectorize(query,qv,m->dim);
    /* idf + idf-weighted, normalized query */
    float *idf=(float*)malloc((size_t)m->dim*sizeof(float));
    for(int i=0;i<m->dim;i++) idf[i]=(float)(log(((double)m->doc_count+1.0)/((double)m->df[i]+1.0))+1.0);
    float *qw=(float*)malloc((size_t)m->dim*sizeof(float));
    double qn=0; for(int i=0;i<m->dim;i++){ qw[i]=qv[i]*idf[i]; qn+=(double)qw[i]*qw[i]; }
    if(qn>0){ float inv=1.0f/(float)sqrt(qn); for(int i=0;i<m->dim;i++) qw[i]*=inv; }
    int lim=topK<cap?topK:cap; int n=0;
    for(int i=0;i<m->n_hot;i++){ float c=idf_cosine(qw,m->hot[i].vec,idf,m->dim);
        if(c>0){ topk_insert(m,out,&n,lim,c,m->hot[i].id,m->hot[i].label,m->hot[i].source,m->hot[i].value); m->hot[i].heat++; } }
    char wp[300]; warm_path(m,wp,sizeof(wp)); FILE *f=fopen(wp,"rb");
    if(f){ Tile t; while(tile_read(f,&t,m->dim)==0){
        float c=idf_cosine(qw,t.vec,idf,m->dim);
        topk_insert(m,out,&n,lim,c,t.id,t.label,t.source,t.value);
        tile_free(&t); } fclose(f); }
    free(qv); free(idf); free(qw); return n;
```
(`tilemem_ingest`/`hot_best` still use the plain `cosine` for fuzzy-dedup — unchanged.)

- [ ] **Step 2: Build — regression first**

Run: `make tiermem_test`
Expected: `18/18 checks passed`. (`test_search` only asserts a positive top score; idf-weighted cosine still yields positive scores.)

- [ ] **Step 3: The proof test `tests/test_tfidf.c`**

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../include/corpus/tile_memory.h"
#include "../include/corpus/corpus_split.h"
#include "../include/pdf/pdf_extract.h"

static int g_fail=0, g_checks=0;
#define CHECK(c,m) do{ g_checks++; if(!(c)){ g_fail++; printf("  FAIL: %s (%s:%d)\n",m,__FILE__,__LINE__);} }while(0)
static void reset_store(const char *dir){ char p[300];
    snprintf(p,sizeof(p),"%s/hot.bin",dir); remove(p);
    snprintf(p,sizeof(p),"%s/warm.bin",dir); remove(p);
    snprintf(p,sizeof(p),"%s/idf.bin",dir); remove(p); }

/* common term "ai" in many tiles, rare term "mile" in one -> idf must surface the rare. */
static void test_controlled(void){
    reset_store("tfidf_ctl");
    TileMemory *m=tilemem_open("tfidf_ctl",256,1000,0.99);
    const char *ai[]={"ai topic alpha","ai topic beta","ai topic gamma","ai topic delta",
                      "ai topic epsilon","ai topic zeta","ai topic eta","ai topic theta"};
    for(int i=0;i<8;i++) tilemem_ingest(m,ai[i],ai[i],"","c");
    tilemem_ingest(m,"mile zone region","mile zone region","","c");
    TileHit h[3]; int n=tilemem_search(m,"ai mile",3,h,3);
    CHECK(n>0,"search returns hits");
    CHECK(n>0 && strstr(h[0].value,"mile")!=NULL,"idf surfaces the rare-term tile first (not an ai-heavy tile)");
    tilemem_close(m);
}

static unsigned char *readfile(const char *p,size_t*len){ FILE*f=fopen(p,"rb"); if(!f)return NULL;
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET); if(sz<=0){fclose(f);return NULL;}
    unsigned char*b=malloc(sz); *len=fread(b,1,sz,f); fclose(f); return b; }

static void test_last_mile(void){
    const char *path="C:/Users/Mokason/Downloads/aivalueplaybook.pdf";
    FILE *pr=fopen(path,"rb"); if(!pr){ printf("[book] not found - skipping\n"); return; } fclose(pr);
    size_t len=0; unsigned char *buf=readfile(path,&len);
    char *text=malloc(len*4+16); size_t tl=0;
    if(pdf_extract_text(buf,len,text,len*4+16,&tl)!=PDF_OK){ free(text); free(buf); return; }
    StrList s; strlist_init(&s); corpus_split(text,&s);
    reset_store("tfidf_book");
    TileMemory *m=tilemem_open("tfidf_book",256,9000,0.6);
    clock_t t0=clock();
    for(size_t i=0;i<s.count;i++) if(corpus_quality_keep(s.lines[i])) tilemem_ingest(m,s.lines[i],s.lines[i],"","book");
    double ing=1000.0*(double)(clock()-t0)/CLOCKS_PER_SEC;

    TileHit h[5]; t0=clock(); int n=tilemem_search(m,"what is the last mile problem in ai projects",5,h,5);
    double q=1000.0*(double)(clock()-t0)/CLOCKS_PER_SEC;
    printf("\n[bench] ingest(df build) %.0f ms ; search(idf) %.2f ms ; tiles=%zu\n", ing, q, tilemem_total(m));
    printf("[lastmile] top-%d for 'last mile problem in ai projects':\n", n);
    int found=0;
    for(int i=0;i<n;i++){ printf("   [%.3f] %.90s\n", h[i].score, h[i].value); if(strstr(h[i].value,"last mile")) found=1; }
    CHECK(found,"a 'last mile' sentence is now in the top-5 (idf fixed the miss)");
    tilemem_close(m);
    free(text); free(buf); strlist_free(&s);
}

int main(void){
    printf("=== test_tfidf ===\n");
    test_controlled();
    test_last_mile();
    printf("\n%d/%d checks passed\n", g_checks-g_fail, g_checks);
    return g_fail?1:0;
}
```
Makefile: add `TFIDF_TEST := tests/test_tfidf.c` near the other test vars, and the target:
```make
tfidf: $(TILEMEM_SRC) $(CORPUS_SRC) $(PDF_SRC) $(TFIDF_TEST) include/corpus/tile_memory.h include/corpus/corpus_split.h include/pdf/pdf_extract.h
	$(CC) $(CFLAGS) -Wno-unused-function -o $@ $(TILEMEM_SRC) $(CORPUS_SRC) $(PDF_SRC) $(TFIDF_TEST) $(LDFLAGS)
	./tfidf
```
Add `tfidf` to `.PHONY` and the `clean` rm list; add `tfidf_ctl tfidf_book` to the `rm -rf` line.

- [ ] **Step 4: Build and run (acceptance + benchmarks)**

Run: `make tfidf; echo "exit=$?"`
Expected: `test_controlled` passes (rare-term tile first); `[bench] ingest … search(idf) … ms`; `[lastmile] top-5 …` listing with at least one line containing `last mile`; `last mile sentence now in top-5` check passes; `N/N checks passed`, `exit=0`.
If the controlled flip fails: the 8 `ai topic *` tiles may dedup (cosine ≥ 0.99) — they share "ai topic" but differ in the third word, so cosine < 0.99; if not, lower the shared prefix.

- [ ] **Step 5: Downstream regression** — Run `make tiermem_test`, `make graduate`, `make fontdecode`, `make pdftest`; all green.

- [ ] **Step 6: Checkpoint (no git)** — re-run `make tfidf`.

---

## Self-Review (against the spec)

**Spec coverage:**
- §1.1 idf-weighted retrieval (df + idf cosine) → Tasks 1–2. ✓
- §1.2 fixes the last-mile miss → Task 2 `test_last_mile`. ✓
- §1.3 no regression → Task 1 Step 4 + Task 2 Step 2/5. ✓
- §1.4 benchmarks → Task 2 `[bench]`. ✓
- §3 mechanism (df on new tile, idf at score, persisted) → Tasks 1–2. ✓
- §4 components (tile_memory edits, test_tfidf) → Tasks 1–2. ✓
- §8 limits respected (still BoW; monotonic df). ✓

**Placeholder scan:** none — complete code + exact commands. The dedup-flip note is a diagnostic contingency.

**Type consistency:** `df`/`doc_count` used consistently; `idf_cosine(qw,tv,idf,dim)` matches its call; `idf_path`/`tilemem_save`/`tilemem_load`/`tilemem_open`/`tilemem_close` edits align; `tilemem_search` signature unchanged (internal upgrade); `corpus_quality_keep` reused from Lever 1.

---

## Execution note

Recommended: inline execution via `superpowers:executing-plans`. Task 1 only accumulates df (no behavior change — regression stays green); Task 2 flips search to idf-weighted and proves the last-mile fix on the book. The book task auto-skips if absent.

---

## Implementation notes (executed inline — 2026-06-26) — INCLUDES A PIVOT

The plan's approach (idf on the existing **dense dim=256 hashed** vecs) **does not work**,
found by running it:
- **Small dim → idf is blind.** ~12k terms over 256 buckets = ~47 terms/bucket, so
  `df[bucket]` is the union of many terms' frequencies — idf can't isolate the rare `mile`.
  The last-mile query stayed a miss.
- **Large dim → ingest explodes.** Bumping dim to 16384 makes fuzzy-dedup's `hot_best`
  (a full HOT scan per ingest) **O(n²·dim)** → the run timed out.

**Pivot (user-approved): term-dictionary TF-IDF.** Replaced the per-bucket `df[256]` with a
real **`term → df` open-addressing string hash** (`TermDF`), no collisions. Ingest bumps df
once per distinct tile term. Search = **TF-IDF term-overlap**: tokenize the query + each
tile's key, score = Σ `idf(term)` over the distinct query terms a tile contains, length-
normalized by `sqrt(tile_terms+1)`. The dense vec/cosine stays for **fuzzy-dedup only**
(cheap, dim=256). Persisted as text (`doc_count` + `term df` lines in `idf.bin`).

**Result:** `make tfidf` = **3/3**, warning-clean, zero core edits; downstream all green
(tiermem 18/18, graduate 9/9, fontdecode 20/20, pdftest 19/19). The query *"last mile
problem in ai projects"* now returns **4 of 5** top hits about the last mile (was 0):
top `[4.643] "the concept of the last mile has been essential in our change process"`.
Bench: ingest(df build) 1687 ms, search 9 ms over 5127 tiles. Full 8-question Q&A sharpened
across the board.

**Honest:** still lexical (no synonyms — the deferred co-occurrence/embedding layer);
`df` monotonic (not decremented on evict). The dead-end lesson: **dense hashed BoW cannot
support idf at scale** — use a real term dictionary. The spec's dense-idf mechanism is
superseded by this term-dict design.
