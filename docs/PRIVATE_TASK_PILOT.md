# Private task pilot

Source checkpoint: a586a788529d8686a2aaba3c4b0d0296fe0afc6b (September 10, 2026).
This is a separate local CLI pilot, not the Discord service. It starts with no
approved sources and no learned capsules. Two synthetic rehearsal observations
are retained and clearly marked; no genuine requests have been fabricated.

## Send a real request

```bash
/home/marble/cnet-task-pilot-20260910-ZNTmfo/ask "What's the uppercase of µ?"
```

The helper prints `request_id=...` to stderr before execution, then the typed
receipt to stdout. Save that ID. A first covered request returns
`awaiting_evidence`, not an invented answer. Missing direction clarifies;
unsupported domains/actions abstain. Real requests are `unreviewed`, never
automatically certified human-origin or eligible for training. Tests must use
`ask --synthetic "request"`. Do not place confidential text in shell history.

For the owner commands below, define this Bash function in your terminal:

```bash
PILOT_ROOT=/home/marble/cnet-task-pilot-20260910-ZNTmfo
pilot() { env -i /home/marble/dotnet/dotnet "$PILOT_ROOT/managed/cnet-control.dll" learning "$1" "$PILOT_ROOT" "${@:2}"; }
pilot inbox 0 100
pilot gaps 32
pilot status
```

If output is lost, use the SAME ID and exact text, rather than generating another
logical request. Replace the placeholder below with the saved 32-digit ID:

```bash
pilot task unreviewed YOUR_SAVED_REQUEST_ID "What's the uppercase of µ?"
```

The ledger has a 4,096-identity bound with no automatic eviction; retries using
the same ID preserve deduplication. State is private to this installation. The
helper is an owner convenience, not an authentication boundary against same-UID
code. Host, Python/.NET framework and the owner account remain trusted.

## Explicitly approve independent evidence

Nothing below has been executed in this pilot. `sources-for-review/` only holds
copies of the existing Unicode 17 excerpt, tables and provenance guide; copying
them is not approval or import. Review their contract before choosing a source.
The simple-uppercase table deliberately excludes identity mappings such as ß.
Hashes detect changed bytes; they do not authenticate truth or the source author.

For uppercase, if you approve the supplied table and have a real pending request:

```bash
pilot import unicode17_upper_latin1 "$PILOT_ROOT/sources-for-review/unicode17_upper_latin1.tsv" d5a314a48db2e2e86712bf012c20ac40fd1ef68b6120a5cdbf6c5a6aa93916e3
pilot approve YOUR_SAVED_REQUEST_ID d5a314a48db2e2e86712bf012c20ac40fd1ef68b6120a5cdbf6c5a6aa93916e3
```

Only import once: existing-source retries refuse. An approval must match that
request's dataset and key. Lowercase is a separate choice, using
`unicode17_lower_latin1.tsv` with hash
`54d8c8aa294fa803fb49d0f5ee2cb46357d09950b664163d70f43c2dfaf8d3b7`.
The raw excerpt hash is
`75dfecc13fe9b1202e3f7c787e4e7f2c848c97c8b092dd75b4f6a2b99990cdc4`.
Do not approve CNET answers as source evidence or confuse source approval with
human-origin review. Do not change initialized policy/manifests/code.

After approval, run bounded ticks until the receipt says `accepted`. Probation
requires three probes, at least 30 and no more than 120 seconds apart. Do not
leave a partially promoted candidate unattended between manual commands.
This optional foreground loop performs at most 20 ticks, with no automatic
retry of failed commands and no durable supervisor run:

```bash
/usr/bin/python3 -I - <<'PY'
import json, subprocess, time
root = '/home/marble/cnet-task-pilot-20260910-ZNTmfo'
command = ['/home/marble/dotnet/dotnet', root + '/managed/cnet-control.dll',
           'learning', 'tick', root]
for attempt in range(20):
    result = subprocess.run(command, cwd=root, env={}, capture_output=True,
                            text=True, timeout=45)
    print(result.stdout, end='', flush=True)
    result.check_returncode()
    if json.loads(result.stdout)['action'] == 'accepted':
        break
    time.sleep(30)
else:
    raise SystemExit('Acceptance not reached; inspect status and follow cleanup below.')
PY
pilot verify unicode17_upper_latin1
```

Successful `verify` alone is not completed probation. Require `accepted` first,
then all 256 verification checks. On timeout, nonzero exit, interruption or an
exhausted loop, inspect status and preserve pending obligations; do not retry
mutations blindly. After acceptance, a new request can receive a checked offline
answer. No owner supervisor is currently running: `max_run_seconds=3600` limits
an explicitly started durable run, NOT daemon-only capture lifetime. Do not
start `learning run` casually: its terminal/expired/stale row prevents further
executable task observations and cannot be reset or renewed in this ledger.

## Stop and recovery

Before any learning has been approved (current initial state), stop only this
daemon; the installation and pending observations remain recoverable:

```bash
systemctl --user stop cnet-learning-daemon@cnet-task-pilot-20260910-ZNTmfo.service
```

After learning starts, keep the daemon available for cleanup. With no active
owner, use `pilot quiesce` and require exit 0 with settled cleanup BEFORE stopping
the daemon. Quiescence persists pause/stop state; read status and the repository
recovery contract before attempting resume. If you explicitly started an owner,
use `pilot pause`, stop ONLY
`cnet-learning-owner@cnet-task-pilot-20260910-ZNTmfo.service`, then quiesce.
If transport is already down, restore this same pinned private daemon before
cleanup. Never delete the ledger, pending tokens or snapshots to clear a failure.

This instance is not enabled at boot and does not restart automatically. Start
it explicitly with the same daemon unit when appropriate. It has AF_UNIX-only
network access, one CPU, 1 GiB memory, 128 tasks, and no teacher/self-answer/core
auto-evolve. The worker policy permits four attempts/hour and eight promotions/
day, with 256 MiB logical work accounting (not a kernel filesystem quota).
The allocator is disabled. No existing service or Discord route was changed.
