# CNET Replace/Improve — deep completion status (2026-07-24)

## Gates (all green)

```
make cnet_fault_test              → CNET_FAULT_PASS
make cce_adapter_bank_test        → CCE_ADAPTER_BANK_PASS
make cce_dora_test                → CCE_DORA_PASS
make cnet_serve_decode_test       → CNET_SERVE_DECODE_PASS
make cnet_fault_loop_test         → CNET_FAULT_LOOP_PASS
make registry_lora_store_test     → CNET_LORA_STORE_PASS
make jtc_adapter_bench            → JTC_ADAPTER_BENCH_PASS
# jtc_adapter_bench measured: acc_off=0.2975 → acc_on=0.7375  Δ=+0.4400
```

## P0 deploy + durability

| Item | Status |
|---|---|
| `CNET_FAULT_LOG` in personal-ai.env + cnet-deploy.env | done |
| `CNET_LORA_STORE_DIR` + AUTOSAVE/AUTOLOAD | done |
| autosave on cert PASS | done |
| autoload on orchestrator install | done |
| promote + optional `CNET_PROMOTE_EVAL_DELTA` file | done |
| freeform tk off (`CNET_AUTO_LEARN_FREEFORM=0`) | done |

## P1 PEFT depth

| Item | Status |
|---|---|
| adapter bank put/select on cert (`CNET_ADAPTER_BANK=1`) | done |
| VeRA freeze A (`CNET_LORA_VERA=1` → `cce_lora_set_train_A(0)`) | done |
| DoRA-lite unit tests | done (hot-path teach still LoRA default) |
| GGUF residual hook symbol (NULL default) | done |
| Lily bus = head JTC path (residual Lily separate envelope) | documented; JTC is head PEFT |

## P2 product

| Item | Status |
|---|---|
| MCP `decoded_text` on use_skill | done |
| procedure_chunk_seal.sh queue | done |
| janitor PRUNE_GH → candidates file | done |
| hash-embed boost on Ghost KeywordIndex | done |

## P3 cleanup

| Item | Status |
|---|---|
| dag_api.h ownership seam | done |
| AICIMO mechanism already in RouteOnRole | already present |
| dual Llm DEPRECATED banner | already present |
| no push (local master only) | intentional |

## Benchmarks

- **JTC adapter before/after:** +44.0 pp accuracy (29.75% → 73.75%)
- Writes `logs/ghost_eval_delta.txt` for promote gate
- GhostEval writes same delta format when `CNET_PROMOTE_EVAL_DELTA` set (needs Ollama for live run)

## Enable live

```bash
set -a && . config/personal-ai.env && set +a
# ensure logs/ exists; restart personal-ai / MCP as needed
make jtc_adapter_bench   # refresh delta + prove adapter lift
```
