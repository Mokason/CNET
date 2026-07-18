# Async paged KV (hot / warm / cold context memory)

## Why

Qwythos declares **1M** context. CNET used to `calloc(max_ctx × slots)` and
hard-cap at **8192** so short probes would not OOM. That fights Forest
residency: we already load weights only when needed (HOT/WARM/COLD).

Context must follow the same rule:

| Tier | Meaning |
|------|---------|
| **HOT** | Ring of preallocated pages (write + attend window) |
| **WARM** | Evicted page copied to stage while async flush runs |
| **COLD** | Immutable on-disk page + ledger line (hash verified) |

**Endless generation** = rolling HOT window + permanent COLD archive  
(documented, not re-hallucinated). Legal positions up to model 1M; RAM = O(hot).

## Double-buffer async

```
token stream → write HOT page N
                    │
                    ├─ while filling N, background thread flushes N-k (WARM→COLD)
                    └─ free ring slot reused for N+1  (pointer pool, no thrash)
```

## API

`include/cce/cce_kv_page.h` / `src/cce/cce_kv_page.c`

```c
cce_kv_pager_open(&p, &opts);          /* legal_max=1M, hot=4*256 */
cce_kv_pager_write(p, pos, k, v);      /* slides + enqueues flush */
cce_kv_pager_k_row(p, pos);            /* NULL if cold */
cce_kv_pager_clamp_jmin(p, jmin);      /* rolling attend */
cce_kv_pager_sync(p);                  /* drain queue */
cce_kv_pager_verify_cold(p, id, dig);  /* anti-hallucination check */
```

Env: `CNET_KV_PAGE_LEN`, `CNET_KV_HOT_PAGES`, `CNET_KV_ARCHIVE`, `CNET_KV_ASYNC=0`.

## Gate

```bash
make kv_page   # KV_PAGE_PASS
```

## Next wire

1. GGUF/qwen35: `legal_max = context_length` (1M); replace dense `k_cache` writes with pager.
2. Attention: `jmin = clamp_jmin(...)` for rolling full-attn layers.
3. Optional COLD rehydrate for true long-range attend.
4. int8/ternary pack for COLD pages (quality gate).

## Non-goals (this slice)

- Full 1M dense attention every step  
- Replacing hybrid GDN state protocol  
- Python / llama.cpp paging  
