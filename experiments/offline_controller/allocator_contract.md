# Allocator objective version 1

This objective ranks independent ready-to-run learning tasks. It is separate
from graph-serving CORE selection and does not certify or activate capsules.
The existing CNETCEL1 404-byte checkpoint stores objective 2 at byte offset 12;
objective 1 remains the graph model. Feature version is 1 at offset 16. Existing
graph load/save functions accept only objective 1. No native C structure is a
portable checkpoint.

Outcome input begins `CNET_ALLOCATOR_ROWS_V1` followed by a newline. Each subsequent
line contains exactly 15 tab-separated columns and ends with a newline:

```
episode task family cursor population jobs work wait_limit f0 f1 f2 cost age coverage_hex receipt_sha256
```

`CNET_ALLOCATOR_TASKS_V1` has exactly the first 13 columns. This is the ONLY format
accepted by `choose`; outcome masks and receipts cannot enter the policy API.

All integers are unsigned decimal, except the lowercase hexadecimal coverage
mask. Receipt identity is 64 lowercase SHA256 characters and cannot be all zero.
Inputs must be owner-private regular files, one link, at most 1 MiB. No symlinks,
embedded NUL, trailing extra fields, nonfinite features or unterminated last rows
are accepted. Maximum 2048 rows, 256 episodes, 32 tasks per episode. Episode IDs
are positive and strictly increasing across groups; task IDs are positive and
unique inside a group. Tasks are in the incumbent's actual queue enumeration
order. All metadata is identical within a group. Cursor is the incumbent's next
index. Population is 1..64, jobs 1..8, work 1..2048, wait_limit 1..65535, cost
1..work and age 0..65535. Features are finite numbers in [0,1].

Feature meanings are fixed before outcome generation:

- f0: fraction of CURRENT observed unmet requests whose approved tool plan
  touches this task. This is potential relevance, not outcome coverage.
- f1: fraction of the associated plan's required blocks already certified.
- f2: estimated missing evidence rows divided by 2048.

The outcome mask names independently tool-labelled FRESH evaluation probes
newly verified after that task alone is completed against the same incumbent
snapshot. Population is the number of those probes. It is not the f0 denominator.
The owner must establish that unioning individual masks is valid: no unrecorded
cross-task synergy or interference is supported by version 1. Failed admission
has an empty mask but still consumes cost. The receipt must bind the actual
isolated trial, policy/tool identities, incumbent, probe labels and task. A hash
is integrity, not authentication. A CNET answer may be a tested prediction, never
the target. These source and independence duties cannot be proven from a TSV.

Families are closed: 0 direct, 1 shared-prefix, 2 chain-completion, 3
evidence-rejection. They classify independent workload episodes, not rows.

The cell is fitted to popcount(mask)/population. Its online rank is prediction
divided by cost. Controls are the actual rotating cursor order, f0/cost, and
descending f1 then f0/cost. Every policy first chooses overdue tasks (age at least
wait_limit), oldest first. Equal ranks use cursor order. All policies use the
same job/work budgets and skip jobs that cannot fit remaining work. This is one
scheduling decision; the durable owner advances ages/cursor across decisions.

Frozen gain gate: at least 32 independent held-out episodes, at least four in
each family. Against EACH control require mean verified coverage gain >=0.05,
paired two-sided 95% lower bound >0, and no negative family mean. The lower bound
is mean minus 2.040 times sample standard deviation divided by sqrt(episodes);
2.040 conservatively bounds the t critical value for n>=32. Training/evaluation
episode and receipt identities must be disjoint. The owner must freeze evaluator,
comparators, feature definitions, checkpoint, budgets and fresh split custody
before examining outcomes; renamed copies are not independent episodes. Existing
serving, certificate, coverage and growth floors remain additional obligations.
After the first promotion the owner MUST supply the actual active allocator as
a fourth comparator. Require the SAME >=0.05 coverage gain, positive paired95
lower bound and every-family non-regression against it, in addition to all three
original controls. An unchanged or merely reshuffled incumbent therefore fails.

CLI (all paths supplied by the trusted owner; no shell is used):

```
allocator train WORKER_ABSOLUTE DEVICE ROWS_FILE OUTPUT_DIR NAME EVALUATION_SHA256 [INITIAL_NAME]
allocator evaluate CANDIDATE_DIR NAME TRAIN_ROWS EVAL_ROWS [ACTIVE_NAME]
allocator choose CANDIDATE_DIR NAME TASKS_FILE
```

DEVICE is 0 or 1. Training uses fixed 4096 epochs, learning rate 1, <=30-second
worker deadline and deterministic initialization unless INITIAL_NAME names an
allocator checkpoint in OUTPUT_DIR. The owner supplies the held-out evidence
digest without exposing its records to the worker. Checkpoint creation is
exclusive, owner-private and fsynced. Evaluate verifies both evidence hashes and
split disjointness. Exit 0 means completed training/choice or a passed evaluation;
exit 3 means a well-formed evaluation failed the unchanged gate; exit 2 means
refusal. Successful training explicitly reports approved=false. No command
activates a candidate or changes a serving registry.

The allocator_worker receives a same-build WorkerBatch through a sealed memfd.
Filesystem confinement precedes trusted HIP warmup on a fixed zero cell/row.
Network and process seals are then installed before the first supplied-data
read. Warmup outputs never become training evidence. Completion requires exact
IPC record/EOF/clean exit, finite results and CPU/GPU parity below 1e-6.

`allocator_worker_test` uses a supplied numerical continuum fixture to check
arbitrary-row fitting, both devices and immutable evaluation. Its output is
expressly NOT acquisition evidence or proof that the gain gate passes.

## Reproducible private builds and CPU-only repeats

From the repository root, create a fresh task-owned output directory:

```sh
CNET_ALLOCATOR_OUT=$(mktemp -d /tmp/cnet-allocator-build-XXXXXX)
make -C experiments/offline_controller OUT="$CNET_ALLOCATOR_OUT" allocator-build allocator-test
```

`allocator-build` compiles the CPU CLI and diagnostic chooser helper.
`allocator-test` runs the allocator policy/objective checks, supplied-row parser
tests, numerical gate algebra and legacy canonical checkpoint tests. Neither
target compiles HIP, links the serving library, rebuilds shared `bin`, launches
a worker, fits a model or activates anything. No ROCm installation is required
for these CPU targets. They do not change the existing default or product tests.
Direct binary targets also create a missing `OUT` directory, and source/header
dependencies trigger rebuilds without using directory modification times.

Read-only CLI repeats use `allocator choose ...` with the input-only TASKS
schema, or `allocator evaluate ...` with existing frozen checkpoint and evidence
files. Evaluate's exit 3 is an honest failed gate, not a build failure. Repeating
an exposed evaluation does not make it fresh confirmation. `train` is **not** a
CPU-only repeat: it launches the explicitly supplied AMD worker.

AMD paths are separately opt-in. Inspect their recipes without launching any
compiler or GPU job:

```sh
make -n -C experiments/offline_controller OUT="$CNET_ALLOCATOR_OUT" allocator-worker-build
make -n -C experiments/offline_controller OUT="$CNET_ALLOCATOR_OUT" allocator-worker-test
```

With separate authorization and an idle per-device reservation, removing `-n`
from `allocator-worker-build` compiles for the existing `gfx1201` target using
`HIPCC` (default `hipcc`) without running training. Removing `-n` from
`allocator-worker-test` actually runs the numerical fixture: at most two workers,
one per device 0/1, bounded train then frozen evaluation. Its result remains
numerical correctness/parity evidence, not independently acquired gain. Keep
this execution target out of CPU CI and do not run it concurrently with other
jobs holding those devices. Preserve or explicitly clean only the exact
task-owned output directory; do not reuse a broad repository or home directory.

## Experiment-only worker lifetime and authority limits

This existing C worker pool is not the managed production `LearningChild` plus
CPU acquisition guard. It uses `CLOCK_MONOTONIC`, so its cooperative deadline
excludes suspend time; it has no parent-death binding. It also inherits stderr
without validating whether that descriptor grants writable regular-file
authority. Filesystem seals do not revoke already-open writable descriptors.
The pool therefore does not establish production parent-lifetime, BOOTTIME or
stderr-authority guarantees. Owner termination or a blocked owner is not a
demonstrated immediate worker-stop boundary. These are documented limits, not
runtime fixes. Production policy continues to refuse `allocator_enabled`.
