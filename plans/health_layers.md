# Layered health diagnostics

**Status**: first slice — five independent layers + SoulHost JSON  
**Gate**: `make health_layers` → `HEALTH_LAYERS_PASS`

## Separation of concerns

| System | Job |
|---|---|
| `specialist_health_pass` | **Fix** (audit → label → heal → promote → shadows) |
| **`cnet_health_layers`** | **Measure** five ladders without mutating |

## Layers (ordered)

| # | Layer | PASS means |
|---|---|---|
| 0 | **registry** | Name present in the live registry |
| 1 | **loadable** | BTN resident, ports/dims coherent |
| 2 | **execution** | `btn_forward` returns non-NULL on a probe |
| 3 | **semantic** | `btn_certify` against contract (or certified flag) |
| 4 | **utility** | Production-usable (certified/active; not demoted/shadow) |

A FAIL at layer *k* makes layers *k+1…* **SKIP** (not FAIL).  
`deepest_pass` = consecutive PASS from registry; `production_ready` = all five PASS.

## API

```c
cnet_health_check_unit(reg, name, cfg, &report);
cnet_health_check_registry(reg, cfg, &agg, on_unit, ctx);
cnet_health_layers_format_json(&report, buf, cap);
soul_unit_health_layers(host, name, out, cap);
```

## Config defaults

- utility reliability floor 0.9 (only demotes certified units *with* enough live evidence below floor)
- utility_min_evidence 16 (0 allowed for freshly sealed SoulHost checks)
- require_certify 1 when a contract lookup is supplied
