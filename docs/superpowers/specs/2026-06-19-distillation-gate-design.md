# Distillation Gate — 2-Leg Admission + Canonicality-as-Dedup — Design

> **IMPLEMENTATION NOTE (2026-06-19):** Shipped as **`EVIDENCE_CLEAR`-only**. The canonicality-as-dedup leg described below was **dropped as redundant** — implementation proved `library_evolve`'s worth-it guard + chunk-collapse already deduplicate same-goal plans pre-distill (a mint records the canonical digest but also collapses every future same-goal plan to length-1, rejected before the dedup check). The structural digest still ships in `src/scan.c` as reusable groundwork (and benchmarked at ~0.065 ms/call, not the ~20 ms asserted below). Original design text preserved for the record. See `memory/distillation-gate-built.md`.

**Date:** 2026-06-19
**Branch:** `chunk-capacity`
**Status:** Approved (brainstorm converged)
**Milestone:** Make library distillation *gated* — a proven plan becomes a frozen chunk
only when it is logically sound AND backed by enough live evidence; structurally-redundant
plans are deduplicated, not re-distilled.

## Context

`library_evolve` ([include/library.h](../../../include/library.h)) currently distills **every**
proven multi-step plan into a certified chunk. That is too eager in two ways:

1. **Trust:** a plan can win planning on *stale or thin* reliability evidence (a structural
   accident). Freezing it mints a permanent primitive on statistically thin ice.
2. **Redundancy:** the same goal-function is reachable by multiple plan structures (a *lure*,
   D>1 in the structural-preference sense). Distilling each variant wastes the (expensive)
   consolidation compute and clutters the library.

This design adds an **admission gate** in front of distillation. It is the payoff of the
structural-preference arc: now that the structural residual is a clean, sandboxed signal and
the ORDER_ONLY consumer is provably membership-safe, we can use the structural digest where it
actually belongs — and, importantly, *not* where it doesn't.

## The load-bearing insight (why the gate is 2 legs, not 3)

`consolidate_*` verifies the student against the strict teacher across the **entire enumerated
input domain** at `min_verify_rate` (default **1.0**). Therefore:

> If a plan passes certification, it computes the goal function correctly over the whole domain.
> Two *different* plan structures that both certify compute the **same** input→output function →
> produce the **same** training labels → distill to the **same** chunk.

The distilled artifact is a function of the **goal**, not the structure. So **structural
canonicality is not a correctness property** in this architecture (it would be in a probabilistic
system where paths carry semantic drift; here the finite, fully-enumerated, certified domain
makes all certified variants mathematically identical). Canonicality's real roles are
**deduplication** (don't re-distill the same goal-function) and, later, **priority** (which goals
to spend distillation budget on first).

This collapses the gate to two *correctness* legs and demotes canonicality to an *index*.

## The gate (Hard-AND, two legs)

A distilled chunk is **admitted** iff:

```
[CERTIFIED_TRUE]  AND  [EVIDENCE_CLEAR]
```

A **hard AND**, never a weighted score. A weighted score could admit an overwhelmingly
well-evidenced plan that fails certification by a hair — poison to a registry whose entire
premise is strict algebraic boundaries.

- **`CERTIFIED_TRUE`** — the chunk passes `btn_certify` against the goal contract (signature +
  behavior over the enumerated domain). This already exists inside `library_evolve` ("distill
  every proven plan into a *certified* chunk") plus the post-registration law guard
  (`rolled_back`). It is evaluated **after** distillation (you need the trained chunk to certify
  it).
- **`EVIDENCE_CLEAR`** — every primitive the plan composes has accrued enough live evidence to
  be trusted, using the **exact** lifecycle promotion criterion:
  `btn_reliability(p) >= promote_threshold (0.9)` AND
  `output_successes + output_failures >= min_evidence (16)`
  — the same bar `lifecycle_promote_provisional` ([src/router.c:4926](../../../src/router.c))
  applies for FUZZY→PROVISIONAL. The plan's evidence is the **weakest link**: ALL constituent
  primitives must clear the bar (the plan is only as trustworthy as its least-evidenced step).
  It is evaluated **before** distillation (it is cheap — just read counters — and it gates
  whether we spend distillation compute at all).

The two legs gate **different objects** and are not redundant: `EVIDENCE_CLEAR` judges the
**teacher plan** (are the parts we are composing trusted enough to freeze their composition?),
while `CERTIFIED` judges the **student chunk** (does the distilled primitive reproduce the goal
function over the whole domain?). A plan can be perfectly evidenced yet distill to a chunk that
fails certification, and vice-versa.

Constants `0.9` / `16` are passed in (matching how `lifecycle_promote_provisional` is invoked
project-wide), not hard-coded into the gate, so a deployment can tune the trust bar in one place.

## Failure routing (asymmetric, because the legs are epistemically different)

`CERTIFIED` is a **deductive** check (decidable now); `EVIDENCE_CLEAR` is a **statistic**
(improvable over time). So their failures route differently:

- **Fails `CERTIFIED`** → **DISCARD.** The chunk's logic is unsound (does not reproduce the
  teacher / violates a law). More data will not fix unsound logic. Throw it away.
- **Fails `EVIDENCE_CLEAR`** → **DEFER.** The plan is logically sound but its primitives are not
  yet trusted. Do not distill; leave the plan eligible. The primitives keep accruing evidence
  through normal runtime execution (and healing via the `RetrainQueue` when they *fail*), and
  `lifecycle_promote_provisional` clears the bar once the evidence arrives. A later admission
  pass re-tests and may then admit.

### DEFER semantics — explicit, because context matters

The evidence a primitive carries depends on where the gate runs:

- **Live / persistent registry** (runtime telemetry + `RetrainQueue` healing +
  `lifecycle_promote_provisional` running): DEFER is exactly the user-facing story — wait for the
  spine to accrue evidence and promote, then re-admit. No new queue is built; the `RetrainQueue`
  is the *healing* path for failing primitives, and ordinary execution is the *accrual* path.
- **In-process `library_evolve` driver** (registry starts empty each run; evidence is *seeded*
  from consolidation verification, not accrued between passes): within a single `library_evolve`
  call there is no live runtime between passes, so a DEFER mostly means "not enough seeded
  evidence this run." `library_evolve` already re-plans every pass to a fixed point, so a deferred
  plan is naturally re-tested on subsequent passes; it simply will not mint until its primitives
  carry the evidence. **DEFER never registers a new queue** — it is "skip + remain eligible."

This is an explicit non-magic point: DEFER is a *no-op-this-pass*, not a side-channel.

## Canonicality as a dedup index (not a gate leg)

- **Dedup key = the smallest plan digest in the goal's equivalence class.** Reproduction-count
  `rank-0` is *grid-dependent* (the order-only-A spec's own honest-semantics note: among symmetric
  lures the count margin is "scheme-dependent and may be thin"), so it can flip across runs and
  break dedup. `min(digest)` over the class is a **content-addressed, run-independent** identity —
  the perfect cache key. The Merkle digest is by name/structure, not pointer, so it is portable.
- **Use:** before spending distillation compute on a proven plan, compute its canonical digest and
  check it against a per-run **dedup index** (a set of already-admitted canonical digests). If
  present → **SKIP** (the goal-function is already frozen). If absent → proceed to the gate; on
  admission, insert the canonical digest into the index.
- **Cost & where it's paid:** the canonical digest requires the structural-preference *sweep*
  (~80 re-plans / ~20 ms; 200–2000× a single `dag_plan`) to enumerate the class. This is an
  **admission-time, batch** cost — never the live planner loop — and is consumed where the
  structural-preference sidecar already pays it (the order-only "consume where the cost is already
  paid" principle). v1 runs the sweep at admission per candidate goal; caching/upstream-artifact
  reuse is a noted optimization, not v1.

## Operational data flow (gate ordering follows cost)

```
for each proven plan P for goal G (in library_evolve):
    if !EVIDENCE_CLEAR(P):            # cheap (read counters)
        DEFER  -> skip, remain eligible
        continue
    key = canonical_digest(P)         # sweep (batch cost), smallest-in-class
    if key in dedup_index:            # cheap (set lookup)
        SKIP  -> already frozen
        continue
    chunk = consolidate(P)            # EXPENSIVE — only reached past the cheap pre-filters
    if !CERTIFIED(chunk):             # btn_certify + law guard
        DISCARD
        continue
    register_frozen(chunk); dedup_index.insert(key)   # FROZEN via registry_add_certified
```

The two cheap pre-filters (`EVIDENCE_CLEAR`, dedup) deliberately precede the expensive
`consolidate` so we never spend distillation compute on a plan that cannot be admitted or is
redundant. `CERTIFIED` is necessarily last (needs the trained chunk).

## Module boundaries (mechanism / policy / promotion)

- **`src/consolidate.c` — mechanism (unchanged).** The pure distiller: domain + teacher → trained
  student. Keeps its `min_verify_rate` self-check and member-counter snapshot/restore. No policy.
- **`src/library.c` — policy (the gate).** `library_evolve` gains: the `EVIDENCE_CLEAR`
  pre-filter, the dedup index + canonical-digest check, and the DISCARD/DEFER/SKIP routing around
  the existing distill→certify→law-guard flow. The gate is a small, testable helper
  (e.g. `library_gate_admit(...)`) so the loop stays readable.
- **`src/scan.c` / `include/scan.h` — promotion.** Move the structural digest + sweep + canonical-
  key extraction out of `tests/structural_pref_common.h` (currently static test-only) into
  compiled source: `spc_plan_digest` / `spc_hash_node` (structural Merkle hash), the sweep, and a
  new `structural_canonical_digest(reg, sources, goal) -> uint64` returning the smallest digest in
  the class. The test header then includes the src versions (single source of truth; existing
  structural-pref tests keep passing).

## What is reused vs. new

**Reused (no core changes):** `lifecycle_promote_provisional` criterion (0.9/16), `btn_certify` /
the existing chunk certification + law guard, `RetrainQueue` (healing path), `library_evolve`'s
re-planning loop and fixed-point stop, `registry_add_certified` (FROZEN), `consolidate_*`.

**New:** the `EVIDENCE_CLEAR` plan check, the dedup index + `structural_canonical_digest`, the
`tests/`→`src/scan.c` promotion of the digest/sweep. The planner/executor cores are untouched.

## Testing (red→green discipline, in `make test`)

- **EVIDENCE gate:** a plan whose primitives are below 0.9/16 → DEFER (not distilled); raise the
  seeded evidence past the bar → admitted. Assert the same constants the lifecycle uses.
- **CERTIFIED gate:** a plan that distills to a chunk that fails `btn_certify` (or violates a law)
  → DISCARD; registry unchanged.
- **Dedup:** two tasks (or two passes) whose proven plans are structural *variants* of the same
  goal-function → exactly **one** chunk minted; the second hits the dedup index and SKIPs. Assert
  the dedup key is the smallest-in-class digest and is **stable** across a re-run / grid change
  (content-addressed).
- **Attractor vs lure parity:** an attractor (D=1) and a lure (D>1) for equivalent goals each mint
  exactly one chunk (canonicality changes *which teacher labels*, never *what chunk*).
- **Footprint:** the canonical-digest sweep does not mutate registry / BTN counters (reuses the
  structural-preference footprint-free perturbation).
- **Regression:** `library_evolve` still reaches a fixed point and existing library tests pass.

## Non-goals (YAGNI)

- **Priority scheduler** (attractors-first distillation budget): out of v1. The artifact carries
  `D`, so the scheduler can be layered later without rework.
- **Cross-run persistent dedup** (a dedup index surviving process restarts): v1 is per-run, matching
  `library_evolve`'s in-process model.
- **Canonicalization-distance margin** (the per-plan snap-margin cert): explicitly *not* a gate
  leg — it is a static property that DEFER-to-evidence cannot fix; it belongs in a separate
  diagnostic layer if ever wanted.
- **GPU / heterogeneous execution:** unrelated axis.

## Open points for review

1. **DEFER in the in-process driver:** confirmed above as "skip + remain eligible" (no new queue).
   Acceptable, or do you want an explicit deferred-plan list with a re-test counter for visibility?
2. **Sweep cost at admission:** v1 runs a ~20 ms sweep per candidate goal to get the canonical
   digest. Fine for batch evolution; flag if you'd rather thread the upstream structural-preference
   artifact in to avoid recomputation even in v1.
3. **Dedup granularity:** smallest-in-class (sweep, cross-variant dedup) vs. plan-own-digest (no
   sweep, exact-structure dedup only). Spec commits to smallest-in-class per the brainstorm; the
   cheaper fallback is noted only as a future cost-optimization.
