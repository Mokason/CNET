# Neural Lifecycle: Fuzzy → Provisional → Frozen → Reset, with Self-Healing — Design

**Date:** 2026-06-18
**Status:** Approved

## Goal

Turn the implicit, scattered trust state of a primitive (uncertified vs
`certified`, low vs high `btn_reliability`) into an explicit **lifecycle
state machine**, and use that spine to add four runtime behaviors that the
substrate does not have today:

1. **Self-healing re-route** — a strict execution that aborts because a
   primitive went out of domain re-plans around the fault and retries,
   transparently, instead of returning failure.
2. **Meltdown → retrain** — a runtime invariant failure (strict abort or a
   violated `CNET_PROPERTY` law) flips the offending primitive to `RESET`,
   captures the failing exemplar, and a "wake" pass repairs it — but **only
   ever retrains on a verified target** (scope C below).
3. **Shadow execution** — a fuzzy candidate runs beside the frozen incumbent
   on live traffic, accrues reliability evidence without affecting the
   production result, and hot-swaps in once it certifies and out-scores the
   incumbent.
4. **Cost-aware polymorphism** — among same-contract frozen alternatives, the
   planner picks by reliability blended with an inference-cost penalty gated
   by a power-mode knob.

**Non-goal:** changing the planner/executor/training cores. Like
`library.c`, the new code is a driver over existing public APIs. Every
behavior is **opt-in; zero-init reproduces today's behavior exactly** — the
identical-plan / byte-identical-weight-regen regression gate is the
discipline, as everywhere else in CNET.

## Why this is mostly wiring

Every building block already exists:

| Need | Existing API |
|------|--------------|
| Retrain a primitive | `btn_train`, `btn_train_dynamic` ([nn.h](../../../include/nn.h)) |
| Distill + law-guard + rollback loop | `library_evolve` ([library.c](../../../src/library.c)) |
| Per-primitive evidence | atomic `output_successes/failures`, `btn_reliability` ([nn.h](../../../include/nn.h)) |
| Trust proof | `btn_certify`, `btn_certify_robust`, `property_check` |
| Swap to a better impl | `contract_better_if`, `contract_swap_if_better` ([contract.h](../../../include/contract.h)) |
| Certified-only planning | `certified` flag, `require_certified` ([router.h](../../../include/router.h)) |
| Inference cost | MAC estimate already computed for consolidation artifacts |

The lifecycle enum is the **coordination point** the four behaviors read and
write; it is not a cosmetic relabel.

## Architecture: the spine

```
       registry_add          promote (evidence)        registry_add_certified
            │                       │                          │
            ▼                       ▼                          │
       ┌──────────┐  certify  ┌──────────────┐  certify        │
       │  FUZZY   │──────┐    │ PROVISIONAL  │──────┐          │
       └──────────┘      │    └──────────────┘      │          ▼
            ▲            └─────────────┐  ┌──────────┘    ┌──────────┐
            │  ───────────────────────┼──┼──────────────►│  FROZEN  │
            │                         heal succeeds       └──────────┘
            │                  (retrain + re-certify)          │
            │                                                  │ runtime
   heal fails: stay RESET                                      │ invariant
            │                                                  ▼ failure
            └──────────────────────── RESET ◄──────────────────┘
```

The only path *into* FROZEN is a passing `btn_certify` — from initial
certified registration, or from a successful `registry_heal` retrain. A
failed heal leaves the primitive in RESET.

- **FUZZY (0)** — registered, uncertified, little/no evidence. Default for
  `registry_add`. Numeric 0 keeps zero-init consistent.
- **PROVISIONAL (1)** — uncertified but accruing positive evidence
  (`btn_reliability` ≥ promote threshold AND `successes+failures` ≥ min
  evidence). Statistical trust only.
- **FROZEN (2)** — certified (`btn_certify`) or law-proven. The only path
  *into* FROZEN is a proof, never evidence alone — this preserves the
  meaning of `certified`.
- **RESET (3)** — was trusted, failed a runtime invariant; excluded from
  planning, queued for repair.

**Source-of-truth rule:** `certified` stays the authoritative "passed
`btn_certify`" bit (much code reads it). `state == FROZEN` is set whenever
`certified` is set; `RESET` overrides it for *planning eligibility* without
erasing the historical `certified` fact.

**Opt-in:** a new `int lifecycle_enabled` on `PrimitiveRegistry`
(`registry_init` zeroes it). When 0, planners ignore `state` entirely —
byte-identical to today. When nonzero, candidate enumeration in
`route_plan` / `dag_plan` / `dag_plan_circuit` **skips RESET** entries and
applies a FROZEN tie-break preference.

## Components / phases

Each component is a self-contained, separately-tested increment, mirroring
CNET's one-feature-at-a-time cadence. New orchestration lives in
`src/lifecycle.c` + `include/lifecycle.h` (a driver over public APIs, like
`library.c`); the enum and the `RegistryEntry.state` / `lifecycle_enabled`
fields live in `router.h`; executor fault reporting is a minimal addition
to `router.c`.

**Build order (locked):** Spine → Cost-aware → Self-healing → Meltdown →
Shadow. The spine is the foundation the trust-state behaviors need;
cost-aware is pulled up next because it is fully independent of the
lifecycle state (it needs only the planner and certified alternatives,
gated by its own `power_mode` knob), making it the lowest-risk early win.
The components are described below in conceptual (not build) order.

### Lifecycle spine (the foundation)

- `PrimitiveState` enum + `RegistryEntry.state` + `PrimitiveRegistry.lifecycle_enabled`.
- Transition helpers in `lifecycle.c`:
  - `registry_add` → FUZZY; `registry_add_certified` success → FROZEN.
  - `lifecycle_promote_provisional(reg, promote_threshold, min_evidence)` —
    sweeps FUZZY entries, promotes to PROVISIONAL by evidence (defaults:
    `promote_threshold = 0.9`, `min_evidence = 16` observations). Never
    reaches FROZEN (that needs a proof).
  - `lifecycle_set_state(reg, name, state)` for the RESET transitions used
    by later phases.
- Planner change: candidate filter consults `state` only when
  `lifecycle_enabled` (skip RESET, prefer FROZEN on ties).
- **Tests:** transitions on add/certify/promote; with `lifecycle_enabled`,
  planner skips RESET and prefers FROZEN; with it off, plans are
  byte-identical to the pre-lifecycle baseline (regression gate).

### Self-healing re-route

The exclusion mechanism **is** the RESET state — no new planner exclusion
parameter. On a strict abort, mark the fault RESET and re-plan; the spine's
RESET-skip does the routing-around.

- Executor reports the faulting primitive. New `ExecFault { const
  BinaryTransformNetwork *primitive; char name[CONTRACT_NAME_MAX]; size_t
  step_index; int reason; }`, filled by `route_execute` / `dag_execute` /
  `dag_execute_circuit` on a strict abort (NULL out-param = today's
  behavior).
- New opt-in wrappers `route_execute_healing(...)` / `dag_execute_healing(...)`
  in `lifecycle.c`: plan → strict-execute → on abort, mark the fault RESET,
  re-plan, retry, up to `max_reroutes` (default 3). Success when a re-plan
  executes clean; -1 when no alternative plan exists (genuinely
  unrecoverable).
- Requires `lifecycle_enabled`.
- **Tests:** registry with a deliberately-broken primitive plus a working
  same-contract alternative; strict exec aborts on the broken one; healing
  reroutes to the alternative; result correct; broken primitive ends RESET.
  No-alternative case returns -1.

### Meltdown → retrain (scope C: invariants + teacher + external hook)

**Triggers:** (a) the self-healing strict fault; (b) a runtime
`property_check` violation (`violated > 0`) on an active route — the
runtime analogue of `library.c`'s `law_violated`. The participating
non-FROZEN primitives go RESET and the failing exemplar is captured.

**The honest core:** a frozen net is deterministic and cannot "drift," so
the failures actually caught are composition-level law violations and
genuinely novel out-of-contract inputs. For a novel input we have an input
but no target. **The system never restores a primitive to FROZEN without a
certified target.** Target sources, in priority order:

- **A — invariant-derived.** If the failing input is in the primitive's
  contract domain, the contract exemplar table is the target. If a
  `CNET_PROPERTY` law relates this primitive to others, the law's other
  side (RHS chain, strictly executed) computes the target.
- **B — teacher-primitive.** If another registered primitive/composition
  produces a verified (in-domain, cleanly canonicalizing) output for the
  failing input, use it as the label.
- **C — external-oracle hook.** If neither A nor B yields a target, park
  the `(input, raw_output)` in an **unlabeled queue** behind a documented
  hook API. We build the queue + hook; the external labeler itself
  (human, or the "System-1" perception model) is out of scope.

**Data:** a `RetrainQueue` per RESET primitive (owned by its
`RegistryEntry`), with a labeled section `(input, target, source)` and an
unlabeled section `(input, raw_output)`.

**Hook API (built; labeler not built):**
```c
int    registry_supply_label(PrimitiveRegistry *reg, const char *name,
                             const double *input, const double *target);
size_t registry_pending_labels(const PrimitiveRegistry *reg,
                               const char *name, /* out */ ...);
```

**Wake pass:** `registry_heal(reg, laws, n_laws, cfg, report)` — for each
RESET primitive with a non-empty **labeled** queue: training set =
contract exemplars ∪ labeled queue; retrain via `btn_train_dynamic`;
re-run `btn_certify` **and** re-check `laws`; on success → FROZEN, clear
labeled queue, invalidate the stats sidecar (retraining invalidates
evidence — existing rule); on failure → stay RESET. RESET primitives with
only unlabeled failures stay RESET (self-healing keeps routing around them)
until an external label arrives.

**Chunks need no pointer-level unlinking.** Consolidation produces a
standalone student BTN; it does not call its member primitives at runtime,
so a member going RESET does not break a chunk's execution. A chunk is
itself a registry primitive, so it is subject to the same RESET/heal cycle
through the laws it participates in. (This corrects the "unlink from
compiled chunks" assumption in the source discussion.)

- **Tests:** A-path (under-trained primitive fails an in-contract input →
  heal retrains from contract → re-certifies → FROZEN); B-path (teacher
  supplies label for an out-of-contract input → heal succeeds); C-path (no
  target → stays RESET, shows in `registry_pending_labels`;
  `registry_supply_label` then `registry_heal` → FROZEN); **safety
  invariant** (no FROZEN restoration ever occurs without a passing
  `btn_certify`).

### Shadow execution

- A shadow candidate is a `RegistryEntry` with a back-reference
  `const char *shadow_of` (NULL = normal). Planners skip `shadow_of != NULL`
  entries (candidates never drive production) when `lifecycle_enabled`.
- The executor, after running an `active` primitive, also runs each of its
  shadow candidates on the same input, validates their RAW output, and
  records their `output_successes/failures`. Production output = active
  only. Gated behind `lifecycle_enabled` AND the presence of a shadow, so
  the unused hot path is untouched.
- `shadow_promote_if_ready(reg, contract, min_evidence)`: when a candidate
  is certified AND `btn_reliability(candidate) ≥ btn_reliability(active)`
  AND evidence ≥ min → promote via `contract_swap_if_better` semantics
  (candidate → active/FROZEN, old active demoted).
- **Tests:** register active + a better candidate as its shadow; run
  traffic; candidate accrues evidence while production results stay driven
  by active; promote swaps it in; after swap the candidate drives output.

### Cost-aware polymorphism

- `double btn_cost(const BinaryTransformNetwork *)` — MAC estimate
  Σ(in·h + h·out), the same metric already used for consolidation
  artifacts; optionally cached on `RegistryEntry`.
- `PrimitiveRegistry.power_mode` (0 = OFF = legacy). The per-candidate beam
  score becomes reliability with a cost penalty λ(power_mode)·cost:
  `HIGH_PERF` → λ = 0 (accuracy wins, == today), `LOW_POWER` → large λ
  (cheap wins). Implemented as the candidate comparator in beam ranking;
  same-contract alternatives are already supported.
- **Tests:** two certified same-contract primitives of different sizes;
  `power_mode` OFF → legacy choice (byte-identical plan); `LOW_POWER` →
  cheap primitive; `HIGH_PERF` → accurate primitive.

## Cross-cutting decisions

- **Opt-in / zero-init = legacy.** `lifecycle_enabled`, `power_mode`,
  `ExecFault*`, `shadow_of` all default off/NULL. The regression gate is
  byte-identical plans and weight regen with the lifecycle disabled.
- **Persistence.** Lifecycle state, retrain queues, and shadow links are
  **runtime-only**, like reliability counters and like `library_evolve`'s
  in-process registry ("starts empty each run"). FROZEN is re-derivable
  from `certified`. No new sidecar formats in this milestone (YAGNI); an
  optional RESET/queue sidecar can come later.
- **Threading.** Evidence counters are already atomic; shadow runs reuse
  them, so parallel shadowing is safe with no new synchronization.
- **Module boundaries.** `lifecycle.c`/`lifecycle.h` own orchestration
  (promote, heal, shadow, cost) over public APIs. `router.h` gains the
  enum + two `RegistryEntry`/`PrimitiveRegistry` fields. `router.c`
  executors gain only optional `ExecFault` reporting. The
  planner/executor/training/consolidate cores are otherwise untouched.

## Resolved decisions (review pass, 2026-06-18)

1. **Promotion thresholds** (FUZZY→PROVISIONAL): `reliability ≥ 0.9` and
   `≥ 16` observations, tunable via the promote call's args.
2. **`max_reroutes`** for self-healing: default 3.
3. **Build order:** Spine → Cost-aware → Self-healing → Meltdown → Shadow.
   Cost-aware is pulled up to second (most independent, lowest risk).
4. **`registry_heal`** stays a **separate function** from `library_evolve`
   (cleaner test isolation); the two may share helpers but are distinct
   entry points.
