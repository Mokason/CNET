# Consolidation Candidate Pruning (Lever 6.1) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a `max_df_frac` parameter to `tilemem_consolidate` that skips high-`df` terms during candidate gathering (not coverage), collapsing the near-O(n²) blow-up, and a `comparisons` counter to the report so the speedup is measurable.

**Architecture:** In the per-term candidate-gather loop, `continue` past any term with `df > max_df_frac·doc_count`. `coverage`/`sim` are untouched, so merge quality is preserved. Track `comparisons` (candidate coverage-checks) in `ConsolidateReport`.

**Tech Stack:** C11, MinGW gcc, `make` (Windows). No git on CNET (verify via filesystem + tests; never run git). Spec: `docs/superpowers/specs/2026-06-27-consolidation-candidate-pruning-design.md`.

**Note on "Commit"/"checkpoint":** No git. A passing build/test is the checkpoint. Do not run `git`.

---

## File Structure

- **Modify** `include/corpus/tile_memory.h` — `ConsolidateReport` gains `size_t comparisons;`; `tilemem_consolidate` gains a `double max_df_frac` parameter.
- **Modify** `src/corpus/tile_memory.c` — per-term skip in the gather loop; `comparisons` counter; thread `max_df_frac`.
- **Modify** `tests/test_tile_consolidate.c` — update the two existing call sites (`1.0` unit, `0.1` book + print `comparisons`); add the pruning unit test.

---

## Task 1: Candidate pruning + comparisons counter + pruning test

**Files:**
- Modify: `include/corpus/tile_memory.h`
- Modify: `src/corpus/tile_memory.c`
- Modify: `tests/test_tile_consolidate.c`

- [ ] **Step 1: Update the header (report field + new parameter)**

In `include/corpus/tile_memory.h`, replace the consolidation declarations:

```c
typedef struct { size_t tiles_before, tiles_after, merges; double ms; } ConsolidateReport;
/* Merge paraphrase HOT tiles whose bidirectional synonym-aware soft-coverage >= tau_sem and
   both have >= min_terms distinct terms. Builds the PPMI map if stale; uses the inverted index
   to find candidates. Returns the number of merges; rep may be NULL. HOT-only. */
size_t tilemem_consolidate(TileMemory *m, double tau_sem, int min_terms, ConsolidateReport *rep);
```

with:

```c
typedef struct { size_t tiles_before, tiles_after, merges, comparisons; double ms; } ConsolidateReport;
/* Merge paraphrase HOT tiles whose bidirectional synonym-aware soft-coverage >= tau_sem and
   both have >= min_terms distinct terms. Builds the PPMI map if stale; uses the inverted index
   to find candidates. max_df_frac: during candidate GATHERING, skip terms with
   df > max_df_frac*doc_count (<=0 or >=1 disables pruning) — coverage still uses all terms, so
   merge quality is unchanged; this only bounds how many pairs are examined. rep->comparisons
   reports the number of candidate coverage-checks. Returns merges; rep may be NULL. HOT-only. */
size_t tilemem_consolidate(TileMemory *m, double tau_sem, int min_terms, double max_df_frac, ConsolidateReport *rep);
```

- [ ] **Step 2: Add the parameter, the skip, and the counter in the impl**

In `src/corpus/tile_memory.c`, change the function signature line:

```c
size_t tilemem_consolidate(TileMemory *m, double tau_sem, int min_terms, ConsolidateReport *rep){
```

to:

```c
size_t tilemem_consolidate(TileMemory *m, double tau_sem, int min_terms, double max_df_frac, ConsolidateReport *rep){
```

Just after `size_t merges=0;` (the line before the `for(int s=0;s<ns;s++)` outer loop), add:

```c
    size_t comparisons=0;
    double df_cut = (max_df_frac>0.0 && max_df_frac<1.0) ? max_df_frac*(double)m->doc_count : 0.0;
```

Replace the candidate-gather term loop header:

```c
        for(int i=0;i<na;i++){
            for(int pass=0;pass<2;pass++){
```

with (add the high-`df` skip):

```c
        for(int i=0;i<na;i++){
            if(df_cut>0.0 && m->doc_count>0){
                unsigned *dfp=tdf_slot(&m->tdf,TA[i],0); unsigned dft=dfp? *dfp:0;
                if((double)dft > df_cut) continue;   /* skip a common term in candidate gathering */
            }
            for(int pass=0;pass<2;pass++){
```

In the candidate-scoring loop, find the lines:

```c
            if(nc<min_terms) continue;
            double cab=coverage(m,TA,na,TC,nc), cba=coverage(m,TC,nc,TA,na);
```

and insert the counter between them:

```c
            if(nc<min_terms) continue;
            comparisons++;
            double cab=coverage(m,TA,na,TC,nc), cba=coverage(m,TC,nc,TA,na);
```

Finally, update the report write at the end:

```c
    if(rep){ rep->tiles_before=before; rep->tiles_after=(size_t)m->n_hot+m->warm_count;
             rep->merges=merges; rep->ms=1000.0*(double)(clock()-t0)/CLOCKS_PER_SEC; }
```

to:

```c
    if(rep){ rep->tiles_before=before; rep->tiles_after=(size_t)m->n_hot+m->warm_count;
             rep->merges=merges; rep->comparisons=comparisons;
             rep->ms=1000.0*(double)(clock()-t0)/CLOCKS_PER_SEC; }
```

- [ ] **Step 3: Update the existing test call sites + add the pruning test**

In `tests/test_tile_consolidate.c`:

In `test_consolidate_unit`, change:

```c
    size_t merges=tilemem_consolidate(m,0.7,3,&rep);
```

to (pruning off — preserves the exact 1-merge result):

```c
    size_t merges=tilemem_consolidate(m,0.7,3,1.0,&rep);
```

In `test_book_report`, change:

```c
    size_t merges=tilemem_consolidate(m,0.7,3,&rep);
    printf("\n[bench] consolidate %.0f ms ; tiles %zu -> %zu ; merges=%zu\n",
           rep.ms, rep.tiles_before, rep.tiles_after, merges);
```

to (prune common terms + report comparisons):

```c
    size_t merges=tilemem_consolidate(m,0.7,3,0.1,&rep);
    printf("\n[bench] consolidate %.0f ms ; tiles %zu -> %zu ; merges=%zu ; comparisons=%zu\n",
           rep.ms, rep.tiles_before, rep.tiles_after, merges, rep.comparisons);
```

Add the pruning test above `main`:

```c
static void test_prune_candidates(void){
    reset_store("cons_prune");
    TileMemory *m=tilemem_open("cons_prune",256,1000,0.99);
    /* 10 tiles all sharing the common term "hub"; the other two terms of each are unique
       (df=1). No paraphrases (coverage between any two = just hub = 1/3 < tau). With pruning,
       "hub" (df=10) is skipped in gathering, so no candidates are examined. */
    const char *docs[]={
        "hub fa1 fa2","hub fb1 fb2","hub fc1 fc2","hub fd1 fd2","hub fe1 fe2",
        "hub ff1 ff2","hub fg1 fg2","hub fh1 fh2","hub fi1 fi2","hub fj1 fj2"};
    for(int i=0;i<10;i++) tilemem_ingest(m,docs[i],docs[i],"","c");

    ConsolidateReport r_off, r_on;
    size_t m_off=tilemem_consolidate(m,0.7,3,1.0,&r_off);   /* pruning disabled */
    size_t m_on =tilemem_consolidate(m,0.7,3,0.3,&r_on);    /* prune df>3 -> "hub"(df=10) skipped */
    CHECK(m_off==0 && m_on==0,"hub-only tiles never merge (coverage below tau both ways)");
    CHECK(r_off.comparisons>0,"without pruning, hub links every pair -> work done");
    CHECK(r_on.comparisons==0,"with pruning, the common term is skipped -> no pairs examined");
    CHECK(r_on.comparisons<r_off.comparisons,"pruning strictly reduces candidate work");
    tilemem_close(m);
}
```

Add `test_prune_candidates();` in `main` after `test_consolidate_unit();`, and add `cons_prune` to the Makefile `clean` `rm -rf` list.

- [ ] **Step 4: Run the unit + pruning tests**

Run: `make consolidate`
Expected: with the book present, `13/13 checks passed` (8 original + 4 pruning + ... count it: original unit 8, pruning 4, book report 2 = **14/14**); without the book, `12/12` + skip. Warning-clean. Capture the `[bench] consolidate ... comparisons=...` line — `ms` should now be in the seconds range (down from ~154 000) and `comparisons` far below the unpruned count, with `merges` ≈ 11.

(If the original unit test's merge count changed, the `1.0` pruning-off path is wrong — `df_cut` must be 0 when `max_df_frac>=1`, so no term is skipped.)

- [ ] **Step 5: Full regression sweep**

Run: `make consolidate`, `make tileindex`, `make tiermem_test`, `make synonyms`, `make tfidf`, `make graduate`, `make fontdecode`, `make pdftest`.
Expected: all green (`tileindex` 24/24, `tiermem_test` 18/18, `synonyms` 25/25, `tfidf` 3/3, `graduate` 9/9, `fontdecode` 20/20, `pdftest` 19/19). `graduate`'s pre-existing `src/router/*` warnings are expected. If any regress, stop and debug (superpowers:systematic-debugging).

---

## Self-Review

**Spec coverage:**
- §1.1 `max_df_frac` skip in gather → Task 1 Step 2. ✓
- §1.2 coverage unchanged / quality preserved → Step 2 (skip is only in the gather term loop; `coverage` calls untouched). ✓
- §1.3 `comparisons` counter → Steps 1–2 + pruning test. ✓
- §1.4 big speedup on book → Step 4 (bench capture). ✓
- §1.5 no regressions, unit still 1 merge → Steps 4–5. ✓
- §3 mechanism (`df_cut`, `continue`, neighbor inherits skip) → Step 2. ✓
- §5 pruning unit test (hub corpus, comparisons drop, merges 0) → Step 3. ✓
- §6 knob (`max_df_frac` default/disable semantics) → Steps 1–2. ✓

**Placeholder scan:** No TBD/TODO; complete code in every step. The check-count in Step 4 is computed explicitly (8+4+2=14 with book / 12 without). ✓

**Type consistency:** `tilemem_consolidate(TileMemory*,double,int,double,ConsolidateReport*)` — identical in header (Step 1), impl (Step 2), and all three test call sites (Step 3: unit `1.0`, book `0.1`, pruning `1.0`/`0.3`). `ConsolidateReport` adds `size_t comparisons` consistently. ✓

---

## Project notes

- **No git on CNET** — never run git; verify via filesystem + `make`.
- Tunes Lever 6. Touches only `tile_memory.{c,h}` + `tests/test_tile_consolidate.c`. **Zero core/router/contract edits.**
