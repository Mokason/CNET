# Policy-bounded unattended specialist learning

Status: deployment controls implemented and regression-verified; useful learned
allocator gain and unattended acceptance WITHHELD.
Baseline: `f805be899fc9aca589742366b8ae32d7c958cec7`.
The owner requested all six stages: learning supervisor, independent evidence,
independent evaluation, automatic activation, useful core improvement and a
72-hour unattended acceptance run. This branch is private and opt-in. Main and
the previous product worktree remain unchanged. No existing service restart,
live configuration replacement, remote push or external teacher call is implied.

## Outcome and non-negotiables

Current requested slice (owner: "do 1. and 2."): finish managed installation
pinning, operator commands and durable supervision; then evaluate a fresh
allocator experiment against the unchanged benefit gate. Do not launch the
72-hour acceptance run in this slice. Its result remains WITHHELD.

Operator telemetry must answer which immutable installations/policy are bound,
whether the original run is live/failed/complete, and which fixed action last
completed. Use a bounded singleton heartbeat plus structured command/tick
events; never log source rows, control tokens or full exception messages.
Schema 2 adds managed binding and run accounting; schema 1 experimental ledgers
are refused, never automatically reset or migrated. A run budget completing
does not certify acceptance or useful learning.

Close the actual loop: an uncovered request creates durable demand; a bounded
job acquires independently authorized evidence; the existing capsule mechanism
builds a candidate; an independent evaluator checks its exact claims and old
obligations; policy permits native activation; monitoring either retains the
candidate or performs revision-checked rollback. Process restarts do not lose
budget charges or create duplicate activation. Failures produce evidence, not
success markers.

No training on CNET Tier-A answers. No lowered certification floor, rewritten
holdout, worker self-approval, self-edited policy or unrestricted host command.
CNB/CNU1 and schema-2 assets remain the knowledge packaging system. A digest is
not source authentication. New data are owner-authorized local datasets; this
does not promise arbitrary text understanding or unbounded task acquisition.

## Boundaries and decisions

- Reuse the maintained SQLite-backed managed control plane for one trusted
  supervisor, strict policy, job/budget ledger and restart reconciliation.
  Existing suggestion activation is not native serving activation.
- Keep the native daemon and its owner-only socket as capsule activation
  authority. Record an intent before sending a mutation. At most one unresolved
  mutation and one candidate in probation. Never stage the next candidate while
  rollback is required. Unexpected owner changes pause instead of guessing.
- The old scheduler charter stays a legacy policy surface; it cannot act as
  this supervisor's fail-closed authority. The new explicit policy does not
  modify or silently extend any running profile.
- Workers receive fixed trusted executable code, frozen task input and one
  private output subtree. CPU workers enter a Linux filesystem/process/socket
  sandbox before parsing source. GPU workers combine filesystem, offline and
  process seals; a filesystem write guard alone cannot protect Unix control.
  Workers cannot write policy, evaluator, ledger or active state. The installed
  kernel/runtime and owner are trusted; malicious same-UID owner activity is
  not an isolation guarantee.
- The first expanded domain is a canonical numeric local table, uint8 keys and
  uint16 values, 1..256 rows. Source authority is explicit owner configuration.
  A separately implemented evaluator reads the source rather than builder
  labels. It verifies every declared key and refusal at every omitted key.
  Versioned ports plus full source identity preserve old capsule history;
  `data DATASET KEY` selects only a unique active, fresh matching version.
- Core improvement first targets allocation of learning effort. The current
  incumbent is rotating queue order, not a weakened FIFO substitute. The
  learned allocator has a distinct objective from graph-serving checkpoints;
  reuse the FP32 cell math and canonical checkpoint family, never reinterpret
  graph weights. Serving authority and certification remain unchanged.
- Resource accounting uses suspend-inclusive boot time and boot identity.
  Reserve work before launching it; interrupted/uncertain jobs keep their charge.
  Reboot cannot refill a rolling budget from an untrusted wall-clock jump.

## Ordered implementation slices

### 1. Strict policy and private filesystem boundary

Files: managed Learning policy, Linux file boundary, focused tests and example.
Acceptance: unknown/missing/duplicate fields and unsafe paths refuse; immutable
policy identity is pinned; defaults cannot enable autonomy. Verify actual RED
and focused policy/path tests, including links and nonregular files.

### 2. Durable demand, reservation and operation ledger

Files: managed Learning ledger/clock and focused tests.
Acceptance: duplicate demand is counted without duplicate jobs; pending work
reserves budget transactionally; corruption, policy mismatch, reboot and clock
rollback never silently reset state. Verify real SQLite restart, competing
owners and injected transaction failures. Depends on 1.

### 3. Confined native table acquisition

Files: table evidence/parser/compiler/producer and CPU worker guard, split into
reader, compiler, sandbox and serving increments with independent tests.
Acceptance: complete exact-table fidelity, sparse abstention, stale refresh and
namespace refusal; worker cannot mutate control state or use control IPC.
Verify producer RED/GREEN, adversarial guard tests and same-process native asks.

### 4. Independent evaluation and native promotion

Files: managed source reference/evaluator, bounded process/control transport,
supervisor and tests, delivered in separate increments.
Acceptance: builder PASS cannot grant approval; wrong/missing/extra evidence
refuses; native STAGE preserve=1 and durable activation use exact persisted
identity/revision/token. Verify real private daemon end-to-end, stale evidence,
publication uncertainty and crash/retry. Depends on 1..3.

### 5. Probation, automatic rollback and operator controls

Acceptance: verify real answers during probation, preserve old obligations,
rollback a known bad active candidate through native control, pause on unknown
state or unreconciled rollback. Provide status/pause/resume and bounded event
records. Verify crash points before/after sends and no next-stage interference.
Depends on 4.

### 6. Separately typed allocator training

Files: canonical objective-aware checkpoint APIs, bounded sealed-row AMD worker,
allocator inference/CLI and tests, each as a separate reviewed increment.
Acceptance: graph loaders reject allocator checkpoints; training accepts only
bounded finite independent-outcome rows; both AMD workers train actual supplied
data with immutable transfer, CPU/GPU parity and confinement. No outcome mask or
evaluation-only field is an online policy input. Depends on frozen row contract.

### 7. Useful allocator gain and supervised adoption

Root creates independent outcome records by actual tool/acquisition/capsule
execution in private registries. Split whole queue/inventory episodes, numeric
blocks and policy topologies; fresh confirmation follows frozen candidates.
Compare the exact rotating incumbent, demand/work ordering and completion-first
ordering under identical cost/job/fairness constraints. The prospective gate is
at least 0.05 more verified demand coverage and a positive paired 95% lower
confidence bound against each comparator, no per-family regression and no wrong
verified answer. Require >=32 independent episodes and >=4 per declared family.
Failures retain the incumbent; neither labels nor comparators may be changed to
manufacture a pass. Investigate objective/representation limits if this fails.
Persist allocator generation and rollback separately from serving authority.
Depends on 2, 4 and 6.

### 8. Short integrated fault and resource campaign

Acceptance: fresh independent demand improves, old tasks remain certified,
poisoned/stale/failed jobs refuse, budgets bound attempts and storage, and
process restart reconciles exactly. Run native/managed regression, sanitizers,
dependency audits and frozen existing capability gates. Compare quality and
resource cost before/after; unit count alone is not a benefit. Depends on 5, 7.

### 9. Actual 72-hour unattended acceptance

Freeze executable, source, policy, evaluator and candidate identities into a
new private deployment. Exercise new evidence and independent probes throughout
the run, not only at startup. Record boot ID, observed boot-time span, bounded
probe gaps, budgets, jobs, native revisions and rollback receipts. Permit tested
supervisor/daemon process restarts; require one boot for continuous qualification.
Suspend/reboot gaps are not successful monitoring. Simulated clocks and fast
iterations are tests only and cannot satisfy 72 actual hours. The final
acceptance marker remains WITHHELD until elapsed time and all criteria pass.
Depends on 8; no live user service is replaced.

### 10. Handoff

Update current documentation/checklist with exact commands, result/source
identities, limits, interruption/recovery behavior and owner stop controls.
Only actual completed gates can be checked off. A launched soak is not a
completed soak, and a usable trainer is not demonstrated allocator benefit.

## Observability questions

1. What changed, under which policy/evidence/candidate identity, and why?
2. What verified demand did that change satisfy, and at what bounded cost?
3. If progress stopped, was it policy, source, evaluation, budget or recovery?
4. Can the recorded activation/rollback and elapsed acceptance be independently
   reconciled to the actual native daemon and persistent state?

Use bounded structured events with job IDs and fixed reason codes; no secrets
or raw unstructured user content. Monitor queue age, attempt/promotion/refusal
counts, budget saturation, child duration, verification failures and heartbeat
gaps. Exercise failure telemetry in private tests before relying on it.

## Review checkpoints

Actual failing tests precede production behavior. Main owns shared Make and
supervisor integration. Independent agents own table evidence, allocator and
worker-boundary slices; changes to shared contracts are coordinated first.
Author-separated adversarial reviews target small artifacts and classify real
findings. Optional external review is offered before invocation; no credentials,
private datasets or unrelated work may be sent. Missing skill reference files
are handled with inline skill checklists and the project gates, not invented
instructions.

## Implementation evidence: authority primitives

The managed private-file boundary retains directory descriptors, refuses links
and non-private files, and publishes exclusively with file/directory fsync.
SQLite is different: its VFS canonicalizes descriptor paths, so renaming or
replacing a live ledger root explicitly refuses. Reopening a closed ledger at
its new private path retains accounting. Metadata checks must not open/close
the database inode: an actual regression demonstrated that doing so drops other
connections' POSIX locks in this process. Validation uses descriptor-relative
`statx` instead. This is an owner-trusted boundary, not malicious-owner isolation.

The native-child runner clears the environment, uses literal argument vectors,
bounds both raw output pipes, and enforces suspend-inclusive deadlines. Actual
tests verify direct-child kill/reap on timeout, overflow and cancellation. An
adversarial cancellation-before-launch regression is fixed. The runner requires
attested fixed native executables that cannot fork; it is not an arbitrary
program sandbox or a guarantee of cleanup for reparented descendants. The fixed
native installation is attested; the managed deployment closure now has a
separate exact-byte identity and typed running-provider binding.

The separately implemented table reference checks all possible uint8 inputs,
including abstention as distinct from a verified zero. The strict native-control
codec preserves exact revisions, digests and request tokens; a successful
status with uncertain durability is not approval. These tested primitives do
not by themselves demonstrate an unattended learning loop.

The operation ledger now reserves exact native STAGE/ACTIVATE/ROLLBACK/DISCARD
requests before sending. One pending intent and one probation candidate exclude
competing promotion work. Activation and rollback recovery require exact native
token replay; STATUS alone cannot acknowledge them. Lost volatile stages fail
their jobs while preserving all charges. Unexpected identities and uncertain
durability pause. Probation starts from the earliest possible publication time
(the persisted promotion reservation), not the late reconciliation time. Missed
monitoring or a boot change requires rollback. An already prepared but uncertain
native rollback stage is conservatively frozen for owner recovery.

The independent receipt factory, not raw hash strings, is the ledger's evaluation
input. It binds dataset, source and actual staged snapshot; caller-side child
success/freshness checks remain necessary. SQLite structural checks include
foreign-key validation and timestamp/accounting constraints: physical
`quick_check` alone did not detect a missing epoch that could release charges.
Calls on each ledger connection must be serialized; separate processes or
separate connections share SQLite's transactional budgets.

Private native integration passes acquisition, complete-domain observations,
same-process refresh, stale refusal, retained incumbent answers and durable
rollback/restart (`make learning_daemon`). That Python test is a coordinator,
not an unattended supervisor. A separate managed integration now exercises the
actual learning cycle, as described below; neither test qualifies elapsed time.

## Implementation evidence: managed learning cycle

The internal managed supervisor now consumes normalized native table misses,
reserves durable jobs, runs the confined builder, copies the complete active
inventory, freezes it using the existing canonical snapshot code, stages with
preserve=1, compares all 256 native observations against the independent source,
and records an exact native activation. Actual resident probes govern probation
and revision-checked rollback. One owner lock and one serialized ledger lane
exclude competing supervisor instances and overlapping calls.

Its work directory contains the ledger, `data`, `jobs`, `sets`, `frozen`, and
native `state`; private daemon IPC and installed code remain outside measured
work storage. The fixed builder requires 50 MiB of free logical-byte headroom
(three files each limited to 16 MiB, plus bookkeeping). Preparation reserves
space for three complete-set copies. Metadata-only preflight/post-reap checks
retain and count failed artifacts without disturbing SQLite locks. This is not
a kernel disk quota or a guarantee against arbitrary compromised native code.
Small policies may validly parse but refuse acquisition for insufficient space.

Two reproduced recovery defects are fixed. A known failed resident probe is
persisted before a subsequent control request can fail or be cancelled. An
unsent activation whose evidence became stale is refused and discarded without
publication, retaining its promotion charge. An already-published activation
still requires exact native token replay before monitoring or rollback.

The fresh ledger pins SQLite `user_version=2`. Unversioned experimental databases
and unsupported versions refuse; there is no implicit migration or reset.
Opening WAL state refuses without converting its journal. Missing, wrong-kind,
or out-of-epoch promotion charges refuse before budget writes or epoch advance.
This detects the tested logical corruption, not arbitrary malicious-owner edits.

Native worker bytes and the exact manifest are pinned across ledger restarts.
The managed supervisor/evaluator and packaged dependency closure, operator CLI,
and durable run/heartbeat are implemented: the final combined regression passes
628 cases with zero failures/skips. Review-driven fixes include thread-bound
native-child lifetime, stop during cleanup and SQLite-serialized quota scans.
Separate-process tests load the actual private entry assembly and packaged SQLite.
Restart retains the original run budget; missed gaps and owner cancellation do
not qualify as completion. See `docs/AUTONOMOUS_LEARNING.md` for the owner contract.
The learned allocator remains inactive after its unchanged useful-gain gate
failed. A fresh development sequence pilot now demonstrates useful deterministic
multi-step planning: mean verified coverage .423828125 versus .1142578125 for
rotating order. The bounded exact control proves all 16 development optima,
leaving zero residual headroom for a learned improvement on this workload.
The pilot is not integrated production planning, not a held-out gain pass, and
does not justify another GPU fit. See
`result/cnet_allocator_sequence_pilot_20260907.md` for conditional rejection
phase ordering, native receipts and limits. Fresh residual-headroom evidence,
a new confirmation split and actual 72-hour acceptance remain outstanding.
Current deployment regression details and exclusions are recorded in
`result/cnet_autonomous_learning_20260906.md`.
