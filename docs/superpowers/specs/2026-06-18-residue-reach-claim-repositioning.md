# Claim Repositioning: the Residue-Scan Reach Test (post-v4.4)

**Date:** 2026-06-18
**Branch:** `chunk-capacity`
**Genre:** claim-history (how the central claim moved) — not a design spec or a changelog entry
**Closes the loop on:** [2026-06-15-residue-scan-generalization-design.md](2026-06-15-residue-scan-generalization-design.md)

## The event

Before this test, the project's implicit headline was that the typed boundary
*beats a monolith on the same task*. Every prior domain was **enumerable**
(16 digits, 256 bytes, 20000 additions), so a flat net could simply memorize it —
composition was elegant but never *needed*. The residue-scan reach test was the
first task built where the input space (b^N) is astronomically past enumeration,
so success *requires* generalization. It forced a cleaner statement of the claim —
and moved the claim off the axis it had been implicitly measured on.

## What it measured (so the result is reproducible, not paraphrased)

`n mod k` for an N-digit base-b number, composing one frozen transition
`δ(r,d) = (r·b + d) mod k` over the digit string (k=7, b=4). Harness:
`tests/residue_study.c` (`make residue`; budgeted, not in `make test`).

- **Composed path** — δ trained + certified exact on its full 28-pair (k·b) table,
  then composed over the string; the snap recanonicalizes the residue at every
  step, so margin resets per position and does not erode with depth.
- **Flat monolith** — one net over the whole N·b-wide input, trained on 5000
  sampled length-8 strings (~179× the composed path's 28 pairs).
- Both evaluated on 20000 held-out length-8 strings (disjoint RNG stream).

## The result, and the repositioning

- Composed: **exact** on the held-out set (certified δ ⇒ 100%, structurally, not
  by luck), and exact at N = 4, 8, 16, 32 — lengths it never trained on.
- Flat: **competitive at the short length** (a representative `make residue` run
  put it in the mid-90s% exact). A monolith is *fine* at N=8.

That flat result is the whole point: **"boundary beats monolith" was the wrong
axis.** At short, near-enumerable length a monolith is fine. The defensible edge is
three things a fixed-width monolith cannot offer at *any* data budget:

1. **Length-generalization** — composed is exact at lengths it never saw; the
   monolith is *structurally* N/A beyond its trained input width.
2. **Exactness** — composed is provably exact, not "high accuracy"; flat plateaus
   below exact even with ~179× more data.
3. **Machine-checkable proof** — δ certified full-domain (28/28, margin ≥ floor)
   bounds the composition for *all* b^N strings; a flat net cannot express that.

## Extensibility (the corollary)

Swapping δ for another modulus (k = 3, 5, 7) recomputes the new residue with **zero
change to the scan** — the composition, not the weights, is the reusable artifact.
This is a different kind of claim from the three edges above: those are about the
*result* being correct; this is about the *artifact* being reusable. Swap the
frozen part, keep the structure, get a new exact computation for free.

## Consequence for the 7-segment "concession"

On this repositioned axis, the 7-seg named blind spot is **designed-correct
behavior**, not a loss. The claim was never a per-instance performance race; it is
length-generalization + exactness + proof. A leaf that fails confidently below its
representational floor is outside what those three guarantees ever covered. "The
point is the mechanism" is the actual claim, not a hedge.

## Honesty note on the number

No precise number for the flat net is committed anywhere — **by design.** It is not
load-bearing: all three axes above hold independent of whether the flat net scores
94%, 96%, or 98%. The figure would also vary with seed and training budget, so any
number committed here would be a hostage to one RNG state. Pinning it invites an
argument about the number, which is the wrong argument to have. To see a value,
re-run `make residue`; do not quote a remembered number as fact.
