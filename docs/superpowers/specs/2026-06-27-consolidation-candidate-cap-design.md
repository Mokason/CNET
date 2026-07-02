# Consolidation Candidate Cap (Lever 6.2) — Design

**Date:** 2026-06-27
**Status:** Approved (design); ready for implementation plan
**Topic:** Actually bound `tilemem_consolidate`'s cost. Lever 6.1 (`max_df_frac`) only cut
154 s → 123 s because moderate-frequency terms + the ~5× neighbor multiplier still produce
13.3 M comparisons. Add a **rarest-first candidate gather with a hard per-tile cap** so the pass
is `O(n·max_cand)` — deterministically seconds — and tune the `max_df_frac` pre-filter.

---

## 1. Goal & acceptance criteria

1. **Hard cap**: `tilemem_consolidate(m, tau_sem, min_terms, max_df_frac, max_cand, rep)` examines
   at most ~`max_cand` candidates per tile (default 64; `≤0` = unbounded). Bounds the whole pass
   to `O(n·max_cand)`.
2. **Rarest-first**: candidates are gathered from a tile's terms in **ascending `df` order**, so
   when the cap is hit the kept candidates are the ones reached via the most discriminative
   (rarest) terms — preserving merge recall.
3. **Big speedup, merges hold**: on the book the pass drops to the **seconds** range with
   `merges ≈ 11` (report the actual number).
4. **Coverage unchanged**: `coverage`/`sim` and the `min`-guard are untouched — only *which*
   pairs are examined changes.
5. **No regressions**: consolidate unit test still merges exactly its one paraphrase; the Lever
   6.1 pruning test still holds; all suites green; zero core/router/contract edits.

## 2. Why a cap (grounded)

13.3 M comparisons / 5127 tiles ≈ 2,600 candidates examined per tile. `df_frac=0.1` only removes
terms above df≈512; terms at df 50–500 remain, and each gathered term also pulls its ~5 PPMI
neighbors' postings. `df_frac` *narrows* but does not *bound* the fan-out. A hard cap does: stop
after `max_cand` candidates. Ordering the gather **rarest-first** means the cap keeps the
candidates reached via a tile's distinctive terms (where real paraphrases live), so the recall
cost is minimal. `O(n·max_cand)` with `max_cand=64` ⇒ ≤ ~328 K comparisons on the book (≈40×
fewer) → seconds.

## 3. Mechanism

In `tilemem_consolidate`, before the candidate-gather loop for tile A:

```
order[0..na) = indices 0..na-1
sort `order` by ascending df(TA[idx])      # rarest term first (insertion sort; na is small)
cap = (max_cand > 0) ? max_cand : INT_MAX
```

Then gather in that order, stopping at the cap:

```
for oi in 0..na  while cn < cap:
    i = order[oi]; t = TA[i]
    if df_cut>0 and df(t) > df_cut: continue          # Lever 6.1 pre-filter (very common terms)
    add postings of t        (dedup via seen; stop if cn reaches cap)
    for each PPMI neighbor n of t:
        add postings of n     (dedup; stop if cn reaches cap)
```

`df(t)` via the existing `tdf_slot(&m->tdf,t,0)`. The candidate-scoring loop, `comparisons`
counter, merge logic, and `coverage` are all unchanged. The cap strictly bounds `cn` (break the
inner posting loops when `cn >= cap`).

## 4. Components (change list)

- **Modify** `include/corpus/tile_memory.h`: add `int max_cand` parameter to `tilemem_consolidate`
  (after `max_df_frac`).
- **Modify** `src/corpus/tile_memory.c`: build the rarest-first `order[]` (sort by df); thread
  `max_cand`; cap the gather (break posting loops + outer term loop at the cap). Add `#include
  <limits.h>` if `INT_MAX` is used (or store the effective cap as a large `int`).
- **Modify** `tests/test_tile_consolidate.c`: update the three existing call sites with
  `max_cand` (unit `64`, pruning test `0`/`3`, book `0.05, 64` + report); add a cap test.

No new core/router/contract edits.

## 5. Testing / proofs

- **Cap test (deterministic)**: reuse the hub corpus (10 tiles sharing `hub`, unique `df=1`
  fillers, no paraphrases). Run consolidate with `max_df_frac=1.0` (no pre-filter) and:
  - `max_cand=0` (unbounded): `comparisons` high (~90, every hub pair), `merges==0`.
  - `max_cand=3`: `comparisons <= 10*3 == 30` and `< the unbounded count`, `merges==0`.
  Proves the cap bounds work.
- **Recall preserved**: the original paraphrase unit test runs with `max_cand=64` and still
  produces exactly 1 merge (subset pair survives, distinct tiles survive).
- **Lever 6.1 pruning test** still passes (now with a `max_cand` argument; use a large/0 cap so
  it isn't the binding constraint).
- **Book report**: `tilemem_consolidate(m, 0.7, 3, 0.05, 64, &rep)`; print `tiles before→after`,
  `merges`, `comparisons`, `ms`. Expect `ms` in the seconds range, `comparisons ≤ ~328 K`,
  `merges ≈ 11` (report actual).
- **Regressions**: `consolidate`, `tileindex`, `tiermem_test`, `synonyms`, `tfidf`, `graduate`,
  `fontdecode`, `pdftest` all green.

## 6. Knobs

| knob | default | role |
|---|---|---|
| `max_df_frac` | book: `0.05` | pre-filter: skip gather terms with `df > max_df_frac·doc_count` (`≤0`/`≥1` off) |
| `max_cand` | `64` | hard cap on candidates examined per tile (`≤0` = unbounded) |

## 7. Honest scope / limits

- If a paraphrase's only shared/related term is common (above `df_cut`) **and** the cap fills from
  rarer terms first, the pair is missed. Rarest-first minimizes this; the book bench reports
  whether `merges` moved off 11.
- Cap is on candidate *count*, gathered rarest-first; it does not change `coverage` accuracy.
- Deferred (still): WARM consolidation, coverage memoization, keeping merged variants.

## 8. Project notes

- **No git on CNET.** Spec written, not committed; verify via filesystem + `make`.
- Tunes [[tile-semantic-dedup-built]] (Levers 6 / 6.1). Touches only `tile_memory.{c,h}` +
  `tests/test_tile_consolidate.c`. **Zero core/router/contract edits.**
