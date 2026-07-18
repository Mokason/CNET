# DeepSeek Multi-head Latent Attention (MLA)

## Status
Full CCE engine: `include/cce/cce_mla.h`, `src/cce/cce_mla.c`  
Gate: `make mla` → **MLA_PASS**

## Algorithm (DeepSeek-V2/V3)
1. **Compress** hidden `h` → latent `c^{KV}` (rank `kv_lora_rank`) + shared `k^R` (rope dim)
2. **Cache** only `(c^{KV}, k^R)` per token — not full multi-head K/V
3. **Query** `q^C`, `q^R` (optional q_lora down-proj first); RoPE on `q^R` only
4. **Attend**: scores = `q^C·(W^{UK} c)` + `q^R·k^R` (absorb path avoids materializing K history)
5. **Values**: `v = W^{UV} c` per head, weighted sum → `W^O`

## Memory (per token per layer)
| Path | Bytes (example n_h=4, d=48, rank=64, rope=16) |
|------|-----------------------------------------------|
| Dense MHA | `2 * n_h * d_head * 4` |
| **MLA** | `(kv_lora_rank + qk_rope) * 4` |

Often **5–50×** smaller than dense MHA at DeepSeek scale.

## API
```c
cce_mla_config_default(&cfg, d_model, n_heads);
cce_mla_weights_alloc_synthetic(&w, &cfg, seed); /* hermetic */
cce_mla_init(&m, &cfg, &w, max_ctx);
cce_mla_forward_seq(&m, x, T, y);  /* prefill */
cce_mla_forward_token(&m, x_t, pos, y_t); /* decode */
```

## With DSA (single-user)
- MLA compresses **what** is stored
- DSA selects **which** latents to attend (top-k on lightning index over expanded or latent scores)
- Combine: long context fits + sparse attend = local tok/s path

## Loading real DeepSeek GGUF (next)
Tensor names (llama.cpp / HF style) for a full loader:
- `blk.N.attn_kv_a_mqa.weight` → W_DKV (+ k_rope)
- `blk.N.attn_kv_a_norm.weight`
- `blk.N.attn_kv_b.weight` → split W_UK | W_UV
- `blk.N.attn_q_a` / `attn_q_b` when q_lora_rank > 0
- metadata: `kv_lora_rank`, `q_lora_rank`, `qk_nope_head_dim`, `qk_rope_head_dim`, `v_head_dim`

Hermetic synthetic weights prove the math today without a 671B download.

## Env
```bash
# engine is always available via C API / make mla
# future: CNET_MLA=1 on deepseek2 GGUF selects this attention path
```
