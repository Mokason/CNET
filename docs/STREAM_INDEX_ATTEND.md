# Stream index in live GGUF attend

## Structure choice

**Pre-attention block mask on the HOT support set** — not layer-by-layer mid-GEMM filtering of QK products.

```text
write K/V for pos t
     │
     ▼
stream_ix (optional) marks active subset under budget
     │
     ▼
for each layer/head:
  score all j in [jmin, t]   (or skip compute later)
  if stream_ix bound:
      scores[j] = -inf  for j not in active and j != t
  softmax → V gather     ← only active mass survives
```

| Approach | Verdict |
|----------|---------|
| **Pre-attention mask (chosen)** | One policy for all layers; matches stream index + sparse_kv spirit; simple |
| Mid-GEMM per-layer filter | Different support per layer; harder to reason; more branches in hot loop |

DSA / `CNET_SPARSE_KV` remains a separate opt-in path (lightning index).  
Stream index mask applies on the **dense** path when `stream_ix` is bound.

## API

```c
cce_kv_stream_index ix;
cce_kv_stream_index_init(&ix, &budget, legal_max);
cce_gguf_qwen2_bind_stream_index(model, &ix);
/* host: after each decode token, cce_kv_stream_index_on_append(&ix, pos, ...) */
/* MTK flush clears stream_ix with weight_epoch */
```

## Epoch

On `.tskill` apply/revert, `cce_mtk_gguf_kv_flush` clears `stream_ix` with HOT.
