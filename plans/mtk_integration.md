# CNET Micro-Trensor Kernel (MTK) integration

## Source lineage

| Project | Contribution |
|---------|----------------|
| **Marble-s-BitnetMamba** `hot_swap` + MTSK | Shadow apply/revert, ternary skill layout |
| **BonsaiRotor** micro-blocks | LEGO kernel slots |
| **CNET Forest / GGUF** | Live weight residency |

## Status

| Phase | Status | Notes |
|-------|--------|--------|
| 1 CMSK + Forest bind | **done** | 9/9 needles, exact revert |
| 2 MTSK/.tskill reader | **done** | BitnetMamba-compatible mask+2bit vals → f32 |
| 3 Kernel vtable → block | **done** | `cce_block_set_linear_hook` + GEMM slot |
| 4 GGUF registry | **done** | `bind_gguf`: tok_emb, output, norms, forest |
| 5 Auto-router + KV flush | **done** | routes file + `cce_mtk_gguf_kv_flush` |

## API

```c
cce_mtk *m;
cce_mtk_open(&m);
cce_mtk_bind_gguf(m, model);          /* or bind_forest */
cce_mtk_set_kv_flush(m, cce_mtk_gguf_kv_flush, model);
cce_mtk_router_load(m, "routes.txt");
cce_mtk_router_apply(m, user_prompt, 1.f);  /* or apply_file */
/* forward ... */
cce_mtk_revert(m);
cce_mtk_close(m);

cce_mtk_install_block_hook();  /* optional LEGO linear probe / takeover */
```

## Formats

- **CMSK** — sparse f32 delta/overwrite (native)
- **MTSK** — `MTSK` magic, bitmask + 2-bit ternary pack (BitnetMamba)

## Routes file

```
# path<whitespace>kw1,kw2
skills/code.cmsk  code,python,function
skills/math.cmsk  integral,derivative
```

## Resource posture

Apply/revert = CPU snapshot + sparse write. KV flush clears GGUF `cur_pos` /
dense cache so skill changes do not contaminate attention history.
Pair with `CNET_GOV_PROFILE=eco` for duty-cycle GPU.

## Gate

```bash
make mtk   # MTK_PASS, 9/9 needles, failures=0
```
