#ifndef CCE_KV_PAGE_H
#define CCE_KV_PAGE_H

/*
 * Async paged KV (Forest-aligned hot / warm / cold)
 * =================================================
 * Weights already use HOT/WARM/COLD leaves. Context memory should too:
 *
 *   HOT  – ring of preallocated pages (live write + attend window)
 *   WARM – just-evicted page still in RAM while async flush runs
 *   COLD – documented on-disk page + ledger line (immutable, hash-checked)
 *
 * Double-buffer: while token N writes into a hot page, a previous page is
 * saved on a background thread. Pointers are reused from a fixed pool —
 * no per-token malloc, no full max_ctx dense slab.
 *
 * "Endless" generation = rolling hot window + permanent cold archive
 * (documented, not re-hallucinated). Legal positions may go to model 1M;
 * RAM stays O(hot_pages * page_len).
 *
 * Env:
 *   CNET_KV_PAGE=1          enable (callers)
 *   CNET_KV_PAGE_LEN=256    positions per page
 *   CNET_KV_HOT_PAGES=4     pages kept hot in the ring
 *   CNET_KV_ARCHIVE=dir     cold page + ledger directory
 */

#include "cce_defs.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CCE_KV_TIER_HOT = 0,
    CCE_KV_TIER_WARM = 1, /* queued / flushing */
    CCE_KV_TIER_COLD = 2  /* on disk + ledger */
} cce_kv_tier;

typedef struct cce_kv_pager cce_kv_pager;

typedef struct cce_kv_pager_opts {
    int         page_len;     /* default 256 */
    int         n_hot;        /* default 4  → hot window = page_len*n_hot */
    int         k_slot;       /* floats per position K */
    int         v_slot;       /* floats per position V */
    int         legal_max;    /* model context_length (e.g. 1M) */
    const char *archive_dir;  /* NULL → ./kv_archive */
    int         async;        /* 1 = background flush (default) */
} cce_kv_pager_opts;

void cce_kv_pager_opts_default(cce_kv_pager_opts *o, int k_slot, int v_slot,
                               int legal_max);

cce_result cce_kv_pager_open(cce_kv_pager **out, const cce_kv_pager_opts *opts);
void       cce_kv_pager_close(cce_kv_pager *p);

/* Hot window size in positions (page_len * n_hot). */
int cce_kv_pager_hot_capacity(const cce_kv_pager *p);
int cce_kv_pager_window_start(const cce_kv_pager *p);
int cce_kv_pager_cur_pos(const cce_kv_pager *p);
int cce_kv_pager_legal_max(const cce_kv_pager *p);

/* Ensure logical pos is writable in the hot ring; may slide window and
 * enqueue async cold flush of the evicted page. Returns 0 ok, -1 fail. */
int cce_kv_pager_prepare_write(cce_kv_pager *p, int pos);

/* Row pointers for logical pos if it is currently HOT (or WARM still in ring).
 * NULL if cold / out of window — caller must not attend (rolling) or rehydrate. */
float *cce_kv_pager_k_row(cce_kv_pager *p, int pos);
float *cce_kv_pager_v_row(cce_kv_pager *p, int pos);

/* Write K/V row at pos (copies k_slot/v_slot floats). prepare_write first. */
int cce_kv_pager_write(cce_kv_pager *p, int pos, const float *k, const float *v);

/* Attention lower bound: max(jmin, window_start) for rolling hot-only attend. */
int cce_kv_pager_clamp_jmin(const cce_kv_pager *p, int jmin);

/* Block until flush queue drains (tests / shutdown). */
void cce_kv_pager_sync(cce_kv_pager *p);

/* Telemetry */
int cce_kv_pager_pages_flushed(const cce_kv_pager *p);
int cce_kv_pager_pages_reused(const cce_kv_pager *p);
int cce_kv_pager_queue_depth(const cce_kv_pager *p);

/* Verify a cold page file against ledger digest (no hallucination check). */
int cce_kv_pager_verify_cold(const cce_kv_pager *p, int page_id,
                             uint64_t expect_digest);

#ifdef __cplusplus
}
#endif

#endif /* CCE_KV_PAGE_H */
