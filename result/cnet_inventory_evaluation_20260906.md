# Capsule inventory evaluation — September 6, 2026

## Delivered

Added a repeatable, correctness-gated inventory benchmark and included its
small result-contract regression in the default authority gate. The benchmark
publishes ordinary certified capsules from independent tool evidence, checks
direct and composed coverage, and reports timing and acquisition work in JSON
Lines. It does not mutate the live capsule stores or restart services.

The attempted 256-capsule sweep is **RED**, not complete: capsule 65 refuses
with `candidate_import_refused:coverage_restore_failed`. The shared
`HybridAi.coverage` array has `HYBRID_COVERAGE_MAX == HYBRID_TRACE_MAX == 64`.
The core loader's 256-directory limit does not make room for 256 sampled
coverage records. Existing refusal correctly prevents an unguarded import.
No capacity, certification floor or search/replay budget was changed.

## Measured workload

Each distinct tag group has two independently published 8-bit kernels:
`alpha → beta` is XOR 15 and `beta → gamma` is XOR 7. Each has 32 supplied
rows (inputs 0–31). Expected answers are computed independently; serving
answers are never used as training data. Systematic tags follow the existing
double-ID convention to avoid the base's intentional one-edit typo guard.

Per group, the probe checks 64 direct and 32 composed queries. The composed
task pair has no directly supplied training rows, but its primitive inputs
are covered: this is not unseen-input or broad-domain generalization. It also
checks uncovered input 32 on all three pairs and an unknown-interface query.
Each stage repeats this complete suite three times, imports three fresh core
instances, and checks twelve reload/query/close cycles.

Three independent fresh-store runs on an AMD Ryzen 9 9950X, a shared host:

| Capsules | Unique covered queries | Correct per run, 3 repeats | Expected refusals per run | Resident p95, ms range | Reload/query p95, ms range |
|---|---|---|---|---|---|
| 2 | 96 | 288/288 | 12/12 | 0.0099–0.0102 | 0.237–0.309 |
| 16 | 768 | 2,304/2,304 | 75/75 | 0.0564–0.0577 | 1.827–1.852 |
| 64 | 3,072 | 9,216/9,216 | 291/291 | 0.2110–0.2133 | 6.903–7.178 |

All passing stages reported zero wrong verified answers, zero missed covered
answers and zero invalid result states. At 64 capsules, each run acquired
2,048 tool labels. Cumulative publication took 1.522–1.536 seconds, excluding
evidence acquisition and benchmark execution. The failed next publication
consumed another 32 labels, so the terminal failure records 2,080 tool calls
and only 64 published capsules. No residual teacher is called by this probe.

Timing uses `CLOCK_MONOTONIC`, nearest-rank percentiles and actual sample
counts. Import includes certification replay and may use warm filesystem
caches; it is not a cold-disk benchmark. Reload/query follows the native
open/ask/close sequence used by cnetd but excludes socket, protocol and intent
overhead. The load-dominated timings identify a future optimization candidate,
not permission to cache away validation. Concurrency, monetary cost and energy
cost remain unmeasured; the latter two are explicitly `null`.

## Verification and evidence

The initial missing executable/runner failed with `CAPSULE_SCALE_BENCH_RED`.
The completed contract suite verifies multiple inventory sizes, parameter
refusal, under/overdeclared inventory, empty inventory, child exit propagation,
missing executable, output-write failure, and production-evaluation environment
isolation. Fault-injected publication failure and zero-exit/missing-receipt
success both refuse and retain machine-readable failure records.

Focused checks passed: `capsule_scale_contract`, `capsule_core_growth`,
`capsule_core_budget`, `capsule_history`, `capsule_history_coverage`,
`capsule_value_search`. Existing knowledge gates also passed: portable capsule
94 checks, coverage/abstention 55 checks, accumulation 32/32 isolation and 96
OOD refusals, composition guards at every hop. Those are separate existing
gates; the scale probe itself does not measure portable transfer.

Final isolated `make verify` exited 0: all 28 suites passed with fresh-log
binding. Audit: `/tmp/cnet-scale-audit-GuwPVe`; log:
`/tmp/cnet-scale-verify.log`. The audit reused the prior isolated build/fixture
tree, refreshed the current source/include/tools/tests/scripts/config/mk and
managed sources, and copied the current Makefile. All four implementation,
test and Make-fragment files compare byte-identical to the workspace after
the run. The pre-existing dirty workspace was not represented as clean HEAD.
No new capability-cert score is claimed by this evaluation-only change.

Independent review required finer publication timing, retained terminal
failure records (including missing PASS receipts), explicit unknown energy
cost, and multi-group result tests. These were implemented; production
runtime behavior was not changed. The performance skill kept measurement
separate from speculative optimization; interface and review skills enforced
versioned results and failure-status integrity.

Measurement artifacts (temporary; raw records are also retained alongside
this report):

- `/tmp/cnet-scale-bench-qASBPN`: full attempt, refuses at publication 65.
- `/tmp/cnet-scale-bench-J8iYSb`: explicitly bounded 2/16/64 repeat.
- `/tmp/cnet-scale-bench-0StAOZ`: explicitly bounded 2/16/64 repeat.

Each contains evidence rows, per-publication logs/times, host information,
binary/source hashes and `results.jsonl`. These three runs used the final
native probe but preceded the runner's final missing-receipt reporting fix;
that reporting change does not alter successful-stage measurements.

A final full rerun on the reviewed runner also refused capsule 65:
`/tmp/cnet-scale-bench-UZU3La`. Its retained `results.jsonl` exactly matches
stdout. A fresh probe after that refusal checked all 3,072 covered queries
and 97 expected refusals successfully: the failed publication preserved the
existing inventory. Raw stage records from all four runs are in
[`cnet_inventory_evaluation_20260906.jsonl`](cnet_inventory_evaluation_20260906.jsonl).

Final review has no remaining blocking findings. `git diff --check` passed.
Live cnetd and shared MCP retained PIDs 2619124 and 2119296; the acquisition
timer stayed active. A read-only socket check still returned verified `96`
for bytes 12 → bits, with `teacher=false`.

## Remaining solution sequence

1. Fix sampled-coverage capacity with an ABI-reviewed change and a failing
   64→66 regression first. The shared public structure means a casual macro
   edit without rebuilding consumers is not a safe live fix.
2. Re-run the unchanged 256-capsule gate, then evaluate socket concurrency and
   residual/acquisition economics. Do not claim the larger inventory yet.
3. Broader language intent, held-out primitive/domain transfer, trustworthy
   teachers outside the approved graph, and legacy writer power-loss recovery
   remain separate gates, WITHHELD.

Usage: `make capsule_scale_contract`; `make capsule_scale_bench` for the full
attempt, or `CNET_SCALE_COUNTS='2 16 64' make capsule_scale_bench` for the
explicitly bounded supported workload. No user data was deleted and no
unrelated changes were committed.
