# Captured tasks and recurring-gap inspection

This source extension connects the [verified task core](VERIFIED_TASK_CORE.md)
to the existing owner-DM capture flow. It adds opt-in task dispatch, exact
capture/history linkage and a grouped review queue. It is **not deployed** to
the existing live gateway, learner or frozen soak. See the
[September 9 evidence](../result/cnet_capture_task_inbox_20260909.md).

## Compatibility and opt-in

The private `bridge.json` retains the existing exact six fields and pins from
[the ingress installation contract](DISCORD_VERIFIED_LEARNING.md). Its integer
`schema_version` selects the route:

| Bridge version | Selected owner-DM behavior |
| --- | --- |
| 1 | Existing four exact Unicode forms; legacy ask/lookup can create demand; unrelated requests retain the existing peer route |
| 2 | Task-only: bounded numeric uppercase/lowercase proposals; observations create no demand; every refusal is terminal |

Other versions and extra configuration fields refuse. Version 2 still requires
the approved, pinned four-table installation and the independent pinned Unicode
excerpt; it does not fetch or import sources. Symbolic category/bidi commands,
ordinary conversation, unsupported intents and out-of-bounds input abstain in
this mode. There is no hidden peer or demand-producing fallback. Missing or
ambiguous case input asks for clarification without creating an experience.

Use a **fresh private schema-3 learning installation** with this source and its
own verified managed/native manifests. Capture SQL remains schema 1; no table,
export format or native ledger migration is added. Changing `bridge.json`
requires a new exact configuration hash. Do not edit pinned running manifests,
replace a live schema-2 runtime, renew an existing budget, or attach rehearsal
traffic to the real journal. A reviewed rollout/migration is separate work.

## Dispatch and evidence boundaries

The gateway establishes the existing individual-owner DM scope and commits
`Journal.begin` before task dispatch. Only the fresh `True` return authorizes
that control-flow path. Historical lookup of an unfinished request grants no
dispatch authority. Capture duplicates, including crash-before-dispatch and
crash-after-native-acceptance cases, never invoke it again.

The stable native request ID is the first 32 lowercase hex characters of SHA256
over a namespaced JSON tuple containing owner ID, channel ID, Discord message
ID and the SHA256 of exact delivered UTF-8 text. It excludes mutable segment,
timestamps, delivery/completion fields and replies. It is a link identifier,
not a signature, independent custody, origin attestation or dispatch token.
Changing scope, message identity or delivered text changes that link. Existing
native request-ID binding provides an additional at-most-once boundary.

Captured observations are `unreviewed`. That means neither human-attested nor
synthetic-attested. Test rehearsals live only in disposable private stores and
are explicitly reported as synthetic fixtures; they never count as genuine
capture or enter training/export. Direct development task commands should
continue to use the native `synthetic` origin.

Before and after dispatch, the bridge checks runtime, policy, configuration,
source and reference pins. Admission also checks the existing running owner's
original duration, boot, heartbeat and pause state. Native verified values must
agree with the independently pinned reference. Misses show an approval-needed
abstention; the reference value is not returned as a substitute answer.

Only explicit local-owner `learning approve` can admit an eligible miss using
an independently reviewed source hash. Remote task dispatch has no approval,
import, resume, activation, worker or budget-renewal command. Unknown outcomes
are not misses and never automatically retry. Malformed envelopes, conflicting
values and replayed conflicts latch the bridge and attempt to pause learning.
A failed pause is logged and never clears that latch. Non-conflicted replay is
reported as retained history, not current verification.

## Read-only commands

Use the installed DLL and trusted absolute host, not a development DLL against
live data. The following extend the existing local-owner CLI:

```sh
env -i "$LEARNING_DOTNET" "$LEARNING_DEPLOYMENT/managed/cnet-control.dll" \
  learning gaps "$LEARNING_DEPLOYMENT" 32

env -i "$LEARNING_DOTNET" "$LEARNING_DEPLOYMENT/managed/cnet-control.dll" \
  learning trace "$LEARNING_DEPLOYMENT" "$TASK_REQUEST_IDS"
```

`gaps ROOT LIMIT` accepts canonical limits 1–32. It reads one bounded snapshot
of at most 4,096 retained experiences, grouping by dataset, key, original
observation source hash and origin. Each group reports states, approved and
pending-review counts, insertion sequence boundaries and same-boot completed
request-time samples. Groups sort by pending review count, frequency and first
insertion sequence. This order is not a learned scheduling policy.

`PendingApprovals` is a historical review count, not permission to run: current
pause, budget, source and admission checks still govern an approval. Original
observation source hashes are not overwritten or merged with later approval
hashes. `LatestState` follows the last inserted observation, not finish order or
an approval-event log. `Truncated` states whether further groups were omitted.

`MeanRequestMilliseconds` summarizes the sampled historical requests, not
training/acquisition duration or a service latency SLO. No acquisition-cost
estimator has been measured: `EstimatedLearningSeconds` remains null,
`CostStatus` is `acquisition_cost_not_measured`, and `WorkerBudgetSeconds` is
only the policy's per-worker cap. `TrainingEligible` remains false.

`trace ROOT IDS` accepts 1–10 distinct, comma-separated 32-lowercase-hex IDs. One
native read snapshot returns matching experiences in requested order, with
explicit nulls for absent IDs. It does not scan only a chronological page and
mistake absence from that page for absence from the ledger. The small batch
preserves the bridge's existing 32-KiB per-pipe subprocess output limit.

The capture-side CLI joins these IDs to a page of capture records:

```sh
"$DISCORD_PYTHON" tools/discord_capture/task_inbox.py \
  "$CAPTURE_ROOT" "$LEARNING_DEPLOYMENT" "$LEARNING_BRIDGE_SHA256" \
  --after 0 --limit 10
```

`--after` is a nonnegative capture insertion ordinal; `--limit` is 1–10. Follow
`next_after` and `has_more`. The view returns capture ordinal, derived task ID,
segment, recorded times, delivery statuses and any matching native experience.
Raw/delivered text and Discord account/channel/message IDs are not emitted.
Keep even this redacted metadata private.

Capture-only rows remain `no_observation`. That alone cannot distinguish
clarification, unsupported input, pre-native interruption or an older bridge.
The native-only side remains available through `learning inbox`; this view
does not enumerate it. The capture and native reads are **separate snapshots**,
not an atomic cross-store transaction, a replay service or a whole chronological
task-event log. Delivery success, native result and source approval remain
different fields. Neither a matching ID nor a verified value establishes human
origin, continuous whole windows, task-policy return or training eligibility.

The inspector uses the existing private-file/schema boundary and rollback-mode
capture database. Unsupported WAL headers or WAL/shared-memory sidecars refuse
before SQLite opens them, because even a WAL reader can create sidecars. No
`immutable` locking bypass is used. The supported writer is the existing
rollback-mode Journal; protection against a malicious same-UID process racing
filesystem/database changes is not claimed. Read-only inspection works after a
learning run ends and does not renew it. Refusal returns exit code 2 and a
fixed `capture_task_inbox_refused` event, without private exception text.

## Operational diagnostics

The new structured events answer three questions: was a task admitted, what
kind of result/refusal occurred, and did a conflict's pause attempt fail?
`captured_task_result`, `captured_task_refused` and
`captured_task_pause_failed` carry the derived `request_id`, fixed status fields
and no message text, source labels or arbitrary child errors. Native CLI
receipts retain their correlation IDs. These logs are not approval evidence.

For `outcome_unknown`, inspect the linked history and existing service status;
do not replay the captured request. For `task_refused` or pause failure, stop
and review runtime/source pins, native status and retained conflict state.
For `unavailable`, inspect the original run budget, heartbeat, pause and private
installation checks. Do not repair availability by lowering guards or resetting
history. Existing capture health monitors and alert delivery are unchanged.

## Verification and next work

The managed `LearningCapturedTaskTests` rehearsal creates an isolated private
installation, real daemon and real 30-second owner, with no network/teacher.
It uses Python's standard library (`/usr/bin/python3`, or explicit
`CNET_TASK_PYTHON`), simulated capture/delivery and the production bridge.
Actual Discord/WebSocket dispatch is covered separately by mocked gateway
tests; no real Discord requests are sent by either suite.

The [result](../result/cnet_capture_task_inbox_20260909.md) records exact
regressions, coverage and limitations. Next source work is useful verified
composition reuse and an independently frozen paraphrase/OOD evaluation.
Real origin review, measured learning costs, chronological training returns,
dependency-aware freshness and useful AMD task-policy gains remain unfinished.
