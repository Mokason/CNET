# Operational expansion — September 7, 2026

**Delivered: explicit evidence import, independent complete-domain live
verification, and native GPU-worker lifetime/output hardening.** This is a
tested implementation increment, not completion of the wider product roadmap.
Baseline `3457f5c`; live verification commit `ce15250`, worker commit `3c408b8`,
approved evidence import commit `f15b05a`.
No existing service, live policy, ledger, GPU allocation or remote branch was
changed. The main checkout's unrelated edits remain untouched.

## Delivered behavior

- `learning import DEPLOYMENT DATASET SOURCE SHA256` initially publishes only
  approved exact source bytes under existing policy authority. Private paths,
  regular single-link files, canonical format, owner lock, storage headroom and
  exclusive publication are required. No overwrite, demand, job, activation or
  policy/run reset. A digest pins bytes, not truth or source authentication.
- `learning verify DEPLOYMENT DATASET` compares all 256 live observations to
  the independently parsed authorized numeric source, including omitted-key
  abstention and verified zero. It reports disjoint correct-answer/abstention,
  missing-answer and wrong-answer counts. All 256 must match for exit zero.
  It creates no learning demand; ordinary transactional ledger observations and
  transient native query telemetry remain possible.
- Verification binds source hashes and durable native revision/state tuples
  before/after the sweep. A single suspend-inclusive budget also cancels pending
  control/ASK exchanges. Interrupted, changed or uncertain observations do not
  yield a successful sweep receipt. Fences do not establish uninterrupted daemon
  process identity or hostile-owner isolation.
- Native workers bind lifetime to the exact spawning thread, including death
  before signal arming, and arm an absolute boot-time SIGKILL deadline before
  source/GPU use. Stdio is `/dev/null`; result output is a checked pipe. Parent
  structured exit/signal records remain, but worker runtime stderr is discarded.

The owner guide is `docs/AUTONOMOUS_LEARNING.md`; new worker requirements and
same-build caller/entry compatibility are in `experiments/offline_controller/README.md`.

## RED, review and verification

The initial live command and import end-to-end tests emitted their RED markers
against the old CLI before implementation. Astra-only independent review found
an in-flight whole-sweep deadline gap; two pending-ASK/STATUS RED cases reproduced
it before the global deadline polling fix. The second review found no further
required issue; cancellation and invalid-initial-clock suggestions were added.

Import review found no concrete implementation violation but identified missing
quota and publication-uncertainty tests. Exact fit succeeds; one byte over the
remaining quota refuses. An author-separated fixed test subprocess shim forces
directory fsync to fail after rename: the CLI refuses, preserves exact source
bytes, releases both locks and refuses identical/different overwrite retries.
This is an injected syscall failure, not a physical power-loss test.

Worker RED tests reproduced inherited stderr authority, suspend-exclusive pool
deadline, unavailable-clock admission, missing self-deadline and spawning-thread
death before/after guard. An additional RED case caught elapsed-time underflow
on clock failure. Changes passed independent review before commit.

| Verification | Observed result |
| --- | --- |
| Combined managed Learning + LocalTableReference + NativeControlProtocol | 670 passed, 0 failed/skipped, 13 seconds |
| Live verifier subset | 25 cases, including actual copied-entry/native-daemon coverage and stale-source refusal |
| Import subset | 16 cases plus 1 actual post-publication fsync-fault case |
| Native `learning_daemon` prerequisites and integration | PASS, including acquisition, stale refresh, rollback/restart |
| `knowledge_capsule` | 94 checks PASS |
| `coverage_abstain` | 55 checks PASS; declared held-out 4/4 |
| `knowledge_accumulation_bench` | 32/32 isolation, 8/8 replay, 96 OOD refusals |
| `knowledge_composition_bench` | 3 capsules, coverage enforced at every hop |
| Worker boundary CPU suite | 22 cases PASS; also included in default offline-controller CPU `test` |
| Existing CPU/sandbox/allocator regression | PASS |
| AMD graph/allocator worker builds | PASS; invalid direct entry refuses before GPU initialization |
| Focused worker ASan/UBSan | 22 cases PASS; no visible diagnostics |
| Production/test NuGet known-advisory audit | No reported vulnerabilities |

Combined managed receipt:
`/tmp/cnet-operational-regression-mk9S5Q/_marble-system_2026-09-07_19_16_16.trx`.
SHA256: `e726673d89ab01e47fbcd0095c482cffb1cc2400d320a4ea7e448ced4f8e08ed`.
Worker outputs: `/tmp/cnet-worker-hardening-20260907` and
`/tmp/cnet-worker-sanitizers-MKOjYm`. These are local working receipts, not a
portable archived release. Intentional SIGKILL/_exit bypass child leak-at-exit
checks; child stderr is discarded, so sanitizer detail there is unavailable.
Parent leak detection was enabled. Audit results cover known advisories, not
absence of vulnerabilities. Existing test-only platform warnings remain.

Reproduction (repository root, native prerequisites built):

```sh
dotnet test dotnet/CnetControlPlane.Tests/CnetControlPlane.Tests.csproj \
  --no-restore \
  --filter 'FullyQualifiedName~Learning|FullyQualifiedName~LocalTableReference|FullyQualifiedName~NativeControlProtocol'
make -j2 learning_daemon knowledge_capsule coverage_abstain \
  knowledge_accumulation_bench knowledge_composition_bench
make -C experiments/offline_controller test worker-sandbox-test allocator-build allocator-test
make -C experiments/offline_controller worker-build allocator-worker-build
dotnet list dotnet/CnetControlPlane/CnetControlPlane.csproj package --vulnerable --include-transitive
dotnet list dotnet/CnetControlPlane.Tests/CnetControlPlane.Tests.csproj package --vulnerable --include-transitive
```

## Outstanding scope — WITHHELD

1. **Real workload and broader adapters.** Import is numeric-table onboarding,
   not arbitrary text learning. Tests use synthetic calibration fixtures. No
   approved production corpus was selected or ingested in this increment.
2. **Useful learned controller.** The prior unchanged improvement gate remains
   failed, allocator inactive, with no confirmation retuning or new GPU fit.
   The deterministic sequence pilot is still offline, not production-integrated.
3. **Actual-device qualification of new guards.** During checks, live services
   occupied GPU 0 at 100% and GPU 1 at varying activity up to 63%. No jobs were
   displaced. Prior successful GPU fits used earlier worker bytes; they do not
   validate these changes. Hardware suspend/driver teardown remain untested.
4. **Unattended acceptance.** The new independent live probe is a prerequisite,
   not a continuous collector/checker. No actual 72-hour run or large-inventory
   RAM/latency campaign was started. The 262143/262144-entry import boundary has
   source review but no physical test fixture at that size.
5. **Rollout.** These source changes require a newly built, pinned installation.
   Existing deployments were not modified, enabled for boot or restarted.

Next decisions: select an authorized real dataset/corpus; arrange an idle or
explicitly reserved AMD test window; then qualify the new worker and integrate
the workload under unchanged certification gates. Production promotion and
acceptance still require their own evidence and deployment decision.
