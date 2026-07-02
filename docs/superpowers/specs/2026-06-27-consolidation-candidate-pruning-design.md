# Consolidation Candidate Pruning (Lever 6.1) — Design

**Date:** 2026-06-27
**Status:** Approved (design); ready for implementation plan
**Topic:** Fix the `tilemem_consolidate` perf wart (154 s / 11 merges on the book) by **pruning
high-`df` terms from candidate gathering**. A tile pulls paraphrase candidates only from its
*distinctive* (low-`df`) terms, collapsing the near-O(n²) blow-up — while `coverage` keeps using
all terms, so merge quality is unchanged.

---

## 1. Goal & acceptance criteria

1. **Candidate pruning**: `tilemem_consolidate(m, tau_sem, min_terms, max_df_frac, rep)` skips a
   term during *candidate gathering* when `df(term) > max_df_frac · doc_count` (both the term's
   own posting list and its use as a PPMI-neighbor source). Default `max_df_frac = 0.1`.
2. **Merge quality preserved**: `coverage`/`sim` are unchanged (still over all terms), so the
   pruning changes *which pairs are examined*, not *how they score*. On the book the merge count
   stays at/near 11.
3. **Measurable**: `ConsolidateReport` gains `size_t comparisons` (candidate coverage-checks), so
   the speedup is asserted in a test and reported on the book.
4. **Big speedup on the book**: the pass drops from ~2.5 min to seconds (comparisons fall by
   orders of magnitude).
5. **No regressions**: consolidate unit test still merges exactly its one paraphrase; all suites
   green; zero core/router/contract edits.

## 2. Why this is the right fix (grounded)

`tilemem_consolidate` gathers candidates from the postings of every term in a tile (plus their
PPMI neighbors). Common content words ("data", "value" — not in the small `is_stop` list) have
huge posting lists, so a single tile pulls hundreds of candidates, ×5127 tiles → effectively
near-O(n²), each pair paying two `coverage` passes (154 s measured). The inverted index makes
*search* cheap because a query has few terms; consolidation feeds *every* term, so the common
ones dominate. Paraphrases of the same fact share **distinctive** (low-`df`) terms, so gathering
candidates only from low-`df` terms preserves recall while removing the explosion.

## 3. Mechanism

In the per-term candidate-gather loop of `tilemem_consolidate`:

```
for each term t in TA:
    if doc_count > 0 and df(t) > max_df_frac * doc_count:  continue   # skip common term entirely
    ... pull postings of t ...
    for each PPMI neighbor n of t:
        ... pull postings of n ...           # (n inherits the skip: we never reach here for common t)
```

`df(t)` comes from the existing `tdf_slot(&m->tdf, t, 0)`. `max_df_frac <= 0` or `>= 1` disables
pruning (process all terms — the pre-change behavior). `coverage(...)` is untouched: a candidate
found via a rare shared term is still scored over *all* of both tiles' terms, including common
ones.

`rep->comparisons` increments once per candidate that reaches the `coverage` computation (i.e.
each examined pair), giving a direct measure of the work done.

## 4. Components (change list)

- **Modify** `include/corpus/tile_memory.h`: add `size_t comparisons;` to `ConsolidateReport`;
  add the `double max_df_frac` parameter to the `tilemem_consolidate` prototype.
- **Modify** `src/corpus/tile_memory.c`: the per-term skip in the gather loop; `rep->comparisons`
  bookkeeping; thread `max_df_frac` through.
- **Modify** `tests/test_tile_consolidate.c`: pass `max_df_frac` at the existing call sites
  (`1.0` in the original unit test → pruning off → unchanged 1 merge; `0.1` in the book report);
  add a dedicated pruning test (below); print `comparisons` in the book bench.

No new core/router/contract edits.

## 5. Testing / proofs

- **Pruning unit test (deterministic, no synonyms needed)**: a corpus of ~10 tiles each
  containing a shared hub term `hub` plus two unique (`df=1`) fillers, so the tiles are **not**
  paraphrases (coverage between any two = the single hub term → below `tau`). Run consolidate
  twice on fresh stores:
  - `max_df_frac = 1.0` (no prune): `rep.comparisons > 0` (every hub-tile pair examined), `merges == 0`.
  - `max_df_frac = 0.3` (prune `hub`, `df=10 > 3`): `rep.comparisons == 0` (hub skipped → fillers
    are `df=1` self-only → no candidates), `merges == 0`.
  - Assert `comparisons` strictly dropped and `merges` identical (0). This proves pruning cuts
    work without changing outcomes.
- **Original unit test unchanged**: passes `max_df_frac = 1.0`; still exactly 1 merge (the
  churn/attrition paraphrase), subset pair survives, distinct tiles survive.
- **Book report**: `max_df_frac = 0.1`; print `tiles before→after`, `merges`, `comparisons`,
  `ms`. Expect `ms` to fall from ~154 000 to seconds and `comparisons` to fall by orders of
  magnitude, with `merges` ≈ 11 (report the actual number — a small change is acceptable and
  honest if a paraphrase shared only common terms).
- **Regressions**: `consolidate`, `tileindex`, `tiermem_test`, `synonyms`, `tfidf`, `graduate`,
  `fontdecode`, `pdftest` all green.

## 6. Knobs

| knob | default | role |
|---|---|---|
| `max_df_frac` | `0.1` | skip candidate-gather terms with `df > max_df_frac·doc_count`; `≤0` or `≥1` disables pruning |

## 7. Honest scope / limits

- A paraphrase pair that shares **only** common terms (no distinctive term above neither tile's
  rare vocabulary) will not be gathered → missed. This is a deliberate speed/recall trade; real
  paraphrases of a fact share distinctive terms, so the loss is expected to be ~0 (the book bench
  reports whether merges changed from 11).
- Pruning is candidate-gather only; `coverage` quality and the `min`-coverage subset guard are
  unchanged.
- Deferred (still): WARM consolidation, coverage memoization, keeping merged variants.

## 8. Project notes

- **No git on CNET.** Spec written, not committed; verify via filesystem + `make`.
- Tunes [[tile-semantic-dedup-built]] (Lever 6). Touches only `tile_memory.{c,h}` +
  `tests/test_tile_consolidate.c`. **Zero core/router/contract edits.**
