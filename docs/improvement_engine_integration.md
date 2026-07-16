# Improvement Engine Integration

## Scope

Phase 5 registers the completed CNET model-compression work through a C-only bridge with local
self-improvement artifacts. The external `closed-loop-self-improvement` and
`perpetual-improvement-engine` repositories were not present under
`/home/marble/AI`, so CNET now emits deterministic handoff files that those
engines can ingest when available.

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
python3 tools/ingest_cnet_suggestions.py \
  --suggestions suggestions/cnet_compression_suggestions.jsonl \
  --acceptance-report reports/qwythos_real_model_acceptance.json \
  --db /path/to/suggestion_registry.db
```

The ingester validates the existing schema and uses stable SHA-256 keys with `INSERT OR IGNORE`. Historical Phase 1–5 rows retain `implemented` status rather than becoming duplicate work. A valid real-model campaign with denied candidate admission creates one priority-5 `proposed` repair item keyed by candidate SHA-256. This makes the scheduling input current and evidence-backed while keeping execution local and manual; no cron, GitHub Actions, or external write is installed.

## Perpetual Engine Handoff

Consumers should schedule only rows with `status='proposed'` and `meta.actionable=true`. Implemented milestone rows remain queryable evidence but are not follow-up tasks.

## Self-Development Handoff

The next self-development cycle should summarize this Phase 5 bridge and the
Phase 1-4 implementation pattern in `Self-Development-Log.md`. The key reusable
pattern is: every compression feature lands with a native contract, a focused
test, a plan artifact, an implementation-log row, and a generated registry/training
handoff row.
