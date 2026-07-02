# Strict Execution (Hard-Fail on Ambiguous Output) — Design

Date: 2026-06-11
Status: Implemented 2026-06-11 (TDD; `make test` + all demos green).
Repo: G:\AI\CNET (not a git repo — written, not committed)

## Problem

When a primitive's RAW output is ambiguous (a bit at 0.55, a mushy one-hot),
the executors record a reliability failure but snap the value and keep
going. That is the right lenient default — but some deployments want
fail-fast: better no answer than an answer laundered through a rounding
step. Deferred as "hard-fail strictness knob" since the learned-scoring
design.

## Decision: opt-in `strict` flag on the plan

`RoutePlan` and `DagPlan` gain `int strict`. Zero (the value every existing
caller already has via `= {0}` init) = today's lenient behavior, so the
change is invisible until opted into. Nonzero = an out-of-domain RAW output
aborts the execution (-1 / NULL unwind) instead of being canonicalized.

- The knob is EXECUTION policy, so it lives on the plan object (what to run
  + how to run it), not on the primitive (a primitive is not inherently
  strict) and not as a new executor parameter (API churn at every call
  site). Flow: plan, then set `plan.strict = 1`, then execute.
- Planners RESET strict to 0 on success (route_plan copies a queue node over
  *out, so an explicit reset is also a correctness requirement — otherwise
  the field would be stack garbage).
- **Evidence is recorded either way**: strict mode aborts AFTER incrementing
  output_failures. Fail-fast must not blind the learner.
- Scope: the knob governs only producer-side RAW-output ambiguity — the one
  place execution currently snaps-and-continues. External inputs and
  handoffs already hard-fail on out-of-domain values in both modes.
- For multi-output primitives the whole-output health rule applies: in
  strict mode an ambiguous UNCONSUMED segment also aborts (a sick primitive
  is not trusted just because the consumed segment looks clean).

## Tests (TDD)

- test_router.c: strict plan over the crafted ambiguous primitive
  (sigmoid(0) = 0.5) returns -1 AND still records the failure; the same
  plan with a saturated output executes normally; flipping back to lenient
  snaps and proceeds (regression pin).
- test_dag.c: same shape through dag_execute; plus the multi-output case —
  consumed segment saturated, unconsumed segment ambiguous, strict -> -1.

## Out of scope (YAGNI)

- Per-step / per-primitive strictness overrides.
- A threshold knob (how ambiguous is too ambiguous) — port_validate's
  domain bands are the single source of truth.
