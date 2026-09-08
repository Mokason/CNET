# Captured task linkage and recurring-gap inbox

Continue from `82ce283` on the existing isolated task-core branch. Keep the
primary checkout, live gateway/learner and frozen soak unchanged. No schema
migration, historical request replay, inferred origin attestation or automatic
external-correction approval is authorized by this implementation.

## Slice 1: read-only recurring gaps

Add `learning gaps ROOT LIMIT` (1–32 groups). Take one bounded snapshot of the
existing 4,096-row experience ledger; group by dataset, key, observation source
and origin. Show historical states, approvals, pending review and request-time
samples without merging synthetic/unreviewed populations. Bound output and
report truncation. Request time is not acquisition cost; unknown learning cost
must remain explicit. Tests prove grouping, bounds and byte-identical read-only
behavior. No new scheduler priority or training eligibility follows from rank.

## Slice 2: opt-in captured task dispatch

Extend the existing pinned bridge configuration with an explicit version-2
task mode; version 1 retains its exact-command semantics. Reuse the existing
private runtime, policy, four pinned Unicode tables and independent reference.
Only a selected owner-DM request that was durably admitted by the capture
journal can invoke this mode. Derive a stable 32-hex ID from the capture scope,
message identity and captured delivered-text hash, never mutable completion
fields. Dispatch requires the fresh successful `begin` control-flow handoff;
looking up an unfinished row cannot authorize dispatch or replay. A query is
observed as `unreviewed`, not human-attested or training truth.
Validate native task envelopes, identity, source and expected/native values.
Every parser outcome, including abstention and input bounds, is terminal in
this opt-in finite-task mode. Unsupported tasks cannot fall through to a peer
or the legacy demand-producing lookup. No bridge import/approval/activation
authority is added.

Tests: committed capture before dispatch; duplicates/foreign scope/unfinished
replay refusal; malformed native replies and source mismatches; no automatic
demand; unchanged legacy route. A private actual-native rehearsal uses synthetic
capture fixtures and explicitly reports them as such.

## Slice 3: owner inspection of linked history

Add a bounded read-only capture-side page joined to exact native request IDs.
A native batch trace (at most 10 IDs) stays below the existing 32-KiB subprocess
output bound. Capture-only rows remain `no_observation`, without guessing why.
Native-only history remains available through the existing native inbox; this
capture-side view does not claim to enumerate it. The two stores are separate
snapshots, not one atomic cross-store transaction. Do not mutate/export/replay
the live capture or widen its schema. Retain explicit unreviewed origin;
no match alone implies correctness, whole-window continuity or eligibility.
Do not emit raw private conversation text by default. Show independently
approved source, actual observation and existing capture delivery outcome as
separate fields. Tests cover altered text/scope, pagination and read-only state.

## Verification and remaining limits

Use ECC RED/GREEN checkpoints and coverage, Graft graph discovery/freshness,
native integration and full managed/capture regressions. Fresh-context review
is required for capture dispatch and evidence joining. Cross-model review stays
skipped under the owner's Astra-only preference. Keep the existing failed
allocator floors intact. Grouped frequency is not measured learning value;
true acquisition-cost estimation, origin review, chronological eligible
trajectories, composition reuse and AMD task-policy experiments remain separate.
