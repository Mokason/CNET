# Slot-candidate benchmark evidence

Frozen parser source `8cefc80793116426df0d6bb780dfa36d43830194` was measured
with the existing harness and input, without rebuilding parser artifacts.
Median allocation remains **2.418× baseline (+141.82%)**, **0.68% above the
first slice**, and **0.16% above vocabulary**. Baseline variability across
sessions prevents a clean speedup claim. Language-quality acceptance is
reported separately.

## Warm managed parser

Three alternating baseline–slot pairs used 128 identical synthetic requests,
8,192 warmup calls and 32,768 measured calls per process. Values below are
median [observed minimum–maximum] across three per-run metrics. Quantiles are
not pooled.

| Metric | Baseline rerun | Slots |
|---|---:|---:|
| p50, µs | 15.990 [15.760–16.051] | 11.782 [11.652–11.832] |
| p95, µs | 31.940 [31.540–31.991] | 37.300 [37.059–37.400] |
| p99, µs | 40.837 [39.965–40.867] | 46.277 [45.997–46.327] |
| Calls/second | 62,847.57 [62,800.27–63,673.81] | 64,927.62 [64,623.81–65,717.87] |
| Allocated bytes/call | 2,653.60 [2,653.60–2,653.60] | 6,417.06 [6,417.06–6,417.06] |
| Gen0 collections/run | 5 [5–5] | 12 [12–12] |

Slot median p95/p99 is 16.78%/13.32% above the current baseline. The same
baseline binary's p50 medians across the three sessions were 10.870, 18.184
and 15.990 µs. Its historical range reached 10.560–21.300 µs; this variation
limits comparisons based on the current lower slot p50.

Earlier candidate medians are retained for context. These columns compare
different sessions, without contemporaneous reruns of the earlier candidates.

| Metric | First slice, historical | Vocabulary, historical | Slots |
|---|---:|---:|---:|
| p50, µs | 11.732 | 11.621 | 11.782 |
| p95, µs | 36.759 | 37.561 | 37.300 |
| p99, µs | 46.508 | 48.471 | 46.277 |
| Calls/second | 63,659.14 | 64,595.53 | 64,927.62 |
| Allocated bytes/call | 6,373.70 | 6,406.96 | 6,417.06 |
| Gen0 collections/run | 13 | 12 | 12 |

[Summary JSON](latency/summary.json) preserves every cohort's full-precision
median/range, percentage changes, source revision and assembly hash. Six new
per-run JSON files retain only metrics, counts, hashes and scope notes.
Request contents and private corpus files are excluded. Parser assemblies
are Debug builds; the unchanged harness is Release.

| Identity | SHA256 |
|---|---|
| Shared input | `75a0f5d0697b4efd585ec530ddefbaa6a0e47c101b86d030ac99a66c40f04147` |
| Unchanged harness | `f4984e77cb3f3960e4b67a52184453b2fd569b95dff1736adb1ccdcc9eea4df4` |
| Baseline parser, `5041f00` | `50c7dd24adec1024437320a6670f1e4e4ca2ca58f5f27fc7cef05338ff7a2568` |
| First slice, `29a4091` | `cd0bbecef103ca6f0d97b3fddc6cf79babae3ba2ee00332516f534dfe048dbf2` |
| Vocabulary, `61d92cf` | `d547e11d0ddc3a591f940f4432c23e0379c0380faec636b6f37b3460e66c967f` |
| Slots, `8cefc807` | `590d1ba2e46e6fafadb5d522253a3194ed8f6a5328d3f2f46494520c3941b213` |

Current input, harness and frozen parser hashes were checked before and after.
All six latency processes completed before native execution and before the
completion signal allowing parser probes to resume. The benchmark covers
warm parser calls with delegate/timer and loop overhead, zero native calls
and zero learning. Startup, HTTP and server work are excluded. Each run
measures less than one second. Three observed ranges are not confidence
intervals; CPU affinity, frequency controls and host isolation are not
established. No statistical significance claim is made.

## Reused native executables

All four binaries were executed sequentially in a fresh temporary directory
with 1,200-second limits. Their hashes match the executables built at
`29a4091`. Native `src/`, `include/`, `tests/` and `Makefile` have no
changes between that revision and `8cefc807`. No native rebuild occurred.

| Executable gate | Checks | Failures | Executable elapsed |
|---|---:|---:|---:|
| `knowledge_accumulation_bench` | 12 | 0 | 0.92 s |
| `knowledge_composition_bench` | 74 | 0 | 0.97 s |
| `knowledge_capsule` | 94 | 0 | 0.15 s |
| `coverage_abstain` | 55 | 0 | 0.07 s |

These times cover executable runtime only and are not comparable with the
first bundle's build-plus-gate times. [Native summary](native/summary.json)
preserves PASS markers, binary provenance, resources and fixture metrics;
neighboring files retain full stdout/stderr, timings and accumulation JSON.
The 32/32 isolated units, 14/14 covered compositions, 94 capsule checks and
55 coverage checks passed. Coverage warnings/errors are expected negative
controls. Portable replay measures serialization fidelity on certified
exemplars. Native fixture success does not establish language acceptance.

The [first](../benchmarks-first/README.md) and
[vocabulary](../benchmarks-vocabulary/README.md) bundles, original runtime
logs and frozen binaries remain unchanged. [SHA256SUMS](SHA256SUMS) covers
every file here except itself; run `sha256sum -c SHA256SUMS` in this directory.
