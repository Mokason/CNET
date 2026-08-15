# A Real Task That Decomposes: Compositional Generalization of a Finite-State Residue Scan — Design

**Date:** 2026-06-15
**Status:** Proposed
**Branch:** `chunk-capacity`
**Backbone:** one frozen transition primitive, composed over a digit sequence, vs a flat
monolith — measuring whether composition *generalizes beyond what can be enumerated*. Core
untouched; reuses BTN training, robust certification, and the DAG executor.

## The question

Every CNET demo so far has an **enumerable** domain (16 digits, 256 bytes, 20 000 additions),
so a flat network can simply memorize it — the composition is elegant but never *needed*. The
throughput lanes made composition fast and verifiable; none of it tested the thesis's **reach**:
is the decomposable-with-large-margin island big enough to do something a monolith can't?

This is the first task built to answer that. It computes **`n mod k`** for an N-digit base-`b`
number by composing a single frozen transition `δ(residue, digit) = (residue·b + digit) mod k`
over the digit sequence. The state — the residue — is the canonical *digest-not-dump* interface:
the finite, recoverable summary of an arbitrarily long prefix. The input space (`b^N`) is
astronomically beyond enumeration, so success *requires* generalization, not memorization.

## The task and parameters

- **Transition** `δ`: input ports `residue` (ONEHOT `k`, tag `residue`) + `digit` (ONEHOT `b`,
  tag `digit`); output port `residue` (ONEHOT `k`). `δ(r,d) = (r·b + d) mod k`.
- **Whole composition's output** is the final residue (ONEHOT `k`). "Divisible by k" = residue 0.
- **Parameters (default, tunable):** `b = 4`, `k = 7`, lengths `N ∈ {4, 8, 16, 32}`.
  - δ's domain is `k·b = 28` (residue, digit) pairs — trivially trainable and **exhaustively
    certifiable**.
  - Input space at length N is `b^N`: ~16.8M at N=12, ~4.3B at N=16 — orders of magnitude past
    the ~20 000 ceiling anything has ever enumerated. The composed path trains on **28 pairs** and
    must be right on billions of possible numbers.
  - `k=7` coprime to `b=4` gives a genuine 7-state cycle (not a degenerate automaton); `b=4`
    keeps δ tiny while `b^N` outruns enumeration fast. All tunable in the harness.

## Components (each a clear, independently-testable unit)

1. **`residue_step` (δ)** — a `BinaryTransformNetwork` with the two input ports and one output
   port above, trained on the full `k·b` table (the multi-input pattern of `combine` /
   `conditional_increment` in `src/main.c`), then certified exactly with `btn_certify_robust`
   (`src/contract.c`). The **only learned part** of the composed path.
2. **The scan (hand-built DAG)** — sources `[start_residue, digit_0, …, digit_{N-1}]`
   (`start_residue` is a constant ONEHOT residue 0); nodes `r_{i+1} = δ(r_i, digit_i)` with the
   same δ pointer at every step; the root is the last δ node, output = its residue segment. Built
   as a `DagPlan` with stack `DagNode`s and `owned = NULL` (the hand-built pattern from
   `tests/circuit_demo.c` / the existing `test_dag_shared_node_split`), run through `dag_execute`
   (`src/router.c`). Hand-built, so depth is bounded only by node count (~64 in the fast lane,
   unbounded in `dag_execute`), not by the planner's depth-8 search cap. Optionally also runnable
   through the `fp_dag` fast lane (bit-faithful) — a free tie-in, not the focus.
3. **`flat_mod` (the monolith baseline)** — a single `BinaryTransformNetwork` mapping the whole
   N-digit string (`N·b` one-hot inputs) → residue (ONEHOT `k`), trained on a **budget of `S`
   randomly-sampled** (string, residue) pairs via `btn_train_dynamic`.
4. **`residue_study` harness** (`tests/residue_study.c`) — trains + certifies δ, builds scans at
   several N, trains `flat_mod`, measures the four results below, and prints a
   `capacity_results`-style table. A budgeted study (`make residue`), like `margin`/`capacity` —
   not part of `make test`.

## Data flow

```
 d0 d1 … d{N-1}  (each ONEHOT b)        start r0 = residue 0 (ONEHOT k)
        └───────────► δ(r0,d0)=r1 ─► δ(r1,d1)=r2 ─► … ─► δ(r_{N-1},d_{N-1}) = n mod k
                      every handoff is a residue: finite, typed, snapped clean each step
```

The snap (validate + canonicalize) recanonicalizes the residue at every step, so margin is reset
per position and does not erode with depth — the margin-study result, now carrying a real
recognizer over many steps.

## The four results (what the experiment delivers)

1. **Data-efficiency gap (measured).** Composed (trained on 28 pairs) and `flat_mod` (trained on
   `S` sampled strings) are both evaluated on a **held-out** set of length-N strings neither saw.
   Expect composed ≈ 100%, flat ≪ that, at an enormous training-data ratio. Reported as exact-match
   accuracy on the held-out set for each path, plus the data ratio.
2. **Length-extrapolation (airtight).** Compose the scan at N = 4, 8, 16, 32 — all exact, because
   δ is position-agnostic and the snap does not erode. The monolith is **structurally** N/A: a
   fixed input width cannot accept a longer string. This axis holds regardless of axis 1.
3. **Verify-the-parts → bound-the-whole.** δ certified exact (28/28) with margin ≥ floor ⇒ the
   composed scan is correct for *all* strings, not only the tested ones — a guarantee a flat net
   cannot express. Reported: δ's certification result + min margin.
4. **Extensibility.** Swap δ for a different modulus (e.g. mod-3, mod-5) → the *same* scan
   composition computes the new residue with zero machinery change. Reported for 2–3 moduli.

## Metrics / success criteria

- Composed held-out exact-match ≈ 100% at every tested N; flat held-out exact-match materially
  lower, at orders-of-magnitude more training data.
- Composed exact at lengths well beyond any training; flat structurally inapplicable.
- δ certification: full-domain exact, margin ≥ the certification floor.
- Extensibility: each modulus's scan exact, composition code unchanged.

## Error handling / faithfulness

- The scan runs through the existing executor's validate-then-canonicalize discipline; an
  out-of-domain handoff is an error (should not occur for in-domain digits + a certified δ).
- Ground truth is `n mod k` computed directly in C; composed and flat outputs are compared by
  argmax of the k-way output. The held-out set is disjoint from any training data (the new
  train/test split this task introduces — absent from prior demos).

## Testing & scope

- **In `make test`** (fast, deterministic): δ trains and certifies exactly on its 28-pair domain;
  a short scan (small N) matches ground-truth `n mod k` on a sample. This is the correctness gate.
- **The study** (`make residue`, budgeted, not in `make test`): the data-efficiency gap, the
  length sweep, and the extensibility moduli — the parts that train `flat_mod` and run large
  held-out evaluations.
- **Reuse, not reinvention:** `btn_train_dynamic` (training), `btn_certify_robust` + the contract
  pattern (certification), `dag_execute` + hand-built `DagPlan` (the scan), the exact-count
  reporting pattern (`tests/capacity_study.c` / `capacity_demo.c`). New code: the residue domain
  (δ data generator + flat data generator), the scan builder, and the study. **Core untouched** —
  no changes to `nn.c`/`router.c`/`consolidate.c` semantics; this adds a new domain + a new study,
  exactly as the decimal domain did.

## The honest risk

Axis 1 (the data-efficiency gap) *assumes* `flat_mod` cannot generalize from a sample. Modular
arithmetic over digit strings is a sound bet — a known-hard "grokking" target for monoliths — but
**the study measures it; it does not presuppose it.** If the flat net surprisingly generalizes at
fixed N, that is itself a reportable finding, and **axis 2 (length-extrapolation) stays airtight**
either way (it is structural, not empirical). So the experiment cannot come back empty. If mod-k
proves flat-learnable, a non-arithmetic automaton (e.g. "contains 01", a harder DFA) is the
documented fallback for the data-efficiency axis.

## Out of scope (YAGNI)

- Variable-length within a single composed plan (each length is its own hand-built scan; that is
  the point — δ is length-agnostic). No scan/loop operator added to the core.
- Consolidating the length-N scan into one chunk (it would hit the capacity wall for large N — a
  nice connection to `capacity_results.txt`, but a separate follow-up, not part of this).
- The `fp_dag` fast lane for the scan is optional (bit-faithful, free) — measured only if cheap.

## Reproduce (target state)

```sh
make residue      # train + certify δ; the gap table, length sweep, extensibility moduli
make test         # includes the δ-certifies + short-scan-correct gate
```

`make residue` trains δ on its 28-pair table, certifies it exactly, then reports composed-vs-flat
held-out accuracy across lengths and moduli. Deterministic; the study trains its own nets. Core
untouched; this is a new domain + study in the spirit of the decimal domain and the instrument
suite.
