# Bounded capsule branching — 2026-09-13

The isolated test supports explicit branching with retained evidence and
bounded capsule residency. All 4,096 controlled numeric joins matched their
arithmetic reference. On 200 compound passage questions, separate routing
raised model-judged complete correct answers from 6–7 to 27 and reduced wrong
answers from 24–25 to 7. The two experiments exercise different runtimes;
an integrated production path is not measured or enabled.

No production source, certification floor, corpus or shipped model was
changed by this experiment. Existing workspace changes were preserved.
Nothing was committed.

## Controlled join and cache

Four fixture capsules were built using the existing CNET `teach` command,
`verified_tool` provenance, finite-domain compilation, certification,
export and verified import. Each root contains one CNU1 capsule. An
independent arithmetic program supplied the labels:

- Left: `(x + 1) mod 64`, all 64 inputs.
- Right: `(y * 2) mod 64`, all 64 inputs.
- Extra: `(x + 3) mod 64`, used for eviction pressure.
- Partial right: only inputs 0–31, used to test coverage refusal.

No joined answer was supplied as a training label. A Python checked operator
sums the first two capsule outputs. It retains the split ID, branch role,
capsule identity, context, typed ports, original input, output, hop count
and source units. It verifies receipts against those issued by that request
and checks context agreement. This operator is experimental; it is not a
certified multi-input capsule. The production single-input admission guard
is unchanged.

Three unit tests passed after a recorded missing-module RED. The actual
native-capsule campaign passed 21 checks, including exhaustive 4,096-pair
correctness; modified, reversed and duplicate receipts; incompatible entity,
regime and hypothesis; cross-request evidence; uncovered input; wrong ports;
identity mismatch; corrupted-package refusal; admission/close refusal when
all cores are pinned; and evidence surviving eviction and full cache close.

The single-thread LRU kept at most two live capsule cores throughout 600
churn operations. Active leases are hot and cannot be evicted. Released
cores remain warm until pressure evicts them; capsule files remain available
for reload. The count bound includes admission because a warm victim is
closed before a new core opens.

| Complete structured request | Mean | p95 | p99 | Maximum |
|---|---:|---:|---:|---:|
| Warm, 4,096 cases | 0.02345 ms | 0.02550 ms | 0.03024 ms | 0.15546 ms |
| Reload two cores, 100 cases | 0.50457 ms | 0.57776 ms | 0.68426 ms | 0.74558 ms |

Timing includes Python cache/lease handling, two native parse/route/coverage
calls, receipt construction, checked sum and simple rendering. Inputs and
capsule selection are already structured. Reload timing has cold handles
but a warm filesystem; it is not a storage-cold benchmark. Warm mean thread
CPU time was 23.14 microseconds. The local p95 <= 1 ms and p99 <= 5 ms budget
passes for this fixture on an AMD Ryzen 9 9950X with Python 3.11.15.

Process RSS was about 27.1 MB before loading, 29.0 MB after the exhaustive
test and 30.9 MB after churn. It stayed near 30.9 MB after closing the cache.
These measurements include Python and measurement arrays, and do not isolate
capsule bytes. Closing cores does not guarantee returning allocator memory
to the OS. A two-core limit also does not bound bytes for arbitrary capsules.
Concurrent access, production-scale residency and CPU use under load remain
WITHHELD.

## Passage answer quality

All 400 previously exposed component questions were shuffled with seed
20260913 and paired into 200 requests. Each pair has different reference
capsules; no pair was selected by baseline success. The request explicitly
contains `Question 1:` and `Question 2:`. This tests independent subquestions,
not a deduction that depends on joining facts.

The unchanged VSA CLI processed the same requests in three conditions:

| Condition | Answered | Judged correct | Judged wrong | Refused | Net: correct − 2 × wrong |
|---|---:|---:|---:|---:|---:|
| One route, one passage | 31 | 7 | 24 | 169 | −41 |
| One route, two passages | 31 | 6 | 25 | 169 | −44 |
| Two explicit branches, one passage each | 34 | 27 | 7 | 166 | +13 |

The two-passage control was added after the initial one-passage result, using
the same fixed pairs, judge and cached judgments. All 31 accepted control
outputs contained two passages. Separate routing therefore improved this
diagnostic beyond merely allowing a second passage from the original route.

The split path returns a compound answer only when both branches pass their
existing guards. Its accepted-answer precision was 79.4%; complete correct
answers covered only 13.5% of all 200 requests. This is a useful improvement
on compound requests with substantial remaining refusal, not a general
solution to missing passage coverage.

The local `mistral-small-3.2-24b-offline` model judged whether each answer
correctly and substantively answered both questions. Temperature was zero;
96 distinct raw judgments and the exact prompt are preserved. These are
model judgments, not human annotations. There was no transformer answer
baseline, automatic decomposition test or unseen-content certification gate.

| Native route + rank fields | Mean | p95 | p99 |
|---|---:|---:|---:|
| One route, one passage | 0.05025 ms | 0.08042 ms | 0.09985 ms |
| One route, two passages | 0.04953 ms | 0.08110 ms | 0.09983 ms |
| Sum of both branches | 0.07844 ms | 0.10013 ms | 0.11492 ms |

The first pass warms each CLI process; the second supplies these timings.
They exclude Python dispatch, text assembly, startup, network and offline
judging. The VSA cache in this experiment is the existing CLI cache, not the
bounded CNU1 cache tested above. Do not combine the two tables into a claim
about one integrated natural-language serving implementation.

## Review and disposition

The review checked lease release on refusal, identity verification before
cache admission, covered native branch execution, context agreement, copied
evidence lifetime and the passage-count control. No dependencies were added.
Unit tests and Python compilation passed. Production gates were not rerun
because this task changes only isolated experiment and record files.

Receipt checks establish local consistency with issued results; they are
not signed transferable proofs. Context is supplied metadata, not a semantic
interpretation of prose. The test demonstrates an exact finite-domain join,
not a general-purpose inference engine. Broader capability claims remain
WITHHELD.

The next implementation should integrate explicit decomposition, separately
guarded branches and assembly, then measure the complete request on held-out
compound and single questions. Keep context alternatives separate and refuse
unsupported dependencies. Automatic decomposition and certified multi-input
joins need their own tests before enablement.

Reproduction: [experiment instructions](../experiments/capsule_branching/README.md).
Decision: [plan record](../plans/cnet_capsule_branching_test_20260913.md).
Evidence: [JSON record](cnet_capsule_branching_20260913.json), containing all
200 question/output records, raw judgments, fixture identities, build
receipts, test logs, environment and source/binary/artifact SHA-256 hashes.
Raw runtime files remain under `var/capsule_branching_20260913/`.
