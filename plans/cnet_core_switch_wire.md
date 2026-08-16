# CORE self-evolve switch — WIRED

Status: **LIVE — CORE CAN FLIP THE SWITCH**

## Architecture

```
live ask
  → light .lut CERT serve (cnet_core_serve)
  → else RLM CERT only
  → else ABSTAIN + miss_log
       → fork cnet_core_evolve (if CNET_CORE_AUTO_EVOLVE=1)
            → factory / miss→admit
            → write .lut bricks
       → reload bank
```

## Env

| Var | Meaning |
|-----|---------|
| `CNET_CORE_BUS_BRICKS_DIR` | brick directory (`.lut` files) |
| `CNET_CORE_AUTO_EVOLVE=1` | enable switch on abstain |
| `CNET_CORE_EVOLVE_EVERY=N` | evolve every N waist misses (default 4) |
| `CNET_CORE_EVOLVE_BIN` | path to `bin/cnet_core_evolve` |
| `CNET_CORE_EVOLVE_FACTORY=1` | seed factory if bank empty |
| `CNET_BONSAI_GGUF` | host weights |
| `CNET_MISS_LOG` | live miss jsonl (set by evolve from cnetd) |

## Gates

```bash
make cnet_core_evolve
make cnet_core_switch_wire
# CNET_CORE_SWITCH_WIRE_PASS switch=1 human_mid_loop=0
```

## Law

- Residual never auto-CERT
- Evolve only TABLE≥0.95 then admit
- Outside table still abstains
