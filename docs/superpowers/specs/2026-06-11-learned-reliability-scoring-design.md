# Learned Reliability Scoring — Design

Date: 2026-06-11
Status: Implemented 2026-06-11 (TDD; `make test` + all demos green).
Repo: G:\AI\CNET (not a git repo — written, not committed)

## Problem

When several primitives can fill the same role (compatible representation and
tag, or wildcard), the planners pick by registry order — arbitrary. The
project already hit the failure this invites: an undertrained `combine`
(loss stalled at 0.0269) silently broke a downstream DAG (see
2026-06-09-dag-composition-design.md). "Learned scoring" has been the
deferred frontier since the routing layer was built.

## What is learned, and from what signal

**Reliability statistics from execution outcomes.** Both executors already
stand at the one place where quality is observable: the primitive's RAW
output, before producer-side canonicalization snaps it. An output that is
in-domain under the primitive's own output port (`port_validate`) is evidence
of a well-trained primitive; an ambiguous one (e.g. a bit at 0.55 about to be
rounded to 1.0) is exactly the early signature of the undertrained-`combine`
failure. Note this raw-output check is NEW — previously a marginal internal
output was snapped silently and no one ever looked at it.

Each `BinaryTransformNetwork` gains two runtime counters
(`output_successes` / `output_failures`), updated by `route_execute` and
`dag_execute` after every forward. Recording never changes execution
behavior: an ambiguous output is still snapped and the run still succeeds
(a hard-fail strictness knob is deferred). Score:

    btn_reliability(btn) = (successes + 1) / (successes + failures + 2)

Laplace-smoothed: a fresh primitive scores the 0.5 prior; evidence moves it
toward the observed output-validity rate. RAW output ports always validate,
so RAW-output primitives drift toward 1.0 — benefit of the doubt, documented
(RAW means "cannot verify"; it cannot produce failure evidence either).

Counters are runtime-only (zeroed by `btn_init`/`btn_load`): the weight file
stays v4 and frozen artifacts stay pure function definitions. Persisting a
reliability sidecar is deferred until cross-run accumulation matters.

Rejected alternatives: static authored scores (stored loss — metadata, not
learning, and incomparable across output dimensionalities); a learned value
network over plans (massive scope, violates keep-the-core-domain-general for
no demonstrated need).

## How planners use it

Both planners iterate primitive alternatives in **reliability order**
(descending `btn_reliability`, ties broken by registry order — stable
insertion sort over an index array, built once per plan call):

- `route_plan` (BFS): hop-count optimality is untouched — shorter chains
  still always win. Reliability decides which producer claims an output type
  when several produce the same type, and orders same-length alternatives.
  Per-type greedy, not a global chain-score optimum; documented.
- `dag_solve` (complete backtracking): alternative ORDER changes, the
  alternative SET does not — completeness is unaffected. First-found plan is
  now greedy-by-reliability. Sources are still tried before primitives
  (real data beats synthesis).

**Backward-compatibility invariant:** with no recorded outcomes all scores
equal 0.5, the stable sort preserves registry order, and every plan is
identical to today's — all existing tests and demos must pass unmodified.

## Tests (TDD)

- test_contract.c — score unit: fresh = 0.5; 3 successes -> 0.8; mixed ->
  4/7 (fields are public; set directly).
- test_router.c — recording: a saturated output (output_bias forced high,
  output weights zeroed -> sigmoid ~ 1.0) records a success; a zeroed output
  path (sigmoid(0) = 0.5, ambiguous) records a failure and the run still
  succeeds. Preference: two same-contract producers, unreliable one
  registered first (failures=10 vs successes=10); the equal-length route must
  go through the reliable one. Pin: with fresh stats, registry order holds.
- test_dag.c — preference: same shape through the DAG planner (assert the
  consuming slot's child is the reliable producer); recording via a one-slot
  DAG on a crafted-output primitive.

## Verification gate

Full rebuild + suite + all demos (no weight regeneration needed: training
math and file format untouched; demos run on existing v4 weights and must
find identical plans by the invariant above).

## Out of scope (YAGNI)

- Persisting stats (reliability sidecar / v5).
- Hard-fail on ambiguous raw output (strictness knob).
- Global plan-score optimization (product of reliabilities, beam search).
- Context-dependent scores (per-consumer, per-tag) or learned value models.
