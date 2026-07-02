# Structural Preference A — Post-hoc Attractor Recommendation (SHADOW_PLUS_RECOMMEND) — Design

**Date:** 2026-06-19
**Branch:** `chunk-capacity`
**Status:** Approved (brainstorm converged)
**Milestone:** Step A = **SHADOW_PLUS_RECOMMEND** — a ranked recommendation over an already-paid
sweep/candidate set. It is **NOT** ORDER_ONLY: `planner_influence` stays **0** (planner / executor /
registry untouched); evidence merely *ranks* alternatives. **Step B** is the first real **ORDER_ONLY**
(frozen rank artifact → traversal bias, full set preserved — the v2.2 immutable-rank-artifact pattern),
and remains the deferred influence-on step.

## The cost constraint that shapes this (recap)

A derivation-lock residual costs a perturbation sweep (~80 re-plans / ~20 ms full grid; ~8 / ~2 ms
lean) — **~200–2000× a single `dag_plan`**. So structural preference can never be consulted *inside*
the live planner loop. ORDER_ONLY must consume the signal where the cost is already paid. Step A does
that: it ranks a candidate set the sweep **already produced**, paying nothing extra.

## Goal

Upgrade the structural-preference sidecar from **report** (the residual) to **recommend**: over the
distinct valid structures a perturbation sweep surfaces for a goal, return them **ranked by
reproduction-count** (most-attractor-like first), each with a deterministic re-derivation recipe,
plus a demo consumer that selects rank-0 and re-derives it. **The planner is untouched** —
`planner_influence` stays **0**; A makes the sidecar *recommend*, B makes the planner *consume*.

## Non-goals

No planner/registry/executor modification (that's B). No prune / mutate / certify. No frozen
artifact. Compression / lawfulness / boundary-lock still deferred. A does **not** fix corrupted
(stale/adversarial) ranking — that's the margin signal's job (the later distillation-gate conjunction).

## What A adds

- `structural_pref_rank(reg, sources, goal, out_ranked)`: run the existing sweep (`spc_run_sweep`),
  **group the planned cells by digest**, and rank the distinct structures by **reproduction-count
  descending** (ties broken deterministically by lowest first-seen `perm_idx`). Each ranked entry =
  `{ digest, count, recipe = {perm_idx, beam, memo} }`. **All D distinct structures are returned — no
  prune.**
- **Order key = reproduction count** (number of perturbation cells that produced that digest). The
  most-reproduced structure is the most order-*robust* — it wins regardless of the arbitrary
  registry-order tiebreak — so it sorts to rank 0 as the recommended canonical structure.
- **Demo consumer:** take rank-0's recipe, re-derive via `spc_plan_under_perturbation`, and confirm
  the resulting plan's digest == rank-0's digest. Proves the recommendation is *actionable* (a real,
  reproducible plan) and *correct* (matches the ranked structure), without touching the planner.

## Honest semantics (when the recommendation is strong vs weak)

- **Genuine attractor (D = 1):** rank-0 is the unique structure (count = K). Confident — there *is*
  no alternative.
- **Genuine symmetric lure (D > 1, near-equal counts):** rank-0 is the more order-robust equivalent,
  but the count margin between ranks is **scheme-dependent and may be thin**. A recommends the
  more-robust one *harmlessly* — every returned structure is a valid equivalent, so any choice is
  correct; the recommendation is merely a canonicality preference. The recommendation is **strong
  only when one structure dominates reproduction**; among true symmetric equivalents it is honestly
  weak (and that's fine — there's no wrong answer to be weak about).
- This mirrors the sign-vs-magnitude rule: A confidently surfaces a dominant attractor; it is
  appropriately indifferent among symmetric equivalents.

## Authority (A is still `planner_influence = 0`)

A produces an advisory ranking + a demo selector; the core planner/registry/executor are unmodified.
Invariants (assert in the anchor): `planner_influence = 0` (planner untouched), `registry_mutation =
0`, `prune_influence = 0` (the ranked set contains **all** D distinct valid structures), footprint-
free (reuse the sweep's permuted-copy; registry + BTN counters byte-identical after). "Ordering off"
= registration-order = the planner's default pick, which **must still appear in the ranked set**
(reachable, never pruned).

## Anchor (TDD, in `make test`)

- **attractor:** `structural_pref_rank` returns exactly 1 structure, count == K; rank-0 re-derives to
  a valid plan whose digest == the baseline digest.
- **lure:** returns ≥ 2 structures, ordered by count descending; rank-0 re-derives to a valid plan
  matching rank-0's digest; **all D structures present** (no prune); the planner's default (baseline)
  digest is still in the set (reachable, possibly not rank-0 — that's the recommendation differing
  from the arbitrary default, the whole point).
- **footprint:** registry + BTN counters byte-identical after ranking both fixtures.

## Study

Extend `make struct_pref` to print, per fixture, the ranked candidate table (rank, digest, count,
recipe) and the rank-0 recommendation. CSV gains the ranked view (non-fatal write, as before).

## Implementation (small — extends the existing module; recon banked)

Everything reuses `tests/structural_pref_common.h` (the sweep, digests, `spc_plan_under_perturbation`
recipes). New: the `structural_pref_rank` grouping/sort + a `SpcRanked` struct + the demo selector;
anchor + study additions. No core changes. Subagent + TDD; verify anchor + `make test` green.

## Step B (deferred — the real influence-on step)

Frozen order artifact: compute the structural ranking **offline**, freeze it, and have the planner
consume it as cached **ORDER_ONLY traversal bias** (`planner_influence` 0 → order-only), reusing the
v2.x `CircuitRankArtifact` ORDER_ONLY pattern — never pruning, never mutating. Its own milestone.
