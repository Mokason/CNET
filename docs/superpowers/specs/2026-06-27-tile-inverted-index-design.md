# Unified Inverted Index for Tile Memory (Lever 5) — Design

**Date:** 2026-06-27
**Status:** Approved (design); ready for implementation plan
**Topic:** Replace `tilemem_search`'s "scan every HOT tile + re-read the entire WARM file each
query" with a **unified inverted index** (term → tids) over all tiles, so a query scores only
the tiles that actually contain a query/expansion term. Output is **byte-identical** to today;
only non-matching tiles are skipped.

---

## 1. Goal & acceptance criteria

1. **Sublinear-ish search**: `tilemem_search` consults a `term → tids` index and scores only
   candidate tiles, instead of tokenizing all HOT tiles and streaming the whole `warm.bin`
   every query.
2. **Byte-identical results (the regression gate)**: for any store + query, the returned
   `TileHit[]` (ids, scores, order) is identical to the pre-change linear scan. This holds
   because today's scorer already drops any tile with zero query-term overlap (`score<=0` →
   `topk_insert` rejects), so the contributing set is exactly the index's candidate set; we
   iterate it in the same HOT-array-then-WARM-file order to preserve tie-breaks.
3. **WARM random access**: spilled tiles are fetched by byte offset, not by streaming the
   file — the candidate WARM tiles only.
4. **No persisted index**: rebuilt in one pass during `tilemem_open`/load; always consistent
   with the tiles. On-disk tile format unchanged.
5. **No regressions**: tiermem 18/18, tfidf 3/3, synonyms 25/25, graduate 9/9, fontdecode
   20/20, pdftest 19/19 all green. Zero core/router/contract edits.

## 2. Why this is the right fix (grounded)

`tilemem_search` ([src/corpus/tile_memory.c:258](../../src/corpus/tile_memory.c)) tokenizes
every HOT tile per query and, for WARM, `fopen`s `warm.bin` and `tile_read` + tokenizes
**every** spilled record on **every** query — O(total tiles × terms) per search, dominated by
disk I/O as the corpus grows. `tilemem_ingest`'s `hot_best` is a separate O(n) scan, but that
is a different lever (near-dup blocking); this lever targets search only.

The scorer sums idf over the distinct query terms a tile contains and length-normalizes; a
tile sharing no query/soft term scores 0 and is dropped. So the *only* tiles that can appear
in results are those in at least one query/soft term's posting list — which is precisely what
an inverted index yields. The index changes *which tiles we bother to score*, never *how* they
score, so results are identical.

## 3. Data structures (in `tile_memory.c`, alongside `TermDF`)

- **`tid`** — stable per-tile integer, assigned at ingest/load from a monotonic counter
  (`m->next_tid`). Added to the in-memory `Tile` (`include/corpus/tile_memory.h`). **Ephemeral
  — never written to disk**, so `tile_write`/`tile_read` and existing `hot.bin`/`warm.bin`
  files are unchanged.
- **Postings** — grow `TermDF` with a per-term tid list: `unsigned **post; int *post_n,
  *post_cap;` parallel to `key`/`df`. `tdf_slot(...,add=1)` appends the tid when a tile's
  distinct term is recorded. Postings may contain stale tids (see liveness below).
- **Location map** — `tid → location`, an open-addressing hash `tid → {tier, where}` where
  `where` is the HOT array index (`tier=HOT`) or the byte offset into `warm.bin`
  (`tier=WARM`). **Presence in this map = liveness**: an evicted tile is removed from the map;
  search skips any candidate tid absent from it (no separate tombstone set needed).
- **Per-search candidate marker** — an epoch-stamped `int *seen` (sized to `next_tid`),
  stamped with a per-search counter so the candidate set is built and tested in O(1) without
  clearing between searches.

## 4. Build on open (one pass, no separate index file)

`tilemem_load` already reads `hot.bin` and streams `warm.bin`. Extend that single pass:
- HOT records: assign `tid`, set location `{HOT, hot_index}`, add each distinct term → its
  posting list (and the existing `df` bump stays).
- WARM records: capture `offset = ftell(g)` **before** each `tile_read`; assign `tid`, set
  location `{WARM, offset}`, add postings; then free the tile (WARM stays non-resident, as
  today). `df` for WARM terms is already loaded from `idf.bin`, so the load must add postings
  but **not** re-bump `df` (avoid double counting) — postings and df are populated on separate
  paths: df from `idf.bin`, postings from the tile scan.

## 5. Search (identical output, candidates only)

```
candidates = ∅ (epoch-stamped seen[])
for each distinct query term + each PPMI soft term:
    for tid in postings[term]:
        if tid live (in location map) and not already seen: mark seen, add to candidates
/* HOT pass — preserve array order for identical tie-breaks */
for i in 0..n_hot:
    if hot[i].tid is a candidate: score hot[i] with the SAME idf-overlap+lengthnorm; topk_insert
/* WARM pass — read candidates in increasing offset order = file order */
collect candidate tids whose location is WARM; sort by offset
fopen warm.bin once; for each: fseek(offset); tile_read; score; topk_insert; tile_free
```

The scoring body (idf weights `ww[]`, soft-term expansion, `hit[]` length-norm) is unchanged
from Lever 3/4 — only the *iteration set* changes. With an empty index (no tiles) or a query
whose terms have no postings, the result is empty, exactly as today.

## 6. Mutation (keep index consistent)

- **Ingest new tile**: assign `tid`, append to HOT, add distinct terms → postings, location
  `{HOT, n_hot-1}`. (Dedup-*reuse* path: no index change — same tile.)
- **Spill HOT→WARM** (`spill_coldest`/`warm_append`): record `offset` at append, flip
  location to `{WARM, offset}`. Postings unchanged (tid stable).
- **Hot-array swap-on-remove**: `spill_coldest`/`evict` do `hot[b]=hot[--n_hot]`; update the
  moved tile's location entry to index `b`.
- **Evict (graduation, `tilemem_evict_containing`)**: remove the tid from the location map
  (→ dead); its stale tids in postings are skipped at query via the liveness check.

## 7. Persistence

The index (postings, location map, tid counter) is **not** persisted — rebuilt in the
`tilemem_load` pass (§4). `hot.bin`, `warm.bin`, `idf.bin`, `synonyms.bin` are unchanged.
`warm.bin` remains append-only exactly as today (eviction is HOT-only, as today), so no dead
WARM records accrue and **no compaction is needed** for v1.

## 8. Components (change list)

- **Modify** `include/corpus/tile_memory.h`: add `int tid;` to `Tile`.
- **Modify** `src/corpus/tile_memory.c`:
  - grow `TermDF` with posting lists + `tdf` helpers to append a tid;
  - add the location-map hash + `next_tid` + epoch `seen[]` to `TileMemory`;
  - record offsets in `warm_append`; assign tids + postings + locations in `tilemem_load`,
    `tilemem_ingest`; fix locations in `spill_coldest`; drop locations in
    `tilemem_evict_containing`;
  - rewrite `tilemem_search` to the candidate-gather + HOT-order + WARM-by-offset scan;
  - free the new structures in `tilemem_close`.
- **Modify** `tests/test_tile_memory.c` (or a new `tests/test_tile_index.c` + `make` target):
  add the identity property test + the forced-WARM benchmark.

No new core/router/contract edits.

## 9. Testing / proofs

- **Identity property test (the gate)**: build a store (mix of HOT-only and forced-spill
  WARM), run a batch of varied queries, and assert the indexed `tilemem_search` returns
  `TileHit[]` **byte-identical** (id, score to float bits, order) to a reference linear scan
  over the same store. Include: multi-term queries, queries with no matches, queries hitting
  only WARM, queries after an eviction, and (with expansion on) PPMI soft terms.
- **All existing suites unchanged**: tiermem 18/18 (its persistence/spill paths now also
  exercise the index), tfidf 3/3, synonyms 25/25, graduate 9/9, fontdecode 20/20, pdftest
  19/19.
- **Benchmark (forces WARM — required output)**: ingest the real book with `hot_cap=500` so
  ~4,600 tiles spill, then time per-query search **old linear scan vs index** for a few
  selective and a few common-term queries, plus the build-on-open cost and index memory.
  Report the latency drop (large for selective queries; bounded for very common terms).

## 10. Honest scope / limits

- The win is visible only when WARM is populated (`hot_cap < total`); the current book tests
  keep everything HOT, so the benchmark deliberately shrinks `hot_cap`.
- Very common terms have long postings → little pruning; that is the same work as today, not a
  regression.
- `mmap` for WARM, eager posting compaction, and extending eviction to WARM are deferred.
- The HOT pass still iterates the HOT array (O(n_hot)) to preserve order, but only *scores*
  candidates — the per-query re-tokenization of non-matching HOT tiles is eliminated.

## 11. Project notes

- **No git on CNET.** Spec written, not committed; verify via filesystem + `make`.
- Largest single `tile_memory` change to date; the byte-identical invariant is the safety net.
- PPMI semantic-dedup (ingest-path quality) follows as its own spec → plan → build cycle.
