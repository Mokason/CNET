# Prospective allocator sequence pilot — DEVELOPMENT only

This is a new finite workload, not a repair or reuse of the exposed v1 holdout.
The v1 checkpoint, feature semantics, masks, reports and gain floors are unchanged.
No candidate is fitted, no confirmation is generated, and nothing is activated.

The frozen campaign contains 16 seeded, distinct graphs: four instances each of
direct, shared-prefix, chain-completion and evidence-rejection. Motifs and the
small XOR tool vocabulary repeat; this is not broad task-family independence.
There are 32 queued primitive acquisitions, 64 public current requests, a horizon
of eight jobs, and a 512-row budget. Each primitive has 48–64 of the 64 six-bit
keys. The row budget is intentionally generous: the eight-job bound can bind
even when all rows fit. Direct graphs have 32 distinct goals. Shared graphs
have eight common first-hop edges and 24 second-hop edges, with alternative
paths to eight goals. Chain graphs have 4 + 8 + 20 edges over three layers.
Every edge belongs to a requested structural plan; there are no irrelevant
padding tasks. All capsules initially are missing. Thus original completion
features are zero, and its honest demand/cost tie-break can equal demand/cost.
This is explicit lack of initial-certification diversity, not a measured
completion-control advantage. Exactly one task is overdue and fits every
policy's budget. In rejection episodes its independently checked source row
is wrong; every policy charges this same mandatory rejection before proceeding.
The accepted/rejected outcome of that forced action is shared information.

All planners receive the same prospective graph, planned tool transformations
and key domains, current requests, installed state, costs, ages and real cursor.
No future request history is fabricated: none was found in the scoped existing
allocator experiments, allocator report or autonomous-learning plan. Their
offline verification masks and receipts are never extra online information.

The controls are:

1. Actual native rotating cursor.
2. Actual native demand/cost, with structural unmet-request fraction as f0.
3. Actual native completion-first, then f0/cost, then cursor.
4. Exact newly reachable marginal coverage per cost, recomputed after selection.
5. Bundle-greedy, considering full remaining paths and their shared acquisition
   cost, as well as single actions; no singleton-union approximation.
6. Deterministic branch-and-bound with a one-CPU-second scheduling budget and
   the same public information. It starts from the preceding strong greedy
   solutions. A separate 30-CPU-second reference uses the identical algorithm.

Native chooser outputs must equal the Python original-control actions/work or
the campaign refuses. Every solver records measured wall and CPU time, seed
initialization time, overrun, visited nodes, feasible reward and an admissible
upper bound. The CPU clock includes initialization. Checks occur between finite
nodes, so one node/initialization may overrun; that is reported, not hidden.
An unfinished search is never called optimal. The DFS frontier is bounded by
33 nodes. For each not-yet-covered request, let r be its minimum feasible
missing-path length. Giving every potentially useful action weight 1/r for
that request produces an admissible top-K-action bound: completing any path
requires at least r of those actions. Per-request impossibility, row cost,
remaining actions and slots also prune. Exact results and bounds are checked
against exhaustive small-subset enumeration, including 100 randomized cases.

Before the first native call, freeze this contract, producer/test/support code,
compiler, oracle and native library hashes in an owner-private temporary root.
The original allocator control function is not exported by the serving library.
A separate private shared library compiles the unchanged canonical allocator
and cell sources; its output, two sources and two headers are frozen as well.
It calls the actual canonical comparator, not a claimed Python-only equivalent.
Each of four XOR operands × 64 keys is actually labelled by the native pure
tool and independently compared with integer XOR. Each good primitive is
actually compiled once by generic native `teach` into an isolated bank. This
trusted experimental generator is unsealed and grants no unattended production
admission authority. The original certification margin and native guard/work
limits are untouched. Standard capsule artifacts are used, not new packaging.

Each control and reference then executes its whole chosen sequence in a fresh
registry. A disposable process canonically opens each actual candidate against
that sequence's incumbent, validates growth, probes all 64 current requests,
and publishes the accepted capsule into only that private registry. Final
probes exhaust all 64 inputs for every requested root/goal pair. Values come
from the independent tool receipts. Planned reachability is checked against
actual native outcomes at every step; any disagreement, wrong verified value,
old-answer loss or native admission refusal stops the campaign. Each step binds
source and capsule hashes, native growth counts and actual probe outcomes.
Candidate compilation is reused only across counterfactual policies; each
policy is charged the same acquisition-row cost. This does not benchmark
wall-time learning throughput.

The campaign has a 15-minute BOOTTIME budget. One native child at a time has
at most 30 seconds, 512 MiB address space, 64 MiB file output, and 64 KiB per
pipe. Whole-trajectory children launch no grandchildren. Python/system loader
libraries remain trusted installed dependencies; this is not a deployment
manifest or hostile-owner sandbox proof. All private evidence is retained.

Stop before GPU fitting or confirmation unless a proven optimum leaves at
least .05 mean coverage headroom over the strongest fair control. If upper
bounds exclude that headroom, or the requisite bound remains unresolved, the
gain gate is WITHHELD. A later distinct objective would still need prospectively
frozen fresh whole-episode confirmation, >=.05 gain and positive paired95 lower
bound against all original and strong controls (plus the active allocator),
per-family non-regression and all prior serving/certification invariants.

Hypothesis only if headroom survives: learn a sequence search ordering/value-to-go
from independently verified whole-trajectory returns, with explicit graph and
remaining-budget features. That requires a separately versioned CORE objective
and training contract. This pilot does not implement or validate such a model.
