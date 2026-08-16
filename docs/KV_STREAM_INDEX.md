# Streaming-aware KV index (C)

Budgeted KV **reads** while tokens stream — less memory traffic, faster long-ctx decode.

## Stack (already C)

| Layer | Role |
|-------|------|
| `cce_specialist_select_kv_tokens` | one-shot sparse pick under budget |
| **`cce_kv_stream_index`** | **streaming index** rebuilt each append |
| `cce_kv_pager` | HOT/WARM/COLD page residency (`CNET_KV_PAGE`) |
| `cce_gguf_qwen2_set_sparse_kv` | wire fraction into attention |

## API

```c
cce_kv_stream_index ix;
cce_specialist_kv_budget b;
cce_specialist_kv_budget_default(&b, 4096);
b.max_tokens = 256; /* hard read budget */
cce_kv_stream_index_init(&ix, &b, 4096);
cce_kv_stream_index_set_slot_sizes(&ix, k_dim, v_dim);

for (pos = 0; pos < n; ++pos) {
    /* write KV row for pos, then: */
    cce_kv_stream_index_on_append(&ix, pos, scores_or_NULL, pos + 1);
    /* attend only ix.active[0..active_n) */
}
float save = 1.0f - cce_kv_stream_index_mem_ratio(&ix);
```

## Policy (active set)

1. **Sink / prefix** (`initial_tokens`)  
2. **Recent window** (`recent_tokens`) — streaming tip  
3. **Stride landmarks** (`long_range_stride`)  
4. **Heavy hitters** from last attention scores (optional)

## Commands

```bash
make sparse_kv_test    # includes CCE_KV_STREAM_INDEX_PASS
make kv_stream_index
```

## Law

Index chooses **which rows to read** — never seals truth / CERT.
