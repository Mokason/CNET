# Vocabulary-candidate benchmark evidence

Frozen parser candidate `61d92cf078ff89443883d39fa13eb6427c51fd87` was measured
with the existing harness and input, without rebuilding either parser artifact.
Median allocation is **2.414× baseline (+141.44%)** and **0.52% above the first
slice**. Tail latency is also higher. The variable baseline p50 prevents a
clean speedup claim. Language-quality acceptance is reported separately.

## Warm managed parser

Three alternating baseline–vocabulary pairs used 128 identical synthetic
requests, 8,192 warmup calls and 32,768 measured calls per process. Values are
median [observed minimum–maximum] across three per-run metrics; quantiles are
not pooled. The first-slice column comes from the earlier preserved session.

| Metric | Baseline rerun | First slice, historical | Vocabulary |
|---|---:|---:|---:|
| p50, µs | 18.184 [11.411–21.300] | 11.732 [11.672–11.932] | 11.621 [11.471–11.802] |
| p95, µs | 32.391 [32.091–33.042] | 36.759 [36.619–37.651] | 37.561 [37.290–37.852] |
| p99, µs | 41.397 [41.348–41.588] | 46.508 [45.255–50.094] | 48.471 [48.081–48.681] |
| Calls/second | 57,936.79 [49,085.09–64,795.84] | 63,659.14 [63,202.17–63,670.98] | 64,595.53 [64,457.37–65,596.69] |
| Allocated bytes/call | 2,653.60 [2,653.60–2,653.60] | 6,373.70 [6,373.70–6,373.70] | 6,406.96 [6,406.93–6,406.97] |
| Gen0 collections/run | 5 [5–5] | 13 [13–13] | 12 [12–12] |

Vocabulary median p95/p99 increased 15.96%/17.09% against the baseline rerun
and 2.18%/4.22% against the first slice. Its p50 and throughput comparisons
change with baseline variability: the earlier baseline medians were 10.870 µs
and 65,114.59 calls/second. All runs are retained; no run was discarded.

[Summary JSON](latency/summary.json) retains full precision, percentage changes,
all source IDs and both baseline sessions. Six new per-run JSON files in
`latency/` retain only metrics, counts, hashes and scope notes. Input contents
and private corpus files are excluded. The parser assemblies are Debug builds;
the unchanged harness is Release.

| Identity | SHA256 |
|---|---|
| Shared input | `75a0f5d0697b4efd585ec530ddefbaa6a0e47c101b86d030ac99a66c40f04147` |
| Unchanged harness | `f4984e77cb3f3960e4b67a52184453b2fd569b95dff1736adb1ccdcc9eea4df4` |
| Baseline parser, `5041f00` | `50c7dd24adec1024437320a6670f1e4e4ca2ca58f5f27fc7cef05338ff7a2568` |
| First slice, `29a4091` | `cd0bbecef103ca6f0d97b3fddc6cf79babae3ba2ee00332516f534dfe048dbf2` |
| Vocabulary parser, `61d92cf` | `d547e11d0ddc3a591f940f4432c23e0379c0380faec636b6f37b3460e66c967f` |

All input, harness and frozen parser hashes were checked before and after.
The six latency processes completed before the native runs and the completion
signal allowing parser probes to resume. Measurements cover warm parser calls
with delegate/timer and loop overhead, zero native calls and zero learning.
They exclude startup, HTTP and server work. Each run measures less than one
second; three observed ranges are not confidence intervals. Host isolation,
CPU affinity and frequency controls are not established, and comparisons with
the first slice cross sessions. No statistical significance claim is made.

## Reused native executables

The four existing binaries were executed sequentially in a fresh temporary
directory with 1,200-second limits. Their bytes match the first bundle's
SHA256 identities. They were built at `29a4091`; `src/`, `include/`,
`tests/` and `Makefile` have no changes between that revision and `61d92cf`.
No native rebuild occurred.

| Executable gate | Checks | Failures | Executable elapsed |
|---|---:|---:|---:|
| `knowledge_accumulation_bench` | 12 | 0 | 0.94 s |
| `knowledge_composition_bench` | 74 | 0 | 0.97 s |
| `knowledge_capsule` | 94 | 0 | 0.15 s |
| `coverage_abstain` | 55 | 0 | 0.08 s |

These times cover executable runtime only; the first bundle's times include
compilation and are not comparable. Full stdout/stderr, resource timings,
PASS markers, binary identities and fixture metrics are retained in
[native/summary.json](native/summary.json) and its neighboring logs. The same
32/32 isolated units, 14/14 covered compositions, 94 capsule checks and 55
coverage checks passed. Coverage WARNING/ERROR messages are expected negative
controls. Portable replay measures serialization fidelity on certified
exemplars; native success does not establish language acceptance.

The original [first-candidate bundle](../benchmarks-first/README.md), original
runtime logs and frozen binaries still verify against their existing hashes.
[SHA256SUMS](SHA256SUMS) covers every file here except itself; verify with
`sha256sum -c SHA256SUMS` from this directory.
