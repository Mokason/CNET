# Sparse adaptive hash board laws → CNET (integration order)

**Status:** slices 1–5 complete.  
**Date:** 2026-08-01  
**Non-negotiable:** never lower CNET certification floors; CNU1 + coverage remain the portable object.

## Decision

Integrate **Brain board laws** into main CNET as **serve / mine policy**, not as a second capsule format.

| Keep in CNET | Take from Brain |
|---|---|
| CNU1 + coverage + behavior digest | Sparse top-M / rank-not-fire-all |
| `specialist_admit` door | Center-primary among CERT candidates |
| Fail-closed abstain | Hash as **index**, geometry decides |
| External teacher only | STM→LTM = provisional→frozen after verify |
| AMD/ROCm GPU lanes | Residual same-domain add (later) |

Brain remains lab + optional continuous/runtime host (`pieces.bin`, `cnet_capsule_step`).

## Order (gate-closed)

| # | Slice | Gate stop |
|---|---|---|
| **1** | This ADR | docs only | **DONE** |
| **2** | Center-rank CERT candidates that **admit** coverage (read path) | `coverage_abstain` + `sparse_serve` | **DONE** (`SPARSE_SERVE_PASS`) |
| **3** | Mine/admit: recall-before-spawn + promote-only-after-verify | `knowledge_capsule`, `sparse_mine` | **DONE** (`SPARSE_MINE_PASS`) |
| **4** | Residual LINK_ACCUM-style same-domain hop | `sparse_residual` (+ composition bench unchanged) | **DONE** (`SPARSE_RESIDUAL_PASS`) |
| **5** | Optional continuous sidecar (Brain pieces) | `brain_sidecar` | **DONE** (`BRAIN_SIDECAR_PASS`) |

## Serve law (slice 2)

```
candidates = coverage records that admit(input)   # fail-closed membership
if empty → ABSTAIN
else pick argmin score:
  score = mean_L2(input, coverage_rows)   # center geometry
  (exact row match → score 0)
never: open-world scan of all units ignoring coverage
never: hash alone as quality path
```

Hash multiprobe may **narrow** candidates later; geometry + coverage remain decisive.

## Explicit non-goals

- Replace CNU1 with Brain float `.cap`
- Lower floors so continuous W packs into CNU1
- Bare multiprobe serve as product default
- Equate Brain verify-receipt with CNET cert suites

## Evidence from Brain (why this shape)

- Center/CERT serve + deploy freeze: board == warm snap MSE  
- Bare multiprobe: MSE ~4, mode ~0.28  
- Cold quality (centers, no pieces): MSE ~0.19, mode ~0.995  

## Implementation map

| Artifact | Role |
|---|---|
| `include/cnet_sparse_serve.h` | API |
| `src/cnet_sparse_serve.c` | rank/pick among admitting coverage |
| `tests/test_sparse_serve.c` | unit gate |
| `make sparse_serve` | RED→GREEN |

PersonalAi / MoE may call `cnet_sparse_pick_covered_unit` behind a flag later; slice 2 is policy + API first.
