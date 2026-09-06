# Executable capsule core

The core now serves **Artificial Specialized Intelligence** through the existing
CNB/capsule authority. It is a working typed execution and external-teaching
path, not a claim of LLM-like breadth. A constrained language adapter proposes
typed intent; the existing scheduler ingests bounded, externally supplied
evidence jobs. An opt-in demand worker now acquires finite labels from
operator-approved integer tools. Free-form interpretation and discovery of
teachers for unknown domains remain outside this implementation.

## Run the complete proof

```sh
make capsule_frontdoor
make capsule_curriculum capsule_finite_compile capsule_publication_interfaces
make capsule_demand_growth capsule_demand_security capsule_acquire_security
make capsule_value_search capsule_history capsule_history_coverage capsule_core_budget
make capsule_tool_plan capsule_composed_acquire capsule_composed_refusal
make capsule_capacity capsule_socket_bench_contract
make checkpoint_binding checkpoint_durability capsule_publish_recovery
make authority
make verify
make capability_cert
```

Run the last two sequentially, with the source tree stable: evidence receipts
correctly refuse when their source/worktree changes during evaluation.
`capsule_frontdoor` uses an isolated Unix socket and temporary directories;
it does not restart the deployed daemon or change its base.

The socket proof starts with an unknown minutes-to-seconds capability, teaches
six external arithmetic-tool rows, and tests all six against independently
computed expected answers. A second capsule composes seconds into metered
credits. A missing intermediate input refuses. A separately taught version
covers that input without replacing the old capsule; prior answers still pass.
The test also exercises process restart, malformed inputs and corrupt imports.
These are finite-domain tests, not evidence for unseen arithmetic or broad
language competence.

## Teach and use your own transform

```sh
make capsule_core
# Produce external evidence, never copy CNET's certified answers into this file.
for x in 0 1 2 3 4 5; do printf '%d\t%d\n' "$x" "$((x * 60))"; done > minutes.tsv
bin/cnet_capsule_core teach ./capsules minute_conversion minutes seconds \
  3 9 verified_tool minutes.tsv
bin/cnet_capsule_core ask ./capsules 'capsule minutes seconds 3'
# LOCAL verified=1 value=180 hops=1 units=minute_conversion
bin/cnet_capsule_core ask ./capsules 'capsule minutes seconds 6'
# ABSTAIN verified=0 ... (outside this capsule's certified coverage)
```

Rows are two unsigned decimal integers separated by whitespace, with no header,
comments or duplicate inputs. Training accepts at most 256 rows and 1–16 bits
per side. A proposal must pass every supplied contract row and a 0.05 robustness
margin. `user_correction` and `verified_tool` identify trusted **local** ingestion
sources; these labels and capsule checksums are not authentication. Do not expose
the teaching command to untrusted network clients or relabel model answers as
tool evidence.

The command fits a native BTN (small domains) or compiles an exact finite-domain
BTN (up to 256 supplied rows). The receipt names `gradient_fit` or
`finite_domain_compile`; neither establishes accuracy on unseen inputs.
The compiler uses one binary-equality hidden neuron per row inside the existing
BTN format. Both methods must pass the same certification floors.
It verifies the BTN, attaches exact coverage and
provenance, exports the existing `unit.cnb` + `manifest.cknow` format, verifies
import, then atomically publishes an immutable directory without overwriting
an installed unit. Publication is serialized and checks the proposed complete
inventory before admission. Conflicting contracts and ambiguous per-direction
tag/signature pairs refuse. Identical artifact retries succeed with `reused=1`;
different content under an installed name refuses. Improvements use a new
versioned unit name and externally verified rows; they do not mutate old weights.
Every publication also replays guarded exact joins of sealed contract labels in
both the old and proposed inventories. A new direct or composed path cannot
contradict those obligations, even if an optional evaluation file omits them.
Historical labels are evaluation obligations, never self-generated training
data. `label_obligations` counts replayed joins, not unique tasks.

Set `CNET_CAPSULES_DIR` for `cnetd` to enable the same path over its existing
socket. Send `{"q":"capsule minutes seconds 3"}`. Only successful certified
execution returns `source=LOCAL, verified=true`. Explicit capsule refusals do
not fall through to notes, residual prose, or legacy self-learning. The daemon
also accepts whole-request forms `convert N INPUT_TAG to OUTPUT_TAG` and
`how many OUTPUT_TAG in N INPUT_TAG?`. Grammar keywords ignore case and repeated
whitespace; tags must match exactly. Recognized
ambiguous or malformed conversion requests ask for clarification, unverified.
This is a constrained parser, not a free-form semantic model.
The daemon currently reloads capsule files
for each capsule request, so edits/corruption cannot retain stale authority.
The CLI includes the executed unit chain in its receipt.

## Scheduled evidence ingestion

Set `CNET_CAPSULE_QUEUE` and `CNET_CAPSULES_DIR` in the existing autoteach
environment. Each immediate job directory contains three regular files:

| File | Contents |
|---|---|
| `request.tsv` | One line: `UNIT INPUT_TAG OUTPUT_TAG INPUT_BITS OUTPUT_BITS verified_tool` (or `user_correction`) |
| `training.tsv` | External `INPUT EXPECTED` pairs, as above |
| `evaluation.tsv` | Independent expected cases: `INPUT_TAG OUTPUT_TAG INPUT EXPECTED`; include previously covered tasks as well as new tasks |

Run `bash scripts/cnet_capsule_curriculum_tick.sh` to process a tick manually.
The existing autoteach timer calls the same script. Each tick snapshots evidence,
hashes that snapshot, and allows at most two teaching attempts by default
(`CNET_CAPSULE_JOBS_PER_TICK`, 1–8), each with a 120-second timeout. A persistent
round-robin cursor and scan budget (`CNET_CAPSULE_SCAN_PER_TICK`, default 64,
maximum 256) prevent a failing prefix from starving later jobs. Keep this trusted
local queue below 4,096 entries; enumeration still visits directory names.

Before publication, every evaluation case must be correct with zero wrong
certified answers and strictly greater correct coverage than the installed
inventory. Exact retries may retain equal coverage. Success atomically replaces
the job's `receipt.txt`, binding it to the evidence SHA-256; failures write
`refusal.txt` and do not publish the candidate. Training/evaluation each allow
at most 256 cases. Evidence files are bounded to 64 KiB. These are local trusted
operator inputs, not an authenticated upload API. No teacher calls are made by
this ingestion path; evidence production elsewhere has its own cost.

The scheduler does not infer correct labels from misses or consume CNET's own
answers. Without the opt-in configuration below, a miss creates no job.
Capsule checksums and provenance labels do not authenticate operator evidence.

## Opt-in demand acquisition

Build `make capsule_core capsule_tool cnetd`. Configure these paths in the
daemon and existing autoteach environment (absolute paths recommended):

```sh
export CNET_CAPSULES_DIR=/your/private/capsules
export CNET_CAPSULE_QUEUE=/your/private/curriculum
export CNET_CAPSULE_DEMAND_DIR=/your/private/demand
export CNET_CAPSULE_TOOL_POLICY=/your/private/tool-policy.tsv
```

Create the three directories as the service user with mode 700. Copy
`config/capsule_tools.example.tsv` to the policy path and set mode 600. These are
trusted local paths, not a remote upload API; do not enable this against a
shared writable parent. The example does not change the deployed configuration.

An uncovered `convert 12 bytes to bits` remains **unverified** and returns
`demand=queued` (or `existing` for the same normalized pending intent). Run
`bash scripts/cnet_capsule_acquire_tick.sh`, or let the existing autoteach tick
run it. The next request returns verified `96` after successful admission.
`queued` means demand recorded, not acquisition or certification completed.

Policy columns are `INPUT_TAG OUTPUT_TAG INPUT_BITS OUTPUT_BITS OP OPERAND MIN
MAX`. Supported operations are integer `mul` and bitwise `xor`, with unsigned
16-bit operands/results and 1–16-bit typed ports. Duplicate domain policies,
unknown operations, invalid ranges, unsafe permissions and overflow refuse.
Existing tag near-miss, signature and certification rules still apply.

The separate native `cnet_capsule_tool` executable computes every expected row
without a BTN or registry. Each acquired block has at most 32 inputs, aligned
on a multiple of 32 and intersected with policy bounds. Training and evaluation
cover that same finite block: this proves exact acquisition and serving, not
held-out generalization. Policy/tool SHA-256 identities accompany the existing
curriculum job; they identify local evidence production, not remote trust.

Each tick scans at most 64 pending requests and publishes at most two new jobs;
there are at most 256 pending demands and 4,096 visible curriculum entries.
Duplicate intents and deterministic identical jobs are idempotent. The worker
removes a demand only after durable job publication or a logged terminal policy
refusal. Unknown/out-of-policy demands release capacity; retry after changing
policy. Tool failures and full queues retain demand; failed certification
retains the curriculum job and its refusal receipt. Existing curriculum limits
bound teaching attempts. Tool call counts are reported separately from serving,
which makes no teacher calls. Inspect acquisition logs and job receipts to
distinguish queued, terminally refused and certified work.

### Discovering approved multi-step paths

Requests no longer need a direct policy pair. The same worker discovers paths
through existing approved tools, using actual computed values and exact binary
port widths. For example, a policy may contain:

```text
minutes seconds 5 11 mul 60 0 31
seconds frames_24fps 11 16 mul 24 0 1860
```

An uncovered `convert 3 minutes to frames_24fps` can acquire the two primitive
capsules and then return verified `4320`, without creating a minutes-to-frames
capsule. Only covered intermediate values work; other requests may acquire
additional blocks. Approved tools are discovered locally, not invented or
downloaded from an unknown teacher.

Inspect a proposed path with
`bin/cnet_capsule_tool plan PRIVATE_POLICY INPUT_TAG OUTPUT_TAG VALUE`. It emits
one row per hop only on success: `INPUT OUTPUT IB OB OP OPERAND MIN MAX X Y`.
`MIN/MAX` intersect operator policy with the tool's representable output range,
so neighboring block overflows cannot prevent acquiring a valid requested value.
The complete policy is validated before any path is emitted; duplicate pairs
and inconsistent tag widths refuse.

The planner supports eight hops, 1,024 value states and 65,536 charged
edge/visited comparisons per request. Exit 3 means no approved path within
that supported hop limit (terminal demand refusal). Invalid policy, arithmetic
failure or work/state exhaustion returns 2 and preserves demand. Exit 0 is a
proposal for acquisition, not certification or permission to answer.

A path longer than two edges progresses over multiple ticks. Exact existing
jobs consume no new-publication allowance; the original demand remains until
all edge jobs are durable. Up to 2,048 evidence-tool invocations, including
failed calls and existing-job checks, are allowed per tick. Native planning
oracle calls and charged work are reported separately. Curriculum certification
can still refuse an individual job; serving remains guarded and unverified
until the complete required chain is certified and covered.

`capsule_composed_acquire` tests three composed task pairs absent from all
primitive training jobs: 96/96 covered socket cases after targeted suffix
expansion, prior-task preservation, fresh-process restart and a portable-root
query. This is held-out **task-pair composition of covered primitive inputs**,
not accuracy on unseen primitive inputs or unrelated domains. Work-limit and
planner-output failure tests accompany the positive path.

`capsule_demand_growth` measures 32/32 newly covered socket answers, a second
block while preserving the first, a second XOR domain, and process restart.
Security gates cover normalization, concurrent enqueue, capacity recovery,
queue saturation, overflow, malformed/symlink inputs and unsafe policy files.

## Boundary and limits

- Exact typed grammar: `capsule INPUT_TAG OUTPUT_TAG UNSIGNED_INTEGER`.
- Planning uses bounded breadth-first `(typed port, actual value)` states and
  the existing strict executor, up to eight hops. Coverage is checked on the
  actual value at every hop; an uncovered value cannot blacklist a shared
  suffix reached later with a covered value.
- Search allows at most 1,024 states and 65,536 charged scans/comparisons.
  Growth replay keeps the same per-search ceiling. Its aggregate work is
  `2,000,000 * max(1, ceil(max(old_count, new_count)/256))`, capped at 32 million,
  with a cooperative deadline of two seconds per 256 capsules (maximum 32
  seconds). Coverage-label work scales by the same count formula; contract
  consistency retains its separate two-million-operation ceiling. Queries,
  coverage-label and direct-contract checks retain two-second phase deadlines.
  These are not hard real-time limits; loading/certification is separate.
  Each selected kernel is audited through the canonical certificate auditor
  before execution, at every hop, including historical replay.
- Serving supports one-field binary ports up to 16 bits and one-hot ports up to
  64 symbols, at most 256 hidden neurons and 4,096 sealed rows per kernel.
  Actual guard rows must have sealed labels. Multi-slot/asset-dependent
  execution refuses here.
- At most 4,096 immediate capsule directories; publication staging directories
  start with a dot and are not served. Keep unrelated files out of this root.
- The library is single-threaded. The current tools target Linux; atomic
  no-clobber publication uses `renameat2`. No GPU backend is required.
- Larger inventories, free-form intent fidelity, performance/cost at scale,
  unknown-domain evidence acquisition and cross-domain competence remain WITHHELD.
- Atomic publication prevents observers seeing a half-published directory;
  complete power-loss durability across all directory metadata is not proven.

## Deployed learner

`bash scripts/check_deployed_learning.sh` inspects the running lane's effective
environment and executable generation instead of guessing `logs/personal.cnb`.
`bin/cnet_capsule_core inspect BASE` identifies mined units without editing them.
`bin/cnet_revalidate_coverage BASE UNIT URL WINDOW NEW_SIDECAR` can reacquire a
missing guard only if **every** sealed row matches a fresh external teacher
answer. It does not overwrite an existing sidecar or change the base.

The approved September 5 deployment preserved the original base and moved to
`artifacts/deployments/certified-core-20260905/active.cnb`: 1,095 retained units,
with the unguarded `hyb_struct_bonsai` isolated in `quarantined.cnb`. Its fresh
external revalidation had refused. `original.cnb` preserves the full source.
`cnet_quarantine_base` verifies exact retained/quarantined sealed payloads after
reload. The active base initially has no mined units, so its explicit empty
coverage store grants no new authority. Deployed health passes with coverage
enabled, zero unguarded mined units and the residual teacher reachable.

`config/certified-core-deployment.env` is loaded by user-systemd
`zzz-certified-core.conf` drop-ins for the learner, daemon, shared MCP service
and relevant maintenance jobs. The legacy miner now seals actual recorded
teacher rows and saves coverage before checkpointing. Scheduled legacy writers
pause and resume the same-base learner under maintenance/writer locks; failed
jobs also restore it. This scopes coordination to the configured maintenance
entrypoints, not every possible direct CNB writer in the repository.

The live capsule root is `artifacts/certified_capsules`. Its two seeded jobs in
`artifacts/capsule_curriculum` have these exact domains:

| Capsule | Certified domain | Transform |
|---|---|---|
| `time_seconds_v1` | All 5-bit minute values, 0–31 | `seconds = minutes * 60` |
| `video_frames_24fps_v1` | 32 second values, 0, 60, …, 1860 | `frames_24fps = seconds * 24` |

The first job improved its 32-case evaluation from 0 to 32 correct. Adding the
second improved the combined direct/composed suite from 32/64 to 64/64, with
zero wrong certified answers. A separate deployed-socket run checked all 64
cases plus three refusal/clarification cases. For example,
`convert 3 minutes to frames_24fps` returns verified `4320`; before acquisition
was enabled, `capsule seconds frames_24fps 1` refused. These results cover only
the stated finite domains.

### Live acquisition enabled September 6

The deployment environment now enables `artifacts/capsule_demand` and the
owner-private `config/capsule_tools.live.tsv` policy. Approved tools are
bytes → bits, u8 → masked8 (XOR 255), minutes → seconds, and seconds →
frames_24fps. Policy bounds authorize evidence acquisition, not immediate
answer coverage: a miss still abstains while its demand is queued, and only
certified evidence can expand coverage.

The existing `cnet-autoteach.timer` remains enabled (20-minute interval,
2-minute scheduling accuracy); each tick permits at most two new jobs. Two
explicit starts of its real service acquired three capsules from 96 verified
tool calls. After a daemon restart, 160 finite-domain socket checks passed:
64 byte/XOR checks, 64 original time/composition checks and 32 second/frame
checks. The last group includes 31 newly covered inputs and one prior input.
For example, `capsule seconds frames_24fps 1` now returns verified `24`.
Out-of-domain refusal and fractional clarification remain enforced.

See [activation evidence and rollback](../result/cnet_live_acquisition_20260906.md).
These are finite approved-tool domains, not arbitrary autonomous learning;
unmeasured broader capability remains WITHHELD.

The later remaining-sequence deployment adds durable publication, 256-capsule
capacity and expanded constrained intent forms. Its live acquisition check
expanded bytes → bits to inputs 0–63: 192 covered socket cases now pass across
the stated domains, with all preexisting capsules byte-identical. See
[current deployment evidence](../result/cnet_remaining_sequence_20260906.md).

## Inventory evaluation

`make capsule_scale_contract` checks the benchmark's result and failure
contracts on two and four capsules; it is included in the default authority
gate. `make capsule_scale_bench` attempts a separate opt-in sweep of 2, 16, 64
and 256 capsules. The September 6 remaining-sequence build passes the full
256-capsule sweep. The subsequent capacity change raises the limit to 4,096;
private coverage banks retain the public 64-record ABI. Typed-input and
coverage-owner indices avoid unrelated scans, and direct contract comparison
sorts auxiliary pointers without changing route tie order. Replay budgets now
scale with count as documented above. The earlier 65th- and 257th-publication
failures remain historical evidence, not the current ceiling.

For an explicitly bounded passing sweep:

```sh
CNET_SCALE_COUNTS='2 16 64' CNET_SCALE_REPEATS=3 make capsule_scale_bench
```

The runner retains private `/tmp/cnet-scale-bench-*` stores and evidence logs;
stdout contains JSON Lines (Make may also print rebuild commands), while
stderr reports progress and the artifact path. `results.jsonl` is the retained
report. Each unit is published through ordinary certification and historical
replay using 32 independent tool labels. Per two-unit chain, all 32 composed
queries are evaluated without supplying direct composed training rows. The
probe reports wrong verified answers, missed covered answers, OOD/unknown
refusals, monotonic load/resident/reload timing, supplied rows, acquisition
calls, and cumulative publication time. It exits nonzero on a failed gate.

`CNET_SCALE_COUNTS` must be strictly increasing even counts in 2–4,096;
`CNET_SCALE_REPEATS` is 1–20. Defaults are `2 16 64 256` and `3`.
The internal probe also offers `--time COMMAND [ARGS...]` for monotonic
subprocess timing: elapsed seconds go to stdout, child output to stderr,
and child failure remains nonzero. It does not interpret a shell command string.

`make capsule_large_inventory_bench` is a separate opt-in 4,096-capsule gate
(`CNET_LARGE_CAPSULES=258` selects a smaller boundary check). It canonically
exports renamed copies of two externally labelled XOR seed capsules, verifies
all covered direct/composed queries and sampled OOD refusals, asserts all
`96 * capsule_count` historical replay obligations, refuses capsule 4,097,
and checks ordinary final publication from N-1 to N. This proves synthetic
inventory capacity and final-boundary publication, not 4,096 independently
acquired domains or 4,096 sequential growing publications. It reports heap
deltas and timings; heap delta is not a universal per-capsule RAM requirement.

Explicit resident hot-swapping and a configurable RAM admission budget are
still separate pending work. The daemon currently reloads the directory per
request; higher library capacity does not remove that cost.

This is a synthetic, sequential CPU workload over distinct XOR chains, not
broad-domain accuracy, a concurrency test, a portable transfer test, or socket
latency. Load measurements may benefit from filesystem caches. Monetary and
energy costs are explicitly `null`, not zero. See
[measured inventory results](../result/cnet_inventory_evaluation_20260906.md).

For actual socket measurements against that synthetic inventory:

```sh
CNET_SOCKET_REQUESTS=128 CNET_SOCKET_CLIENTS='1 4 16' \
  bash scripts/cnet_capsule_socket_bench.sh /path/to/benchmark/capsules 128
```

The second argument is the number of two-capsule chains, not capsule count.
This starts a private daemon and never connects to the live socket. Timing
includes client-process startup, socket round trip, and queueing; it excludes
JSON validation. The daemon remains serial. Neither this measurement nor the
resident probe establishes a production latency SLA. See
[remaining-sequence evidence](../result/cnet_remaining_sequence_20260906.md).

The constrained intent adapter additionally accepts `please convert 3 minutes
into seconds`, `what is 3 minutes in seconds?`, `3 minutes in seconds`, and
`how many seconds are in 3 minutes?`. Tags must still match exactly; fractions,
signed quantities and trailing alternative instructions clarify/refuse.

## Recovery

On startup, mined-unit coverage must bind both the exact interface and a
byte-exact subset of the sealed contract inputs. A wider or mismatched sidecar
loses its guard and arms fail-closed; a narrower/reordered valid sidecar remains
usable. Stale-record removal preserves entries moved during compaction.

Coverage sidecars use private exclusive temporary files, file sync, atomic
rename and parent sync. Capsule manifests sync before rename; publication
syncs the capsule directory and its parent, including identical retries.
A sync failure after rename reports failure but retains the complete certified
artifact: retry the identical evidence to repair durability, not overwrite it.
Linux fault-injection gates cover these boundaries. They are not physical
power-cut tests, a multi-file transaction across arbitrary writers, Windows
durability certification, or protection from a malicious owning OS account.
Legacy base writers still require the existing exclusive writer coordination.

Do not overwrite the active base with an older snapshot while writers run.
Pause the affected timers/services first and preserve the current active base
and sidecars, including any newly learned evidence. The new deployment is
selected by the environment file and the named systemd drop-ins; changing that
selection requires `systemctl --user daemon-reload` and restarting affected
services, followed by `check_deployed_learning.sh` and socket checks.

The original source and `original.cnb` had SHA-256
`841ef0790da8d9fb08da307132ab26a7fdbfb4f2f5d94aff7b1cf776d4fa5eda`
at migration. `quarantined.cnb` had SHA-256
`ec7cf2cc637f22328392e6d3d91754ea26ecc7d998858eb9c5e8db44bd541b38`.
These snapshots are recoverable data, not permission to restore the unguarded
unit to certified service. Reacquire valid external evidence and revalidate it
before re-admission; never disable coverage to make restoration pass.

Decision and remaining sequence: [authority and growth plan](../plans/cnet_authority_and_growth_20260905.md).
