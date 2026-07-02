# Structural Preference Sidecar — Derivation Lock (SHADOW_ONLY) — Design

**Date:** 2026-06-19
**Branch:** `chunk-capacity`
**Status:** Approved (brainstorm converged); review before recon/build
**Milestone:** a new CNET-D sub-track. **Not** "taste," not compression, not planner-visible, not
mutation. Narrow claim only (see Claim).

## Claim (deliberately narrow)

Among **already-valid** structures, CNET can measure whether a structure is **attractor-like** under
bounded re-derivation perturbations. This is an *advisory structural-preference signal* — not a
validator, not a certifier, not a learned human-taste model.

## Context — why this is non-trivial

Validity is binary and stays pure (the hard gate is untouched). "Structural preference" is an
advisory ranking *over the valid set only* — a bad candidate still fails the gate; a valid-but-ugly
one passes but may rank below a valid-and-elegant one. It sits **above** the validator, never inside
it (the validator must never accept/reject on preference).

The naive self-consistency metric is a **trap**: CNET's transforms are deterministic and idempotent,
so re-executing a valid plan, re-canonicalizing its output (`snap(snap(x))=snap(x)`), or re-planning
the same task with the same registry all return *identical* results — the residual is structurally
zero and discriminates nothing. The real signal is **reproducibility under perturbation**: does the
same structure reappear when the *derivation path* is varied? Tasteful = attractor-like (many routes
converge); arbitrary = fragile (one ordering only).

## Two residuals (kept separate)

### 1. Derivation lock — the headline (structural attractor score)

Re-derive the same task under planner/search perturbations; count how often the same **canonical
plan digest** reappears.
```
K = perturbations_tried
R = perturbations reproducing the baseline canonical plan digest
D = distinct valid plan digests seen
X = perturbations yielding invalid / no plan
derivation_lock_residual = 1 − R/K      (0 = attractor/settled · →1 = fragile/arbitrary)
```

### 2. Boundary lock — OPTIONAL telemetry this slice (execution stability)

Perturb valid *inputs* within the accepted canonicalization margin; does the canonical output / law
result stay stable? Measures input/boundary robustness (relevant to learned leaves + the
accepted-leak lesson), **not** structural re-derivation — so it is not merged into derivation lock.
```
P = input perturbations tried ; S = same canonical output ; L = laws still hold
boundary_lock_residual = 1 − S/P ; law_stability = L/P
```
First slice: include only as optional telemetry; **headline is derivation lock.**

## Perturbations (recon-corrected)

For **single-root** planning with deterministic ranking, beam size and memo do **not** change *which*
producer wins among equal-reliability alternatives — the top-ranked is always picked first; the beam
only governs fallback availability. So they cannot, by themselves, surface alternative structures.
The discriminating perturbation is **candidate ordering**.

- **Primary — candidate-order permutation.** Plan against a *permuted copy of the registry's entry
  array* (a fresh `RegistryEntry[]`, shallow per-entry so the borrowed BTNs are shared; the array is
  permuted, the BTNs and the original array are never written → **footprint-free, no Δ-4 needed**).
  Different permutations break reliability ties differently, surfacing genuinely different valid
  structures wherever alternatives exist.
- **Secondary — search-depth robustness** (footprint-free on a scalar shallow copy): beam sizes
  `{1,2,4,8,0}` and `disable_plan_memo` on/off. These test whether the structure survives search-depth
  changes, not selection.
- **Still deferred (needs Δ-4):** reliability jitter (writes shared counters). Candidate-order gives
  the discrimination without it.

Footprint: both the permuted-entries copy and the scalar shallow copy leave the production registry,
its entry array, and all BTN counters **byte-identical** (the hard invariants assert this).

## Canonical plan digest (the comparison key, NOT a signal)

Two plans with the same real structure must hash equal even if allocated differently.
- **Include:** task_key; root goal signatures; node count; node primitive *names*; output ports
  used; edge list; source-slot bindings; root mappings; port signatures/tags.
- **Exclude:** memory addresses; allocation order; temporary node IDs (if renumbering changes them);
  wall time; search counters; telemetry-only fields.

## Hard invariants (explicit pass/fail gates)

```
structural_preference_authority = 0   planner_influence = 0   prune_influence = 0
certification_influence = 0   registry_mutation = 0   reliability_mutation = 0
contract_mutation = 0   weight_mutation = 0
validity_gate_unchanged = 1
normal_plan_digest_unchanged = 1      normal_execution_result_unchanged = 1
```
The sidecar perturbs **shadow copies**; the production registry/planner/execution path stays
**byte-identical** (verified by snapshot before/after, including BTN counters — Δ-4).

## Report

```
valid=1   advisory_only=1   structural_preference_authority=0
baseline_plan_digest=…   baseline_valid=1
perturbations_tried=K   reproduced_structure=R   distinct_valid_seen=D   invalid_or_no_plan_seen=X
derivation_lock_residual = 1 − R/K
boundary_perturbations_tried=P   same_canonical_output=S   boundary_lock_residual = 1 − S/P   (optional)
laws_checked=N   laws_held=M
structural_preference_score = advisory_only
```
Score shape — kept boring:
```
score = reproduced_fraction + law_hold_fraction − distinct_valid_penalty − invalid_or_no_plan_penalty
```
No compression term yet (it only becomes meaningful once the library has enough reusable chunks).

## SHADOW_ONLY → (later) ORDER_ONLY

Slice one is SHADOW_ONLY: report the score, `planner_influence = 0`, Δ-4 preserved. Only after
evidence accrues may it become ORDER_ONLY — "try the lower-derivation-lock-residual valid candidate
first" — never prune, never certify, never mutate. (Matches the v2.x memory-hint / engram /
rank-artifact lineage: surface evidence first, traversal-bias only later.)

## First fixture (symbolic — NOT learned leaves)

**Single-root** (`dag_plan` / `DagPlan`) for slice one — the multi-root ripple-carry attractor is
`dag_plan_circuit` / `CircuitPlan` (a different API/struct), deferred. Both fixtures built fresh from
small synthetic producers (like the below-beam decoys):
- **Attractor:** a goal produced by exactly **one** distinct-named chain → every candidate-order
  permutation reproduces it → D = 1, residual 0.
- **Lure:** a goal produced by **≥2 DISTINCT-named** chains (genuinely different structures, equal
  reliability) → permutations pick different ones → D ≥ 2, residual > 0.

**Do NOT reuse the `planner_scale_study` collision** (its k producers are all named `"producer"`):
under a name+port digest, same-named interchangeables are the *same structure*, so it collapses to
D = 1 and reads as an attractor. That is the *correct* structural reading — which is precisely why
the lure needs **distinct** names. Boundary lock (learned leaves) comes later.

## Recon (MANDATORY before build)

1. Confirm the shallow-safe perturbations (beam, memo, goal-order) change *only* scalar registry
   fields / call arguments — never the shared `entries` array or BTN counters — so a shallow copy
   (`lifted = *reg`) suffices with zero footprint (extends the v5.1 finding).
2. Confirm a canonical plan digest is computable from the `DagPlan` node tree (names/ports/edges/
   source bindings) excluding addresses/IDs/counters; identify the traversal.
3. Confirm an existing domain offers a goal with **both** a robust (near-unique) derivation **and** a
   multi-equivalent one (for attractor vs counter-fixture). The collision setup gives the latter; find
   or construct the former.
4. Confirm planning is deterministic given fixed (registry, beam, memo, goal-order) so each
   perturbation point is reproducible.

## Recon outcome (2026-06-19)

- **Plan digest is computable** from a planner-built `DagPlan`: the planner sets `node->name`
  (`src/router.c:2209`) and each `DAG_PRIMITIVE` node's produced port is
  `node->btn->output_ports[node->output_index]`. Use a **Merkle-style structural hash** — `hash(node)
  = FNV(kind, name, out-port family/width/tag, output_index, [hash(child) for each slot in order],
  child_ports)` — reusing the FNV mixing of `plan_cache_signature` (`src/router.c:1803-1814`).
  Identity is by **name/structure, not pointer** → stable across re-plans.
- **Single-root scope** for slice one (`dag_plan` → `DagPlan`); multi-root `dag_plan_circuit` /
  `CircuitPlan` digesting deferred.
- **Footprint:** `dag_beam_limit` / `disable_plan_memo` are scalars (shallow copy safe); the
  candidate-order perturbation uses a permuted *copy* of the `entries` array (BTNs shared, never
  written) — also footprint-free, so **no Δ-4 needed** for slice one.
- **Discrimination caveat:** beam/memo do not change single-root producer selection (deterministic
  top-rank); candidate-order permutation is the perturbation that surfaces alternatives. Lure must use
  distinct-named producers (same-named interchangeables collapse to one structure — correctly).

## Implementation plan (subagents; TDD)

1. Recon: the four questions, file:line.
2. Test first: a hermetic anchor — attractor fixture → low residual; counter-fixture → high residual
   / D>1; all hard invariants hold (incl. byte-identical production path). In `make test`.
3. The digest function + the perturbation sweep (shallow-safe set) + the score + reporting (table +
   CSV under `artifacts/structural_pref/`).
4. Makefile: a budgeted `make struct_pref` target (build+run, **not** in `make test`); wire the
   anchor into `test_all`.
5. Verify: anchor green in `make test`; study emits table + CSV; no new warnings.

## Out of scope

Compression scoring (deferred until the library is meaningful). Boundary lock beyond optional
telemetry. Reliability-jitter / candidate-order perturbations (need isolation; follow-up). Any
planner visibility / ORDER_ONLY (later). Mutation/prune/certification authority (never). Human/
semantic taste (the representation wall — a learned distribution's job, not this).
```
