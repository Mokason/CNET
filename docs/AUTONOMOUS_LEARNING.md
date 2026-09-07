# Private, policy-bounded table learning

This opt-in control plane learns **authorized numeric tables**, not arbitrary
text or self-certified facts. A real uncovered request records demand. Fixed
confined workers build a capsule; an independent evaluator checks all 256 input
keys, including every required abstention. Exact persisted native operations
govern staging, activation, probation and rollback. CNET answers never become
training labels.

The learned allocator remains inactive after its failed improvement gate.
`allocator_enabled=true` refuses. Completing a run budget does not certify
learning gain or 72-hour acceptance. See the [plan](../plans/cnet_autonomous_learning_20260906.md)
and [evidence](../result/cnet_autonomous_learning_20260906.md).

## Installation contract

Linux x86-64, non-root owner, trusted .NET 8 host/framework and kernel are
required. Build the native artifacts with `make learning_native`. Publish the
locked managed application without an apphost, debug files or RID override:

```sh
dotnet publish dotnet/CnetControlPlane/CnetControlPlane.csproj \
  --self-contained false -p:UseAppHost=false -p:DebugType=None \
  -p:DebugSymbols=false -p:RestoreLockedMode=true \
  --output /absolute/private-build/publish
```

The lock file is framework-dependent; adding `-r linux-x64` requires a different
restore graph and is not part of this deployment. Do not unlock dependencies to
make installation pass. Provision a new private owner directory with this exact
code inventory (other published RID assets are not installed):

```text
DEPLOYMENT/                              owner 0700
  managed.json                           owner 0400 or 0600
  runtime.json                           owner 0400 or 0600
  policy.json                            owner 0400 or 0600
  managed/                               all directories owner 0700
    cnet-control.dll                     all eight files owner 0400
    cnet-control.deps.json
    cnet-control.runtimeconfig.json
    Microsoft.Data.Sqlite.dll
    SQLitePCLRaw.core.dll
    SQLitePCLRaw.batteries_v2.dll
    SQLitePCLRaw.provider.e_sqlite3.dll
    runtimes/linux-x64/native/libe_sqlite3.so
  native/                                owner 0700
    cnet_table_capsule                    five executables owner 0500
    cnet_table_verify
    cnet_learning_snapshot
    cnet_capsulectl
    cnetd
    libcnet_capsule_core.so               owner 0400
```

Copy native files from `bin/` and managed files from the publish directory.
All code files must be regular, single-link, non-symlinks: each at most 64 MiB,
each installation at most 128 MiB. Do not install by hard-linking build outputs.
Canonical absolute paths and trusted ancestors are required; IPC paths must
fit the 107-byte native socket limit.

`managed.json` has exactly `schema_version: 1`, `target: "linux-x64"`, and
`files`: a JSON object mapping each of the eight relative file names above to
its lowercase 64-character SHA256. `runtime.json` has exactly
`schema_version: 1` and `files`, with the six native names and their hashes.
Each manifest is at most 4096 bytes. The ledger pins the **exact manifest bytes**,
not just the member hashes. A hash does not authenticate the builder: the owner
must approve the installation being hashed.

Start from [the disabled policy example](../config/autonomous_learning.example.json).
Choose an explicit short run duration and authorized datasets before enabling
it; the example's 259200 seconds is three days and is **not** a default smoke
test. Policy bytes are immutable after initialization. Never change live DLLs,
native code, manifests or policy. Managed assembly locations/load contexts are
checked; this is not retrospective in-memory code attestation. The trusted host,
framework, libc and prior startup hooks are outside the manifest boundary.
Launch in a clean environment before any SQLite use. The packaged SQLite import
is bound to the verified native inode; an existing provider resolver refuses.

## Owner commands

With `LEARNING_DEPLOYMENT` set to that canonical private directory and
`LEARNING_DOTNET` set to the trusted absolute dotnet host, invoke:

```sh
env -i "$LEARNING_DOTNET" "$LEARNING_DEPLOYMENT/managed/cnet-control.dll" \
  learning inspect "$LEARNING_DEPLOYMENT"
env -i "$LEARNING_DOTNET" "$LEARNING_DEPLOYMENT/managed/cnet-control.dll" \
  learning initialize "$LEARNING_DEPLOYMENT"
```

`inspect` verifies the running installation and exercises the packaged SQLite
provider without creating work. `initialize` exclusively creates `work/`,
`ipc/`, the fresh schema-2 ledger, and work subdirectories. It never overwrites,
repairs or migrates an existing deployment. Failure can leave partial artifacts;
retain them for diagnosis. Later commands require both original installation
pins; a missing pin refuses and pauses instead of silently rebinding.

All commands use the same clean launch prefix above:

| Arguments after `learning` | Effect |
| --- | --- |
| `status DEPLOYMENT` | Structured pin, pause, job, pending-operation and durable-run status. Does not assert daemon health. |
| `ask DEPLOYMENT DATASET KEY` | Actual daemon answer/abstention; records normalized demand, never answer-as-label. KEY is canonical decimal 0..255. |
| `tick DEPLOYMENT` | One serialized recovery/probe/acquisition cycle. Requires the owner lock. |
| `pause DEPLOYMENT` | Persistently prevents new work; no healthy daemon required. A running owner observes it and performs safety cleanup. |
| `resume DEPLOYMENT` | Requires no running owner, exact healthy native state and no pending/required rollback. Cannot restart a terminal run. |
| `run DEPLOYMENT` | Supervise until the original persisted budget expires or a failure/owner stop occurs. |
| `quiesce DEPLOYMENT` | With no active owner, persist stop/pause and retry cleanup only; never replay an unpublished activation or acquire demand. Exit 0 means settled cleanup, not a successful run. |

Only requests through `learning ask` create this ledger's demand; direct daemon
requests are not automatically ingested. A standalone `tick` does not refresh a
durable run heartbeat. Status uses a transactional ledger open, so observed
clock epochs and detected clock failures can be persisted; it is not a forensic
read-only database viewer.

## Private daemon and independent evidence

The owner launches a **separate private daemon**; these commands never restart
or reconfigure a service. Keep its IPC and packs outside measured `work/`.
Create an owner-private `packs/ROUTES.jsonl` appropriate to the private instance.
The integration fixture uses one inert route
`{"pattern":"fixture","pack":"fixture"}`. The working directory must also be
the private deployment: the daemon reads some relative `config/` paths.
A cleared environment alone does not isolate those reads. Launch with both:

```sh
cd -- "$LEARNING_DEPLOYMENT" && \
env -i \
  CNET_PACKS_ROOT="$LEARNING_DEPLOYMENT/packs" \
  CNET_MINIMAL_ROOT="$LEARNING_DEPLOYMENT" \
  CNET_SOCK="$LEARNING_DEPLOYMENT/ipc/ask.sock" \
  CNET_CAPSULE_CONTROL_SOCK="$LEARNING_DEPLOYMENT/ipc/control.sock" \
  CNET_CAPSULE_SETS_DIR="$LEARNING_DEPLOYMENT/work/sets" \
  CNET_CAPSULE_STATE_DIR="$LEARNING_DEPLOYMENT/work/state" \
  CNET_CAPSULE_DATA_ROOT="$LEARNING_DEPLOYMENT/work/data" \
  CNET_SELF_ANSWER=0 CNET_TEACHER_ON_MISS=0 CNET_CORE_AUTO_EVOLVE=0 \
  "$LEARNING_DEPLOYMENT/native/cnetd"
```

Place independently supplied canonical source files at
`work/data/DATASET.tsv`, owner 0400 or 0600, single-link regular files,
at most 4096 bytes. Example format (the last line uses a literal tab):

```text
CNET_LOCAL_TABLE_V1
dataset calibration
authority verified_tool
input_bits 8
output_bits 16
rows 1
7	42
```

The authority must match policy. All omitted keys abstain. Source identity
travels through admission and serving freshness checks. An owner source update
invalidates old answers until a fresh candidate passes; stale in-flight
evidence is discarded or rolled back, never silently relabelled.

## Stop, restart and limits

SIGINT/SIGTERM to the owned `run` process stops acquisition and allows bounded
safety cleanup. Unpublished activation is withdrawn; already published but
unaccepted candidates are rolled back after exact token reconciliation.
Unknown transport/native outcomes retain pending obligations and exit nonzero.
After restoring the original private daemon/control transport, retry
`learning quiesce DEPLOYMENT`. It preserves terminal history and charges; it
also works when the ask socket is unavailable. A competing owner lock refuses
before pause/stop changes: use `pause` first and wait for that owner to exit.
Do not substitute `tick` for stop recovery: a normal tick intentionally may
reconcile an already authorized pending activation.
Do not delete the ledger, refund charges, blindly resend mutations or manually
remove retained snapshots to manufacture recovery.

Same-boot restart of an interrupted **running** row preserves its original
deadline and heartbeat. Missed gaps, reboot and invalid time fail and pause;
terminal rows never become new runs. Cleanup has a separate allowance after
the acquisition budget. `budget_complete` requires settled native state and no
pause; it is elapsed-budget accounting, not an acceptance certificate. A run
with no useful demand can complete its budget without learning anything.

Logical storage accounting retains failed artifacts and reserves complete-set
copy headroom. Scans hold an immediate SQLite transaction so ordinary
`ask`/`status` writes cannot invalidate their ledger/journal metadata; all those
bytes still count. The scan itself does not update clock epochs. Other owner
file edits remain nontransactional. This is not a kernel disk quota. A valid
small storage policy may refuse all acquisitions. Native children are fixed attested no-fork programs;
the child runner is not an arbitrary descendant-process sandbox. Same-UID
malicious-owner isolation is not claimed.

Structured events use fixed names/codes and a correlation ID; no source rows,
native tokens or arbitrary exception messages are logged. The singleton run
row bounds heartbeat storage. Owner stdout capture is outside the work quota
and needs an owner retention limit. Monitor nonzero exit, `failed`, `frozen`,
pending mutations and stale heartbeats; none is an unattended success marker.
