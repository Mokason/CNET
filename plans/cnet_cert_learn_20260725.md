# Certified learning close-loop (2026-07-25)

## Sequence executed (high → low leverage)

| Pri | Action | Result |
|---|---|---|
| P0a | Restore on-demand Gemma teacher (remove no-heavy-teacher) | Lane loads teacher again for seal demand |
| P0b | JTC fault seed (dedupe-aware unique) → PEFT tick → cert → store | **`json_toolcall_v2.lora` on disk**, `jtc_lora_certified=1` |
| P1a | Residual structure-mine hammer | mine_rc=1 (no student) — **W=256 traces skip expand (cap 16)** |
| P1b | Procedure chunk seal + gate | `CNET_PROCEDURE_CHUNKS_PASS n=5` |
| P2 | `CNET_PROMOTE=1` + eval delta file | wired in personal-ai.env |
| Bench | jtc / fault_loop / residual_http / lora_store / procedure | all PASS |

## Benchmark scoreboard

| Gate | Marker |
|---|---|
| JTC adapter | **acc_off=0.2975 → acc_on=0.7375 Δ=+0.4400** (`JTC_ADAPTER_BENCH_PASS`) |
| Fault loop | **certified=1** (`CNET_FAULT_LOOP_PASS`) |
| Residual HTTP | **RESIDUAL_HTTP_PASS checks=9** |
| LoRA store | **CNET_LORA_STORE_PASS** |
| Procedure chunks | **PASS n=5 serves=25** |
| Live store | `logs/lora_store/json_toolcall_v2.lora` |

## Tools

- `tools/cnet_cert_learn_tick.c` → `bin/cnet_cert_learn_tick`
- Env: `CNET_FAULT_DEDUPE=0` when seeding diverse JTC pairs

## Known gap (next)

Structure-mine from Bonsai residual needs either:
- window width ≤16 for expand labeling, or
- extend `hybrid_structure_mine` expand path for large windows / sample subsets.

## Run

```bash
CNET_FAULT_DEDUPE=0 CNET_RESIDUAL_HTTP=http://127.0.0.1:8080 \
  CNET_RESIDUAL_WINDOW=$PWD/english_window_256_bonsai.txt \
  ./bin/cnet_cert_learn_tick
make jtc_adapter_bench cnet_fault_loop_test residual_http_real
```
