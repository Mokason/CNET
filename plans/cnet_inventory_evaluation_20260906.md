# Capsule inventory evaluation

Continue the approved roadmap's remaining measurement gate, without expanding
live acquisition authority. Reuse ordinary CLI publication, historical replay
and existing capsule/core interfaces. No new packaging, network teacher,
production cache or raised work budget. Preserve the dirty worktree uncommitted.

## Ordered acceptance

1. [x] RED-to-green benchmark contract: machine-readable, correctness-gated
   results; empty/mismatched inventory, invalid parameters and failed output
   cannot report success. Add focused test and a native measurement executable.
2. [x] Isolated growth sweep attempted: 2, 16, 64, 256 capsules, using independent XOR
   labels for distinct two-edge chains. Publish every unit through existing
   certification/history gates. Record acquisition calls and publication wall
   time; report any admission limit as failure, never lower it.
3. [x] Measure every direct and composed covered input (0–31), per-pair
   uncovered input 32, unknown-interface refusal, load time, resident ask
   p50/p95/max and reload+ask time. Repeat measurements; distinguish held-out
   composed task pairs from supplied primitive labels. Costs in money/energy
   and live socket/load-concurrency overhead remain unmeasured.
4. [x] Review, run adjacent/full gates and document exact results and next scope.

Completed measurement scope: through 64 sampled capsules. The 256 target
remains RED at publication 65 (`coverage_restore_failed`), not a scale pass.
No shared-layout capacity change was attempted in this measurement slice.
See [results and next solution sequence](../result/cnet_inventory_evaluation_20260906.md).

Benchmark output is JSON Lines schema version 1. The probe exits 0 only for
complete correct coverage plus all expected refusals; operational/validation
failures exit nonzero. Timing uses CLOCK_MONOTONIC, nearest-rank percentiles,
and reports load separately (filesystem caches are not forcibly cold).
The runner uses mktemp stores, ignores production CNET configuration, retains
evidence/logs, and never restarts services. Native code measures the same
open/ask/close used by cnetd, not socket end-to-end latency.

Files: `tools/cnet_capsule_scale_probe.c`,
`scripts/cnet_capsule_scale_bench.sh`, `tests/test_capsule_scale_bench.sh`,
`mk/authority.mk`; usage/results follow verification. No absolute latency floor
is invented before baseline measurement.
