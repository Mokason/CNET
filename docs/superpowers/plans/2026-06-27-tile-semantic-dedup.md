# PPMI Semantic-Dedup via Batch Consolidation (Lever 6) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add `tilemem_consolidate()` — an opt-in batch pass that merges paraphrase HOT tiles (synonym-aware soft-Jaccard ≥ `tau_sem`, both ≥ `min_terms`) using the PPMI map for similarity and the inverted index to find candidates cheaply.

**Architecture:** Factor the existing HOT-removal into `remove_hot_by_tid`; add `coverage`/`sim` helpers over `syn_neighbors`; add `tilemem_consolidate` that snapshots live HOT tids, gathers candidates per tile from postings (of its terms + their PPMI neighbors), and merges paraphrases (keep the more-supported tile, absorb the other's `count`, remove the loser). HOT-only, opt-in, conservative `min`-coverage guard.

**Tech Stack:** C11, MinGW gcc, `make` (Windows). No git on CNET (verify via filesystem + tests; never run git). Spec: `docs/superpowers/specs/2026-06-27-tile-semantic-dedup-design.md`.

**Note on "Commit"/"checkpoint":** No git in this repo. A passing build/test is the checkpoint. Do not run `git`.

---

## File Structure

- **Modify** `include/corpus/tile_memory.h` — `ConsolidateReport` struct + `tilemem_consolidate` prototype.
- **Modify** `src/corpus/tile_memory.c` — `remove_hot_by_tid` (refactor `tilemem_evict_containing` onto it); `term_in`/`coverage` helpers; `tilemem_consolidate`.
- **Create** `tests/test_consolidate.c` + a `make consolidate` target — deterministic unit test + real-book report.
- **Modify** `Makefile` — `CONSOLIDATE_TEST` var, `consolidate` target, `.PHONY` + `clean` entries.

---

## Task 1: Consolidation pass + deterministic unit test

**Files:**
- Modify: `include/corpus/tile_memory.h`
- Modify: `src/corpus/tile_memory.c`
- Create: `tests/test_consolidate.c`
- Modify: `Makefile`

- [ ] **Step 1: Declare the report + API in the header**

In `include/corpus/tile_memory.h`, before the final `#endif`, add:

```c
/* ---- Lever 6: opt-in semantic consolidation (merge paraphrase tiles via the PPMI map) ---- */
typedef struct { size_t tiles_before, tiles_after, merges; double ms; } ConsolidateReport;
/* Merge paraphrase HOT tiles whose bidirectional synonym-aware soft-coverage >= tau_sem and
   both have >= min_terms distinct terms. Builds the PPMI map if stale; uses the inverted index
   to find candidates. Returns the number of merges; rep may be NULL. HOT-only. */
size_t tilemem_consolidate(TileMemory *m, double tau_sem, int min_terms, ConsolidateReport *rep);
```

- [ ] **Step 2: Write the failing unit test**

Create `tests/test_consolidate.c`:

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

static void test_consolidate_unit(void){
    reset_store("cons_ctl");
    TileMemory *m=tilemem_open("cons_ctl",256,1000,0.99);
    /* churn~attrition become PPMI neighbors via the two cooc tiles (fillers are df=1 -> no
       neighbors). P1/P2 are a churn/attrition paraphrase. subA/subB exercise the min-coverage
       subset guard (subB's extra unique terms drag coverage(subB->subA) below tau). D1/D2 are
       unrelated and must survive. */
    const char *docs[]={
        "churn attrition foo bar",          /* cooc1 */
        "churn attrition baz qux",          /* cooc2 */
        "churn rises sharply",              /* P1 */
        "attrition rises sharply",          /* P2  (paraphrase of P1) */
        "weather sunny today",              /* D1 */
        "market crashed hard",              /* D2 */
        "alpha beta gamma",                 /* subA */
        "alpha beta gamma delta epsilon zeta eta theta" /* subB (superset of subA) */
    };
    for(int i=0;i<8;i++) tilemem_ingest(m,docs[i],docs[i],"","c");
    CHECK(tilemem_total(m)==8,"8 tiles ingested (no lexical dedup collisions)");

    ConsolidateReport rep;
    size_t merges=tilemem_consolidate(m,0.7,3,&rep);
    CHECK(merges==1,"exactly one merge (the churn/attrition paraphrase)");
    CHECK(rep.tiles_before==8 && rep.tiles_after==7,"report: 8 -> 7 tiles");
    CHECK(tilemem_total(m)==7,"one tile merged away");

    TileHit h[8];
    CHECK(tilemem_search(m,"rises sharply",8,h,8)==1,"P1/P2 collapsed to a single tile");
    CHECK(tilemem_search(m,"weather sunny",8,h,8)>=1,"unrelated D1 survived");
    CHECK(tilemem_search(m,"market crashed",8,h,8)>=1,"unrelated D2 survived");
    CHECK(tilemem_search(m,"alpha beta gamma",8,h,8)==2,"subset pair NOT merged (min-coverage guard)");
    tilemem_close(m);
}

int main(void){
    printf("=== test_consolidate ===\n");
    test_consolidate_unit();
    printf("\n%d/%d checks passed\n", g_checks-g_fail, g_checks);
    return g_fail?1:0;
}
```

In `Makefile`: after `TILEINDEX_TEST` add `CONSOLIDATE_TEST := tests/test_consolidate.c`. Add `consolidate` to `.PHONY`. After the `tileindex:` target add:

```make
# Lever 6: opt-in PPMI semantic consolidation (merge paraphrase tiles).
consolidate: $(TILEMEM_SRC) $(SYNONYMS_SRC) $(CORPUS_SRC) $(PDF_SRC) $(CONSOLIDATE_TEST) include/corpus/tile_memory.h include/corpus/synonyms.h include/corpus/corpus_split.h include/pdf/pdf_extract.h
	$(CC) $(CFLAGS) -Wno-unused-function -o $@ $(TILEMEM_SRC) $(SYNONYMS_SRC) $(CORPUS_SRC) $(PDF_SRC) $(CONSOLIDATE_TEST) $(LDFLAGS)
	./consolidate
```

Add `consolidate` to the `clean` `rm -f` list and `cons_ctl cons_book` to the `rm -rf` list.

- [ ] **Step 3: Run to verify it fails (no implementation)**

Run: `make consolidate`
Expected: **link error** — `undefined reference to 'tilemem_consolidate'`.

- [ ] **Step 4: Factor `remove_hot_by_tid` and refactor eviction**

In `src/corpus/tile_memory.c`, add this helper (place it after the `loc_*`/`seen_ensure` helpers, before `tilemem_search`):

```c
/* Remove the live HOT tile with this tid: free, swap the last HOT tile into its slot (fixing
   that moved tile's location), and mark the tid dead. No-op if tid isn't a live HOT tile. */
static void remove_hot_by_tid(TileMemory *m, unsigned tid){
    if(!loc_live(m,tid) || m->loc[tid].tier!=0) return;
    int idx=(int)m->loc[tid].where;
    tile_free(&m->hot[idx]);
    int last=--m->n_hot; m->hot[idx]=m->hot[last];
    if(idx!=last) loc_set(m, m->hot[idx].tid, 0, (size_t)idx);
    loc_kill(m,tid);
}
```

Replace the body of `tilemem_evict_containing`:

```c
size_t tilemem_evict_containing(TileMemory *m, const char *needle){
    if(!needle||!*needle) return 0;
    size_t ev=0;
    for(int i=0;i<m->n_hot;){
        if(strstr(m->hot[i].key, needle)){ loc_kill(m,m->hot[i].tid); tile_free(&m->hot[i]);
            int last=--m->n_hot; m->hot[i]=m->hot[last]; if(i!=last) loc_set(m,m->hot[i].tid,0,(size_t)i); ev++; }
        else i++;
    }
    return ev;
}
```

with the DRY version:

```c
size_t tilemem_evict_containing(TileMemory *m, const char *needle){
    if(!needle||!*needle) return 0;
    size_t ev=0;
    for(int i=0;i<m->n_hot;){
        if(strstr(m->hot[i].key, needle)){ remove_hot_by_tid(m, m->hot[i].tid); ev++; }   /* swaps last into i */
        else i++;
    }
    return ev;
}
```

(`remove_hot_by_tid(hot[i].tid)` resolves `idx==i` via `loc`, frees, and swaps the last tile into slot `i`; the loop does not advance so it re-checks the moved tile — identical behavior to before.)

- [ ] **Step 5: Add the coverage helper**

Add above `tilemem_consolidate` (after `score_tile` is fine):

```c
static int term_in(const char *t, const char **ty, int ny){
    for(int i=0;i<ny;i++) if(strcmp(t,ty[i])==0) return 1;
    return 0;
}
/* fraction of TX terms covered by TY: exact match, or a top-k PPMI neighbor of the term is in TY */
static double coverage(TileMemory *m, const char **tx, int nx, const char **ty, int ny){
    if(nx==0) return 0.0;
    int c=0;
    for(int i=0;i<nx;i++){
        if(term_in(tx[i],ty,ny)){ c++; continue; }
        const char *nb[16]; float pp[16]; int kk=m->syn_k<16? m->syn_k:16;
        int got=m->syn? syn_neighbors(m->syn,tx[i],nb,pp,kk):0;
        int hit=0; for(int z=0;z<got;z++) if(term_in(nb[z],ty,ny)){ hit=1; break; }
        if(hit) c++;
    }
    return (double)c/(double)nx;
}
```

- [ ] **Step 6: Implement `tilemem_consolidate`**

Add (near the other public functions, e.g. after `tilemem_set_discount`):

```c
size_t tilemem_consolidate(TileMemory *m, double tau_sem, int min_terms, ConsolidateReport *rep){
    if(min_terms<1) min_terms=1;
    clock_t t0=clock();
    if(m->syn_dirty) tilemem_build_synonyms(m);     /* ensure the PPMI map exists */
    size_t before=(size_t)m->n_hot + m->warm_count;
    /* snapshot live HOT tids so the outer loop is stable while tiles are removed */
    int ns=m->n_hot;
    unsigned *tids=(unsigned*)malloc((size_t)(ns>0?ns:1)*sizeof(unsigned));
    for(int i=0;i<m->n_hot;i++) tids[i]=m->hot[i].tid;
    size_t merges=0;
    for(int s=0;s<ns;s++){
        unsigned at=tids[s];
        if(!loc_live(m,at) || m->loc[at].tier!=0) continue;   /* already merged away */
        char ta_store[256][32]; const char *TA[256];
        int na=tile_distinct_terms(m->hot[(int)m->loc[at].where].key, ta_store, TA, 256);
        if(na<min_terms) continue;
        /* gather candidate tids via postings of TA's terms AND their PPMI neighbors */
        m->seen_epoch++; seen_ensure(m);
        unsigned *cand=NULL; int cn=0, cc=0;
        for(int i=0;i<na;i++){
            for(int pass=0;pass<2;pass++){
                int pn=0; unsigned *pl=NULL;
                if(pass==0){ pl=tdf_postings(&m->tdf,TA[i],&pn); }
                else { const char *nb[16]; float pp[16]; int kk=m->syn_k<16? m->syn_k:16;
                       int got=m->syn? syn_neighbors(m->syn,TA[i],nb,pp,kk):0;
                       for(int z=0;z<got;z++){ int qn; unsigned *ql=tdf_postings(&m->tdf,nb[z],&qn);
                           for(int q=0;q<qn;q++){ unsigned tid=ql[q];
                               if(tid==at||!loc_live(m,tid)||m->loc[tid].tier!=0) continue;
                               if(m->seen[tid]==m->seen_epoch) continue; m->seen[tid]=m->seen_epoch;
                               if(cn==cc){ cc=cc?cc*2:16; cand=(unsigned*)realloc(cand,(size_t)cc*sizeof(unsigned)); }
                               cand[cn++]=tid; } }
                       continue; }
                for(int p=0;p<pn;p++){ unsigned tid=pl[p];
                    if(tid==at||!loc_live(m,tid)||m->loc[tid].tier!=0) continue;
                    if(m->seen[tid]==m->seen_epoch) continue; m->seen[tid]=m->seen_epoch;
                    if(cn==cc){ cc=cc?cc*2:16; cand=(unsigned*)realloc(cand,(size_t)cc*sizeof(unsigned)); }
                    cand[cn++]=tid; }
            }
        }
        for(int ci=0; ci<cn; ci++){
            unsigned ct=cand[ci];
            if(!loc_live(m,ct) || m->loc[ct].tier!=0) continue;
            char tc_store[256][32]; const char *TC[256];
            int nc=tile_distinct_terms(m->hot[(int)m->loc[ct].where].key, tc_store, TC, 256);
            if(nc<min_terms) continue;
            double cab=coverage(m,TA,na,TC,nc), cba=coverage(m,TC,nc,TA,na);
            double sim=cab<cba?cab:cba;
            if(sim>=tau_sem){
                Tile *A=&m->hot[(int)m->loc[at].where];
                Tile *C=&m->hot[(int)m->loc[ct].where];
                if(A->count>=C->count){ A->count+=C->count; if(C->heat>A->heat) A->heat=C->heat; remove_hot_by_tid(m,ct); }
                else { C->count+=A->count; if(A->heat>C->heat) C->heat=A->heat; remove_hot_by_tid(m,at); }
                merges++;
                if(!loc_live(m,at)) break;   /* A itself was absorbed -> done with its candidates */
                /* TA still valid (tokenized into ta_store; A only moved in the array, not freed) */
            }
        }
        free(cand);
    }
    free(tids);
    if(rep){ rep->tiles_before=before; rep->tiles_after=(size_t)m->n_hot+m->warm_count;
             rep->merges=merges; rep->ms=1000.0*(double)(clock()-t0)/CLOCKS_PER_SEC; }
    return merges;
}
```

Add `#include <time.h>` near the top of `src/corpus/tile_memory.c` if not already present (for `clock`).

- [ ] **Step 7: Run the unit test**

Run: `make consolidate`
Expected: `8/8 checks passed`, warning-clean. (1 merge: the churn/attrition paraphrase; subset pair and unrelated tiles survive.)

If a check fails, debug:
- `merges != 1`: inspect which pair merged unexpectedly — likely an incidental PPMI neighbor (a filler term reached `df>=2`). Confirm filler terms are unique (df=1). Do NOT loosen the test to pass; fix the corpus or the logic.
- subset pair merged: the `min(coverage)` guard or `tile_distinct_terms` is wrong.

- [ ] **Step 8: Confirm the evict refactor didn't regress**

Run: `make tiermem_test` (18/18), `make graduate` (9/9 — it calls `tilemem_evict_containing`), `make tileindex` (24/24).
Expected: all green — `remove_hot_by_tid` must behave exactly like the old eviction loop.

---

## Task 2: Real-book consolidation report + full regression

**Files:**
- Modify: `tests/test_consolidate.c`

- [ ] **Step 1: Add the book report (auto-skips if absent)**

In `tests/test_consolidate.c`, add the corpus/pdf includes after the tile_memory include:

```c
#include "../include/corpus/corpus_split.h"
#include "../include/pdf/pdf_extract.h"
```

Add above `main`:

```c
static unsigned char *readfile(const char *p,size_t*len){ FILE*f=fopen(p,"rb"); if(!f)return NULL;
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET); if(sz<=0){fclose(f);return NULL;}
    unsigned char*b=malloc((size_t)sz); *len=fread(b,1,(size_t)sz,f); fclose(f); return b; }

static void test_book_report(void){
    const char *path="C:/Users/Mokason/Downloads/aivalueplaybook.pdf";
    FILE *pr=fopen(path,"rb"); if(!pr){ printf("[book] not found - skipping report\n"); return; } fclose(pr);
    size_t len=0; unsigned char *buf=readfile(path,&len); if(!buf) return;
    char *text=malloc(len*4+16); size_t tl=0;
    if(pdf_extract_text(buf,len,text,len*4+16,&tl)!=PDF_OK){ free(text); free(buf); return; }
    StrList s; strlist_init(&s); corpus_split(text,&s);
    reset_store("cons_book");
    TileMemory *m=tilemem_open("cons_book",256,9000,0.6);   /* large hot_cap: tiles resident for consolidation */
    for(size_t i=0;i<s.count;i++) if(corpus_quality_keep(s.lines[i])) tilemem_ingest(m,s.lines[i],s.lines[i],"","book");
    ConsolidateReport rep;
    size_t merges=tilemem_consolidate(m,0.7,3,&rep);
    printf("\n[bench] consolidate %.0f ms ; tiles %zu -> %zu ; merges=%zu\n",
           rep.ms, rep.tiles_before, rep.tiles_after, merges);
    /* sanity: never increases the tile count; at most removes one per merge */
    CHECK(rep.tiles_after<=rep.tiles_before,"consolidation never grows the store");
    CHECK(rep.tiles_before-rep.tiles_after==merges,"each merge removes exactly one tile");
    tilemem_close(m);
    free(text); free(buf); strlist_free(&s);
}
```

Add `test_book_report();` in `main` after `test_consolidate_unit();`.

- [ ] **Step 2: Run and capture the report**

Run: `make consolidate`
Expected: `10/10 checks passed` (8 + 2) with the book, or `8/8` + skip line without it. Capture the `[bench] consolidate ...` line (tiles before→after, merges, ms) for the report.

- [ ] **Step 3: Full regression sweep**

Run: `make consolidate`, `make tileindex`, `make tiermem_test`, `make synonyms`, `make tfidf`, `make graduate`, `make fontdecode`, `make pdftest`.
Expected: all green (`tileindex` 24/24, `tiermem_test` 18/18, `synonyms` 25/25, `tfidf` 3/3, `graduate` 9/9, `fontdecode` 20/20, `pdftest` 19/19). `graduate`'s pre-existing `src/router/*` warnings are expected. If any regress, stop and debug (superpowers:systematic-debugging).

---

## Self-Review

**Spec coverage:**
- §1.1 semantic merge via bidirectional coverage → Task 1 Step 6. ✓
- §1.2 min-guard + min_terms + report → Task 1 Steps 5–6 + unit test (subset pair). ✓
- §1.3 candidates via inverted index + PPMI neighbors → Task 1 Step 6 (postings of terms + neighbors). ✓
- §1.4 opt-in, zero default change, regressions green → Task 1 Step 8 + Task 2 Step 3. ✓
- §3 soft-Jaccard metric → Task 1 Step 5. ✓
- §4 mechanism incl. `remove_hot_by_tid`, snapshot iteration, re-fetch after removal → Task 1 Steps 4,6. ✓
- §5 components (header + .c + test) → Tasks 1–2. ✓
- §6 unit (paraphrase merge, distinct survive, subset no-merge) + book report → Tasks 1–2. ✓
- §7 knobs (tau_sem, min_terms) → Task 1 Step 6 signature. ✓

**Placeholder scan:** No TBD/TODO; complete code in every code step. ✓

**Type consistency:** `tilemem_consolidate(TileMemory*,double,int,ConsolidateReport*)` matches header (Step 1), impl (Step 6), and both test call sites. `ConsolidateReport{size_t tiles_before,tiles_after,merges; double ms;}` consistent. `remove_hot_by_tid(TileMemory*,unsigned)`, `coverage(TileMemory*,const char**,int,const char**,int)`, `term_in(const char*,const char**,int)` consistent across uses. Reuses existing `tile_distinct_terms`, `tdf_postings`, `syn_neighbors`, `loc_live`, `seen_ensure`, `seen_epoch`. ✓

---

## Project notes

- **No git on CNET** — never run git; verify via filesystem + `make`.
- Composes Levers 3/4 (PPMI map) + 5 (inverted-index candidate gather). Touches only `tile_memory.{c,h}` + new `tests/test_consolidate.c` + `Makefile`. **Zero core/router/contract edits.**
