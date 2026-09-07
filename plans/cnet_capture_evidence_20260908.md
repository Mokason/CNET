# Capture evidence and operations sequence

## Scope and decisions

Owner approved all four next steps after the owner-only DM collector: typed
mapping/independent labels, chronological datasets, operational safeguards and
improvement qualification. Baseline `94e7746`, existing isolated worktree. No
push, broad collection, model provider, lowered certification floor or alteration
of immutable learning deployments is inferred. Astra-only review continues.

### Slice 1: versioned mapping and independent evidence

Reuse the native semantic capsule-intent parser and independent `mul`/`xor` tool.
Start with the existing example tool catalog (bytes→bits and u8→masked8), explicitly
pinned for offline evidence only, not activated as live acquisition policy. Exact
typed requests and the canonical conversion grammar may map; unrelated/ambiguous
text, out-of-domain inputs and unapproved tag pairs remain unknown. Native tool
answers must agree with an independent integer calculation. Bind each receipt
to the original captured record, catalog, parser/tool artifacts and checker.
This verifies the task's expected answer, not the live CNET response.

Capture bytes, per-request IDs and labels stay private, outside git. Hashes bind
identity, not independent custody against a compromised owner/runtime. No input
text may choose executables, source paths, policy, origin or correctness status.

### Slice 2: immutable chronological export

Read capture schema 1 in a bounded read-only SQLite transaction, without changing
it or blocking the writer indefinitely. Export into a fresh owner-private
directory: snapshot records, independent evidence and manifest, all exclusive
creates with fsync and a last-published manifest. An incomplete export is invalid.
Never overwrite/relabel an existing dataset or use it as a capsule package.

Freeze UTC episode windows, provenance review and split policy before scoring.
Use whole fixed 30-minute observed windows; no request-level random splitting.
Exclude windows with capture gaps/reconnect boundaries, backwards timestamps,
unfinished attempts, unknown/test/automation provenance or unverified mappings.
Require explicit owner attestation for origin: Discord identity alone does not
prove human demand. An unreviewed window remains diagnostic, not training data.
Keep unknown/excluded counts in the denominator report; never silently drop them
and call the remaining subset representative. Already observed input is development
only; confirmation requires a new prospectively frozen cutoff after candidate
selection. No current record becomes confirmation by slicing it differently.

### Slice 3: monitoring and scoped privacy repair

Questions: is capture connected and making progress; are requests stuck; is
storage approaching refusal; do permitted legacy log paths remain private?
Install a local bounded health check/timer with metadata-only alert state and
journal output. No external notification service or fake Discord test message.
Track connected heartbeat/progress, reconnects, stale attempts, DB/quota/free-space
limits and exact legacy file privacy. Idle demand alone is not an incident.
Retain data, do not auto-delete or reset it to clear an alert. Timed observation
must use real elapsed BOOTTIME; simulated fault fixtures are separate.

Resolve exact legacy log directories/files, reader/writer ownership and rotation
before permission changes. Harden only agreed CNET-private paths and relevant
service umasks, preserving bytes. Rollout may restart the bridge and the identified
legacy writer services as required; preserve overrides and record liveness and
rollback. Do not change certified runtime pins or the private learning ledger.

### Slice 4: development qualification and long-run observation

Run dataset readiness and, when eligible episodes exist, equally budgeted
deterministic comparisons. Separate structural opportunity bounds from actual
native verified capsule outcomes. Reuse existing native controls/trajectory
harness; do not claim a Python reachability score is certified coverage.
Only useful demonstrated headroom permits bounded AMD fitting. Fresh whole-episode
confirmation keeps >=.05 absolute gain, positive paired confidence lower bound
and no negative family mean. Existing failed/exposed campaigns stay unchanged.

Long-run acceptance needs 72 real elapsed hours after frozen relevant artifacts,
adequate traffic and complete healthy observation; a running monitor, empty
dataset or successful fixture does not pass it. Report missing prerequisites and
retain an operational collector instead of manufacturing a successful gate.

## Acceptance / verification

- [ ] RED mapping/independence/receipt tests; canonical parser and tool negatives.
- [ ] RED snapshot identity, immutable publication, episode leakage/gap/origin tests.
- [ ] RED monitor fault/idle/quota/privacy tests and actual local alert delivery.
- [ ] Exact legacy privacy repair and service creation settings, with rollback.
- [ ] Export real local demand without displaying payloads; record actual readiness.
- [ ] Development outcome/headroom gate; AMD fitting/confirmation only if justified.
- [ ] Start frozen local observation; no premature 72-hour acceptance claim.
- [ ] Focused regressions, independent reviews, operator docs and local commits.
