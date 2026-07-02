# CNET-D v5.0 — Proposal Sidecar (SHADOW_ONLY) — Design

**Date:** 2026-06-18
**Branch:** `chunk-capacity`
**Status:** Approved (brainstorm converged)
**Milestone:** v5.0 of the CNET-D arc (v5.1 Below-Beam Recovery · v5.2 Leak-Abstention Sidecar · v5.3 Diffusion Backend)

## Context

CNET-D is an *advisory imagination lane* beside the deterministic core: a proposer
generates candidates, the existing strict CNET machinery validates them, and nothing the
proposer does carries authority. The slogan is **"diffusion proposes; CNET disposes"** —
but diffusion is only a *later backend* (v5.3). The durable win is the **no-authority
sidecar architecture**, which slots into the already-proven SHADOW_ONLY / `influence_on_planner = 0`
pattern from the v2.x engram/rank-artifact thread.

v5.0 does **not** answer "can the sidecar improve CNET?" (that is v5.1). It answers only:

> **Can CNET safely listen to an imagination lane without granting it authority?**

## Goal

A mechanism-agnostic, plan-typed proposal sidecar that can (1) produce candidate plans,
(2) validate them through *existing* CNET strict execution, and (3) emit telemetry — while
provably changing nothing about the planner, registry, executor, stats, or library.

## Non-goals (verbatim)

No learned model. No symbol proposals. No registry mutation. No planner ranking influence.
No automatic fallback. No plan replacement. No new validation semantics.

## The contract (`include/proposal_sidecar.h`)

Plan-typed envelope — **not** a generic opaque-payload + validator-hook (that would create a
second validation universe beside CNET, the exact shape authority holes sneak through).

```c
typedef enum {
    CNET_PROPOSAL_PLAN_ROUTE   = 1,
    CNET_PROPOSAL_PLAN_DAG     = 2,
    CNET_PROPOSAL_PLAN_CIRCUIT = 3
} CnetProposalPlanKind;

/* Documented growth path for v5.2+ (symbols). NOT a field in the v5.0 report —
   a one-value shell adds nothing now; it earns a home when symbols exist. */
/* CNET_PROPOSAL_FAMILY_PLAN, CNET_PROPOSAL_FAMILY_SYMBOL (future) */

typedef struct {
    CnetProposalPlanKind kind;

    /* no-authority invariants (constants for v5.0; asserted in the gate) */
    int advisory_only;             /* 1 */
    int authority;                 /* 0 */
    int planner_influence;         /* 0 */
    int registry_mutation_allowed; /* 0 */
    int strict_validator_required; /* 1 */

    /* outcome counters — "accepted" is never one bit (Δ-1) */
    int proposed_count;
    int strict_executed_ok_count;  /* ran with no out-of-domain handoff (strict mode) */
    int strict_rejected_count;     /* aborted: out-of-domain / type-inconsistent */
    int matched_expected_count;    /* executed AND output == fixture ground truth (when known) */

    const char *proposer_name;
    const char *task_key;
} CnetPlanProposalReport;
```

**Δ-1 rationale:** strict execution can *succeed* on a type-valid plan that still computes the
wrong output. Those are different facts and the difference is load-bearing for v5.1, where a
recovery means `matched_expected && ranked-below-beam`, not merely "ran clean."

## The validator — boring by mandate

No new validation logic. The validator **is** the existing executor:

```
candidate DagPlan
  -> port type-consistency check (existing)
  -> dag_execute(...) with plan.strict = 1   (aborts on any out-of-domain handoff)
  -> strict_executed_ok = (rc == 0)
  -> matched_expected   = strict_executed_ok && argmax(output) == fixture ground truth
```

The sidecar never asserts "valid." It records: *I produced this candidate; CNET's strict
executor accepted/rejected it.*

**Δ-4 (discovered during API recon — load-bearing):** `dag_execute` records reliability evidence
(`output_successes` / `output_failures`) into the *borrowed* BTNs as a side effect, and those
counters feed the planner's reliability ranking and beam. So naively strict-executing candidates
would let the sidecar *indirectly influence the planner* — violating `planner_influence = 0`.
Enforcement: before validating any candidate, snapshot the reliability counters of every BTN the
candidate references; **restore them verbatim afterward.** Strict-executing a candidate must leave
the stats reality reads bit-for-bit unchanged. (The counters are runtime-only / never persisted, so
in-memory save+restore is sufficient and exact.) This is the mechanism that makes "imagination
leaves no footprint on reality's evidence" literally true, not aspirational.

## The v5.0 proposer — deterministic, scripted

For the contract-proof milestone the proposer emits a small fixed set, by construction
covering both gate paths:

- **≥1 known-valid candidate** — e.g. replay a known-good plan for the fixture task with
  reordered nodes (a structurally different but semantically equivalent DAG).
- **≥1 known-invalid candidate** — a deliberately type-broken / wrong-producer DAG that strict
  execution must reject.

Deterministic on purpose: byte-identical reproducibility is the project's regression gate, and
v5.0 must not depend on entropy to prove the harness. Randomized / below-beam search is v5.1.

## No-authority proof mechanism

The sidecar receives the registry as `const PrimitiveRegistry *` (read-only by type). The gate:

```
1. run the official planner on the fixture; snapshot (official plan + registry)
2. run the sidecar (proposer + strict validation + telemetry)
3. assert (official plan + registry) snapshot is BYTE-IDENTICAL after
```

This repurposes the project's existing determinism idiom as the authority gate: stats,
rankings, and library are provably untouched. The snapshot **must include each touched BTN's
`output_successes`/`output_failures`** (see Δ-4) — those are the bytes most likely to move, and
the save/restore is what keeps them fixed.

## Fixture & home

Reuse an existing simple task (a route/DAG demo goal + its registry); do not invent a domain.
This is a **hermetic gate in `make test`** (like `test_residue`'s correctness gate), not a
budgeted study — every criterion is a deterministic invariant.

## Pass criteria

- `authority == 0`, `registry_mutation == 0`, `planner_influence == 0`,
  `official_plan_unchanged` (byte-identical) `== 1`, `strict_validator_required == 1`
- **anti-creep centerpiece:** the known-invalid candidate → counted in `strict_rejected_count`,
  *and* no registry change, no stats promotion, no planner fallback, telemetry only
- the known-valid candidate → reported only (counters update), official plan still untouched
- `strict_executed_ok_count` and `matched_expected_count` are tracked separately and both
  non-trivially exercised (≥1 each across the candidate set)
- **(Δ-4)** every touched BTN's `output_successes` / `output_failures` are **byte-identical**
  before vs. after the sidecar runs — strict validation of candidates leaves no reliability footprint

## Implementation plan (ordered; for subagent execution, TDD)

1. **API map (read-only):** confirm exact signatures/types — `DagPlan` / `DagNode` / `DagSource`
   / `dag_execute` / `plan.strict`, the registry type name + how the official planner is invoked
   (`route_plan` / `dag_plan*`), and how to snapshot the registry for a byte-identical compare.
   Reference the hand-built DAG pattern in `tests/residue_common.h::run_scan` and
   `tests/circuit_demo.c` / `tests/test_dag.c`.
2. **Test first:** `tests/test_proposal_sidecar.c` — fixture task, official-plan + registry
   snapshot, run sidecar, assert all pass criteria (esp. the anti-creep reject).
3. **Header:** `include/proposal_sidecar.h` — the contract above.
4. **Impl:** `src/proposal_sidecar.c` — scripted proposer + strict-validation harness +
   telemetry, read-only over the registry.
5. **Makefile:** add `proposal_sidecar.{c}` to `test_all`, plus a standalone `test_proposal_sidecar`
   target (mirroring `test_residue` wiring, with `-Wno-unused-function` if it shares helpers).
6. **Verify:** `make test_proposal_sidecar` then `make test` — both green; warnings clean in the
   new files.

## Out of scope (this milestone)

v5.1 Below-Beam Recovery (the trap fixture; `found_valid_below_beam = matched_expected &&
ranked-below-beam`), v5.2 Leak-Abstention Sidecar (cheap disagreement sources, then the symbol
family), v5.3 Diffusion Backend (one engine among randomized/ensemble/template/diffusion). Each
earns its own spec → plan → build cycle.
```
