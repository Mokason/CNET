# Autonomous learning components — September 6, 2026

Component and private integration gates passed after the fixes below. **This is
not a production unattended-acceptance result.** The initial recording below
preceded the learning CLI, managed-installation pin and run heartbeat; later
implementation receipts are recorded separately at the end. No 72-hour
acceptance run has started. No learned allocator was activated. Its useful-gain gate
**FAILED**; the separate [allocator experiment report](cnet_autonomous_allocator_20260906.md)
retains that experiment's chronology, controls, failed floors and restrictions
on reusing exposed held-out outcomes. Nothing here overrides that result or
lowers a certification floor.

## Recorded gates

Logs are local `/tmp` artifacts, not an archived release bundle. Counts describe
their recorded runs, not every later edit or all possible inputs.

| Artifact under `/tmp/` | Observed result |
| --- | --- |
| `cnet-learning-managed-combined-20260906.log` | 453 passed, 2 failed, 0 skipped; 455 total, 5 seconds |
| `cnet-learning-managed-combined-isolated-20260906.log` | 455 passed, 0 failed/skipped; 9 seconds |
| `cnet-supervisor-recovery-green-20260906.log` | 18 passed, 0 failed/skipped; 1 second |
| `cnet-learning-native-gate-20260906.log` | 17 named sandbox cases, 10 reader checks, 6 producer tests, 13 verifier/snapshot tests passed |
| `cnet-learning-native-regression-20260906.log` | Knowledge capsule 94 checks; source evidence 53 checks; source producer 6 tests; reserved interface 2 checks; control CLI 9 tests; daemon suite 8 tests passed |
| `cnet-learning-product-sanitize-20260906.log` | `CAPSULE_PRODUCT_SANITIZE_PASS`; 110 checks, 0 failures |

Both combined runs used the same command; the recovery run used the same
project/options with filter `FullyQualifiedName~LearningAcquisitionTests`:

```sh
dotnet test dotnet/CnetControlPlane.Tests/CnetControlPlane.Tests.csproj --no-restore --artifacts-path dotnet/CnetControlPlane.Tests/bin/acquisition-build-5rhoUm --filter 'FullyQualifiedName~Learning|FullyQualifiedName~LocalTableReference|FullyQualifiedName~NativeControlProtocol' --nologo
make learning_native
make knowledge_capsule source_evidence source_reserved_interface capsule_control capsule_daemon_lifecycle
make capsule_product_sanitize
```

The two combined-run failures were process-wide descriptor-count assertions:
storage expected 184 and observed 193; runtime expected 216 and observed 215.
Sibling tests were legitimately opening/closing descriptors concurrently.
`LearningResourceCollection` now disables parallel execution for these resource
measurement classes. Both exact equality assertions remain; no tolerance, skip,
or production change was used to obtain the subsequent 455-pass result.
The supplied managed logs retain CA1416 platform warnings; native build logs
also contain compiler warnings. These are not warning-free-build claims.

The fresh daemon regression additionally records 20 cycles, 120 correct answers,
20 OOD refusals, RSS 16,160→18,200 KiB, at most three snapshot copies, and
stage/activate latency median 2.884109038859606 ms, maximum 3.421818953938782 ms.
This short test is not a long-duration soak or a memory-growth bound.

`make capsule_product_sanitize` compiled the linked C runtime and five test
executables in `/tmp/cnet-product-sanitize-FP2NiJ` with
`-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer`.
Runtime settings were `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1` and
`UBSAN_OPTIONS=halt_on_error=1`. The per-test logs report resident lifecycle
27, snapshot 10, durable store 31, publication/crash faults 40, and reserved
interface 2 checks. No sanitizer error was reported on those executed paths;
this is not a sanitizer run of every managed/native worker or proof of memory safety.

## Two actual supervisor recovery defects

1. **A known failed probation probe could be forgotten.** A correctly framed
   but wrong numeric answer set only a local failure flag. A subsequent missing
   control socket or cancellation during STATUS interrupted persistence, so the
   next tick returned `probation` instead of `rolled_back`. The fix persists
   `rollback_required` against the matched binding before that fallible STATUS
   call; rollback still requires a freshly checked native revision.
2. **Recovery could publish a stale, previously unsent activation.** After source
   bytes changed, replay blindly sent pending ACTIVATE and then rolled it back:
   the unpublished regression expected revision 1 but observed 3. Recovery now
   distinguishes exact native `Before` from already-published `Expected`.
   Stale evidence at `Before` refuses the activation without refunding its charge,
   then uses persisted DISCARD. An already-published activation still requires
   exact-token reconciliation followed by rollback; STATUS alone is not approval.

The actual RED receipts are tool sessions, **not saved log files**: session
18558 exited 1 with both cases of
`KnownFailedProbeSurvivesSubsequentControlFailureOrCancellation` failing;
session 92869 exited 1 with the unpublished case of
`StalePendingActivationNeverPublishesIfStillBeforeButReconcilesIfAlreadyPublished`
failing and the already-published case passing. Their commands used
`dotnet test dotnet/CnetControlPlane.Tests/CnetControlPlane.Tests.csproj`,
`--artifacts-path dotnet/CnetControlPlane.Tests/bin/supervisor-review-artifacts`,
`--filter FullyQualifiedName~TEST_NAME --verbosity minimal`; the first used
`-p:RestoreSources=/home/marble/.nuget/packages`, the second `--no-restore`.

The independently recorded 18-pass recovery run exercises real private native
daemon, sealed acquisition/evaluation, managed supervisor and durable ledger
paths. Publication states and socket failures are exercised, but owner-death
states are constructed and probation time is advanced with a **simulated clock**.
It does not establish elapsed probation or 72-hour uptime.

## What table acquisition establishes

The adapter uses existing CNB/CNU1 capsules and the schema-2 `frontend.cvfa`
asset, not new packaging. An explicitly authorized `user_correction` or
`verified_tool` source supplies 1–256 exact byte-key→16-bit-value rows. Private
files and full hashes establish byte integrity, not publisher authentication or
the external truth of supplied numbers; authority remains the trusted owner's
policy responsibility. CNET answers are not valid acquisition labels.

The native worker seals before source/candidate parsing. The parent reaps it,
preserves the complete incumbent inventory, freezes the existing canonical
snapshot identity, and persists STAGE before evaluating the actual staged
snapshot. Independent managed `LocalTableReference` parsing compares **all 256**
native answers/abstentions, not a builder PASS marker. Receipt construction also
binds source and snapshot hashes; source bytes are checked around evaluation.
This establishes finite-table fidelity and missing-key refusal, not prose
understanding, unseen-row inference, or independent external factual verification.

Refresh appends immutable history. Logical `data DATASET KEY` resolution requires
one activated match to the current full source hash; stale old versions refuse,
and ambiguous matches refuse. Native serving rechecks freshness at used table
hops and before returning. Owner source stability during a request is required:
these checks are not an atomic filesystem transaction or post-return guarantee.

## NuGet dependency audit

SDK `10.0.203`, target `net8.0`; both normal transitive audits exited 0 with no
reported advisories from `https://api.nuget.org/v3/index.json`. UTC observation
windows on September 6 were 19:56:42–19:56:56 for production and
19:57:35–19:57:53 for tests, not advisory-database publication timestamps.

```sh
dotnet list dotnet/CnetControlPlane/CnetControlPlane.csproj package --vulnerable --include-transitive --no-restore --source https://api.nuget.org/v3/index.json --format json
dotnet list dotnet/CnetControlPlane.Tests/CnetControlPlane.Tests.csproj package --vulnerable --include-transitive --no-restore --source https://api.nuget.org/v3/index.json --format json
```

The six production packages resolve to `Microsoft.Data.Sqlite.Core 8.0.11`,
`SQLite 3.53.4`, and `SQLitePCLRaw.bundle_e_sqlite3`,
`SQLitePCLRaw.config.e_sqlite3`, `SQLitePCLRaw.core`,
`SQLitePCLRaw.provider.e_sqlite3` all at `3.0.5`.
The 20-package test closure additionally contains `Microsoft.NET.Test.Sdk`,
`Microsoft.CodeCoverage`, `Microsoft.TestPlatform.ObjectModel` and
`Microsoft.TestPlatform.TestHost` at `17.12.0`; `Newtonsoft.Json 13.0.1`;
`System.Reflection.Metadata 1.6.0`; `xunit`, `xunit.assert`, `xunit.core`,
`xunit.extensibility.core` and `xunit.extensibility.execution` at `2.9.2`;
`xunit.abstractions 2.0.3`, `xunit.analyzers 1.16.0`, and
`xunit.runner.visualstudio 2.8.2`.

Both project files/locks and production assets were unchanged by the audits.
Known-advisory matching does not establish absence of vulnerabilities; the
shared .NET runtime, OS/GPU libraries and unknown vulnerabilities are outside
these package-audit results. Static inspection found no explicit shell,
exec/fork or control-socket operation in production table-worker pre-seal paths.
Trusted ordinary build/freeze and parent control clients are separate authority;
the installed code, loader and kernel remain trusted. No hostile-native-code
isolation or read-confidentiality proof is claimed.

## Artifact byte identities

SHA256 values identify inspected local bytes, not authenticated release receipts.

| Artifact | SHA256 |
| --- | --- |
| Combined failed managed log | `ef9509ee8218c8b4c052d7e928a8c8f29656af1a19dc813e6b5e31af5a7f7bc6` |
| Combined isolated managed log | `c5dfb812eed0e0a503a65df76504f30f15d13945c5ed93e1572def4a4e390747` |
| Supervisor recovery green log | `bc74607ef921fbec49b19331f3a6420562a47aaa8ce57cc84a4224faa26726dc` |
| Product sanitizer top-level log | `7542e87cd3db03b869191d2f887973ccc241000680cecb0ce012498c03e508af` |
| Sanitized `libcnet_capsule_core.so` | `a470bda812670c5a0f9437038923f334b709d6787e4859736a30851c8575d8fb` |
| Fresh native regression log | `0cfde07ee68bff7902fda35b6ed6c71b71bb56ec6c48cee7d5afde2cd2d79b5b` |
| Native learning gate log | `88b69fa23729656b3b3dcc6d0ef599522f5f684c23ed9b65580815535b8c49b8` |
| `dotnet/CnetControlPlane/packages.lock.json` | `1b7527d6bffa545f9c7283d8676bd8d5eae3d0ab93c5ec96cc0912d95c139552` |
| `dotnet/CnetControlPlane.Tests/packages.lock.json` | `0a6d4476eccf1513af1401cfe2dcc676508876506e3c4c80e9ba3492ac470d83` |

Production unattended operation and useful allocator gain remain **WITHHELD**.

## Deployment controls: later implementation receipts

Recorded September 6, 2026, through 21:06 UTC (September 7 in Europe/Riga).
The owner requested deployment controls and a fresh allocator investigation,
explicitly excluding launch of the 72-hour acceptance run from this slice.

- Commit `758b973` adds schema-2 durable run accounting. Actual run-ledger REDs
  preceded 108 passing ledger regressions. Independent review additionally
  reproduced four cases: a paused run could become complete or record a fresh
  heartbeat, and a prematurely persisted completion could pass integrity checks.
  Those are fixed without clearing pause or lowering the elapsed floor.
- Commit `9306f6b` adds the fixed eight-file managed manifest, running-entry
  assembly/location checks, exact packaged SQLite resolver and ledger binding.
  The attestor's 65 focused cases passed independently. Installation immutability
  from process launch remains a trusted-owner precondition; loaded assembly
  paths are not in-memory image hashes.
- `/tmp/cnet-learning-command-red-20260906.log` records two real separate-process
  REDs before CLI implementation. A subsequent fixture error was localized to
  `managed/runtimes` having default ancestor permissions; only fixture creation
  changed. The installed real entry assembly then opened packaged SQLite 3.53.4
  successfully. Reinitialization refuses without resetting its ledger.
- `/tmp/cnet-learning-operator-red-20260906.log` records two operator REDs and
  six passing existing cases; the later eight-case run passes a real private
  daemon demand/acquisition/verified-answer cycle and durable pause/resume.
- Missing-pin fault injection then reproduced two silent-rebinding defects.
  All non-initialization CLI paths now require both original pins; missing rows
  durably pause and refuse. `/tmp/cnet-learning-pin-red-run-green-20260906.log`
  records those two REDs alongside three passing bounded-run cases.
- `/tmp/cnet-learning-command-combined-green-20260906.log` records **13 passed,
  zero failed/skipped**, six seconds. These include actual two-second run-budget
  accounting, durable heartbeat, terminal restart refusal, concurrent owner
  pause and rollback of an unaccepted candidate before natural completion.
  These timings are not the simulated probation clocks in older component tests.
- A separate real-process SIGTERM-during-quiesce regression and three clock
  constructor failures produced RED4 before fixes, then 47 passing cancellation,
  run-command and run-ledger cases in `bin/run-cancellation-artifacts` (tool
  sessions 89766 and 34346). Cleanup completes before recording cancellation;
  the cancellation decision is checked immediately before completion, not
  retroactively after a committed terminal transaction.

Quiescence preserves charges and pause state, never reserves new work, withdraws
unpublished activation, and reconciles a published token before rollback. Its
26 cases passed under tracing and received author-separated review. During
repeatability checking, one native producer setup refusal occurred before the
quiescence operation; an untraced repeat reproduced it. Nineteen traced repeats
passed all 76 cases. At this recording the cause remains **unresolved**; those
passes do not erase the failures. Original and diagnostic-build evidence is
retained under the test artifact directories `quiescence-evidence-GEQfUn` and
`quiescence-worker-diagnostic-4fK0hy`. No retry or guard weakening was introduced.

### Later deployment regression and final review

The fixed-runner repetitions and combined results below were recorded after
the preceding 21:06 UTC checkpoint, not substituted for its failures.
Managed test `bin/` artifact paths in this section are relative to
`dotnet/CnetControlPlane.Tests/`.

- An actual native no-fork helper reproduced Linux parent-death termination
  when its invoking managed thread ended while the owner process stayed alive:
  `LEARNING_CHILD_PARENT_THREAD_RED`, exit 137. Commit `f93a581` retains a
  dedicated spawning thread through the complete child kill/reap/pipe lifecycle.
  Arguments, environment and the initial clock are still captured synchronously
  before thread handoff. All 30 child cases passed. An author-separated repeat
  then passed 80/80 untraced quiescence cases, without retries, in
  `bin/fixed-child-evidence-xHR6go`. This fixes a reproduced lifetime defect;
  it does **not** conclusively identify the cause of the two earlier generic
  producer setup refusals, whose failing raw exit was not captured.
- A combined run passed 618 cases but failed one global descriptor assertion
  (expected 199, observed 186) despite test-class serialization. The reduction
  alone is not leak evidence. A deterministic unrelated-descriptor RED then
  reproduced the metric defect. Commit `f482e3d` counts the stable fixture's
  root, nested directories and files only, with positive retained-handle and
  unrelated-prefix-sibling controls. Each of 40 failed loads must leave exactly
  zero fixture descriptors inside a no-GC region; finalizers, tolerances and
  skips do not hide disposal failures. All 67 managed-runtime cases passed.
  Receipts are in `bin/managed-fd-evidence-VlfhqI/results`.
- `/tmp/cnet-learning-quiesce-command-red-20260907.log` records three actual
  CLI REDs. The implemented `quiesce` command persists stop/pause only after
  acquiring the owner lock and retries cleanup without acquiring new work.
  Real separate-process cases cover published probation rollback with either
  working or missing ask socket, idempotent retry and competing-owner refusal.
- `/tmp/cnet-learning-managed-deployment-final-20260907.log` records **624
  passed, zero failed/skipped**, 12 seconds, exit 0. It uses the same combined
  filter above and includes the child, manifest, run, recovery and CLI changes.
- `/tmp/cnet-learning-native-deployment-final-20260907.log`, exit 0, records
  17 sandbox cases, 10 table-reader checks, six producer tests, 13 verifier/
  snapshot tests and the real table-learning daemon case. The same run passes
  knowledge capsule (94 checks), coverage/abstention (55 checks), accumulation
  (32/32 isolation, 8/8 replay, 96 OOD refusals) and three-capsule composition
  with coverage guarded at every hop. These are short regression gates, not a
  soak, a new GPU benchmark or universal capability certification.

The final author-separated deployment review found a normal-concurrency
storage-scan race: supported `ask`/`status` ledger transactions could change
SQLite metadata during a quota scan and unnecessarily fail a healthy run.
Four focused tests ran before the fix: one actual RED, three passing. The RED
measured an unprotected scan returning in 7 ms while a demand writer was still
active. The ledger-owned scan now holds `BEGIN IMMEDIATE` across the unchanged
metadata-only traversal, including ledger/journal bytes. It makes no clock or
epoch update and commits no dirty pages. Acquisition requires this owned ledger;
there is no unlocked fallback. Main independently inspected the helper, all
production call sites and tests after the reviewer implemented the fix.

`bin/storage-concurrency-4r6c0B/results/storage-green.trx` records 79 passed,
zero failed/skipped, six seconds. Tests require waiting for committed demand
growth, exact byte limits, inclusion of cold journals, unchanged clock/epoch,
link refusal and lock release on error. Final combined regression in
`/tmp/cnet-learning-managed-storage-final-20260907.log` records **628 passed,
zero failed/skipped**, 12 seconds, exit 0. The ordinary concurrency defect is
fixed; owner file edits outside SQLite remain nontransactional, and the quota
is not a kernel enforcement mechanism.

The reviewed acquisition/supervisor/quiescence slice, including the scan fix,
is committed as `40960fe`. The actual operator CLI and durable run integration
are committed as `3a15057`. These commits are on the isolated feature branch;
no live configuration, service, main worktree or remote branch was changed.

| Later artifact | SHA256 |
| --- | --- |
| 624-case managed log | `2e08f3239c5deddc08d33e7f93f82c1de1d58b37d66ad3bd25b32eb5f131cefd` |
| Fresh native gate log | `2f0a905e67f1c3608ec0d2584b2409e608a6c48fdf2185aedb3d98aad167ce5a` |
| CLI quiesce RED log | `2fb455c9bd6f6a316a56c0e03409268c4c161c2c49c9f86792024f8150b406d4` |
| Final 628-case managed log | `c84a8747e2efd2e8104fd8527807eac50d85e6aa91d06c1c937158ff9d8e9385` |
| Storage concurrency RED TRX | `c42e2878d2e4a300f0645c7ad8ddb6b31e901c26eb22a38f233e91f7d306cb4d` |
| Storage concurrency GREEN TRX | `bbcc6a8515daf151383f83ac614ea3b6e41858ed8ae4daef4fbd638b63c1b663` |

The fresh [development sequence pilot](cnet_allocator_sequence_pilot_20260907.md)
now records measured deterministic multi-step planning value, but zero residual
headroom against its exact deterministic control. No new GPU fit, learned-gain
pass, allocator activation or 72-hour acceptance is claimed.
