# Forest MTP — parallel multi-token verify

## Problem

Sequential verify paid **one full MLA cache snapshot per accept** and
**one kernel-class launch tax per token**. With `main_steps/token = 1`,
draft was pure overhead → ~0.78–0.90× baseline.

## Design

| Piece | Behavior |
|-------|----------|
| **Draft** | Medusa-lite: head-0 = main head (token-0 always accepts); heads 1..k-1 on a cheap residual walk. One draft step per burst (`CNET_MTP_PARALLEL=1`). |
| **Snap** | `draft_layers==0`: **residual-only** (draft never touches MLA). Full MLA snap only if draft runs real layers. |
| **Verify** | Causal online Leviathan loop, **no per-accept snaps**. |
| **Launch fuse** | Optional `CNET_MTP_SIM_LAUNCH` busy-work models GPU kernel launch. Baseline pays once/token; MTP pays **once per verify burst** (amortized when multi-accept). |

## Env

```bash
CNET_MTP_K=2
CNET_MTP_PARALLEL=1          # default; 0 = sequential residual-walk draft
CNET_MTP_SIM_LAUNCH=1000000  # 0 = off (CPU-only FLOP fair); >0 = GPU-like tax
```

## Gate / bench

```bash
make mtp_spec    # correctness
make mtp_bench   # sweep k∈{1,2,4} × parallel on/off with SIM_LAUNCH
```

Measured (n=128, 4L/d256/MoE8, wall clock):

| k | parallel | SIM_LAUNCH | speedup |
|---|----------|------------|---------|
| 2 | 1 | 1e6 | **1.37×** |
| 4 | 1 | 1e6 | **1.42×** |
| 2 | 1 | 0 | ~0.90× (CPU FLOP-only; trunk dominates) |

Launch amortization wins when multi-token bursts are long
(`draft_steps ≈ n/k` with high agree).

## Non-goals

- Tree attention / true batched prefill GEMMs (token-major residual host)
- Trusting draft without main trunk (integrity)
- Replacing dual-GPU EP place (separate branch)
