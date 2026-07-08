#ifndef CCE_SPARSE_KV_H
#define CCE_SPARSE_KV_H

#include "cce_defs.h"

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

#ifdef __cplusplus
}
#endif

#endif /* CCE_SPARSE_KV_H */
