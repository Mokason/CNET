# Bounded improvement handoff

The native compression bridge exports implementation records into suggestion
rows and training-handoff rows. The managed control plane can ingest those
suggestions into a selected SQLite registry and consume a bounded actionable
item. This is not an autonomous code-writing or self-deploying service.

`make register_compression_improvements` builds/runs the bridge target.
Its inputs and outputs include `implementation_log.jsonl`,
`training_data.jsonl` and a selected suggestions file. Output replacement
preserves unrelated training rows according to the implementation's metadata
scope; use private copies when validating that behavior.

## Registry operations

The control-plane commands are `ingest-suggestions` and
`activate-suggestions`. Both can mutate the selected database.
Inspect their options and schema, use a new private database for tests, and
do not reuse a live registry merely because an old example names it.

Ingestion uses stable identities and preserves historical implemented rows.
The bounded consumer claims an eligible row transactionally, checkpoints its
state and evaluates artifact/acceptance evidence before finalizing it.
It does not create its own scheduler, generate code or publish changes.

Earlier “row 6 is verified / zero actionable rows” statements were dated
observations, not current database status. Preserve and inspect current
state rather than copying them into operational assertions.

[Phase 5](../plans/phase5_improvement_engine_integration.md) retains the
decision and evidence contract. The control-plane SQLite provider was repaired
and its actual loaded version tested; [SECURITY.md](SECURITY.md) records the
scoped audit. Training handoffs still need independent target provenance.
