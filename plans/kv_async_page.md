# Async paged KV (hot / warm / cold context memory)

## Why

Qwythos declares **1M** context. CNET used to `calloc(max_ctx × slots)` and
hard-cap at **8192** so short probes would not OOM. That fights Forest
residency: we already load weights only when needed (HOT/WARM/COLD).

**CNET ceiling is `CNET_CTX_LEGAL_MAX` (1 048 576).** With `CNET_KV_PAGE=1`
the pager opens 1M legal positions. HOT RAM stays `page_len × n_hot`.
Dense mode still caps the f32 slab at 8192. `CNET_CTX_LEGAL` can set
8…1048576. Past a GGUF's own `context_length`, quality is the model's
problem — positions are still legal.

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

## Wired (GGUF / qwen35)

1. `cce_gguf_qwen2_enable_kv_page` when `CNET_KV_PAGE=1` — free dense slabs, pager owns HOT.
2. Writes via `cce_gguf_qwen2_kv_write_slice`; reads via `k_row_ex` / `v_row_ex`.
3. Attention `jmin = cce_gguf_qwen2_kv_jmin` (rolling HOT).
4. `CNET_KV_REHYDRATE=1` loads COLD pages (int8 dequant) for long-range attend.
5. COLD default **int8 quant pack** (`CNET_KV_QUANT=0` for f32 cold).

## Env

```bash
CNET_KV_PAGE=1
CNET_CTX_LEGAL=1048576   # default when paged; 8..1048576
CNET_KV_PAGE_LEN=256
CNET_KV_HOT_PAGES=4
CNET_KV_ARCHIVE=./kv_archive
CNET_KV_ASYNC=1
CNET_KV_QUANT=1
CNET_KV_REHYDRATE=1   # optional long-range
CNET_HELD_N_CTX=1048576  # in-process holder; llama-server is separate
```

## Non-goals

- Full 1M dense attention every step without rehydrate cost  
- Ternary COLD (int8 first; ternary later with quality gate)  
- Python / llama.cpp paging  
- Device residual-stream KV while host pager is on (dense GPU cache cannot
  slide with HOT/COLD; host `k_row_ex` path is used instead)

## Status (wired)

| Point | Status |
|-------|--------|
| 1 GGUF write/read via pager | `kv_write_slice` / `k_row_ex` / `v_row_ex` |
| 2 Attention jmin → HOT | `cce_gguf_qwen2_kv_jmin` + clamp |
| 3 Optional COLD rehydrate | `CNET_KV_REHYDRATE=1` |
| 4 COLD int8 quant pack | CVK2 + `CNET_KV_QUANT` (default on) |

`max_ctx` stays the operational hot/ops bound (scores, MLA, GPU bind).
`kv_legal_max` holds model legal length (e.g. 1M). Generation may advance
past `max_ctx` when the pager is open; attention attends the HOT window
(or rehydrated COLD pages when enabled).  

