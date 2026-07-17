# Colibrì → CNET integration roadmap

**Source:** [JustVugg/colibri](https://github.com/JustVugg/colibri) ideas adapted to
CNET’s certified library + residual architecture (not a engine merge).

**Gate:** `make colibri_integrate` → `COLIBRI_INTEGRATE_PASS`  
**CLI:** `make cnet_plan_cli` → `bin/cnet_plan plan|doctor|json`

## Status (2026-07-17)

| Phase | Item | Status |
|---|---|---|
| P0 | `cnet plan` / `doctor` (MemAvailable + residual/teacher/CNB) | landed |
| P1 | LFRU + hysteresis (`cnet_lfru.h`, `CNET_FOREST_LFRU=1`) | landed |
| P2 | Prefer-warm counters + heat-ranked structure mine | landed |
| P3 | Batch residual labeling (`batch_label_rows`) | landed |
| P4 | Residual session KV (`CNET_RESIDUAL_SESSION_KV=1`) | landed (opt-in) |
| P5 | PILOT research ring (`CNET_PILOT=1`, `cnet_pilot`) | landed (research) |

## Env knobs

| Env | Default | Meaning |
|---|---|---|
| `CNET_FOREST_LFRU` | 0 | Forest residency victim = LFRU not pure LRU |
| `CNET_RESIDUAL_SESSION_KV` | 0 | Residual grows KV (chat); mine path should reset |
| `CNET_PILOT` | 0 | Record next-slot hints after residual forward |
| `CNET_RESIDUAL_GGUF` | — | Residual model path (plan/doctor) |
| `CNET_PERSONAL_TEACHER` | — | Teacher GGUF (plan dual-load) |
| `CNET_BASE_PATH` | soul_…cnb | Library base |

## Quality policy (Colibrì-aligned)

- Insufficient RAM → **slower / refuse dual**, never silent precision drop.
- Residual answers always **uncertified** (Tier C).
- Certified Tier A always outranks residual.
- Session KV is opt-in and **must not** be used for bit-stable mining probes.

## Files

- `include/cnet_placement.h` `src/cnet_placement.c` `tools/cnet_plan.c`
- `include/cnet_lfru.h` (header-only)
- `include/cnet_pilot.h` `src/cnet_pilot.c`
- Forest LFRU: `src/cce/cce_forest.c` + `heat` on `cce_branch`
- Hybrid heat/mine: `src/hybrid_ai.c`
- Residual session/pilot: `src/residual_gguf.c`
- Gate: `tests/test_colibri_integrate.c`
