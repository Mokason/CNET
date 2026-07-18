# CNET Micro-Trensor Kernel (MTK) integration — Phase 1

## Source lineage

| Project | What we took |
|---------|----------------|
| **Marble-s-BitnetMamba** `hot_swap` + `.tskill` (MTSK) | Shadow snapshot, apply/revert, sparse knowledge cartridges |
| **BonsaiRotor** micro-blocks + HotSwapLayer | LEGO op table idea; pointer-blind kernels |
| **CNET Forest** | Live weight residency (`{branch}.b{i}.w`) |

## What landed (Phase 1)

- `include/cce/cce_mtk.h` / `src/cce/cce_mtk.c`
- **CMSK** skill format (CNET Micro Skill): sparse f32 delta or overwrite
- `cce_mtk_bind_forest` — register all HOT cascade weights/biases
- `cce_mtk_apply_file` / `cce_mtk_revert` — knowledge swap without reload
- Kernel **vtable stub** (`cce_mtk_kernel_*`) for future LEGO op replace
- Gate: `make mtk` → **MTK_PASS** including **9/9 needles**

## API sketch

```c
cce_mtk *m;
cce_mtk_open(&m);
cce_mtk_bind_forest(m, forest);          /* or cce_mtk_register(...) */
cce_mtk_apply_file(m, "law.cmsk", 1.f); /* GPU idle — memcpy/scatter */
/* ... normal cascade / GGUF forward ... */
cce_mtk_revert(m);                       /* exact base restore */
cce_mtk_close(m);
```

## CMSK layout

```
u32 magic='CMSK'  u32 ver=1  u32 method  u32 n_tensors  u32 pad
per tensor:
  u16 name_len  u16 pad  u64 n_elem  u64 nnz
  char name[name_len]
  u32 idx[nnz]  f32 val[nnz]
```

`method`: 1 = DELTA (add), 2 = OVERWRITE.

## Resource / GPU posture

Apply/revert are **CPU buffer ops** (snapshot + sparse write). No model reload,
no full GPU rebind. Pair with `cnet_gov` eco profile: swap while IDLE, generate
only in GENERATE bursts.

## Later phases

| Phase | Work |
|-------|------|
| 2 | Read **MTSK/.tskill** ternary (BitnetMamba-compatible) → dequant to f32 sites |
| 3 | Wire kernel vtable into real GEMM/attn call sites |
| 4 | GGUF named-tensor registry (layer.q_proj …) for full-model skills |
| 5 | Auto-router (prompt → skill) + KV flush on swap |

## Gate

```bash
make mtk
```
