# Discounted PPMI Neighbors (Lever 4) — Design

**Date:** 2026-06-26
**Status:** Approved (design); ready for implementation plan
**Topic:** Suppress spurious rare-term neighbors in the synonym map (the garbled-OCR noise
tail like `customer → ffig / satisffed / xf`) by **discounting PMI by support** (Pantel–Lin),
so rare-marginal / low-co-occurrence edges shrink while strong collocations are preserved.

---

## 1. Goal & acceptance criteria

1. **Discounted PPMI**: each edge's score becomes `pmi · (c/(c+1)) · (mₙ/(mₙ+1))` where
   `c` = co-occurrence count, `mₙ = min(df_i, df_j)`. Opt-in via a `discount` flag; **on by
   default** in `tile_memory` (it's strictly a quality improvement).
2. **Noise shrinks, signal preserved**: well-supported neighbors (`mile↔last`,
   `story↔narrative`) keep near-full weight; rare-coincidence neighbors (garbled OCR tokens)
   are pushed down or out of the top-k. Demonstrated on the real book as a discount-off vs
   discount-on A/B.
3. **Raw-PMI math stays testable**: the existing exact-PMI unit test runs with `discount=0`
   and its asserted value (`ln 2 = 0.6931`) is unchanged.
4. **No regressions**: gap-closer, reversibility, and `make tfidf` (3/3) stay green; the
   synonym map is still opt-in (`alpha=0` default disables expansion entirely). Zero
   core/router/contract edits.

## 2. Why discounting (grounded)

The visible noise (`customer → ffig(2.67) satisffed(2.67) xf(2.67)`) is **not** a
garbled-glyph problem that a per-term English-likeness gate would catch — `english_likeness`
(`src/pdf/font_decode.c`) is a sentence metric whose per-token test is only "len≥2 and has a
vowel", which `satisffed`/`ffig` pass. The real cause is **PMI's rare-term bias**: a token
seen ~twice, both beside `customer`, has a tiny marginal → inflated PMI. The principled,
literature-standard fix is to multiply PMI by a support/confidence factor (Pantel & Lin 2002
discounting). It is pure corpus statistics — no dictionary, no external data, no dense
weights — consistent with the project thesis, and it generically suppresses *all* spurious
rare-coincidence neighbors, not only OCR junk.

A heavier per-term plausibility gate (char-trigram / consonant-cluster model) was considered
and rejected for v1: more code, needs English letter statistics, and still wouldn't address
the underlying rare-term bias that produces noise even from non-garbled rare tokens.

## 3. Mechanism

In `syn_finalize` (`src/corpus/synonyms.c`), after computing `pmi>0` for a pair `(i,j)` with
count `c`:

```
if (discount) {
    double mn = (df[i] < df[j]) ? (double)df[i] : (double)df[j];
    double support = (c/(c+1.0)) * (mn/(mn+1.0));
    score = pmi * support;
} else {
    score = pmi;
}
topk_insert(..., (float)score, k);
```

`support ∈ (0,1)`, monotonic in both `c` and the smaller marginal. For a strong collocation
(`c` large, both marginals large) `support → 1` (≈ raw PMI). For the garbled profile (`c=2`,
`mₙ=2`) `support = (2/3)·(2/3) = 0.44`, nearly halving the score — enough to drop it below
genuinely-supported neighbors in the top-k. The score is used both for ranking and as the
stored weight.

## 4. Components (change list)

- **Modify** `include/corpus/synonyms.h`, `src/corpus/synonyms.c`: add `int discount` as the
  final parameter of `syn_finalize`; apply the support factor when set.
- **Modify** `include/corpus/tile_memory.h`, `src/corpus/tile_memory.c`: add `int syn_discount`
  to `TileMemory` (default `1` in `tilemem_open`); pass it through
  `tilemem_build_synonyms` → `syn_finalize`; add `void tilemem_set_discount(TileMemory *m,
  int on)` (sets the flag + `syn_dirty=1`, since it changes the built map).
- **Modify** `tests/test_synonyms.c`: update `test_ppmi_math` and `test_roundtrip` calls to
  pass `discount=0` (preserving the raw-PMI assertions); add `test_discount` (unit, below);
  extend the book benchmark with a discount-off vs discount-on neighbor A/B.
- No Makefile change (the `synonyms` target already compiles these files).

## 5. Testing / proofs

- **Unit — `test_discount`** (deterministic, no book): construct two pairs with **equal raw
  PMI** but different support — e.g. corpus where `(a,b)` co-occur with large `c` and large
  marginals, and `(a,r)` co-occur with small `c`/`r` rare, arranged so raw `PMI(a,b)=PMI(a,r)`.
  Assert: with `discount=1`, `weight(a→b) > weight(a→r)` (support reorders them) and
  `weight(a→r) < rawPMI(a,r)` (rare edge strictly shrunk); with `discount=0` they tie. This
  proves the mechanism without depending on noisy real data.
- **Math unchanged**: `test_ppmi_math` and `test_roundtrip` pass `discount=0`; their existing
  assertions (incl. `PPMI(a,b)=ln 2`) remain valid.
- **Gap-closer / reversibility**: unchanged tests, now running with `tile_memory`'s
  `discount=1` default. The answer tile's soft score stays positive (discount scales, never
  zeroes a positive PMI), so the zero-overlap answer is still retrieved; `alpha=0` still
  reproduces baseline.
- **Book A/B (qualitative, required output)**: for each seed (`story`,`data`,`mile`,
  `customer`), print neighbors with `discount=0` then `discount=1`. Report both lists so the
  noise reduction is visible/measured. Assert only that the map stays non-empty (no claim of
  perfect cleanup — see limits).
- **Regression**: `tfidf` 3/3, `tiermem_test`, `graduate`, `fontdecode`, `pdftest` green.

## 6. Knobs

| knob | default | role |
|---|---|---|
| `discount` (in `syn_finalize`) | n/a (explicit arg) | 0 = raw PPMI, 1 = support-discounted |
| `syn_discount` (TileMemory) | `1` (on) | map built with discounting unless turned off |

No new tuning constants — the `+1` smoothing in `(c/(c+1))·(mₙ/(mₙ+1))` is parameter-free.

## 7. Honest scope / limits

- Discounting **reduces** the rare-term bias; it does not eliminate it. A token occurring
  *only* beside one word (`c = df`, both small) still earns a shrunk-but-nonzero weight, so
  some noise can remain in sparse corpora. This is principled bias-reduction, not a dictionary
  filter — neighbor quality still scales with corpus size and cleanliness
  ([[extraction-quality-fontdecode-built]] bounds the input junk).
- A future per-term plausibility gate and/or sliding-window co-occurrence remain deferred and
  composable on top of this.

## 8. Project notes

- **No git on CNET.** Spec written, not committed; verify via filesystem.
- Extends [[synonym-ppmi-retrieval-built]] (Lever 3). Touches only `synonyms.{c,h}`,
  `tile_memory.{c,h}`, `tests/test_synonyms.c`. **Zero core/router/contract edits.**
