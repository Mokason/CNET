# CNET Deep 8-Priority Use-Loop (2026-07-21)

Substantive engine work — not checkbox theater.

## Safety
- No reserved GPU wake / no Gemma-Bonsai-Qwen teacher start
- No public visibility change; push only private origin when directed
- Hermetic residual via `CNET_SOUL_RESIDUAL_HERMETIC=1`

## Implemented

### P1 Live CNB evidence
- Root cause: `soul_close` never persisted reliability; restore never loaded `.stats`
- `registry_persist_runtime_state` + restore loads `<name>.stats`
- `soul_close` writes per-base `<base>.state/` (not cwd `.` — avoids cross-base contamination)
- Host `soul_serve.stats` counters survive reopen
- Gate: `make serve_feedback`, `make cnet_deep_use_loop` (rel 962→962 reopen)

### P2 Multi-step distill
- Deep gate trains/certifies/admits two members, distills chunk, domain-equality vs plan
- Existing `self_improve` / `post_seal_serve` remain

### P3 Gap economics
- `AcquireReport` extended: oracle_calls/rejects/abstains, train_wall_ms, student_bytes
- Defer histogram: waiting_oracle / oracle_unfit / certify_failed / other
- Drain aggregates oracle counters; train path times wall ms
- Gate: deep loop P3 + `miner_efficiency_bench`

### P4 Residual
- Hermetic residual bind path exercised under deep gate
- Structure-mine/seal already on soul_host health tick

### P5 Planner reliability
- Deep gate: two same-shape certified units, higher live reliability preferred by `route_plan`

### P6 Health / evidence
- Health layers via SoulHost ABI after live serves
- Evidence store path on close records bundles

### P7 Engineering
- `route_execute` outcome counters verified
- State-dir isolation (correctness bugfix)

### P8 Taxonomy
- TruthfulQA CSV present but **withheld** without executed path
- `benchmark_taxonomy` + deep P8

## Umbrella
```
make cnet_deep_use_loop
make cnet_use_loop_acceptance   # includes deep loop first
```
