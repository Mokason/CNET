# Consolidation Candidate Cap (Lever 6.2) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a rarest-first candidate gather with a hard per-tile cap (`max_cand`) to `tilemem_consolidate`, bounding the pass to `O(n·max_cand)` so it runs in seconds while merges stay ≈11.

**Architecture:** Order a tile's terms by ascending `df`, gather candidates in that order, and stop once `max_cand` are collected (break the posting loops at the cap). Keep the Lever 6.1 `df_frac` pre-filter and the `comparisons` counter. `coverage` is untouched.

**Tech Stack:** C11, MinGW gcc, `make` (Windows). No git on CNET (verify via filesystem + tests; never run git). Spec: `docs/superpowers/specs/2026-06-27-consolidation-candidate-cap-design.md`.

**Note on "Commit"/"checkpoint":** No git. A passing build/test is the checkpoint. Do not run `git`.

---

## File Structure

- **Modify** `include/corpus/tile_memory.h` — `tilemem_consolidate` gains `int max_cand` (after `max_df_frac`).
- **Modify** `src/corpus/tile_memory.c` — rarest-first `order[]` + effective `cap`; cap-bounded gather loop.
- **Modify** `tests/test_tile_consolidate.c` — update the four existing call sites with `max_cand`; add a cap test.

---

## Task 1: Rarest-first gather + hard cap

**Files:**
- Modify: `include/corpus/tile_memory.h`
- Modify: `src/corpus/tile_memory.c`
- Modify: `tests/test_tile_consolidate.c`

- [ ] **Step 1: Update the header prototype**

In `include/corpus/tile_memory.h`, replace the `tilemem_consolidate` prototype:

```c
size_t tilemem_consolidate(TileMemory *m, double tau_sem, int min_terms, double max_df_frac, ConsolidateReport *rep);
```

with:

```c
/* ... max_cand: cap on candidates examined per tile, gathered rarest-term-first (<=0 = unbounded).
   Bounds the pass to O(n*max_cand); coverage/merge logic unchanged. */
size_t tilemem_consolidate(TileMemory *m, double tau_sem, int min_terms, double max_df_frac, int max_cand, ConsolidateReport *rep);
```

- [ ] **Step 2: Update the signature in the impl**

In `src/corpus/tile_memory.c`, change:

```c
size_t tilemem_consolidate(TileMemory *m, double tau_sem, int min_terms, double max_df_frac, ConsolidateReport *rep){
```

to:

```c
size_t tilemem_consolidate(TileMemory *m, double tau_sem, int min_terms, double max_df_frac, int max_cand, ConsolidateReport *rep){
```

- [ ] **Step 3: Add rarest-first ordering + effective cap, and cap the gather loop**

In `src/corpus/tile_memory.c`, the current code from the `na<min_terms` guard through the end of the gather loop is:

```c
        if(na<min_terms) continue;
        /* gather candidate tids via postings of TA's terms AND their PPMI neighbors */
        m->seen_epoch++; seen_ensure(m);
        unsigned *cand=NULL; int cn=0, cc=0;
        for(int i=0;i<na;i++){
            if(df_cut>0.0 && m->doc_count>0){
                unsigned *dfp=tdf_slot(&m->tdf,TA[i],0); unsigned dft=dfp? *dfp:0;
                if((double)dft > df_cut) continue;   /* skip a common term in candidate gathering */
            }
            for(int pass=0;pass<2;pass++){
                int pn=0; unsigned *pl=NULL;
                if(pass==0){ pl=tdf_postings(&m->tdf,TA[i],&pn); }
                else { const char *nb[16]; float pp[16]; int kk=m->syn_k<16? m->syn_k:16;
                       int got=m->syn? syn_neighbors(m->syn,TA[i],nb,pp,kk):0;
                       for(int z=0;z<got;z++){ int qn; unsigned *ql=tdf_postings(&m->tdf,nb[z],&qn);
                           for(int q=0;q<qn;q++){ unsigned tid=ql[q];
                               if(tid==at||!loc_live(m,tid)||m->loc[tid].tier!=0) continue;
                               if(m->seen[tid]==m->seen_epoch) continue;
                               m->seen[tid]=m->seen_epoch;
                               if(cn==cc){ cc=cc?cc*2:16; cand=(unsigned*)realloc(cand,(size_t)cc*sizeof(unsigned)); }
                               cand[cn++]=tid; } }
                       continue; }
                for(int p=0;p<pn;p++){ unsigned tid=pl[p];
                    if(tid==at||!loc_live(m,tid)||m->loc[tid].tier!=0) continue;
                    if(m->seen[tid]==m->seen_epoch) continue;
                    m->seen[tid]=m->seen_epoch;
                    if(cn==cc){ cc=cc?cc*2:16; cand=(unsigned*)realloc(cand,(size_t)cc*sizeof(unsigned)); }
                    cand[cn++]=tid; }
            }
        }
```

Replace that **entire block** with (adds the rarest-first `order[]`, the `cap`, and `&& cn<cap` guards):

```c
        if(na<min_terms) continue;
        /* order terms rarest-first (ascending df): the cap then keeps the most discriminative candidates */
        int order[256]; for(int i=0;i<na;i++) order[i]=i;
        for(int a=1;a<na;a++){ int v=order[a];
            unsigned *dva=tdf_slot(&m->tdf,TA[v],0); unsigned da=dva? *dva:0;
            int b=a-1;
            while(b>=0){ unsigned *dvb=tdf_slot(&m->tdf,TA[order[b]],0); unsigned db=dvb? *dvb:0;
                if(db<=da) break;
                order[b+1]=order[b]; b--; }
            order[b+1]=v; }
        int cap = (max_cand>0)? max_cand : (1<<30);   /* large = unbounded */
        /* gather candidate tids via postings of TA's terms AND their PPMI neighbors, rarest-first, capped */
        m->seen_epoch++; seen_ensure(m);
        unsigned *cand=NULL; int cn=0, cc=0;
        for(int oi=0; oi<na && cn<cap; oi++){
            int i=order[oi];
            if(df_cut>0.0 && m->doc_count>0){
                unsigned *dfp=tdf_slot(&m->tdf,TA[i],0); unsigned dft=dfp? *dfp:0;
                if((double)dft > df_cut) continue;   /* skip a common term in candidate gathering */
            }
            for(int pass=0;pass<2 && cn<cap;pass++){
                int pn=0; unsigned *pl=NULL;
                if(pass==0){ pl=tdf_postings(&m->tdf,TA[i],&pn); }
                else { const char *nb[16]; float pp[16]; int kk=m->syn_k<16? m->syn_k:16;
                       int got=m->syn? syn_neighbors(m->syn,TA[i],nb,pp,kk):0;
                       for(int z=0;z<got && cn<cap;z++){ int qn; unsigned *ql=tdf_postings(&m->tdf,nb[z],&qn);
                           for(int q=0;q<qn && cn<cap;q++){ unsigned tid=ql[q];
                               if(tid==at||!loc_live(m,tid)||m->loc[tid].tier!=0) continue;
                               if(m->seen[tid]==m->seen_epoch) continue;
                               m->seen[tid]=m->seen_epoch;
                               if(cn==cc){ cc=cc?cc*2:16; cand=(unsigned*)realloc(cand,(size_t)cc*sizeof(unsigned)); }
                               cand[cn++]=tid; } }
                       continue; }
                for(int p=0;p<pn && cn<cap;p++){ unsigned tid=pl[p];
                    if(tid==at||!loc_live(m,tid)||m->loc[tid].tier!=0) continue;
                    if(m->seen[tid]==m->seen_epoch) continue;
                    m->seen[tid]=m->seen_epoch;
                    if(cn==cc){ cc=cc?cc*2:16; cand=(unsigned*)realloc(cand,(size_t)cc*sizeof(unsigned)); }
                    cand[cn++]=tid; }
            }
        }
```

(The candidate-scoring loop, `comparisons++`, merge logic, and report write below this block are unchanged.)

- [ ] **Step 4: Update the existing test call sites**

In `tests/test_tile_consolidate.c`:

In `test_consolidate_unit`, change `tilemem_consolidate(m,0.7,3,1.0,&rep)` to:

```c
    size_t merges=tilemem_consolidate(m,0.7,3,1.0,64,&rep);
```

In `test_prune_candidates`, change the two calls to keep the cap unbounded (so `df_frac` stays the binding constraint there):

```c
    size_t m_off=tilemem_consolidate(m,0.7,3,1.0,0,&r_off);   /* pruning off, cap off */
    size_t m_on =tilemem_consolidate(m,0.7,3,0.3,0,&r_on);    /* prune df>3 -> "hub" skipped */
```

In `test_book_report`, change the call + print to use the tuned pre-filter + cap:

```c
    size_t merges=tilemem_consolidate(m,0.7,3,0.05,64,&rep);
    printf("\n[bench] consolidate %.0f ms ; tiles %zu -> %zu ; merges=%zu ; comparisons=%zu\n",
           rep.ms, rep.tiles_before, rep.tiles_after, merges, rep.comparisons);
```

- [ ] **Step 5: Add the cap test**

In `tests/test_tile_consolidate.c`, add above `main`:

```c
static void test_cap_candidates(void){
    reset_store("cons_cap");
    TileMemory *m=tilemem_open("cons_cap",256,1000,0.99);
    /* 10 tiles sharing "hub" (df=10) + unique df=1 fillers: not paraphrases. With no df-prefilter
       (max_df_frac=1.0), every tile gathers the other 9 via "hub"; the cap bounds that. */
    const char *docs[]={
        "hub fa1 fa2","hub fb1 fb2","hub fc1 fc2","hub fd1 fd2","hub fe1 fe2",
        "hub ff1 ff2","hub fg1 fg2","hub fh1 fh2","hub fi1 fi2","hub fj1 fj2"};
    for(int i=0;i<10;i++) tilemem_ingest(m,docs[i],docs[i],"","c");
    ConsolidateReport r0, r1;
    size_t m0=tilemem_consolidate(m,0.7,3,1.0,0,&r0);   /* cap off (unbounded) */
    size_t m1=tilemem_consolidate(m,0.7,3,1.0,3,&r1);   /* cap = 3 per tile */
    CHECK(m0==0 && m1==0,"hub-only tiles never merge (coverage below tau)");
    CHECK(r1.comparisons<=30,"cap bounds work to <= n*max_cand (10*3)");
    CHECK(r1.comparisons<r0.comparisons,"cap strictly reduces work vs unbounded");
    tilemem_close(m);
}
```

Add `test_cap_candidates();` in `main` after `test_prune_candidates();`, and add `cons_cap` to the Makefile `clean` `rm -rf` list.

- [ ] **Step 6: Run the tests**

Run: `make consolidate`
Expected: with the book present, **`17/17 checks passed`** (8 unit + 4 pruning + 3 cap + 2 book); without it, `15/15` + skip. Warning-clean. **Capture the `[bench] consolidate ...` line** — `ms` should now be in the **seconds** range (down from ~123 000) with `comparisons` ≤ ~328 K and `merges ≈ 11` (report the actual number — a small change from 11 is acceptable).

(If `merges` dropped well below 11, rarest-first + cap is shedding real paraphrases — raise `max_cand` or `max_df_frac` and report; do NOT change the assertions.)

- [ ] **Step 7: Full regression sweep**

Run: `make consolidate`, `make tileindex`, `make tiermem_test`, `make synonyms`, `make tfidf`, `make graduate`, `make fontdecode`, `make pdftest`.
Expected: all green (`tileindex` 24/24, `tiermem_test` 18/18, `synonyms` 25/25, `tfidf` 3/3, `graduate` 9/9, `fontdecode` 20/20, `pdftest` 19/19). `graduate`'s pre-existing `src/router/*` warnings are expected. If any regress, stop and debug (superpowers:systematic-debugging).

---

## Self-Review

**Spec coverage:**
- §1.1 hard cap (`max_cand`) → Steps 1–3. ✓
- §1.2 rarest-first ordering → Step 3 (`order[]` insertion sort by df). ✓
- §1.3 speedup + merges hold → Step 6 (bench). ✓
- §1.4 coverage unchanged → Step 3 (only gather changes; scoring block untouched). ✓
- §1.5 no regressions, unit still 1 merge, 6.1 test holds → Steps 4,7. ✓
- §3 mechanism (order, cap, `cn<cap` breaks, df_cut kept) → Step 3. ✓
- §5 cap test + recall preserved + book report → Steps 4–6. ✓
- §6 knobs (`max_cand` default 64, `≤0` unbounded; book `max_df_frac=0.05`) → Steps 1,4. ✓

**Placeholder scan:** No TBD/TODO; complete code in every step. Check count computed (8+4+3+2=17 / 15 without book). ✓

**Type consistency:** `tilemem_consolidate(TileMemory*,double,int,double,int,ConsolidateReport*)` — identical in header (Step 1), impl (Step 2), and all five call sites (unit `1.0,64`; pruning `1.0,0`/`0.3,0`; book `0.05,64`; cap `1.0,0`/`1.0,3`). `order[256]` ≤ `na`≤256. `cap` is `int`. ✓

---

## Project notes

- **No git on CNET** — never run git; verify via filesystem + `make`.
- Tunes Lever 6 / 6.1. Touches only `tile_memory.{c,h}` + `tests/test_tile_consolidate.c`. **Zero core/router/contract edits.**
