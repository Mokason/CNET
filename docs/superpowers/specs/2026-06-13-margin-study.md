# Margin and Correctness: Instrumenting the Discrete Fabric — Findings

**Date:** 2026-06-13
**Status:** Complete — all three walls measured; the instrument suite and the diagnostic table validated end to end
**Harnesses:** `tests/margin_study.c` (`make margin`), `tests/fuzzy_study.c` (`make fuzzy`), `tests/stochastic_study.c` (`make stochastic`) — all standalone, link only `src/nn.c`, core untouched

## The question

CNET's guarantees are its discreteness: canonicalization (snap-to-domain),
enumeration, and non-compounding error are three consequences of one
architectural choice — a finite alphabet at every port. That makes the
architecture's reach co-extensive with tasks that decompose into steps whose
handoffs can be a *digest* rather than a dump. The open question is where that
reach ends — and whether there is an instrument that measures it before you
build, rather than an argument about it.

## Two instruments, two failure modes

- **Margin** — the distance of a *raw* (pre-snap) output to the nearest
  canonicalization boundary. For a binary port it is `min |v - 0.5|` over the
  port's bits (range `[0, 0.5]`); `port_validate` flags a value as ambiguous
  inside the closed band `|v - 0.5| <= 0.25`. Margin measures the **channel**:
  is the handoff unambiguous.
- **Divergence** — the snapped symbol versus the arithmetic ground truth.
  Divergence measures the **function**: is it correct.

These are independent: **a primitive can be confidently, maximal-margin
wrong** (a raw output landing deep in the *wrong* symbol's basin snaps to a
clean wrong symbol at full margin). So `port_validate` (margin) catches
ambiguity and novelty; `btn_certify` / `property_check` (divergence over the
enumerated domain) catch incorrectness. You need both; margin certifies the
fabric is *well-formed*, not that it is *correct*. Separating those two
questions is the conceptual payoff of measuring margin at all.

## The harness

`tests/margin_study.c` threads the decimal ripple-carry chain by hand
(`btn_forward` + `port_canonicalize`), so every internal handoff's raw output
is visible before the snap. It is standalone — it links only `src/nn.c`, so
the planner/executor/consolidate core is untouched — and deterministic.

- **Default** (`make margin`): the verified `dec_full_add` over the 2-digit
  ripple (all 20,000 cases) as the baseline, then an 8-digit depth pass.
- **`--stress`**: fixed-width students trained on the 200-member full-adder
  to find where margin first crowds the band.

## Results

### 1. Snap-reset — margin does not erode with depth

```
8-digit ripple, per position (sampled):
  p00.sum  margin[min 0.330  mean 0.384]   ambiguous 0.00%   diverged 0.00%
  p00.carry  margin[min 0.346  mean 0.404]   ...
  ...
  p07.sum  margin[min 0.330  mean 0.383]   ambiguous 0.00%   diverged 0.00%
  p07.carry  margin[min 0.346  mean 0.403]   ...
```

The worst-case margin is `0.330` at *every* one of eight positions, flat to
three decimals. Canonicalization recenters each output on its prototype, so a
boundary is where margin is **restored**, not where it decays. Margin is not a
propagating signal; the snap resets it per port. The propagation worry was
category confusion — treating a quantity that is reset at every boundary as if
it flowed through the chain.

### 2. Margin leads divergence

The carry port at width 4: margin min `0.048` (inside the band),
**2.5% ambiguous, 0.0% diverged** — the channel crowds the boundary while the
function is still perfectly correct, because the snap rescues the borderline
cases. And wherever the fabric crowds, `ambiguity% >= divergence%` (width-8
sum: 54% ambiguous vs 32.5% wrong). Margin is the strictly-earlier, more
conservative instrument: it flags a primitive operating near its limit before
any error appears.

### 3. Sharp edge; architectural crowding recovers

```
capacity stress, fixed-width students on the 200-member full-adder:
  width |  sum margin min/mean  amb%  div% | carry margin min/mean  amb%  div% | exact
      4 |   0.001 / 0.087  100.0  66.0 |   0.048 / 0.484   2.5   0.0 |  68/200
      8 |   0.002 / 0.255   54.0  32.5 |   0.499 / 0.500   0.0   0.0 | 135/200
     16 |   0.478 / 0.497    0.0   0.0 |   0.492 / 0.499   0.0   0.0 | 200/200
     32 |   0.491 / 0.498    0.0   0.0 |   0.496 / 0.500   0.0   0.0 | 200/200
     64 |   0.494 / 0.498    0.0   0.0 |   0.497 / 0.500   0.0   0.0 | 200/200
```

Between width 8 (135/200, crowded) and 16 (200/200, margin snapping from
`0.002` to `0.478`) the adder flips from failing to mastered and margin
recovers to near-maximal. Decimal addition is a **separable** task, so this
crowding is *architectural* (underfitting), recoverable with capacity. This is
one half of the crowding distinction; the other half (fundamental crowding) is
the pending probe — see below.

### 4. Margin–correctness orthogonality (not predicted)

The frozen `dec_full_add` is width-16, trained to 200/200 exact — and sits at
margin `0.330`. A fixed-width-16 student, also 200/200 exact, sits at `0.478`.
**Same architecture, same exact-correctness, margin 0.33 vs 0.48 — the entire
gap is training regime.** Correctness saturates long before margin does.

The verification stack certifies *correctness* and is **blind to margin**, so
it treats a 0.33-headroom primitive and a 0.48-headroom primitive as
equivalent — when the latter is far more robust to drift, perturbation, and
the confident-confusion failure that margin exists to catch. This was in
neither side of the prior argument; the instrument surfaced it.

## Implication: margin-floor certification

`btn_certify` already computes the number — `port_validate` thresholds it at
`0.25` and discards the rest. Certifying **"correct with margin >= X"** turns
the exact-replay contract into a *robustness* contract: same enumerable
mechanism, same instrument, a higher bar on a quantity already in hand. It is a
one-line policy change with no new machinery, and it converts an unused
dimension of the guarantee surface into a robustness guarantee the current
exact-on-clean-inputs check cannot express. Recorded here as the most
actionable consequence; not yet implemented.

## The first study's scope, and the probe it set up

The decimal study is one *separable* arithmetic domain: it establishes the
mechanism (snap-reset, margin-leads-divergence) and demonstrates *architectural*
crowding (capacity recovers it). It cannot, alone, answer the existential
question — **is the decomposable-with-large-margin island big enough?** — because
every concept it tests has a sharp boundary. That needs a domain where the
concept is intrinsically lossy at the interface, so margin *cannot* be made large
at any capacity. The second study builds exactly that.

## The fuzzy probe: separating fundamental from architectural crowding

`tests/fuzzy_study.c` (`make fuzzy`) constructs a **sharp** concept under a
**lossy** interface — no label noise; the ambiguity is purely information loss.
The concept is an M-band square wave (`label = parity(floor(M·x + φ))`, M = 6,
φ = 1/3 non-dyadic so no band boundary lands on a code edge). The net sees only a
k-bit code (the bin of `x`). A code whose bin contains a boundary **straddles**
it and carries both labels at the bin's true ratio — no capacity separates it.
Every other code is clean, but representing a 6-band wave needs ~6 hidden units,
so low-width nets underfit the clean bands (recoverable). Two instruments per
code: **margin** (raw output vs the snap boundary — the channel) and
**divergence** (the net's single *coarse* output vs the *fine-grained* truth
sampled inside the bin — the function).

A width × resolution grid (k = 7: 128 codes, 6 straddles, Bayes floor 25.9%):

```
  width | clean margin min/mean | clean div% | straddle margin max/mean | straddle div%
      2 |    0.452 / 0.480      |    2.5     |     0.466 / 0.398        |    29.6
      4 |    0.390 / 0.489      |    0.0     |     0.458 / 0.373        |    29.6
      8 |    0.478 / 0.499      |    0.0     |     0.373 / 0.311        |    29.6
     16 |    0.497 / 0.500      |    0.0     |     0.381 / 0.336        |    29.6
     32 |    0.497 / 0.500      |    0.0     |     0.401 / 0.360        |    29.6
```

5. **Architectural recovery.** Clean-code margin and divergence recover with
   *width*: below ~8 hidden units the wave is underfit (clean-margin-min dips into
   the band, a few percent divergence); at width >= 8 the clean codes are clean
   (margin ~0.50, 0% divergence), at every resolution. The separable half —
   capacity fixes it — matching the decimal sweep.

6. **The ambiguous fraction shrinks with resolution, not width.** The straddle
   count is fixed by the concept (6 boundaries), so the ambiguous input-fraction
   is `6/2^k` — 18.75% -> 9.38% -> 4.69% across k = 5, 6, 7. Capacity does not
   move it; only resolution does. Information loss is fixed by a better ADC, never
   by a bigger net.

7. **Fundamental crowding — divergence floors, capacity-immune.** Straddle
   divergence sits at the Bayes floor at *every* width: at k = 7, **29.6% flat
   from width 2 to 32**. No capacity reduces it below the irreducible per-bin
   error, because the information to separate the bin's two labels is not in the
   interface. The wall, measured: a residue resolution shrinks the *share* of but
   capacity cannot touch.

8. **Margin is leaky; divergence is the backstop it can't replace.** The straddle
   codes carry margin *mean 0.31–0.40 — above the 0.25 band* — so `port_validate`
   would wave them through as unambiguous, while divergence shows them ~30% wrong,
   irreducibly. Worse, capacity makes margin *less* trustworthy here: a more
   expressive net outputs more confidently on an information-starved code (straddle
   margin max reaches 0.500 at high width), masking the ambiguity behind a
   confident wrong answer. This is the orthogonality finding (#4) returning as an
   *instrument limitation*: on fundamentally-ambiguous inputs margin can certify
   the broken codes as clean, and only divergence — which floors at the Bayes rate
   regardless of capacity — reliably detects the wall. (The k = 5/6 grids are
   noisier — only six straddles, individual ones flip which side they snap — but
   divergence never trends down with width there either.)

## The stochastic probe: the third wall

`tests/stochastic_study.c` (`make stochastic`) replaces the comb's crisp parity
with a *graded* world: `P(label=1 | x) = sigmoid((x − θ)/τ)`, τ = 0.04. The label
is genuinely a coin flip near θ — not because the ADC is coarse, but because the
concept has no sharp boundary. The net still sees a k-bit code; divergence is
measured analytically against the true conditional (the Bayes floor is
`E_x[min(p, 1−p)]`, which no predictor beats).

The result completes the diagnostic, and the discriminator is **count vs
fraction**:

```
  k | ambiguous count | ambiguous fraction | ambiguous div % (width 4 / 8 / 16 / 32)
  5 |        6        |       18.75%        |   28.8 / 28.8 / 28.8 / 28.8   (floor 25.7%)
  6 |       12        |       18.75%        |   27.3 / 27.3 / 26.5 / 26.5
  7 |       22        |       17.19%        |   31.3 / 30.2 / 28.0 / 30.4   (floor 27.1%)
```

- **Divergence floors across BOTH width and resolution** — ~26–31% at every cell,
  at the Bayes rate. Neither a bigger net nor a finer ADC reduces it.
- **The ambiguous count GROWS (~2^k) while the fraction stays CONSTANT** (~18%).
  This is the exact inverse of the comb, where the count was *fixed* at the number
  of boundaries (6, 6, 6) and the fraction *shrank* `count/2^k` (18.75 → 9.38 →
  4.69%). A finer interface dissolves information-loss ambiguity; it cannot touch
  graded-world ambiguity, because the residue is in the world, not the encoding.

Here margin crowds *honestly* (ambiguous-margin mean ~0.24, in the band) rather
than going leaky, because the per-code training labels are genuinely mixed — the
net cannot become spuriously confident. On this wall CNET's only correct move is
to refuse confidence; the open architectural question is whether it can *detect*
the condition rather than be surprised by it, and the count-vs-fraction signature
is exactly that detector.

## The instrument suite and the three-wall diagnostic

Two instruments — **margin** (the channel: distance to the snap boundary, computed
free at every port) and **divergence** (the function: the net's output vs ground
truth) — separate three walls, each validated on a constructed domain:

| Wall | recovers with… | margin | divergence | the fix |
|---|---|---|---|---|
| **architectural** (net too small) | **width** | crowds, then recovers | recovers | a bigger net |

## Upgrade (post-study): data merging + structure connections via leaves/branches/subs

To avoid 10k files when growing to many specialists, CCE now has:
- `cce_forest_merge(dst, src, prefix)` for data merging into single .cce
- `cce_forest_connect(f, from, to, type)` + conn_types/names in branch for structure
  (0=sub-branch, 1=refines, 2=composes, 3=specializes)
- Generalized perceptual sub-forests for glyph/7seg/grid/block (cce_perceptual_*)
- Connections justify the hierarchy: one archive holds the whole connected
  tree of leaves → branches → sub-branches (e.g. general 7seg + amb subs linked).
- CCE leaf path default in habitat; evidence wired downstream; side-by-side
  metrics show effective margin improvement on aliasing cases.
- See cce_forest.h/c, cce_perceptual_leaf.*, glyph_habitat, README updates.

This turns "many tiny parts" into manageable connected structures without file explosion.
| **information-loss** (interface too coarse) | **resolution** (count fixed; fraction = count/2^k shrinks) | **leaky** — capacity disguises it | **floors, capacity-immune** | a finer ADC fixes the *amount* |
| **stochastic** (no sharp boundary) | **nothing** (count grows ~2^k; fraction constant) | crowds honestly | **floors, width- AND resolution-immune** | nothing — refuse confidence |

Operationally: **sweep width and resolution, read divergence.** Recovers with
width → architectural. Floors across width while the ambiguous *count* is fixed and
the *fraction* shrinks with resolution → information loss. Floors across both while
the count grows and the fraction holds → the graded world. Margin is the cheap
early warning you already compute, but it goes leaky exactly on the
information-starved codes — so divergence is the backstop it cannot replace.
Together they tell "your net is too small," "your interface is too coarse," and
"this concept has no boundary" apart — three walls the architecture must not
confuse. The first two have fixes inside CNET's reach; the third is the honest
edge of the discrete-fabric bet — and the suite can now *detect* it rather than be
surprised by it.

## Reproduce

```sh
make margin              # decimal: baseline (20000) + 8-digit depth pass
./margin_study --stress  # decimal: the capacity sweep
make fuzzy               # comb: information-loss grid + divergence backstop
make stochastic          # graded world: the third wall (recovers with nothing)
```

`make margin` loads the committed frozen `dec_full_add_weights.txt`; `make fuzzy`
and `make stochastic` train their own grids. All deterministic; all standalone
(link only `src/nn.c`).
