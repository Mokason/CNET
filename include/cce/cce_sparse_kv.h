#ifndef CCE_SPARSE_KV_H
#define CCE_SPARSE_KV_H

#include "cce_defs.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CCE_CONTEXT_ROUTING_FULL_KV = 0,
    CCE_CONTEXT_ROUTING_SPARSE_ROUTING = 1
} cce_context_routing_mode;

typedef struct cce_specialist_kv_budget {
    int specialist_id;
    int max_tokens;
    int initial_tokens;
    int recent_tokens;
    int long_range_stride;
    float heavy_hitter_fraction;
} cce_specialist_kv_budget;

typedef struct SparseKVSelector {
    cce_context_routing_mode mode;
    cce_specialist_kv_budget budget;
} SparseKVSelector;

void cce_specialist_kv_budget_default(cce_specialist_kv_budget* budget,
                                      int context_tokens);

cce_result cce_sparse_kv_selector_init(SparseKVSelector* selector,
                                       cce_context_routing_mode mode,
                                       const cce_specialist_kv_budget* budget,
                                       int context_tokens);

cce_result cce_specialist_select_kv_tokens(const float* attention_scores,
                                           int token_count,
                                           const cce_specialist_kv_budget* budget,
                                           int* out_indices,
                                           int out_cap,
                                           int* out_count);

/* =========================================================================
 * Streaming-aware KV index (budgeted reads)
 * -------------------------------------------------------------------------
 * Compact index while tokens stream. Attend only |active| ≤ budget positions
 * → less memory traffic + faster decode on long ctx.
 *
 * Policy: sink/prefix + recent + stride landmarks + optional heavy-hitters.
 * Pure C. Does not self-CERT.
 * ========================================================================= */

#define CCE_KV_STREAM_IDX_MAX 4096

typedef struct cce_kv_stream_index {
    cce_specialist_kv_budget budget;
    int legal_max;
    int cursor; /* length so far (= next pos) */
    int active_n;
    int active[CCE_KV_STREAM_IDX_MAX]; /* sorted unique positions */
    uint64_t n_appends;
    uint64_t n_rebuilds;
    uint64_t n_score_refresh;
    size_t bytes_full_kv;
    size_t bytes_index_kv;
    int k_slot_floats;
    int v_slot_floats;
} cce_kv_stream_index;

cce_result cce_kv_stream_index_init(cce_kv_stream_index* ix,
                                    const cce_specialist_kv_budget* budget,
                                    int legal_max);

void cce_kv_stream_index_set_slot_sizes(cce_kv_stream_index* ix,
                                        int k_slot_floats, int v_slot_floats);

void cce_kv_stream_index_clear(cce_kv_stream_index* ix);

/* Streaming step: token `pos` just written. scores may be NULL. */
cce_result cce_kv_stream_index_on_append(cce_kv_stream_index* ix, int pos,
                                         const float* attention_scores,
                                         int score_len);

cce_result cce_kv_stream_index_rebuild(cce_kv_stream_index* ix,
                                       const float* attention_scores,
                                       int score_len);

cce_result cce_kv_stream_index_active(const cce_kv_stream_index* ix,
                                      int* out_indices, int out_cap,
                                      int* out_count);

int cce_kv_stream_index_contains(const cce_kv_stream_index* ix, int pos);

/* index_bytes / full_bytes; 1.0 if full==0 */
float cce_kv_stream_index_mem_ratio(const cce_kv_stream_index* ix);

int cce_kv_stream_index_format(const cce_kv_stream_index* ix, char* buf,
                               size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* CCE_SPARSE_KV_H */
