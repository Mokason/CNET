#ifndef CCE_KV_PAGE_H
#define CCE_KV_PAGE_H

/*
 * Async paged KV (Forest-aligned hot / warm / cold)
 * =================================================
 *   HOT  – ring of preallocated pages (write + attend window)
 *   WARM – stage while async flush runs (double-buffer)
 *   COLD – on-disk page + ledger; optional int8 quant pack
 *
 * Optional COLD rehydrate brings a cold page back for long-range attend.
 * Env: CNET_KV_PAGE_LEN, CNET_KV_HOT_PAGES, CNET_KV_ARCHIVE, CNET_KV_ASYNC=0,
 *      CNET_KV_QUANT=1 (int8 cold), CNET_KV_REHYDRATE=1
 */

#include "cce_defs.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CCE_KV_TIER_HOT = 0,
    CCE_KV_TIER_WARM = 1,
    CCE_KV_TIER_COLD = 2
} cce_kv_tier;

typedef struct cce_kv_pager cce_kv_pager;

typedef struct cce_kv_pager_opts {
    int         page_len;
    int         n_hot;
    int         k_slot;
    int         v_slot;
    int         legal_max;
    const char *archive_dir;
    int         async;
    int         quant_cold;   /* 1: store COLD as int8 + scales */
    int         rehydrate;    /* 1: allow loading COLD back into scratch */
} cce_kv_pager_opts;

void cce_kv_pager_opts_default(cce_kv_pager_opts *o, int k_slot, int v_slot,
                               int legal_max);

cce_result cce_kv_pager_open(cce_kv_pager **out, const cce_kv_pager_opts *opts);
void       cce_kv_pager_close(cce_kv_pager *p);

int cce_kv_pager_hot_capacity(const cce_kv_pager *p);
int cce_kv_pager_window_start(const cce_kv_pager *p);
int cce_kv_pager_cur_pos(const cce_kv_pager *p);
int cce_kv_pager_legal_max(const cce_kv_pager *p);
int cce_kv_pager_page_len(const cce_kv_pager *p);
int cce_kv_pager_quant_cold(const cce_kv_pager *p);
int cce_kv_pager_rehydrate_enabled(const cce_kv_pager *p);

int cce_kv_pager_prepare_write(cce_kv_pager *p, int pos);

/* HOT row only (NULL if cold). */
float *cce_kv_pager_k_row(cce_kv_pager *p, int pos);
float *cce_kv_pager_v_row(cce_kv_pager *p, int pos);

/* Prefer HOT; if cold and rehydrate on, load page into internal buffer and
 * return pointers valid until next rehydrate/slide. */
const float *cce_kv_pager_k_row_ex(cce_kv_pager *p, int pos);
const float *cce_kv_pager_v_row_ex(cce_kv_pager *p, int pos);

int cce_kv_pager_write(cce_kv_pager *p, int pos, const float *k, const float *v);

/* Write a slice into the position row (layer k_off/v_off). */
int cce_kv_pager_write_slice(cce_kv_pager *p, int pos, size_t k_off,
                             const float *k, int k_dim, size_t v_off,
                             const float *v, int v_dim);

int cce_kv_pager_clamp_jmin(const cce_kv_pager *p, int jmin);

void cce_kv_pager_sync(cce_kv_pager *p);

int cce_kv_pager_pages_flushed(const cce_kv_pager *p);
int cce_kv_pager_pages_reused(const cce_kv_pager *p);
int cce_kv_pager_pages_rehydrated(const cce_kv_pager *p);
int cce_kv_pager_queue_depth(const cce_kv_pager *p);

int cce_kv_pager_verify_cold(const cce_kv_pager *p, int page_id,
                             uint64_t expect_digest);

/* Force rehydrate of the cold page containing pos (0 ok). */
int cce_kv_pager_rehydrate_pos(cce_kv_pager *p, int pos);

#ifdef __cplusplus
}
#endif

#endif /* CCE_KV_PAGE_H */
