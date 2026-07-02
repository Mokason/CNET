# Structural Preference B0 — Opt-in ORDER_ONLY Gate Probe — Design

**Date:** 2026-06-19
**Branch:** `chunk-capacity`
**Status:** Approved (brainstorm converged)
**Milestone:** **B0** — a *gated opt-in probe*, **NOT "B complete."** A (SHADOW_PLUS_RECOMMEND,
`planner_influence = 0`) is done/earned. B0 tests whether structural preference can be frozen and
consumed through the **existing** v2.2 ORDER_ONLY rank-artifact path as opt-in traversal bias —
without reopening the attention planner generally. B1 (per-task keying; possibly reopening the
attention planner under the README gate) comes later, only if B0 shows real gate-worthy benefit.

## The gate, stated plainly

The README gates the attention planner: *"resume only if … ≥3× fewer nodes or less wall time …
no fallback-rate increase, plan correctness preserved, and no synthetic dashboards."* B0 does **not**
claim to satisfy that gate or to reopen the attention planner. It is a *probe*: default runtime stays
`attention_mode = OFF`; the probe sets `ORDER_ONLY` **only inside its own fixture/study**, on a real
fixture (not a synthetic dashboard), to test one bounded question.

## Goal

Show that A's structural ranking can be (1) frozen into a `CircuitRankArtifact` and (2) consumed
through the existing ORDER_ONLY path as an **opt-in additive traversal bias** that flips the lure's
selected structure `alt_a → alt_b` (toward the attractor A recommends), while preserving the full
candidate set and every authority boundary — with default runtime unchanged (`OFF`).

## The hypothesis under test (honest)

The v2.2 bias is *additive*: the planner adds `rank_prior * 0.3` to a producer's advisory score
([router.c:947-950](src/router.c)) when `attention_mode >= ORDER_ONLY && rank_artifact`. Whether that
actually **flips a single-root pick** on our trivial fixture is the thing B0 measures. If it flips →
probe succeeds. If it doesn't → that is a **reportable finding** (the existing channel can't bias
single-root selection as-is; B1 must do planner work), not a failure to hide.

## Mechanics (recon-confirmed; test-only, NO core edits)

- **Build the artifact from A's ranking.** Run `structural_pref_rank`; re-derive the rank-0 structure
  via its recipe and read `plan.root->name` (the bridge from A's structure-digest to a producer name —
  rank-0 lure = `alt_b`). Populate a `CircuitRankArtifact` (`circuit_rank_artifact_init`,
  `order_only = 1`, `frozen = 1`): one row `{ task_key = "", producer_name = "alt_b",
  producer_output_port = 0, rank_prior = +高 }`. (`task_key = ""` is required — the planner's lookup
  calls pass `""`, [router.c:949,2564](src/router.c).)
- **Consume it opt-in.** In the fixture only: `reg.attention_mode = CNET_ATTENTION_ORDER_ONLY;
  reg.rank_artifact = &artifact;` then `dag_plan`. Restore to `OFF`/`NULL` after.
- **No router.c edits.** B0 only sets public knobs and populates a public struct. (Per-task keying —
  editing the planner's lookup call sites — is B1.)

## Keying limitation (reported, not hidden)

The lookup matches `(task_key, producer_name, output_port)` and **ignores `root_goal`/`output_sig`**
([router.c:1720-1728](src/router.c)); with `task_key = ""` the bias is **per-producer-name, global**,
not per-task. B0 reports this; B1 is where per-task keying gets solved.

## The B0 invariants (brutal; asserted in the anchor)

```
default_attention_mode_unchanged       = OFF   (registry_init leaves OFF; probe sets it locally)
fixture_sets_attention_order_only_explicitly = 1
artifact_off_reproduces_old_default    = 1     (OFF -> the registry-default pick, alt_a)
artifact_on_changes_order_only_selection = 1   (ORDER_ONLY+artifact -> alt_b, the attractor)
all_candidates_still_reachable         = 1     (alt_a still a valid producer; bias deprioritizes, never prunes)
no_prune_authority                     = 1
no_registry_mutation                   = 1     (registry byte-identical before/after)
no_reliability_mutation                = 1     (BTN counters byte-identical)
no_contract_or_cert_authority          = 1
no_self_write_during_planning          = 1
task_keying_limitation_reported        = 1
readme_gate_not_claimed_reopened       = 1
```

## Test matrix (the anchor)

1. `attention_mode = OFF` (default): pick == `alt_a` (registry-default).
2. `attention_mode = ORDER_ONLY`, `rank_artifact = NULL`: pick == P0 (the natural multihead pick) —
   record it, to isolate the artifact's effect from merely turning the mode on.
3. `attention_mode = ORDER_ONLY`, `rank_artifact = structural`: pick == `alt_b` (the flip) AND differs
   from case 2's P0 (or at least is the artifact's preferred producer) — the artifact *caused* it.
4. Reachability: `alt_a` still plans from an `alt_a`-only registry (proving it was deprioritized, not
   pruned); both producers still present (count unchanged).
5. Footprint: snapshot registry entries + BTN counters before the whole matrix, assert byte-identical
   after (no mutation, no self-write).
6. Restore: set `OFF`/`NULL` → pick reverts to `alt_a` (artifact-off reproduces old default).

If case 3 does **not** flip, the anchor records it as the honest finding (channel insufficient for
single-root bias) rather than asserting a falsehood — i.e. B0 is built to be falsifiable.

## Deliverables (test-only)

- Extend `tests/structural_pref_common.h`: a helper to build a `CircuitRankArtifact` from a
  `SpcRanked` (re-derive rank-0 → producer name → one row), and a helper to read the selected
  producer name under a given `(attention_mode, rank_artifact)` config.
- `tests/test_structural_pref_b0.c`: the gate-probe anchor (the matrix + invariants). In `make test`.
- Makefile: standalone `test_structural_pref_b0` target + wire into `test_all` + `test_all.c` dispatch.
- No `src/` changes.

## Honest claim after B0 (verbatim intent)

*Structural preference can be frozen and consumed through the existing ORDER_ONLY rank-artifact path
as opt-in traversal bias, preserving the full candidate set and all authority boundaries. This does
not reopen the attention planner generally; it is a gate probe with known coarse producer-name keying.*

## B1 (deferred)

Only if B0 shows real gate-worthy benefit: solve task keying properly (per-task lookup — edits the
planner path), and consider reopening the attention planner under the README gate (≥3× node/wall
reduction on non-tiny fixtures, no fallback increase, correctness preserved, no synthetic dashboards).
