# CNET analyze + improve (2026-07-25)

## Live testimony (before → after library APPLY)

| Signal | Before | After |
|---|---|---|
| Units (soul roster) | 228 | **101** |
| CNB size | 225M | **93M** |
| gh_ noise | 26 | **0** |
| JTC v2 | present | **present** |
| certified_serves (MCP) | 31 | (stale process until recycle) |
| residual_bound | false | fixed in source + launch; needs MCP recycle |
| Fault/LoRA/acct on MCP | empty | launch wired; needs recycle |
| Curiosity | on (tk sludge) | **CNET_CURIOSITY=0** (freeform-off) |
| Adapter bank | unset | **CNET_ADAPTER_BANK=1** |

## Ranked actions executed

| Pri | Action | Evidence |
|---|---|---|
| P0 | MCP launch env before exec + PEFT env | `LAUNCH_SH_ENV_ORDER_OK`, hotswap template |
| P0 | Eager hermetic residual | `SOUL_RESIDUAL_SERVE_PASS checks=21` |
| P0 | Makefile G3 target `cnet_consolidate` | no dual-recipe override |
| 1 | G3 APPLY+REPLACE | `CONSOLIDATE_APPLY_OK keep=101 drop=127` pin `artifacts/janitor/pins/pin_20260725_161423_*` |
| 2 | gh_ prune via `CNET_CONSOLIDATE_DROP_GH=1` | gh 26→0 |
| 3 | Curiosity off under freeform-off | lane environ `CNET_CURIOSITY=0` |
| 4 | Commit + push master | (this plan) |

## Gates

```
make soul_residual_serve → SOUL_RESIDUAL_SERVE_PASS
make verify-fast         → VERIFY_FAST_PASS
make cnet_fault_test     → CNET_FAULT_PASS
```

## External

Recycle MCP/gateway so process reopens slim CNB + residual/PEFT env.

## Rollback

```bash
bash scripts/cnet_janitor_restore.sh artifacts/janitor/pins/pin_20260725_161423_soul_gemma4v2_final.cnb
```
