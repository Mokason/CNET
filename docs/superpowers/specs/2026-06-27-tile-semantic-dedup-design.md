# PPMI Semantic-Dedup via Batch Consolidation (Lever 6) — Design

**Date:** 2026-06-27
**Status:** Approved (design); ready for implementation plan
**Topic:** Make the tile store **compound** instead of accumulate: a deliberate
`tilemem_consolidate()` pass uses the PPMI synonym map to merge *paraphrase* tiles
("the customer churned" ≈ "customers are leaving") that today's lexical cosine dedup keeps
separate. Candidates are found cheaply via the new inverted index; merging is conservative and
auditable.

---

## 1. Goal & acceptance criteria

1. **Semantic merge**: `tilemem_consolidate(m, tau_sem, min_terms, &rep)` merges HOT tile pairs
   whose **bidirectional synonym-aware coverage** ≥ `tau_sem`, collapsing paraphrases into one
   tile (absorbing the loser's `count`).
2. **Conservative + auditable**: uses `min` of the two directional coverages (a subset `A⊂B`
   must NOT merge) and a `min_terms` floor; returns a report (tiles before/after, merges, ms).
3. **Composes the prior levers**: paraphrase *candidates* come from the inverted index
   (postings of a tile's terms **and their PPMI neighbors**), not an O(n²) scan; similarity
   uses `syn_neighbors`.
4. **Opt-in, zero default change**: consolidation runs only when called. All suites stay green:
   tileindex 24/24, tiermem 18/18, synonyms 25/25, tfidf 3/3, graduate 9/9, fontdecode 20/20,
   pdftest 19/19. Zero core/router/contract edits.

## 2. Why batch consolidation (grounded)

Today's dedup is lexical: `hot_best` takes the max cosine over the FNV-1a hashed bag-of-words
and merges if ≥ `tau` ([src/corpus/tile_memory.c](../../src/corpus/tile_memory.c) `tilemem_ingest`).
Two passages that share *meaning* but few *words* never merge, so the store holds many
rewordings of one fact. The PPMI map (Lever 3/4) knows which terms are related, but it is built
*from* the corpus — so it can't be consulted during the first ingests. A **post-ingest batch
pass** dodges that chicken-and-egg: ingest normally, then build the map once and merge
paraphrase clusters globally. It is also safer than silent per-insert merging (one explicit,
reportable operation) and is literally the "compounding" idea — the memory analog of
chunk-consolidation in the hard core.

## 3. Similarity metric: synonym-aware soft-Jaccard

For tiles A, B with distinct content terms `TA`, `TB` (via `tile_distinct_terms`):

```
coverage(TX, TY) = |{ x in TX : x in TY  OR  some top-k PPMI neighbor of x is in TY }| / |TX|
sim(A,B)         = min( coverage(TA,TB), coverage(TB,TA) )
merge iff  sim(A,B) >= tau_sem  AND  |TA| >= min_terms  AND  |TB| >= min_terms
```

- **`min`, not average**: both directions must be well covered → a specific fact that is merely
  a subset of a general one (`A⊂B`) scores low one way and is *not* merged. This is the main
  safety property.
- Exact membership is a small linear scan over the other tile's terms; the synonym fallback is
  `syn_neighbors(x)` (top-k) checked against `TY`. Term counts per tile are small (~10–30).
- Defaults: `tau_sem = 0.7`, `min_terms = 3`.

## 4. Mechanism

```
tilemem_consolidate(m, tau_sem, min_terms, rep):
  if m->syn_dirty: tilemem_build_synonyms(m)          # ensure the PPMI map exists
  rep.tiles_before = n_hot (+ warm, reported)
  live_tids = snapshot of all live HOT tids           # stable iteration under removal
  for at in live_tids:
      if not loc_live(at): continue                   # already merged away
      A = hot tile at loc[at].where;  TA = distinct terms of A
      if |TA| < min_terms: continue
      cand = gather_candidates(A)                      # via inverted index, below
      for ct in cand:
          if ct == at or not loc_live(ct): continue
          C = hot tile at loc[ct].where; TC = distinct terms
          if |TC| < min_terms: continue
          if sim(TA,TC) >= tau_sem:
              keep, drop = (A,C) if A.count>=C.count else (C,A)   # keep the more-supported tile
              keep.count += drop.count; keep.heat = max(keep.heat, drop.heat)
              remove_hot_by_tid(m, drop.tid)           # free + swap-last + fix moved loc + loc_kill
              rep.merges++
              if drop.tid == at: break                 # A itself was absorbed; move on
              re-fetch A via loc[at].where             # array may have shifted
  rep.tiles_after = total; rep.ms = elapsed
  return rep.merges
```

**Candidate gathering (`gather_candidates`)** — reuse the inverted index + the `seen[]` epoch:
bump `seen_epoch`; for each term `x` in `TA`, walk `tdf_postings(x)` and, for each top-k PPMI
neighbor `n` of `x`, walk `tdf_postings(n)`; collect each live, HOT, not-yet-seen tid `!= at`.
This yields exactly the tiles that share a term or a related term with A — the paraphrase
candidates — bounded, not O(n²).

**`remove_hot_by_tid(m, tid)`** — the existing eviction removal pattern factored into a helper:
locate the tile at `loc[tid].where` (tier HOT), `tile_free`, `hot[idx]=hot[--n_hot]`, fix the
moved tile's `loc` to `idx`, `loc_kill(tid)`. Stale postings for `tid` are skipped via the
liveness check (no posting rewrite — consistent with Lever 5). `tilemem_evict_containing` is
refactored to call this helper (DRY).

## 5. Components (change list)

- **Modify** `include/corpus/tile_memory.h`: add `ConsolidateReport` struct + declare
  `tilemem_consolidate`.
- **Modify** `src/corpus/tile_memory.c`: factor `remove_hot_by_tid` (used by evict + consolidate);
  add `tile_coverage`/`sim` helpers (using `syn_neighbors`); add `tilemem_consolidate` with the
  inverted-index candidate gather.
- **Modify** `tests/test_tile_index.c` (or a new `tests/test_consolidate.c` + `make consolidate`
  target): unit test + real-book consolidation report.

No new core/router/contract edits.

## 6. Testing / proofs

- **Unit (deterministic)**: a synthetic corpus with (a) a known paraphrase pair worded with
  PPMI-related terms (so the map links them), (b) two genuinely distinct tiles, (c) a subset
  pair (A's terms ⊂ B's terms). After `tilemem_set_expansion`(to build a usable map) +
  `tilemem_consolidate`: assert the paraphrase pair merged (tile count dropped by 1, surviving
  tile's `count` absorbed), the distinct tiles survived, and the subset pair did **not** merge
  (the `min`-coverage guard). Assert `rep.merges` matches.
- **Real book** (large `hot_cap` so tiles are resident): ingest, `tilemem_consolidate`, print
  `tiles_before → tiles_after`, `merges`, `ms`, and a handful of example merged paraphrases.
  Report honestly, including any false merges.
- **Regressions**: all suites green; consolidation is opt-in so nothing else changes.

## 7. Knobs (defaults conservative)

| knob | default | role |
|---|---|---|
| `tau_sem` | `0.7` | min bidirectional soft-coverage to merge |
| `min_terms` | `3` | both tiles must have ≥ this many distinct terms |

No automatic invocation — a caller decides when to consolidate.

## 8. Honest scope / limits

- **HOT-only for v1**: merging a WARM tile would require rewriting `warm.bin`; deferred. The
  book test uses a large `hot_cap` so the tiles to consolidate are resident.
- Semantic merging is inherently riskier than lexical — the `min`-coverage + `min_terms` +
  conservative `tau_sem` are guardrails, but vocabulary-overlapping-but-distinct facts can still
  false-merge; the report makes merges auditable. Neighbor quality bounds it (Lever 4 discounting
  already cleaned the map).
- The merged-away tile's text is discarded (the dedup intent). Keeping variants and WARM
  consolidation are deferred.

## 9. Project notes

- **No git on CNET.** Spec written, not committed; verify via filesystem + `make`.
- Composes [[synonym-ppmi-retrieval-built]] (the map) + [[tile-inverted-index-built]] (candidate
  finding). Touches only `tile_memory.{c,h}` + a test. **Zero core/router/contract edits.**
