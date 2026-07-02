# Discounted PPMI Neighbors (Lever 4) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Suppress spurious rare-term neighbors in the PPMI synonym map by discounting each edge's PMI by a support factor (Pantel–Lin), so garbled-OCR / rare-coincidence neighbors shrink while strong collocations are preserved.

**Architecture:** Add an `int discount` flag to `syn_finalize`; when set, multiply `pmi` by `(c/(c+1))·(mₙ/(mₙ+1))` with `mₙ=min(df_i,df_j)`. Thread a `syn_discount` field (default on) through `tile_memory` so the built map is discounted by default, with a `tilemem_set_discount` toggle for the before/after benchmark. Pure corpus statistics — no dictionary, no external data.

**Tech Stack:** C11, MinGW gcc, `make` (Windows). No git on CNET (verify via filesystem + tests; never run git). Spec: `docs/superpowers/specs/2026-06-26-discounted-ppmi-neighbors-design.md`.

**Note on "Commit" steps:** This repo has **no git**. Wherever a generic TDD flow would commit, instead the passing `make synonyms` build/test is the checkpoint. Do not run `git`.

---

## File Structure

- **Modify** `include/corpus/synonyms.h` — add `int discount` as the final parameter of `syn_finalize`.
- **Modify** `src/corpus/synonyms.c` — apply the support factor inside `syn_finalize`'s pair loop.
- **Modify** `include/corpus/tile_memory.h` — declare `tilemem_set_discount`.
- **Modify** `src/corpus/tile_memory.c` — `syn_discount` field (default 1); pass it in `tilemem_build_synonyms`; add `tilemem_set_discount`.
- **Modify** `tests/test_synonyms.c` — pass `discount=0` in the two existing `syn_finalize` calls (keep raw-PMI assertions); add `test_discount` (unit); replace `test_book_bench` with a discount-off vs discount-on A/B.

No Makefile change — the `synonyms` target already compiles all these files (and Lever 3 already added `$(SYNONYMS_SRC)` to the other tile_memory-linked targets).

**Check-count bookkeeping:** the suite currently has **19** checks with the book present (17 without). Task 1 adds `test_discount` = **+6**, and changing the two existing `syn_finalize` calls keeps their assertions intact. So after Task 1: **25/25** (book) / **23/23** (no book). Task 2's A/B adds no new checks.

---

## Task 1: Discount the PPMI score (mechanism + unit proof)

This task changes the `syn_finalize` signature, so **every** call site must be updated in the same task or the build breaks (the `synonyms` target links `synonyms.c`, `tile_memory.c`, and `test_synonyms.c` together).

**Files:**
- Modify: `include/corpus/synonyms.h`
- Modify: `src/corpus/synonyms.c`
- Modify: `include/corpus/tile_memory.h`
- Modify: `src/corpus/tile_memory.c`
- Modify: `tests/test_synonyms.c`

- [ ] **Step 1: Write the failing unit test**

In `tests/test_synonyms.c`, add this function above `main` (it reuses the existing `df_set`/`df_lookup`/`nbr_weight` helpers from Task 1 of the Lever-3 plan):

```c
static void test_discount(void){
    /* (a,b) co-occur 4x with large marginals; (a,r) co-occur 2x with r rare.
       Chosen so raw PMI(a,b) == PMI(a,r) == log(6.25):
       4*100/(8*8) = 2*100/(8*4) = 6.25. Support-discounting must break that tie. */
    g_ndf=0; df_set("a",8); df_set("b",8); df_set("r",4);
    const char *ab[]={"a","b"}; const char *ar[]={"a","r"};

    /* discount OFF: equal raw PMI -> equal weights == log(6.25) */
    Synonyms *s0=syn_new();
    for(int i=0;i<4;i++) syn_observe_tile(s0,ab,2);
    for(int i=0;i<2;i++) syn_observe_tile(s0,ar,2);
    syn_finalize(s0, df_lookup, NULL, 100, 5, 2, 1.0, 1, 0);
    float wb0=nbr_weight(s0,"a","b"), wr0=nbr_weight(s0,"a","r");
    CHECK(wb0>0.0f && wr0>0.0f,"discount-off: both neighbors present");
    CHECK(fabsf(wb0-wr0)<0.01f,"discount-off: equal raw PMI -> equal weights");
    CHECK(fabsf(wb0-1.8326f)<0.01f,"discount-off: weight == raw PMI log(6.25)");
    syn_free(s0);

    /* discount ON: support factor reorders + shrinks the rare edge */
    Synonyms *s1=syn_new();
    for(int i=0;i<4;i++) syn_observe_tile(s1,ab,2);
    for(int i=0;i<2;i++) syn_observe_tile(s1,ar,2);
    syn_finalize(s1, df_lookup, NULL, 100, 5, 2, 1.0, 1, 1);
    float wb1=nbr_weight(s1,"a","b"), wr1=nbr_weight(s1,"a","r");
    CHECK(wb1>wr1,"discount-on: well-supported (a,b) outranks rare (a,r)");
    CHECK(wr1<wr0,"discount-on: rare edge (a,r) is strictly shrunk vs raw");
    CHECK(wb1<wb0,"discount-on: even the supported edge is scaled below raw PMI");
    syn_free(s1);
}
```

Add `test_discount();` in `main` immediately after `test_roundtrip();`.

- [ ] **Step 2: Run to verify it fails (compile error — signature mismatch)**

Run: `make synonyms`
Expected: **compile error** — `syn_finalize` is called with 10 args (the new `discount`) but still declared with 9. This confirms the test is wired in; fix by completing the signature change below.

- [ ] **Step 3: Update the `syn_finalize` declaration**

In `include/corpus/synonyms.h`, replace the `syn_finalize` prototype block:

```c
/* Raw counts -> PPMI -> keep top-k neighbors per term, then freeze.
   df_of(ctx,term) = # tiles containing term (global doc frequency); N = tile count.
   Eligible terms: pair count >= min_cooc AND df in [min_df, df_frac*N].
   discount: 0 = raw PPMI; 1 = multiply each edge by support (c/(c+1))*(min(df_i,df_j)/(min+1))
   to suppress spurious rare-term neighbors (Pantel-Lin discounting). */
void      syn_finalize(Synonyms *s,
                       unsigned (*df_of)(void *ctx, const char *term), void *ctx,
                       size_t N, int k, int min_cooc, double df_frac, int min_df, int discount);
```

- [ ] **Step 4: Apply the discount in `syn_finalize`**

In `src/corpus/synonyms.c`, change the function signature line from:

```c
void syn_finalize(Synonyms *s, unsigned (*df_of)(void*,const char*), void *ctx,
                  size_t N, int k, int min_cooc, double df_frac, int min_df){
```

to:

```c
void syn_finalize(Synonyms *s, unsigned (*df_of)(void*,const char*), void *ctx,
                  size_t N, int k, int min_cooc, double df_frac, int min_df, int discount){
```

Then, inside the pair loop, replace these two lines:

```c
        double pmi=log((double)c*Nd/((double)df[i]*(double)df[j]));
        if(pmi<=0.0) continue;
        topk_insert(s,i,s->term[j],(float)pmi,k);
        topk_insert(s,j,s->term[i],(float)pmi,k);
```

with:

```c
        double pmi=log((double)c*Nd/((double)df[i]*(double)df[j]));
        if(pmi<=0.0) continue;
        double score=pmi;
        if(discount){
            double mn=(df[i]<df[j])?(double)df[i]:(double)df[j];
            score = pmi * ((double)c/((double)c+1.0)) * (mn/(mn+1.0));
        }
        topk_insert(s,i,s->term[j],(float)score,k);
        topk_insert(s,j,s->term[i],(float)score,k);
```

- [ ] **Step 5: Add the `syn_discount` field, default, and setter to tile_memory**

In `src/corpus/tile_memory.c`, in the `struct TileMemory { ... }` definition, change the synonym-knobs line:

```c
    double syn_df_frac, syn_pmi_scale;
```

to:

```c
    double syn_df_frac, syn_pmi_scale; int syn_discount;
```

In `tilemem_open`, find the synonym defaults line:

```c
    m->syn_df_frac=0.5; m->syn_max_expand=64; m->syn_pmi_scale=5.0; m->syn_dirty=0;
```

and change it to:

```c
    m->syn_df_frac=0.5; m->syn_max_expand=64; m->syn_pmi_scale=5.0; m->syn_discount=1; m->syn_dirty=0;
```

In `tilemem_build_synonyms`, change the `syn_finalize` call:

```c
    syn_finalize(m->syn, tm_df_of, m, m->doc_count,
                 m->syn_k, m->syn_min_cooc, m->syn_df_frac, m->syn_min_df);
```

to:

```c
    syn_finalize(m->syn, tm_df_of, m, m->doc_count,
                 m->syn_k, m->syn_min_cooc, m->syn_df_frac, m->syn_min_df, m->syn_discount);
```

After `tilemem_set_expansion` (near the end of the file), add:

```c
void tilemem_set_discount(TileMemory *m, int on){ m->syn_discount = on?1:0; m->syn_dirty=1; }
```

In `include/corpus/tile_memory.h`, after the `tilemem_set_expansion` prototype, add:

```c
/* Toggle support-discounting of PPMI weights (1=on/default, 0=raw PPMI). Rebuilds the map. */
void tilemem_set_discount(TileMemory *m, int on);
```

- [ ] **Step 6: Update the two existing `syn_finalize` call sites in the test (keep raw-PMI math)**

In `tests/test_synonyms.c`, in `test_ppmi_math`, change:

```c
    syn_finalize(s, df_lookup, NULL, 4, 5, 2, 0.5, 2);
```

to:

```c
    syn_finalize(s, df_lookup, NULL, 4, 5, 2, 0.5, 2, 0);
```

In `test_roundtrip`, change:

```c
    syn_finalize(s, df_lookup, NULL, 4, 5, 2, 1.0, 1);
```

to:

```c
    syn_finalize(s, df_lookup, NULL, 4, 5, 2, 1.0, 1, 0);
```

- [ ] **Step 7: Run to verify it passes**

Run: `make synonyms`
Expected: warning-clean build; `./synonyms` prints **`25/25 checks passed`** (book present) or **`23/23 checks passed`** + `[book] not found - skipping benchmark` (book absent). The new `test_discount` 6 checks pass; the raw-PMI assertions in `test_ppmi_math`/`test_roundtrip` still hold (they pass `discount=0`); the gap-closer + reversibility still pass (the book/integration build now runs with `discount=1`, which only scales positive PMI, never zeroes it).

- [ ] **Step 8: Durability checkpoint (no git)**

Confirm the count line and warning-clean build. Do not run git.

---

## Task 2: Discount-off vs discount-on book A/B + regression

**Files:**
- Modify: `tests/test_synonyms.c`

- [ ] **Step 1: Replace `test_book_bench` with the A/B version**

In `tests/test_synonyms.c`, replace the **entire existing** `test_book_bench` function with this (it builds the same corpus twice — discount off, then on — and prints both neighbor lists so the noise reduction is measured):

```c
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
    const char *seeds[]={"story","data","mile","customer"};

    /* A: discount OFF (raw PPMI) */
    tilemem_set_discount(m, 0);
    clock_t t0=clock(); tilemem_build_synonyms(m);
    double build0=1000.0*(double)(clock()-t0)/CLOCKS_PER_SEC;
    const Synonyms *soff=tilemem_synonyms(m);
    printf("\n[bench] build(discount off) %.0f ms over %zu tiles ; vocab=%zu ; edges=%zu\n",
           build0, tilemem_total(m), syn_term_count(soff), syn_neighbor_edges(soff));
    for(int q=0;q<4;q++){ const char *nb[5]; float pp[5]; int got=syn_neighbors(soff,seeds[q],nb,pp,5);
        printf("[neighbors off] %-9s ->", seeds[q]);
        for(int i=0;i<got;i++) printf(" %s(%.2f)", nb[i], pp[i]);
        printf("%s\n", got? "":" (none)"); }

    /* B: discount ON (support-weighted) */
    tilemem_set_discount(m, 1);
    t0=clock(); tilemem_build_synonyms(m);
    double build1=1000.0*(double)(clock()-t0)/CLOCKS_PER_SEC;
    const Synonyms *son=tilemem_synonyms(m);
    size_t terms=syn_term_count(son), edges=syn_neighbor_edges(son);
    printf("[bench] build(discount on)  %.0f ms ; vocab=%zu ; edges=%zu\n", build1, terms, edges);
    for(int q=0;q<4;q++){ const char *nb[5]; float pp[5]; int got=syn_neighbors(son,seeds[q],nb,pp,5);
        printf("[neighbors on ] %-9s ->", seeds[q]);
        for(int i=0;i<got;i++) printf(" %s(%.2f)", nb[i], pp[i]);
        printf("%s\n", got? "":" (none)"); }

    /* per-query overhead (discount on): baseline (alpha=0) vs expanded (alpha=0.35) */
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

Note: `soff` is printed **before** the discount-on rebuild (which `syn_free`s the old map), so it is never read after being freed. `son` is read only after the second build.

- [ ] **Step 2: Run and capture the A/B output**

Run: `make synonyms`
Expected: **`25/25`** (book present) / **`23/23`** (absent), warning-clean. If the book is present, capture the `[neighbors off]` and `[neighbors on ]` blocks verbatim — the contrast (rare/garbled neighbors shrinking or dropping under discounting while `mile→last` / `story→narrative` persist) is the required deliverable.

- [ ] **Step 3: Full regression sweep**

Run each and confirm:

```
make synonyms       # 25/25 (or 23/23 + book-skip)
make tfidf          # 3/3   (alpha=0 expansion still byte-identical)
make tiermem_test   # 18/18
make graduate       # 9/9
make fontdecode     # 20/20
make pdftest        # 19/19
```

Expected: all green. If any regress, **stop and debug** (superpowers:systematic-debugging) before claiming completion. The `graduate` build emits pre-existing `src/router/*` warnings unrelated to this work — those are expected.

- [ ] **Step 4: Durability checkpoint (no git)**

Confirm all suites green and the A/B captured. Do not run git.

---

## Self-Review

**Spec coverage:**
- §1.1 discounted score formula → Task 1 Step 4. ✓
- §1.1 opt-in flag, on by default in tile_memory → Task 1 Step 5 (`syn_discount=1`). ✓
- §1.2 noise shrinks / signal preserved, A/B on book → Task 2 Step 1. ✓
- §1.3 raw-PMI math stays testable (discount=0) → Task 1 Step 6 (`test_ppmi_math`/`test_roundtrip` pass 0) + `test_discount` discount-off branch. ✓
- §1.4 no regressions, opt-in expansion, zero core edits → Task 2 Step 3. ✓
- §3 mechanism (support factor, mₙ=min(df)) → Task 1 Step 4. ✓
- §4 components (all five files) → Tasks 1–2. ✓
- §5 unit test (equal raw PMI, different support; reorder + shrink) → Task 1 Step 1. ✓
- §5 gap-closer/reversibility still pass under default discount → Task 1 Step 7 + Task 2 Step 3. ✓
- §6 knobs → Task 1 Steps 4–5. ✓

**Placeholder scan:** No TBD/TODO; every code step shows complete code. ✓

**Type consistency:** `syn_finalize(..., int min_df, int discount)` — identical in header (Step 3), impl (Step 4), and all four call sites (tile_memory Step 5; test Steps 1 & 6). `tilemem_set_discount(TileMemory*, int)` — identical in header (Step 5), impl (Step 5), and both call sites (Task 2 `tilemem_set_discount(m,0)` / `(m,1)`). Field `syn_discount` (int) consistent. ✓

---

## Project notes

- **No git on CNET** — never run git; verify via filesystem + `make synonyms`.
- Extends Lever 3. Touches only `synonyms.{c,h}`, `tile_memory.{c,h}`, `tests/test_synonyms.c`. **Zero core/router/contract edits.**
