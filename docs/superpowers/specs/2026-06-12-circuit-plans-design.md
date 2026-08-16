# Circuit Plans: Shared Nodes, Multi-Root Goals, Memoized Search — Design

**Date:** 2026-06-12
**Status:** Approved

## Goal

Close the decimal-domain finding ("multi-output circuits are programs,
not plans"): let the planner discover, execute, verify and distill
circuits — plans where one primitive execution feeds multiple consumers
and several goals are satisfied by one coordinated structure. Plus
planner memoization with an honest measured speedup.

Motivating instance: ripple-carry. Goals {sum0, sum1, cout} from sources
{a0, a1, b0, b1, cin}; the ones-place adder's sum feeds a root while its
carry feeds the tens-place adder — one execution, two consumers.

## The sharing rule: port-disjoint fan-out

**A node may have multiple consumers only if every consumer reads a
distinct output port of that node.**

Why this rule and not "share anything type-compatible": naive sharing is
score-better by construction (one execution = one reliability factor),
so the planner would collapse `combine(hex_value(s0), hex_value(s1))`
into `combine(hv(s0), hv(s0))` — type-correct, cheaper, and a different
function that silently ignores a source. Port-disjointness kills that
class entirely: single-output primitives can never be shared, so **every
existing plan is provably unchanged**. What it permits is exactly the
motivating shape: multi-output fan-out (`split`'s two nibbles to two
slots; an adder's sum to a root and carry to the next adder).

The rule also does semantic work. For goals {sum0, sum1, cout}: both sum
goals are port-0 reads, so they must come from two distinct adder
executions (no degenerate sum0 = sum1 collapse); cin is the only
non-adder carry producer, so one adder consumes it and the other must
consume the first's carry-out; that carry edge uses up the ones-adder's
port 1, so cout is forced onto the tens-adder's port 1. Types + the rule
force the ripple topology. The remaining freedom (which digit pair feeds
which adder) is settled by the existing deterministic source-index
tie-break, and semantic correctness is established the way this project
always establishes it: exhaustive strict verification, not trust.

## Representation

- **Per-edge port selection.** A consumer records which output port of
  each child it reads: `child_ports[k]` alongside `children[k]`.
  Fallback rule for compatibility: an edge value of 0 means "use the
  child's `output_index`" (the planner normalizes shared nodes to
  `output_index` 0, so 0-valued edges are exact). Zero-initialized
  hand-built trees (chunk_demo, test_decimal) keep working untouched.
- **Multi-root plans.** A circuit plan carries `roots[]` with a per-root
  output port; execution writes the root segments concatenated in goal
  order.
- **Plan-level node ownership.** Planner-built plans own their nodes in
  a flat array; freeing iterates the array (never recurses), so shared
  nodes cannot double-free. Hand-built stack plans remain caller-owned
  as today.
- **Scoring.** The plan score multiplies each DISTINCT execution's
  reliability once. Sharing is rewarded because one execution is one
  chance to fail. Unshared duplicates (two adders) still count per
  execution. Sources contribute 1.0.

## API

| Function | Contract |
|----------|----------|
| `dag_plan` (existing) | Signature unchanged. Gains port-disjoint sharing as a search alternative; for every input where sharing is not strictly score-better or newly enabling, output is identical to today (single-output-only registries: provably identical). |
| `dag_execute` (existing) | Signature unchanged. Evaluates each node once per run (per-node result memo); reads per-edge ports with the fallback rule; one reliability outcome per execution; strict mode semantics unchanged. |
| `dag_plan_circuit` (new) | `(reg, sources, n_sources, goals[], n_goals, *out)`. Multi-root branch-and-bound over the same alternative classes (source, reuse, new primitive). REQUIRES every source referenced at least once (arity-truth, matching consolidation's rule; also prevents goal-starved degenerate circuits). Returns -1 if no circuit exists. |
| `dag_execute_circuit` (new) | Executes all roots over shared structure in one run; writes concatenated root segments; out_cap checked against the total. |

`route_plan` / `route_execute` are untouched.

## Planner memoization

Per-planning-call cache, used by both `dag_plan` and `dag_plan_circuit`
(stats are frozen during one planning call, so cached scores cannot go
stale):

- **Key:** (goal type-class — family, width, count, tag — and the
  available-source bitmask).
- **Value:** the Pareto set of (score, consumed-source-delta, sub-plan)
  for the NO-SHARING sub-search. One best answer is not enough: a
  higher-scoring subtree that consumes more sources can starve a
  sibling slot, so dominated entries only (worse score AND superset
  consumption) are dropped.
- **Sharing stays outside the cache.** Reuse alternatives depend on
  which nodes exist in the partial plan (context the key cannot see),
  so they are enumerated per-slot as today. The combined search still
  visits both classes — optimality is preserved.
- **Honest measurement:** a stress benchmark (synthetic registry, ~32
  primitives, deep goal) reports wall-clock planning time with the memo
  on vs off (runtime knob, default on). The speedup claim goes in the
  demo output, not just prose.

Worst case stays exponential in source count (the Pareto sets are
bounded by 2^n masks, n <= 8 today); the memo removes re-derivation,
not the combinatorics. Documented, not hidden.

## Circuit chunks

- `consolidate_circuit`: enumerate the source domain (one-hot boundary
  rule from the decimal milestone applies), execute the circuit as a
  STRICT multi-root teacher, train a MULTI-OUTPUT student (v5 weight
  format already supports it; output ports = root ports in goal order,
  tags ride along), verify every segment in-domain and exact, seed
  evidence with the verification outcome. Same refusal contract as
  `consolidate_dag`.
- `contract_from_circuit`: the same teacher emits a multi-output
  contract; the chunk must certify against it (signature gate already
  handles multi-output ports).
- **Primary demo (guaranteed):** distill the discovered 1-digit circuit
  {sum, cout} over (symbol, symbol, carry) — 200 samples, one shared
  adder execution read at both ports, two roots. Functionally this
  re-derives `dec_full_add` as a certified single chunk — small, but it
  exercises every new mechanism.
- **Stretch (allowed to fail honestly):** distill the 2-digit ripple
  circuit into `dec_add2` (17 inputs, 9 outputs, 20,000 samples — needs
  a raised `max_samples` and a wide student). If the student cannot
  reach 100% verification, consolidation refuses; we record the refusal
  as a capacity finding rather than lowering the gate.

## Demos and tests

- **`circuit_demo`** (new, `make circuit`): (1) single-goal sharing —
  the planner discovers `combine(split(b))` from ONE byte source
  (impossible today; uses existing hex primitives); (2) multi-root
  discovery — `dag_plan_circuit` finds the 1-digit {sum, cout} circuit,
  then the 2-digit ripple circuit, topology asserted; (3) exhaustive
  verification — all 20,000 two-digit additions exact, strict, against
  the DISCOVERED circuit; (4) circuit chunk — distill + certify the
  1-digit circuit; attempt the dec_add2 stretch; (5) memo benchmark —
  timed stress planning, memo on vs off.
- **`test_circuit`** (new, in `make test`): hermetic — port-disjoint
  legality (the hv-collapse is refused; split fan-out is allowed; a
  third consumer on a used port is refused); single-goal compat (a
  registry with no multi-output primitives plans byte-identically with
  the feature present); multi-root topology forcing; all-sources
  refusal; shared-node single execution + single reliability outcome;
  double-free safety; memo on/off produce identical plans across the
  hermetic cases. Frozen half: the discovered-circuit sweep and chunk
  certification against committed weights.
- **Regression:** all 10 existing+new suites green; every existing
  weight/contract/property file byte-identical after `make run` +
  `make decimal` (no training-path changes for existing primitives).

## Alternatives considered

- **Share anything type-compatible** — rejected: the hv-collapse
  produces wrong functions that score better; verification would catch
  it at run time but planning would be systematically degenerate.
- **Projection nodes** (a new node kind wrapping "read port p of X")
  instead of per-edge ports — rejected: every traversal (executor,
  printer, consolidation teacher, free) grows a case forever; per-edge
  ports localize the change to input assembly.
- **Cross-call memoization** — rejected: reliability stats change
  between calls; staleness machinery isn't worth it at this scale.
- **Multi-goal via N independent single-goal plans** — rejected: that
  is exactly today's workaround (the hand-built ripple program); it
  cannot share the ones-adder execution and consumes sources N times.

## Risks

1. **Search rewrite complexity (highest).** dag_search gains reuse
   alternatives, multi-root agendas, plan-level ownership. Mitigation:
   the compat gate is mechanical — single-output registries must plan
   byte-identically — and the hermetic suite pins it.
2. **Memo correctness.** Pareto-set pruning bugs would silently return
   suboptimal plans. Mitigation: memo-on/off equivalence tests.
3. **dec_add2 training (stretch).** 20,000-sample 100% gate may be out
   of reach; the design treats refusal as a finding, not a failure.
4. **Hidden tree assumptions.** Existing code may recurse assuming
   trees (printers, consolidation teacher). Mitigation: audit every
   DagNode traversal during implementation; shared nodes in plans built
   by the old planner cannot occur, so blast radius is the new paths.

## Amendments (pre-implementation)

Three refinements discovered while writing the implementation plan:

1. **Memoization → reachability pruning.** The Pareto-set cache above is
   unsound under cross-subtree sharing: a memoized context-free answer
   for a slot (type, mask) cannot see reuse opportunities — in
   `combine(split(b))` the second slot MUST see the sibling subtree's
   split node, which a context-free cache entry hides. Replaced by a
   per-planning-call type→min-depth reachability table: a slot whose
   type needs more primitive levels than the remaining depth budget —
   and that no existing complete node can serve via an unused output
   port (the reuse escape) — prunes the branch. Pruning only skips
   branches that provably contain NO plan, so memo-on/off plan
   equivalence is guaranteed by construction, not merely tested. A
   positive Pareto cache remains future work if the benchmark shows
   pain on densely-connected registries.
2. **Demo part 1 corrected.** `combine(split(b))` from one byte source
   is not demonstrable on the real registry: the byte source itself
   satisfies any byte-typed goal at score 1.0. It becomes a hermetic
   test with a distinct-tag synthetic join; the real-primitive demo is
   the two-root circuit `{hi, lo} = split(b)` — one execution, two
   roots, evidence recorded once.
3. **Sharing-aware traversals.** `plan_dag_count_sources` and member
   collection recurse trees and would double-count under shared nodes;
   circuit consolidation and emission use pointer-set walkers that
   visit each node once.
