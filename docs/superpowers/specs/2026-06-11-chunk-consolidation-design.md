# Chunk Consolidation (Plan Distillation) — Design

Date: 2026-06-11
Status: Implemented 2026-06-11 (TDD; `make test` + `make chunk` green).
Repo: G:\AI\CNET (not a git repo — written, not committed)

## Problem

A proven multi-step plan is re-planned and re-executed as k hops forever. The
library accumulates, but knowledge never CONSOLIDATES: planner search depth
grows with capability, k forwards cost k inferences, and the planners' plan
space (the asset) becomes the search's combinatorial liability. Deferred from
the global plan-score design as "memoization / shared sub-results"; this is
the stronger form — today's proven plan becomes tomorrow's single primitive.

## Decision: distill, don't macro-cache

A consolidated chunk is a NEW BinaryTransformNetwork trained with the proven
plan as teacher (enumerate the plan's input domain, label by executing the
plan, train, verify against the teacher). The chunk is a regular v5 weight
file plus a regular CNET_STATS sidecar — the planner/executor core does not
change at all, which keeps the core domain-general. Rejected alternative: a
macro/composite registry entry executing the chain internally (exact by
construction, but threads a new node kind through both planners and both
executors, and needs hard invalidation when members retrain; a distilled
chunk is self-contained — it owns its weights and earns its own evidence).

Trigger is an explicit API call (caller decides which plan is proven), not an
automatic policy. Scope covers both plan kinds: a route chunk is a
single-in/single-out primitive; a DAG chunk is a multi-input primitive.

## API (new module: include/consolidate.h, src/consolidate.c)

```c
typedef struct {
    size_t initial_hidden;   /* 0 = auto: min(max_hidden, max(in, out)) */
    size_t max_hidden;       /* 128 */
    double learning_rate;    /* 0.8  (combine lesson: undertrained breaks DAGs) */
    size_t max_epochs;       /* 160000 */
    size_t growth_window;    /* 1000 */
    double target_loss;      /* 0.0015 */
    double min_improvement;  /* 0.01 */
    unsigned int seed;       /* 131u */
    double min_verify_rate;  /* 1.0 — refuse anything less than perfect */
    size_t max_samples;      /* 4096 — enumeration cap */
} ConsolidateConfig;

void consolidate_config_defaults(ConsolidateConfig *cfg);

typedef struct {
    size_t samples;         /* teacher-labeled training samples */
    size_t teacher_aborts;  /* inputs the strict teacher refused (excluded) */
    size_t verified;        /* student raw in-domain AND canonical == teacher */
    size_t missed;          /* everything else */
    double final_loss;      /* btn_train_dynamic result */
} ConsolidateReport;

int consolidate_route(const RoutePlan *plan, const ConsolidateConfig *cfg,
                      BinaryTransformNetwork *out_student,
                      ConsolidateReport *report);
int consolidate_dag(const DagPlan *plan, const DagSource *sources,
                    size_t n_sources, const ConsolidateConfig *cfg,
                    BinaryTransformNetwork *out_student,
                    ConsolidateReport *report);
```

cfg NULL = defaults; report optional. On success (0): out_student is trained,
ports set, counters SEEDED (see Evidence). Caller saves/registers/frees. On
refusal/failure (-1): out_student untouched, report still filled when given.

## Contract derivation

- Route chunk: input port = steps[0]->input_ports[0]; output port =
  steps[length-1]->output_ports[0]. Requires length >= 2 (one step is already
  a primitive; zero is identity).
- DAG chunk: input ports = sources[0..n-1].type in INDEX ORDER (the caller's
  source order is the chunk's slot order); output port = root primitive's
  output_ports[root->output_index] (the projected segment). Requires a
  DAG_PRIMITIVE root, >= 2 primitive executions in the tree, and every source
  consumed (dag_plan guarantees at-most-once; we additionally demand
  exactly-once so the chunk's arity tells the truth).
- Tags ride along by value (Port contains the tag buffer), so chunks remain
  composable wherever the plan's boundary was.

## Domain enumeration (deterministic)

Canonical members only, ports left to right, later ports/fields fastest:
ONEHOT field -> hot index 0..w-1 (w values); BINARY_MSB/LSB field -> value
0..2^w-1 encoded in the family's bit order. Multi-field/multi-port = cartesian
product. RAW is not enumerable -> refuse. Total > cfg->max_samples -> refuse.

## Teacher pass (strict) and evidence hygiene

Labels come from route_execute / dag_execute on a strict=1 shallow copy of the
plan: an input the teacher cannot handle cleanly (any member OOD) is EXCLUDED
from training and verification (teacher_aborts), so the chunk distills only
proven behavior. Executor outputs are already canonical -> clean labels.

Distillation sweeps are not deployment experience: every distinct member
BTN's (output_successes, output_failures) pair is snapshotted before the
teacher pass and RESTORED after it, so consolidating neither inflates nor
poisons member evidence (and re-consolidating is idempotent on stats).

## Verification gate and evidence seeding

After training, every kept sample is replayed through the student alone:
verified requires the RAW output in-domain (port_validate — same bar the
executors score) AND canonical output identical to the teacher's label.
rate = verified / kept; rate < min_verify_rate (or kept == 0) -> refuse, free
the student, nothing registered. Otherwise the verification outcome seeds the
chunk's counters: successes = verified, failures = missed. That is honest
evidence (real executions over the full domain), not an invented prior — a
16/16 sweep gives (16+1)/(16+2) ~= 0.944, so a fresh chunk immediately beats
a fresh 2-chain (0.25) and competes with evidenced chains; the existing
plan-score objective needs NO changes to start preferring chunks.

## Student capacity (implementation finding)

Distillation must NOT start the student from the usual 1-neuron seed. During
the first growth windows every input projects through that bottleneck; at
lr 0.8 the output layer saturates on a low-rank approximation of the target,
and squared-error sigmoid gradients at the rails (~1e-4) are too weak to
repair the confidently-wrong bits afterwards — later neuron growth arrives
with zero output weights and inherits the starved gradients. Measured on the
real primitives: ~1/8 seeds verified from a 1-neuron start (the failures sat
at exactly k wrong bits, fully saturated); 24/24 seed/size combinations
verified when starting at the task's width. Hence initial_hidden 0 = auto
(min(max_hidden, max(input total, output total))). This also explains why
nn_demo's primitives carry hand-tuned seeds/rates: grow-from-1 is a gamble
that consolidation, being automated, cannot take. Restarts on verification
failure were considered and dropped (YAGNI) — auto capacity made every
probed case deterministic-reliable, and the gate still refuses honestly.

## Planner interplay (no code changes)

registry_add the chunk next to its members. route_plan/dag_plan already
maximize Π reliability, so they pick the 1-step chunk exactly when its
evidence beats the chain product — and keep preferring the spelled-out chain
when the members' accumulated evidence still wins. Consolidation never
deletes the chain; both remain, scored on equal terms.

## Tests (TDD, tests/test_consolidate.c)

- Refusals: route length < 2; RAW boundary port; enumeration over
  max_samples; DAG with bare-source root / single primitive / unconsumed
  source; unverifiable student (max_epochs=0) -> -1, report shows missed.
- Route happy path: two tiny in-test primitives (ONEHOT4 -> BINARY2 ->
  BINARY2), consolidate; CHECK samples/verified counts, port + tag copying,
  seeded counters, student forward == teacher on all inputs; after
  registry_add, route_plan returns the 1-step chunk plan.
- DAG happy path: two-source tree (decoder x2 + combiner), consolidate;
  CHECK 2 input ports in source order, projected output port, verification,
  and that dag_plan now roots at the chunk.
- Evidence hygiene: member counters byte-equal before/after consolidate.
- Ladder: an evidence-seeded chunk plans as an INTERIOR member of a deeper
  plan (swap(pair_chunk) beats the spelled-out subtree on score), that plan
  consolidates again (chunk of chunk, verified 4/4), and the deeper goal
  collapses back to one step.
- All synthetic primitives trained in-test with fixed seeds (fast at these
  sizes); CHECK macro + exit-code conventions as in test_router/test_dag.

## Demo (tests/chunk_demo.c, make chunk)

make chunk: nn_demo regenerates frozen weights (sidecars invalidated ->
members fresh at 0.5), then chunk_demo: (1) plan hex_digit -> increment chain
(2 steps, score 0.25), consolidate as "hex_increment" (16 samples), print
verification + before/after plans and scores — planner now picks the chunk;
save hex_increment_weights.txt + _stats.txt. (2) plan the flagship DAG
combine(hex_value(hi), hex_value(lo)) -> byte, consolidate as
"hex_pair_to_byte" (256 samples, 2 onehot16 ports), re-plan — root is now the
chunk fed by the two sources directly. Demonstrates: k forwards -> 1 forward,
search depth k -> 1, chunk reusable as a member of deeper plans.
(3) The ladder: with split fresh, the planner honestly prefers the direct
hex_value for a nibble goal (0.5 vs 0.5 x ~0.99); sixteen hand-run
executions of split(hex_pair_to_byte) give split real evidence, replanning
then routes THROUGH the chunk (score ~0.94), and consolidating that plan
yields "hi_nibble_from_hex_pair" -- a chunk of a chunk (256/256) that wins
the next replan. Deep composition emerges from deployment evidence, not
registration alone.

## Out of scope (YAGNI)

- Provenance manifest (which plan/members a chunk was distilled from) — the
  chunk is self-contained; lineage is narrative metadata for now.
- Sampling for non-enumerable domains (the cap refuses honestly instead).
- Automatic consolidation policy (thresholds, naming, scheduling).
- Macro/composite plan caching (rejected above, revisit if a domain defeats
  enumeration AND distillation).
