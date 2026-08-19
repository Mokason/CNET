# CNET Ember — residual heat host

Status: **GATED** — NOT CORE — NEVER CERT

Date: 2026-08-19.

## Name

**Ember** = residual heat under CORE's star. Renamed product surface for the
former DS4 dual residual launcher + new session craft.

## Pieces

| Module | Role |
|--------|------|
| `cnet_ember_ckpt` | Disk session checkpoints (prefix hash, budget, reasons) |
| `cnet_ember_session` | session_sync extend/rebuild + compact |
| `cnet_ember_steer` | Teacher style card (not activation surgery) |
| `cnet_ember` | draft via held_model; claimed_cert=0 |
| `run_cnet_ember_dual.sh` | dual-GPU residual engine (backend under ROOT) |

## Law

- claimed_cert always 0 on ember paths
- compact / draft → miss_log auto_cert=false
- RLM may call ember only when `allow_ember_draft=1` (default off)
- CORE CERT path unchanged

## Gate

```bash
make cnet_ember
make ember_dual_launcher   # also unified_ds4_launcher legacy
```

## Env

| Var | Meaning |
|-----|---------|
| `CNET_EMBER_HOME` | session + ckpt root |
| `CNET_EMBER_STEER` | style card path |
| `CNET_EMBER_ROOT` | residual engine tree (e.g. ds4 checkout) |
| `CNET_MISS_LOG` | self-improve harvest |

## Related

- `plans/cnet_rlm_autonomy.md`
- `result/EMBER_RLM_20260819.md`
