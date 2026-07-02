# Gap-Triggered Acquisition Loop (v1) — Design

**Date:** 2026-07-02
**Status:** Approved; implemented 2026-07-02 (`src/acquire.c`, `make acquire`, gated in verify)
**Topic:** Close the autonomy loop: the router *detects* a capability gap and the system
*acquires* the missing unit on its own — mine exemplars from a registered oracle, train a
candidate BTN, push it through the EXISTING distillation gate + certification, seal it to
`.cnu`, register it, and the router plans through it. This is the graduation slice's
pipeline made **demand-driven** instead of corpus-driven: nothing new is learned unless a
real task asked for it and no capable plan existed.

Origin: the "liquid → freeze → contract" hybrid discussion (2026-07-02). The plastic
substrate in v1 is BTN training (the domains are enumerable/discrete); an LNN/CfC-style
*temporal* substrate is an explicit v2 direction, not built here. Approach mix chosen by
the user: **A** (acquisition queue + drain) as the spine, **C**'s oracle-fallback with
exemplar capture as the live path, **B** reduced to an inline "acquire-now" mode that
invokes the same drain function synchronously.

---

## 1. Goal & acceptance criteria

1. **Gap detection is recorded, not fatal.** Three trigger kinds land in a persistent
   ledger: `GAP_NO_PLAN` (planner found zero plans for a task signature),
   `GAP_LOW_RELIABILITY` (a plan exists but its product-of-reliabilities score is under a
   configured floor), `GAP_HEALTH` (an externally injected resource/health anomaly for a
   named unit — the lifecycle spine's fault machinery is the intended producer; v1 accepts
   injection, wiring the spine is follow-up).
2. **Acquisition composes only existing machinery.** Drain = mine exemplars from oracle →
   train candidate BTN → EVIDENCE_CLEAR gate (the `LibraryGateConfig` discipline from
   `src/library.c`) → `btn_certify` + coverage (PROOF on enumerable domains, else SAMPLE +
   Wilson) → seal to `.cnu` → register. No new gate, no new certifier, no new format.
3. **The candidate is structurally invisible until certified.** It is not in the registry
   during training/evaluation, so `planner_influence = 0` is a property of the
   architecture, not a flag.
4. **Oracle fallback keeps tasks answered while the gap is open** (single process, plain C
   function call — no network, no servers): when no plan exists and an oracle matches the
   task signature, the executor wrapper answers via the oracle and captures the
   validated input/output pair as a training exemplar for free.
5. **Failure never leaves partial state.** Gate or certification failure → gap marked
   `DEFERRED` with a reason; registry byte-identical, no weight file written, runtime
   unbroken.
6. **Vertical slice proven in the verify chain**: a withheld primitive is re-acquired from
   its reference-implementation oracle and the router then plans and executes through the
   fresh certified unit, end to end, in `make test`.

## 2. Non-goals (v1)

- **No liquid/LNN substrate.** BTN training is the plastic phase here. The temporal
  primitive class (ODE cells, freeze = fixed time-constants, SAMPLE-based certification)
  is the named v2 direction this loop is designed to slot into (shadow-promotion seat).
- **No strict-abort trigger.** Out-of-domain aborts stay out of the trigger set (decided
  during brainstorming); revisit when drift retraining has a story.
- **No spine wiring.** `GAP_HEALTH` entries can be injected (tests do); connecting the
  lifecycle fault detector as a producer is follow-up work.
- **No changes to planner/executor internals.** Detection wraps their public results;
  nothing inside `dag_plan`/`route` changes.

## 3. Components

### 3.1 Gap ledger — `include/acquire.h`, `src/acquire.c`

In-memory table + text sidecar (`CNET_GAPS 1` header line, one record per line), following
the `CNET_STATS` sidecar rules: **never inside a weight file**, load replaces state,
missing/malformed → `-1` with state untouched.

Record fields:

- task signature: input ports/tags → output ports/tags (the same port typing the planner
  matches on; no semantics, per the domain-general rule)
- trigger kind: `GAP_NO_PLAN | GAP_LOW_RELIABILITY | GAP_HEALTH`
- subject unit name (for LOW_RELIABILITY / HEALTH; empty for NO_PLAN)
- oracle name (empty until matched)
- status: `OPEN | DEFERRED | CLOSED`
- counters: times hit, exemplars captured, acquisition attempts
- defer reason (short string, empty otherwise)

Duplicate gap events (same signature + kind) increment `times hit` on the existing record
rather than appending.

### 3.2 Oracle registry — same module

`typedef int (*CnetOracleFn)(const double *in, double *out, void *ctx);`

Named oracles registered with the SAME port-signature typing as primitives. An oracle is
**not** a registry primitive — the planner never sees it. The harness registers reference
implementations (or, later, CCE-loaded external models) at startup. Oracle outputs pass
through validate-then-canonicalize exactly like `registry_label_via_teacher`
(`src/router/registry.c:778`) does for teacher BTNs; an output that fails
`port_validate` is a rejected exemplar, counted against the oracle.

### 3.3 Trigger points (thin wrappers, no core edits)

- `acquire_note_no_plan(ledger, sig)` — called by the executor wrapper / harness when the
  planner returns zero plans.
- `acquire_note_low_reliability(ledger, sig, unit, score, floor)` — called when a plan is
  found but scores under the floor. **Non-blocking**: the plan still runs; the gap is
  bookkeeping for the drain pass.
- `acquire_note_health(ledger, unit, reason)` — external injection point.

### 3.4 Oracle fallback executor — same module

`acquire_execute_or_fallback(reg, ledger, oracles, sig, in, out)`:
plan exists → normal strict execution. No plan + matching oracle → oracle answers;
validated/canonicalized pair appended to the gap's exemplar buffer (in-memory, bounded,
oldest-dropped; buffer size in config). No plan + no oracle → error to caller, gap noted.

### 3.5 Acquisition pass (drain) — same module

`acquire_drain(reg, ledger, oracles, cfg, report)` — for each `OPEN` gap with a matching
oracle:

1. **Assemble exemplars**: captured pairs + active mining (enumerate the input domain if
   enumerable within `cfg.mine_budget`, else uniform sample). Reserve a **held-out
   smoke split** (`cfg.holdout_fraction`) untouched by training. **Class-balance check**
   before training (the JSON-contract lesson: certification needs balanced classes in
   training); unfixable imbalance within budget → `DEFERRED("class imbalance")`.
2. **Train candidate BTN** with the lean teacher-recipe defaults (dual-track 3C); never
   grown from a 1-hidden-neuron start (saturation trap).
3. **Gate**: EVIDENCE_CLEAR against `cfg.gate` (threshold + min evidence, defaults from
   `library_gate_config_defaults`). Fail → `DEFERRED(reason)`.
4. **Certify**: generate the contract from the task signature + oracle behavior;
   `btn_certify`; coverage = PROOF when the domain was fully enumerated, else SAMPLE +
   Wilson bound with the sampled flag set.
5. **Seal + register**: write `.cnu` (weights + contract, one sealed binary), register as
   certified, reliability counters start at the uninformed prior (no fabricated history).
6. **Verify**: replan the task signature — a plan must now exist; run the held-out smoke
   exemplars strict. Then mark `CLOSED`.

`LOW_RELIABILITY` / `HEALTH` gaps take the **rebuild path**: same steps, but the subject
unit stays active until the replacement certifies; then atomic registry replace + stats
sidecar invalidation (the retrainer-invalidates rule).

### 3.6 Inline mode (the "part of B")

`acquire_now(reg, ledger, oracles, cfg, sig, report)` — the same drain body scoped to one
gap, invoked synchronously, followed by one replan. No separate training logic exists in
any call stack; planning and training stay in separate modules.

## 4. The loop (end to end)

```
task arrives ─▶ plan?
   yes, score ≥ floor ─▶ execute (normal path, untouched)
   yes, score < floor ─▶ execute + acquire_note_low_reliability   [gap logged]
   no plan ─▶ oracle for signature?
        yes ─▶ oracle answers (in-process) + capture exemplar     [gap logged]
        no  ─▶ error to caller                                    [gap logged]

acquire_drain (batch, or acquire_now inline):
   exemplars (captured + mined, class-balanced)
   ─▶ train candidate BTN            [candidate NOT in registry]
   ─▶ EVIDENCE_CLEAR gate            fail ─▶ DEFERRED, nothing written
   ─▶ btn_certify + coverage         fail ─▶ DEFERRED, nothing written
   ─▶ seal .cnu ─▶ register (fresh prior counters)
   ─▶ replan: plan now found ─▶ CLOSED
```

## 5. Invariants (the discipline section)

1. **Structural planner_influence=0**: candidates live outside the registry until sealed +
   certified + registered. There is no code path by which an uncertified candidate can be
   planned over.
2. **Δ-4 counter hygiene** (from the CNET-D arc): any acquisition step that
   strict-executes through *registry* BTNs before the gap is closed must snapshot and
   restore their reliability counters verbatim. v1's drain avoids this by construction
   (training/certification never route through the registry; the post-registration verify
   step records evidence legitimately) — and the test asserts byte-identical counters
   across a drain that ends in DEFER.
3. **Sidecars only**: ledger and exemplar buffers never touch weight files. Weight files
   stay pure function definitions.
4. **Validate-then-canonicalize on every oracle boundary** — same discipline as every
   other handoff.
5. **No fabricated evidence**: a freshly acquired unit starts at the uninformed
   reliability prior; oracle agreement during training is training data, not deployment
   experience.
6. **DEFER is total**: a deferred acquisition leaves registry, weight files, and counters
   byte-identical to before the attempt.

## 6. Error handling

- Gap with no matching oracle → stays `OPEN`, counted, skipped by drain (recorded demand
  is itself useful output).
- Oracle emits `port_validate`-failing output → exemplar rejected + counted; rejects over
  `cfg.max_oracle_reject_rate` → `DEFERRED("oracle unfit")`.
- Mining budget exhausted before class balance → `DEFERRED("class imbalance")`.
- Gate / certify failure → `DEFERRED` with the failing check named.
- Ledger sidecar write failure → warn, continue (in-memory state authoritative for the
  process lifetime; same posture as stats sidecars).

## 7. Testing (TDD, gated in verify)

`tests/test_acquire.c` + `make acquire`, wired into the `make test` verify chain. Fixture:
an existing enumerable domain chain (hex increment style) with **one primitive withheld at
registry load** and its reference implementation registered as the oracle.

1. **NO_PLAN acquisition (the headline):** withheld primitive → planner fails → gap logged
   → drain → unit trained, gated, certified (PROOF coverage — domain fully enumerated),
   sealed, registered → replan finds the plan → strict execution correct on the full
   domain.
2. **Fallback capture:** with the gap open, N tasks answered via oracle fallback — answers
   correct, exemplars captured; drain trains from captured pairs + top-up mining.
3. **Inline acquire-now:** one synchronous call takes the same fixture from no-plan to
   executable plan.
4. **LOW_RELIABILITY rebuild:** a unit with poisoned counters under the floor → gap →
   rebuild → certified replacement swapped in, stats sidecar invalidated, fresh prior.
5. **DEFER totality:** a broken oracle (invalid port output) → drain → `DEFERRED("oracle
   unfit")`; registry and all counters byte-identical (Δ-4 assert); no `.cnu` written.
6. **Ledger round-trip:** save/load preserves records + statuses; malformed file → `-1`,
   state untouched.
7. **Duplicate gap coalescing:** same signature hit twice → one record, `times hit` = 2.

Benchmarks (printed, not gated): per-phase wall time (mine / train / gate+certify / seal /
replan) and exemplar counts, matching the graduation-slice reporting style.

## 8. v2 directions (named, not built)

- **Liquid/temporal substrate**: an LNN/CfC-style primitive class whose plastic phase is
  continuous-time; freeze = fixed time-constants; certification = SAMPLE + Wilson +
  conformal abstention (non-enumerable domains). The drain's train step becomes
  substrate-dispatched; everything downstream (gate, certify, seal, register) is already
  substrate-agnostic.
- **Shadow-promotion economics** (approach C in full): candidate trains in the shadow seat
  while the oracle answers, promotion flips the router — reuses
  `registry_set_shadow`/`registry_run_shadows` (`src/router/registry.c:657-870`).
- **Spine as HEALTH producer**: lifecycle fault detection feeds `acquire_note_health`.
- **Oracle-from-CCE**: a loaded external model (GGUF/safetensors) as the oracle — the
  "extract existing models into smaller, certified units" track, on demand.
