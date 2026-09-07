# Bounded resident capsule scale measurements

Status: bounded synthetic qualification complete, 12/12 final measurements
passing on 2026-09-07. One failed qualification attempt is retained below.

## Decision and scope

Measure the actual `CnetCoreHost` / `cnet_capsule_core` registry at 32, 256,
1,024 and 4,096 capsules. Existing capacity evidence relabels two XOR tables;
this fixture instead constructs distinct synthetic answer vectors: unit `i`
maps UINT8 keys 0..4 to `8*i + key` in a 16-bit output. All other UINT8 keys
must abstain. Unique unit names and non-near-miss port tags isolate the units.
The arithmetic checker supplies labels; CNET answers never become labels.
Distinct synthetic functions and identities are **not distinct acquired
knowledge**. Rich assets, generalization, GPU execution and production service
performance remain WITHHELD.

Use ordinary BTN robust certification at the unchanged 0.05 margin, sealed
CNB export, sampled coverage and canonical import. No core production changes,
new package format, service mutation, GPU work or lowered guard budget.

## Ordered implementation and verification

1. Retain a smoke/refusal test and observe its missing-harness RED. Implement
   the private synthetic fixture builder; compile warning-clean and load a
   small fixture through the ordinary core.
2. Measure a real resident N-1 to N upgrade with `stage_registry(preserve=1)`
   and `activate`. Pin the incumbent through activation; check its old answers
   and refusal of the new unit, and exercise rollback. Check exact identity
   counts and distinct hashes. Exhaustively verify all 256 keys per unit on
   the final N snapshot, plus one directed neighboring cross-port refusal per
   unit (sampled interface isolation, not every possible cross-unit pair).
3. Add the resource-bounded runner, retained failure receipts and quantile
   tests. Run smoke/refusal tests, then three fresh-process replicates at every
   required size. Independently review before accepting final measurements.

## Measurement contract

Per replicate, record monotonic load, candidate stage/replay, activation and
rollback duration; RSS baseline, loaded, overlapping candidate, activation and
observed RSS peak; total regular-file serialized bytes and resident-to-serialized
ratios. Report covered, OOD and cross-port latency separately with samples and
nearest-rank p50/p95/p99/max. Repeated fixed query orders are synthetic serial
CPU workloads, not realistic demand. Three process replicates are **not cold
filesystem-cache measurements**. No cache dropping or host settings changes.
Benchmark timings and wall deadlines use monotonic clocks that exclude system
suspend. Unlike production BOOTTIME workers, this is an attended synthetic
benchmark, not suspend-inclusive production lifecycle qualification.

An N-1 incumbent is loaded before the N candidate; activation retains the
incumbent as rollback, so post-activation RSS includes that retained generation.
`load_ms` and `rss_loaded_bytes` therefore describe exactly N-1 capsules, not
a standalone N-capsule snapshot. `stage_ms` includes loading N capsules and
unchanged self-closure/preserve replay. Activation/rollback timings measure
the in-memory owner operations, not durable publication or socket latency.
All timing and correctness counts name their phase and population. No timing
threshold is invented after seeing results; correctness gates every metric.

RSS checkpoints use `/proc/self/smaps_rollup` Rss and include the entire process,
shared-library mappings, allocator retention and benchmark bookkeeping. The
largest checkpoint is `rss_peak_observed_bytes`, a lower bound on a transient
peak. Linux's approximate `VmHWM` is recorded separately as
`rss_hwm_approx_bytes`; it can be lower than a smaps checkpoint. It is not used
as an exact peak or replaced with inherited pre-exec `getrusage` high water.
See [Linux RSS accounting](https://man7.org/linux/man-pages/man5/proc_pid_statm.5.html)
and [VmHWM semantics](https://man7.org/linux/man-pages/man5/proc_pid_status.5.html).

The runner snapshots the existing built shared library into its own private
artifact directory and records source/build hashes and host details. Artifacts
remain for inspection. Check MemAvailable before allocating; each child has a
16 GiB address-space ceiling and at most 300 seconds wall time. The fixed
fixture and aggregate artifact accounting must remain below 2 GiB new disk.
Resource failures and unchanged core cap/budget refusals produce retained
failure records, never silent reduced-size success. No commits or rollout.

An accounting read failure is tolerated only after `Popen.poll()` confirms the
exact child has exited; its exit code and post-exit disk budget are still
checked. A live child's unavailable accounting fails closed. Ordinary SIGTERM
sets a non-throwing termination flag checked before/after each child and while
polling, then kills/reaps the owned child process group. TERM cannot interrupt
cleanup by throwing, including when the first TERM arrives during error cleanup.
The outer test timeout likewise requests TERM and waits for cleanup rather
than killing only the runner. Hard SIGKILL, machine failure and other abnormal
runner death are **not** proven to clean the separate child session; this
benchmark runner is not a production job supervisor or hostile-owner sandbox.

## Reproduction

From the isolated worktree, with the intended native build already complete:

```sh
make bin/libcnet_capsule_core.so
python3 tests/capsule_scale_contract.py
python3 scripts/capsule_scale_bench.py
```

The runner does not rebuild or replace shared binaries. It compiles its own
small executable with the exact command retained in the configuration receipt,
links to its private copied library, and runs each replicate in a fresh process.
Default sizes are 32/256/1024/4096 and default replicates are three; the smoke
test explicitly requests 2/4 with one replicate. The native selector is the
ordinary deterministic path, not the separate 62-capsule experimental selector.
The host is shared and CPUs are not pinned; these are local synthetic serial
measurements with run-to-run variation, not an isolated-machine SLA.

## Qualification failure retained

The approved runner `207fb096af193369af5f044fa648737200c05e8b2d36adeed9a2778d619e852d`
attempted all four sizes on the final native library. It retained six passing
measurements (32 and 256, three each), then refused while monitoring the first
1,024-capsule measurement with `child write accounting unavailable`.
The child's stdout contained a complete passing measurement, but stdout alone
does not prove exit; this attempt is **failed**, not a final qualification.
Its generic error did not distinguish an OS read error from a missing `wchar`
field, so the exact branch of that observed refusal is unknown.

Receipt: `/tmp/capsule-scale-qualified-final-20260907.jsonl`, also retained as
`/tmp/cnet-capsule-scale-220yjq42/results.jsonl`; both SHA-256
`5904db39dc9c3a74e1cb6275bbadc5f718f40cb075e87f34f0fbcff6a64268dd`.
The same failed receipt is retained in the repository at
`result/cnet_capsule_distinct_scale_failed_20260907.jsonl`.
The test then independently reproduced a real contract omission: empty or
missing-field reads refused even when the exact child was confirmed exited.
RED `/tmp/capsule-scale-empty-accounting-red-20260907.log` has two failing
subcases. The bounded fix shares the exact-child exit check between read
errors and missing fields and retains diagnostic detail for any live refusal.
Live accounting failures still refuse and reap the child; completed-child
exit status and post-exit disk checks remain mandatory. All ten contract
tests passed in 2.585 seconds in
`/tmp/capsule-scale-empty-accounting-green-20260907.log`, including actual
live-child cleanup, completed-child disk refusal and first-TERM-during-cleanup.

## Final results and receipts

The independently reviewed accounting fix was qualified with the default
32/256/1,024/4,096 sweep, three fresh processes each. Command:

```sh
python3 scripts/capsule_scale_bench.py > /tmp/capsule-scale-qualified-v2-final-20260907.jsonl 2> /tmp/capsule-scale-qualified-v2-final-20260907.log
```

Exit 0; all 12 measurement records and the final completion record passed.
The final JSONL and `/tmp/cnet-capsule-scale-sdo719d8/results.jsonl` are identical,
SHA-256 `aa568dbb6f828fdeb30dd17234b1a51858c2138dcf259c39cf844f634c64111a`.
The same bytes are retained in the [repository JSONL](../result/cnet_capsule_distinct_scale_20260907.jsonl).
The artifact directory retains fixtures, copied library, executable and every
child's stdout/stderr; total regular-file bytes at completion: **26,889,648**.
The failed attempt above is not pooled into final statistics; earlier pre-final
development runs likewise are not final measurements. No failed size or
replicate was silently dropped from the successful full sweep.

Host: x86-64 Linux 6.17.0-20-generic, AMD Ryzen 9 9950X, affinity CPUs 0..31
(not pinned). Configuration MemAvailable: 63,756,623,872 bytes. The host was
shared; no production services, GPU, cache or host settings were changed.
The coordinating managed regression/build was finished before this sweep.
Warm serial host-ask latency follows the exhaustive correctness sweep in each
fresh process; neither cold-cache latency nor concurrent throughput is measured.

All values below are medians of three independent process replicates unless
explicitly a range. Times retain the emitted precision, which is not a claim
of clock accuracy. Sub-microsecond activation/rollback results time only the
in-memory owner operation and are particularly sensitive to timer overhead.

| Final N | Load N−1, ms | Stage N + preserve replay, ms | Activate, ms | Rollback, ms |
| ---: | ---: | ---: | ---: | ---: |
| 32 | 3.155303 | 4.326183 | 0.000310 | 0.000230 |
| 256 | 27.917047 | 37.935904 | 0.000310 | 0.001252 |
| 1,024 | 173.907376 | 213.035150 | 0.000691 | 0.000902 |
| 4,096 | 1645.483080 | 1811.099247 | 0.000771 | 0.000531 |

Exact byte counts; `loaded` is N−1, and `overlap` retains both N−1 and N.
Delta columns are medians of **per-replicate** RSS minus that replicate's
baseline, not subtraction of the two column medians. They include whole-process
allocation effects, not an isolated capsule allocator measurement.

| Final N | Serialized N bytes | Baseline RSS | Loaded RSS | Loaded delta | Overlap RSS | Overlap delta |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 32 | 74,671 | 8,151,040 | 9,711,616 | 1,556,480 | 10,260,480 | 2,105,344 |
| 256 | 597,305 | 8,138,752 | 11,956,224 | 3,817,472 | 14,589,952 | 6,451,200 |
| 1,024 | 2,389,193 | 8,134,656 | 19,722,240 | 11,587,584 | 30,060,544 | 21,925,888 |
| 4,096 | 9,556,823 | 8,245,248 | 51,290,112 | 43,044,864 | 92,524,544 | 84,279,296 |

Serialized N−1 bytes respectively: 72,338 / 594,973 / 2,386,859 / 9,554,489.
At 4,096 the median two-generation overlap RSS is 9.681517 times the one-N
serialized bytes; baseline-adjusted overlap is 8.818757 times. These are not
standalone-N compression ratios. The largest observed smaps RSS checkpoint
across all final replicates is 92,700,672 bytes; unobserved transient peaks
remain unmeasured. Closed-host RSS may retain allocator/shared-library pages.

Latency p99 ranges below are min–max across the three independently computed
nearest-rank p99 values, **not** a pooled percentile or confidence interval.
The raw receipt retains each replicate's p50/p95/p99/max and exact sample count.
Timing samples per replicate are covered/OOD/cross-port, respectively.

| Final N | Covered p99, µs | OOD p99, µs | Cross-port p99, µs | Samples per replicate C/O/X |
| ---: | ---: | ---: | ---: | ---: |
| 32 | 4.066944–4.397988 | 3.355980–3.867030 | 3.526926–5.790949 | 480/192/96 |
| 256 | 6.271958–7.643938 | 5.560040–7.073045 | 5.550027–7.254004 | 3,840/1,536/768 |
| 1,024 | 13.085008–16.221046 | 12.243032–15.839934 | 12.563944–16.289949 | 15,360/6,144/3,072 |
| 4,096 | 33.412933–34.905910 | 32.842040–34.295082 | 33.422947–34.685016 | 61,440/24,576/12,288 |

Each replicate exhaustively checked 5N certified answers and 251N OOD refusals
on its same final N generation, plus N directed neighboring cross-port refusals.
Across the 12 final replicates that is 81,120 correct answers, 4,072,224 OOD
refusals and 16,224 cross-port refusals, with zero wrong verified answers.
Every replicate also checked old-generation pinning, refusal of the new unit
on that old generation, preserve replay, rollback and close-with-live-pins
refusal. These repeated synthetic observations are not additional knowledge
units, all-pairs interference proof, or held-out generalization evidence.

The following hashes were recorded before and after the full sweep and matched
exactly; no native rebuild occurred during measurement.

| Artifact | SHA-256 |
| --- | --- |
| `bin/libcnet_capsule_core.so` | `2856e370280c56d4f3ca474cc7896398681c764109ecaa493516f3dbce7135dd` |
| `tests/capsule_scale_bench.c` | `06c8cf63e4523408c628b48e4cb8cf7655beb3e6bdee96aca90adc8e09a5c608` |
| `scripts/capsule_scale_bench.py` | `0ffa2821973755d4355e852c6f6c4f616a59e65916f3916ac40477ecf82072e1` |
| `tests/capsule_scale_contract.py` | `bef83d6b5b71176f603ab7fdace80320a14ce9a9bbc74293a2f628d1de421ec0` |
| `src/serve/cnet_capsule_core.c` | `d35fe0d75c78ef08e9998a7df567d6547e9953dbf0992f4ef09e77b56da6e761` |
| `src/serve/cnet_capsule_table.c` | `8ba5f50e9aee7b94eb62a3b4ed8c1f3718b6c038da0de9da3c33f5e1ed7638ff` |
| `src/serve/cnet_core_host.c` | `8c81e91a3fbc5f2a53d03c725e52bc70dff4927c9401d1d8258c00c6cdabed32` |
| `include/cnet_capsule_core.h` | `66631c9e9f9c5a0b5e707ee775c26067c5cd5a49c3a4b8ef97b4769499e04594` |
| `include/cnet_core_host.h` | `5c352e28a8b66060266a101fa648f5c86db9a92f8bb23e3561f351be5439acc1` |
| `include/cnet_capsule_table.h` | `353f14072e364423bb199d13501ecabc4e265ef5bc0447ccc784faeb9be784f2` |

The private benchmark executable hash is
`958cf4c3291cf9ca9838f1b919fe824edb4050eb08ec282eb021f26fbe9e850f`;
its exact compiler command, including private library search/rpath, is retained
in the JSONL configuration record.

## Bounded completion checklist

- [x] Actual public host/lease lifetime exercised, N−1 incumbent to N candidate.
- [x] Distinct certified synthetic functions and identities at every requested N.
- [x] Unchanged certification, self-closure and preserve/replay checks.
- [x] Deterministic runtime RED/GREEN for accounting and owned-child TERM cleanup.
- [x] Independent runner review before the final qualification.
- [x] Three final fresh-process measurements at each of 32/256/1,024/4,096.
- [x] Exact per-replicate samples/nearest-rank tails and failures retained.
- [x] RSS checkpoints, serialized bytes, load/swap timings and hash stability recorded.
- [x] 16 GiB child address-space, 300-second monotonic wall and 2 GiB artifact limits retained.
- [x] No service/GPU mutation, production deployment, new packaging or benchmark-driven floor reduction.

WITHHELD: cold-cache behavior, concurrent throughput, all-pairs cross-unit
interference, heterogeneous real-world capsule RAM, acquired knowledge scaling,
held-out generalization and production lifecycle/72-hour acceptance.
