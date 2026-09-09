# First-candidate benchmark evidence

Preserved results for candidate `29a4091b9a2d52bd06f109f7a588adf11a470a2c`,
compared with baseline `5041f00e39bbaf903eca83f86c296b40cf084f79`.
These measurements predate the follow-up source slice. Native gates passed;
the first candidate failed the separate unchanged language-quality floors
(61/80 ready, 16/24 clarify, 23/24 abstain, zero wrong accepted results).
Timing and native success do not establish language acceptance.

## Warm managed parser

Three completed runs per cohort used the same 128 synthetic requests, with
8,192 warmup calls and 32,768 measured calls per run. Values below are the
median of each run's reported metric, followed by its observed minimum–maximum.
Quantiles were not pooled across runs.

| Metric | Baseline median [range] | Candidate median [range] | Median change |
|---|---:|---:|---:|
| p50, µs | 10.870 [10.560–20.449] | 11.732 [11.672–11.932] | +7.93% |
| p95, µs | 32.531 [32.320–32.952] | 36.759 [36.619–37.651] | +13.00% |
| p99, µs | 41.268 [41.147–41.878] | 46.508 [45.255–50.094] | +12.70% |
| Calls/second | 65,114.59 [54,063.96–65,225.71] | 63,659.14 [63,202.17–63,670.98] | −2.24% |
| Allocated bytes/call | 2,653.60 [2,653.60–2,653.60] | 6,373.70 [6,373.70–6,373.70] | +140.19% |
| Gen0 collections/run | 5 [5–5] | 13 [13–13] | +160.00% |

All six reports have the same input digest; the original input file was also
hashed to verify it. Each cohort reports one stable parser assembly digest.
The baseline and candidate binaries intentionally have different digests.

| Identity | SHA256 |
|---|---|
| Shared input | `75a0f5d0697b4efd585ec530ddefbaa6a0e47c101b86d030ac99a66c40f04147` |
| Baseline parser assembly | `50c7dd24adec1024437320a6670f1e4e4ca2ca58f5f27fc7cef05338ff7a2568` |
| Candidate parser assembly | `cd0bbecef103ca6f0d97b3fddc6cf79babae3ba2ee00332516f534dfe048dbf2` |

The six [latency reports](latency/baseline-1.json) contain only metrics, counts,
hashes and scope notes; they were copied byte-for-byte after checking the fields.
Request contents and private corpus files are excluded. [The summary](latency/summary.json)
retains full precision and the measured harness assembly digest.

This synthetic warm microbenchmark includes delegate/timer and loop overhead,
with zero native calls and zero learning events. It excludes startup, HTTP and
server work. Each measured run lasted less than one second. Three observed
ranges are not confidence intervals; the slower third baseline run is retained.
The reports do not establish CPU affinity, frequency controls or host isolation.
There is no statistical significance claim. File timestamps place completion
of all six latency reports before the first native build began.

## Native fixture gates

These sequential `make -j4` executions used the normal CPU build, with a
1,200-second timeout per gate and no GPU or provider execution.

| Target | Checks | Failures | Build + gate elapsed | Measured fixture outcome |
|---|---:|---:|---:|---|
| `knowledge_accumulation_bench` | 12 | 0 | 8.17 s | 32/32 units isolated; zero drift; 8/8 portable replay; 96 OOD refusals |
| `knowledge_composition_bench` | 74 | 0 | 8.53 s | Three certified members; 14/14 covered compositions exact; every-hop guards |
| `knowledge_capsule` | 94 | 0 | 7.53 s | Five coverage rows preserved; corrupt and incompatible imports refused |
| `coverage_abstain` | 55 | 0 | 7.13 s | Four native fixture holdouts correct with guard, zero with guard disabled |

[Native summary](native/summary.json) retains exact PASS markers, timings,
binary hashes and accumulation scale metrics. Original runtime logs and the
accumulation JSON are in `native/`; `build-*.log` files contain full compiler
output and resource timings. No compiler warnings occurred in the four gates.
Coverage WARNING/ERROR messages are deliberate negative controls. The earlier
16.73-second prerequisites build log retains its existing compiler warnings.

Native elapsed times include compilation. Accumulation's lookup includes
materialization and CNU1 verification; its 0.00013 ms forward timing is only
`btn_forward`, averaged over 200 repetitions in one run without variance.
The original JSON rounds that value to 0.0001 ms. Portable replay tests
serialization fidelity on certified exemplars. Accumulation's
`incompatible_rejected` field actually reports truncated-payload integrity
refusal; the capsule gate separately tests a foreign version.

[SHA256SUMS](SHA256SUMS) covers every preserved/generated file except itself.
Run `sha256sum -c SHA256SUMS` from this directory to verify the bundle.
`native/original-gates.sha256` preserves the original executable/log manifest;
its paths refer to the experiment worktree and original temporary logs.
