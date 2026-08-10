# 3-lane memory runtime (STM / LTM / FORM)

## Law

| Lane | Role | Serve? |
|---|---|---|
| **STM** | Hot CERT pins (cap + LRU-ish eviction) | yes |
| **LTM** | Capsule dir index; cold `cnet_capsule_import` | after fetch |
| **FORM** | Experience candidates; promote only if score>incumbent + import OK | never raw |

Serve never trains. Form never mutates STM without verify/import.

```
resolve(name):
  STM hit → OK
  else LTM fetch capsule → pin STM → OK
  else ABSTAIN

form_tick():
  pending with capsule_dir + score>incumbent → import → LTM + optional STM pin
```

## Run

```bash
make mem_runtime
# MEM_RUNTIME_PASS
# logs/mem_runtime.log
```

Uses `artifacts/skill_pos_capsules/skill_pos_00..` when present.

## Load bench (N=5000, stm_cap=2, sticky 80% hot)

| metric | value |
|---|---|
| wall | ~0.7 s |
| per resolve | ~140 µs |
| stm_hit_rate | high under sticky traffic |
| ltm_fetch | few (cold path) |
| abstain | unknown names |
| form reject worse / promote better | gated |

## API

`include/cnet_mem_runtime.h` · `src/cnet_mem_runtime.c`
