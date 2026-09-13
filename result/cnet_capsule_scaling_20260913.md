# Capsule composition scaling — 2026-09-13

Explicit composition works through 32 isolated capsule cores on the covered
numeric fixtures. The largest warm requests stayed below 0.5 ms p99 across
three runs. Accuracy did not improve exponentially: distinct composed
behaviors stopped doubling, and complete passage-answer coverage fell as
more branches became mandatory. Production remains unchanged.

## Exact composition and isolation

Thirty-two separate CNU1 fixture capsules were compiled from independently
generated `verified_tool` labels using the existing teach/certify/export/import
path. Module i computes `((2*(i % 8)+1)*x+(3*i+1)) % 64` over all 64 inputs.
The odd multipliers preserve information through chains rather than rapidly
collapsing them to a constant. No composite result was used as a training
label and no teacher runs in the measured serving path.

The experiment supplies an explicit graph with at most 32 nodes and two
parents per node. A Python checked adapter sums parent results modulo 64
before the next native capsule call. Each node records its request, context,
capsule identity, input, output, source units and dependencies. The final
answer retains those receipts after capsules unload. The adapters and graph
are experimental tools outside the production single-input capsule guard;
they are not certified multi-input capsules or an automatic prose planner.

The primary campaign covered 51 cells: independent widths and chain lengths
1/2/4/8/16/32, plus binary trees with 1/2/4/8/16 leaves, each under three cache
regimes. Every cell ran 256 requests and compared every intermediate and final
value with the arithmetic reference. All **13,056 requests passed**. Chain
inputs cover the entire 64-value domain four times; larger multi-input graphs
sample their much larger domains. The largest conditions added 3,072 passing
requests across two repeat runs.

Three new unit tests and the original three tests passed. Additional native
tests put an uncovered capsule halfway through chains of 2/4/8/16/32 nodes:
each refused at that hop and executed zero later nodes. With all 32 cores
pinned, admission of another capsule refused. Fixture file hashes remained
unchanged during the native guard campaign. These checks preserve local
coverage and identity; they do not establish semantic truth of arbitrary
declared contexts or cryptographically authenticated receipts.

## Latency and residency

Each timing cell ran in a fresh Python process on the local AMD Ryzen 9 9950X,
using the existing native shared library. Timings include plan construction,
cache/lease work, native execution and coverage, receipt checks, joins and
simple rendering. They exclude process startup, random input generation and
the reference oracle. All filesystem data was warm. Single-worker results
do not establish latency or CPU utilization under concurrent load.

| Warm chain length | Primary p95 | Primary p99 |
|---|---:|---:|
| 1 | 0.016 ms | 0.032 ms |
| 2 | 0.034 ms | 0.069 ms |
| 4 | 0.054 ms | 0.071 ms |
| 8 | 0.117 ms | 0.145 ms |
| 16 | 0.189 ms | 0.200 ms |
| 32 | 0.359 ms | 0.379 ms |

| Largest graph | Distinct capsules | Warm p99, three-run range | Four-core LRU p99, three-run range |
|---|---:|---:|---:|
| Independent branches | 32 | 0.392–0.473 ms | 7.199–7.429 ms |
| Dependent chain | 32 | 0.379–0.388 ms | 7.204–7.835 ms |
| Binary fan-in, 16 leaves | 31 | 0.375–0.400 ms | 6.996–7.364 ms |

All fully resident primary cells pass p95 <= 1 ms and p99 <= 5 ms. With a
four-core LRU, working sets above four cores repeatedly reload modules;
larger cells miss the 1 ms target, and 31–32-core cells also miss the 5 ms
ceiling. The primary full-reload p99 was 8.60 ms for 32 independent branches,
9.55 ms for the 32-node chain and 8.25 ms for the 31-node tree. No floor was
relaxed to improve these results.

Keeping 32 fixture cores resident added about 3.7–3.8 MB RSS to an approximately
28 MB Python worker before timed work. These tiny numeric capsules are not
representative of arbitrary production capsule sizes. The count limit is
verified for live handles, not a general resident-byte limit. Warm largest
graphs used about 355–368 microseconds of mean thread CPU per request in the
primary run; request rate still determines CPU utilization.

The primary full-reload runs showed substantially higher RSS after repeated
reloads, so a separate cache-only memory campaign was added. Its result is
recorded below and in the JSON; do not infer memory efficiency merely from
the number of live handles.

Each memory condition executed 1,024 cycles of all 32 modules, checking all
32,768 native outputs, in its own fresh worker without retaining result
graphs. Across all three conditions, 98,304 outputs matched their references.
Sampled RSS stabilized: all-resident at 31.8 MB, four-core LRU at 32.9 MB,
and full-reload at about 59.8 MB before falling to 58.1 MB. All handles were
closed at the end. This run did not show continuing growth, but the retained
allocation explains why aggressive unloading was not the smallest-RSS policy.
It does not prove the absence of leaks over other workloads or longer runs.

## Programs versus distinct behavior

With a fixed library of two modules, every sequence through depth eight was
executed for every input 0–63. All **32,640 sequence/input cases passed**, with
every intermediate output also checked. Distinct behavior means the full
64-input output vector, so equivalent programs count once.

| Steps | Possible programs | Distinct input/output functions |
|---|---:|---:|
| 1 | 2 | 2 |
| 2 | 4 | 4 |
| 3 | 8 | 8 |
| 4 | 16 | 16 |
| 5 | 32 | 32 |
| 6 | 64 | 62 |
| 7 | 128 | 108 |
| 8 | 256 | 154 |

Program count doubles by construction. Distinct behaviors initially double
but then collide. This shows useful reuse of a fixed module library without
training on combinations. It does not establish an exponential learning or
answer-quality law, and program selection was supplied by the harness.

## Passage quality as more branches become required

The same exposed 400 question components were placed in nested groups using
the prior fixed shuffled order. Every component appears once at each width.
No group was chosen by correctness. The unchanged VSA CLI answers each
component independently; the compound output is accepted only if every
required branch accepts. This uses the CLI's existing cache, separately from
the bounded CNU1 experiments above.

| Required branches | Requests | Split accepted | Split judged correct / wrong | Single route judged correct / wrong |
|---|---:|---:|---:|---:|
| 1 | 400 | 164 | 114 / 50 | 114 / 50 |
| 2 | 200 | 34 | 25 / 9 | 2 / 29 |
| 4 | 100 | 1 | 1 / 0 | 0 / 6 |
| 8 | 50 | 0 | 0 / 0 | 0 / 2 |
| 16 | 25 | 0 | 0 / 0 | 0 / 1 |

One uniform judge prompt required every supplied question to be correctly
and substantively answered. The local `mistral-small-3.2-24b-offline` model
produced 239 distinct cached judgments at temperature zero. The generalized
prompt differs from the earlier two-question-only prompt: two-branch correct
answers were judged 25 here versus 27 previously. These counts are prompt-
and judge-dependent diagnostics, not human reference scores. Compare
conditions within this run rather than treating the difference as a model
regression. There was no transformer answer baseline or new held-out gate.

The single-route control requests the same number of passages as questions;
the current CLI returns at most four. It therefore matches passage allowance
through width four only. At widths eight and sixteen the split path refused
every complete request anyway. Splitting retained its advantage at width two
but did not repair the individual branches' limited coverage. The underlying
component acceptance remained 164/400 at every grouping width.

Summed native split route/rank p99 was 0.065/0.116/0.195/0.373/0.722 ms for
widths 1/2/4/8/16. These exclude dispatch, assembly and judging, and are not
end-to-end latency measurements for an integrated natural-language path.

## Disposition and reproduction

The result supports fast, isolated composition on covered inputs with an
adequately resident working set. Exponential quality improvement, automatic
decomposition and production enablement remain WITHHELD. Fully dependent
answers must refuse when a required predecessor fails. Independently useful
partial answers would need an explicit separate output policy and evaluation.

Only experiment files and records were changed. The existing experimental
cache ceiling expanded from four to 32 cores; production code, admission,
certification floors and capsules were not changed. Nothing was committed.

See [reproduction instructions](../experiments/capsule_branching/README.md),
[decision record](../plans/cnet_capsule_scaling_20260913.md) and
[JSON evidence](cnet_capsule_scaling_20260913.json). Raw files are under
`var/capsule_scaling_20260913/`. The JSON preserves cell and repeat results,
question/output records, raw judgments, guard results, memory samples,
protocol, fixture identities, test logs and SHA-256 hashes.
