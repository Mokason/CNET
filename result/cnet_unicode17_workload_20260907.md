# Unicode 17 real-source workload evidence — 2026-09-07

## Scope

Owner-delegated corpus choice: the versioned official Unicode Character Database.
Implemented a fixed offline extractor and two existing-format evidence tables,
with raw excerpt, full/excerpt/output hash pins, Unicode license, independent
managed extraction and a real installed-entry acquisition gate. See the
[contract and reproduction commands](../data/unicode17/README.md) and
[decision](../plans/cnet_unicode17_workload_20260907.md).

These are explicit case-change lookups for Latin-1 code points, not full case
conversion. Unicode empty case fields default to identity; this partial CNET
capability instead abstains. Outputs can exceed 255. No labels come from CNET.

## RED and independent source verification

- `UNICODE17_CORPUS_RED`: missing offline extractor, observed before implementation.
- `UNICODE17_LEARNING_RED`: missing pinned corpus, observed before adding evidence.
- Full official source: 2,198,209 bytes. Exact first 256 raw lines reproduce the
  15,707-byte excerpt; each record is the expected U+0000..U+00FF with 15 fields.
- Both full-source conversion branches byte-match their committed tables.
- Six Python boundary tests pass: semantics/edge outputs, exact reproduction,
  mutation/truncation/newline/oversize refusal, invalid mode, safe bounded file
  handling and CLI no-partial-output refusal. No new dependency was introduced.

## Real installed-entry lifecycle

The fixture publishes the actual managed control plane and pins six native
artifacts in its own private deployment. Both imports require approved hashes.
Before acquisition, exhaustive verification finds 58 and 56 missing answers,
respectively, and zero wrong answers. Two generated real `ask` misses create
demand; one original 30-second run builds, evaluates, activates and accepts two
capsules with three probation probes each. It finishes `budget_complete`, with
two jobs and no outstanding state or pending operation.

After both accept, installed `verify` checks every dataset/key pair. A second
sweep uses expectations independently extracted from pinned raw Unicode fields.
Both sweeps share the production transport/parser/observation implementation;
the independence is of label extraction, not a separate verifier implementation.
Both datasets remain bound to the same final active generation, revision 3.

| Dataset | Correct answers | Correct abstentions | Missing | Wrong |
| --- | ---: | ---: | ---: | ---: |
| `unicode17_upper_latin1` | 58 | 198 | 0 | 0 |
| `unicode17_lower_latin1` | 56 | 200 | 0 | 0 |

Each sweep covers 512 distinct dataset/key pairs. Repeating a sweep does not
increase distinct knowledge coverage: this is 114 explicit external facts and
398 required abstentions, not 1,024 learned facts or a held-out accuracy claim.

Initial focused gate: 1 passed, 0 failed/skipped, approximately 31 seconds.
TRX `/tmp/cnet-unicode17-gate-sjTcyhbt/_marble-system_2026-09-07_19_36_27.trx`,
SHA256 `370b88c9718d02baad4ddc7d7c9e353d53db50b901e1b4331cef3bb491c5163d`.
Its owner interval was 30.032657070 seconds; both acceptances occurred by
7.440557438 seconds. These are one-run observations, not throughput benchmarks.

## Regression-discovered ownership-release defect

The first combined managed regression was **not green**: 670 passed, 1 failed,
0 skipped. The existing `ChildCreationAndExclusiveOwnershipArePinned` test
could not reacquire its own lock immediately after disposing the first owner.
The isolated test then passed. Preserve the failed TRX:
`/tmp/cnet-unicode17-regression-B1wDZ1UH/_marble-system_2026-09-07_19_38_19.trx`,
SHA256 `5f70501a39ef2fd62e8c5725462b14c0fdb7e84d4544d718b745794280b5bf98`.

A deterministic duplicate-descriptor reproduction failed before the fix with
`LEARNING_LOCK_RELEASE_RED ... learning_owner_already_running`. Linux `flock`
belongs to an open-file description shared by duplicates/forked descriptors;
closing just the original does not release it. [Linux flock documentation](https://man7.org/linux/man-pages/man2/flock.2.html)
Concurrent process launch retaining a pre-exec descriptor is a plausible cause
of the original failure, not a traced fact about that particular run.

The owner now has an explicit lease: nonblocking exclusive acquire, atomic
single disposal, checked `LOCK_UN` before close, and guaranteed handle cleanup.
Supervisor cleanup releases remaining resources even if unlocking reports a
failure. Tests retain exclusion before disposal, allow replacement while the
duplicate stays open, and prove repeated old disposal/duplicate closure cannot
unlock a new owner. All 16 file-boundary cases pass after the fix. No retry,
test-serialization workaround or lower certification floor was used.

## Native gates and review

Final combined managed regression, including the ownership fix and latest raw
pin/edge assertions: **672 passed, 0 failed, 0 skipped**, approximately 39 seconds:

```sh
dotnet test dotnet/CnetControlPlane.Tests/CnetControlPlane.Tests.csproj --no-restore \
  --filter 'FullyQualifiedName~Learning|FullyQualifiedName~LocalTableReference|FullyQualifiedName~NativeControlProtocol' \
  --logger trx --results-directory /tmp/cnet-unicode17-final-ekLsq9Ng --nologo
```

TRX `/tmp/cnet-unicode17-final-ekLsq9Ng/_marble-system_2026-09-07_19_43_33.trx`,
SHA256 `b174386ff5f97c997e9034322f9118436de5d7d60338e41d03dae20c51415933`.
The repeated Unicode run accepted both datasets and reproduced all table counts
above under the fixed managed installation:

- Managed manifest: `72f62e02730657647858105b9ca49d525f979ce4bd6460a61ee91d117b516f67`.
- Native manifest: `e4143be05ed833fcd640e9ff15d10254fcf3568762b5e0ff11f904da9aa0fb3b`.
- Policy: `f516038af2c0a643d476ef5652e6f41f6e469f9fea794c266793fa13e7fe2966`.
- Final active set: `adf70551fd171b74992eb7974c5b39e089cb9680414bff2274eed829536d1996`, revision 3.

`make -j2 learning_daemon knowledge_capsule coverage_abstain
knowledge_accumulation_bench knowledge_composition_bench` passed:

- Native sandbox, reader, table acquisition, independent evaluation and daemon
  integration gates passed.
- Capsule: 94 checks. Coverage: 55 checks, its existing held-out gate 4/4.
- Accumulation: 32/32 isolation, 8/8 replay, 96 OOD refusals.
- Composition: 3 capsules, coverage enforced at every hop.

The native gate counts are their existing bounded fixtures, not Unicode scaling
or new-domain predictive performance. No native production code changed here.

Independent Astra reviews checked corpus semantics, extractor security, full
source reproduction and managed lifecycle claim adequacy. No required corpus
findings remained. NuGet vulnerability checks for the control plane and tests,
including transitives, reported no known advisories; this is not a vulnerability
absence guarantee. Existing CA1416 platform warnings remain in older test files.
An additional independent Astra review checked the ownership lease, deterministic
regression and exception cleanup; no required findings remained. The security
and adversarial-review workflow kept explicit source pins, unchanged certification
floors and the failed first regression in the evidence record.

## Operational limits

The integration deployment is disposable; test output preserves receipts, not a
permanent live service. Existing main/private services, ledgers, immutable
policies and budgets were not changed or reset. No GPU workload was displaced.
No git push is part of this task.

Broader nonnumeric text acquisition, meaningful learned GPU allocator benefit,
continuous 72-hour acceptance collection/run, high-scale capsule RAM/latency
measurements and production rollout remain WITHHELD. Exhaustive replay of a
public finite table is useful acquisition evidence, not held-out generalization
or a realistic user-demand benchmark.
