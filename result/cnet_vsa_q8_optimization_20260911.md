# Exact int8 routing optimization — 2026-09-11

Implemented a speed and memory improvement with unchanged routing geometry and certification thresholds. Work is uncommitted. This extends the existing v3 implementation; it does not migrate capsule files or introduce a new format.

## Final measurements

AMD Ryzen 9 9950X, Ubuntu GCC 13.3.0, `-O3`, one pinned CPU (0). Seven alternating baseline/candidate runs per build, with identical queries and synthetic 101-entry registries, 256 warmup routes and 12,000 timed routes per workload per run. Norm timing uses 1,000,000 calls per run over 101 vectors. Both binaries include the same test-only norm wrapper. Native adds `-march=native`; portable omits architecture-specific flags. Reported values are medians.

| Workload | Native before | Native after | Result |
|---|---:|---:|---|
| Int8 norm, ns/vector | 96.10 | 44.50 | 2.16x faster |
| Wide routing, five encoder IDs, us/query | 24.29 | 12.30 | 1.98x faster |
| Wide routing, STEM only, us/query | 8.58 | 8.54 | Within measurement noise |
| Legacy float routing, STEM only, us/query | 26.99 | 26.90 | Within measurement noise |

| Workload | Portable before | Portable after | Result |
|---|---:|---:|---|
| Int8 norm, ns/vector | 361.23 | 179.58 | 2.01x faster |
| Wide routing, five encoder IDs, us/query | 40.71 | 27.28 | 1.49x faster |
| Wide routing, STEM only, us/query | 23.17 | 23.22 | Within measurement noise |
| Legacy float routing, STEM only, us/query | 28.99 | 28.84 | Within measurement noise |

Single-encoder and float timing ranges overlap between baseline and candidate. There is no measured speed regression, and no claim of a meaningful speed gain for those whole-route workloads. These are routing microbenchmarks, not end-to-end generation or production traffic measurements. Mixed-encoder gains apply when the registry contains redundant wide representations; the current default STEM-only path does not get the near-2x routing gain.

Raw samples, source hashes, and test counts are in `cnet_vsa_q8_optimization_20260911.json` beside this report.

## Why the result is exact

The norm sum now uses int32 rather than int64. At width 2048, even every coordinate equal to -128 produces only 33,554,432. Both accumulation types therefore compute the same integer, followed by the same double conversion and square root. The width bound is enforced at compile time.

The existing wide encoder uses plain unigrams for BAG, HD, and CGRAM_C, and stemmed unigrams for STEM and STEM_BI. The router now encodes these two actual representations once each, rather than encoding separately for all five IDs. Each capsule still receives the same query coordinates, norm, cosine score, and gate checks. The float path retains all five distinct encoder behaviors.

Quality evidence:

- Exact baseline/candidate comparison of all 316,240 vectors and norms: all five IDs over 63,248 nonempty sentences in all 1,068 retained corpora.
- Exact printed route-result parity on both native and portable builds, for eight query fixtures across the three timed registry configurations. Fixtures include morphological variants and all-stopword input. The serial/overlap test separately compares every field of `CnetVsaRouteResult`.
- Norm oracle checks on 1,256 vectors, including all 256 uniform int8 values and 1,000 deterministic random vectors, plus null input.
- Full separability sweep: 1,068 corpora; wide STEM remains 871/1,068 = 81.6% separable at 0.80/0.90. Float STEM remains 661/1,068 = 61.9%. Existing floors were unchanged.

Generated-text quality, improved semantic accuracy, and production traffic latency remain WITHHELD. No encoder, centroid quantizer, receipt layout, radius, or ambiguity policy changed.

## Resource and correctness changes

The router no longer uses the 10,240-byte global wide query array. Each wide call uses two local 2048-byte query vectors. The wide and float paths live in separate private helpers, keeping their scratch lifetimes separate without a new public interface or heap allocations.

GCC native `-fstack-usage` reports:

| Function/path | Before | After |
|---|---:|---:|
| Wide route frame | 80,016 bytes | 73,856 bytes |
| Float route frame | 80,016 bytes | 80,016 bytes |
| Dispatch wrapper | included above | 8 bytes; tail calls helper |

The combined text/read-only size reported by `size` for the two production objects decreases by 48 bytes; the capsule object's BSS decreases by 10,240 bytes. Capsule/registry structures and persisted file sizes are unchanged. No additional runtime dependency, hardware requirement, or per-capsule storage is introduced. Stack/code measurements are compiler/build-specific, not ABI guarantees.

The global query array also allowed one overlapping route to overwrite another's query. The new deterministic test reproduced this before editing production code:

```text
CNET_VSA_Q8_BENCH_RED: overlapping routes changed a query's result
```

The test passes with private scratch for both single-encoder and mixed-encoder registries. It warms encoder lookup tables before scheduling overlap. This fixes query-buffer interference; it does not certify cold lazy initialization, concurrent registry mutation, or the entire library as thread-safe.

## Verification and experiment ledger

PASS: q8 (native and portable), calibration (6 gates), router (4), arena (5), gencap (5), text (11 checks), stem, CLI (28 checks), the full 1,068-corpus encoder sweep, and `verify-fast`.

Fresh verification is recorded in `/tmp/cnet-q8-candidate/final-gates.log`; the full sweep is in `/tmp/cnet-q8-candidate/sweep.log`. The new permanent gate is `make cnet_vsa_q8_bench`, included in `cnet_vsa_all_bench`. `--timing-only` prints a distinct completion marker and cannot substitute for the overlap gate.

1. Private two-representation scratch plus int32 norms: native mixed routing and norms improved; an initial three-run portable measurement suggested a small float slowdown. That layout was superseded.
2. Separate wide/float helpers: lowered the wide stack frame by 6,160 bytes and removed the portable float regression in seven alternating measurements. Kept.
3. Norm narrowing: exact bounds and parity passed; native 2.16x and portable 2.01x norm improvements exceeded run-to-run variation. Kept.

Baseline sources are retained under `/tmp/cnet-q8-baseline/`. The paired timing driver is `/tmp/cnet-q8-candidate/measure.py`; its compile inputs are the new benchmark plus the six VSA sources used by the Make target. Baseline builds substitute the two saved baseline source files. The task's decision record is `plans/cnet_vsa_q8_optimization_20260911.md`.

The three integrity/calibration findings in `cnet_vsa_v3_review_20260911.md` remain open. This optimization does not justify treating those findings as resolved or migrating the legacy registry without its own validation.
