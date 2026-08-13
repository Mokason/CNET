# Improvement Engine Integration

## Scope

Phase 5 registers completed CNET model-compression work through deterministic
local handoff artifacts, a SQLite ingester, and a bounded evidence consumer.
The generic cloud auto-implementation stack described by older skills is not
present under `/home/marble/AI`; CNET therefore does not pretend that an
unbounded code-generating daemon exists.

## Bridge Command

```sh
make register_compression_improvements
./bin/register_compression_improvements \
  --log implementation_log.jsonl \
  --training-data training_data.jsonl \
  --suggestions suggestions/cnet_compression_suggestions.jsonl
```

The C bridge reads `implementation_log.jsonl`, keeps the latest record per phase,
and writes:

- `suggestions/cnet_compression_suggestions.jsonl`: registry-ready suggestion
  rows with priority, tags, evidence, artifacts, tests, and next steps.
- `training_data.jsonl`: instruction/response training rows for successful CNET
  compression implementations.

The training export preserves unrelated pre-existing rows and replaces only rows
whose metadata source is `cnet_model_compression_phase_log`.

## Suggested Registry Mapping

Each suggestion row contains:

- `id`: stable phase identifier, for example `cnet-compression-phase-2`.
- `priority`: paper-evidence and CNET-alignment score.
- `status`: implementation status copied from `implementation_log.jsonl`.
- `evidence.tests`: commands that verified the implementation.
- `next_steps`: pending benchmark or runtime work.
- `tags`: routing tags for model compression, sparse KV, uncertainty, narrative
  coherence, or self-improvement ingestion.

## Local Registry Activation

The external SQLite SuggestionRegistry is available locally and is populated on demand with:

```sh
dotnet run --project dotnet/CnetControlPlane -- ingest-suggestions \
  --suggestions suggestions/cnet_compression_suggestions.jsonl \
  --acceptance-report reports/qwythos_real_model_acceptance.json \
  --db /path/to/suggestion_registry.db
```

The ingester validates the existing schema and uses stable SHA-256 keys with `INSERT OR IGNORE`. Historical Phase 1–5 rows retain `implemented` status rather than becoming duplicate work. A valid real-model campaign with denied candidate admission creates one priority-5 `proposed` repair item keyed by candidate SHA-256. This makes the scheduling input current and evidence-backed while keeping execution local and manual; no cron, GitHub Actions, or external write is installed.

## Perpetual Engine Handoff

`dotnet run --project dotnet/CnetControlPlane -- activate-suggestions` is the local bounded consumer. It:

- selects only `cnet-model-compression` rows from `cnet_real_model_acceptance`
  with status `proposed` or recoverable `in_progress` and
  `meta.actionable=true`;
- claims at most one row with `BEGIN IMMEDIATE`;
- records a durable `in_progress` checkpoint before evidence evaluation;
- requires the task SHA-256 to match the quarantined artifact and the
  replacement SHA-256 to match the admitted candidate;
- requires CPU-only acceptance, QGKP byte identity, restart stability, Hermes
  loopback acceptance, and `max_quality_delta=0.0`;
- finalizes as `verified` or `archived`, never re-proposes a failure;
- never schedules itself, generates code, invokes git, or writes externally.

The live row 6 completed `proposed -> in_progress -> verified`; a second
invocation returned `idle`, and zero actionable rows remain. See
`reports/cnet_bounded_activation.json`. Implemented milestone rows remain
queryable evidence but are never scheduled as follow-up tasks.

## Self-Development Handoff

The next self-development cycle should summarize this Phase 5 bridge and the
Phase 1-4 implementation pattern in `Self-Development-Log.md`. The key reusable
pattern is: every compression feature lands with a native contract, a focused
test, a plan artifact, an implementation-log row, and a generated registry/training
handoff row.
