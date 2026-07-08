#include "../../include/cce/cce_sparse_kv.h"

#include <stdlib.h>
#include <string.h>

static int clamp_int(int x, int lo, int hi) {
    if (x < lo) return lo;
    if (x > hi) return hi;
    return x;
}

static int mark_token(unsigned char* selected, int token_count, int idx, int* count) {
    if (idx < 0 || idx >= token_count) return 0;
    if (selected[idx]) return 0;
    selected[idx] = 1;
    ++(*count);
    return 1;
}

void cce_specialist_kv_budget_default(cce_specialist_kv_budget* budget,
                                      int context_tokens) {
    if (!budget) return;
    memset(budget, 0, sizeof(*budget));
    if (context_tokens <= 0) context_tokens = 1;

    int target = (context_tokens + 4) / 5; /* roughly 20%, rounded up */
    budget->specialist_id = -1;
    budget->max_tokens = clamp_int(target, 1, context_tokens);
    budget->initial_tokens = clamp_int(4, 1, budget->max_tokens);
    budget->recent_tokens = clamp_int(context_tokens < 32 ? context_tokens / 4 + 1 : 32,
                                      1, budget->max_tokens);
    budget->long_range_stride = context_tokens > 64 ? 64 : 8;
    budget->heavy_hitter_fraction = 1.0f;
}

cce_result cce_sparse_kv_selector_init(SparseKVSelector* selector,
                                       cce_context_routing_mode mode,
                                       const cce_specialist_kv_budget* budget,
                                       int context_tokens) {
    if (!selector) return CCE_ERR_INVALID_ARG;
    selector->mode = mode;
    if (budget) {
        selector->budget = *budget;
    } else {
        cce_specialist_kv_budget_default(&selector->budget, context_tokens);
    }
    return CCE_OK;
}

cce_result cce_specialist_select_kv_tokens(const float* attention_scores,
                                           int token_count,
                                           const cce_specialist_kv_budget* budget,
                                           int* out_indices,
                                           int out_cap,
                                           int* out_count) {
    if (!out_indices || !out_count || token_count <= 0 || out_cap <= 0)
        return CCE_ERR_INVALID_ARG;

    *out_count = 0;

    cce_specialist_kv_budget local;
    if (budget) {
        local = *budget;
    } else {
        cce_specialist_kv_budget_default(&local, token_count);
    }

    int target = local.max_tokens > 0 ? local.max_tokens : token_count;
    target = clamp_int(target, 1, token_count);
    target = clamp_int(target, 1, out_cap);

    if (target >= token_count) {
        for (int i = 0; i < token_count && i < out_cap; ++i) {
            out_indices[i] = i;
        }
        *out_count = token_count < out_cap ? token_count : out_cap;
        return CCE_OK;
    }

    unsigned char* selected = (unsigned char*)calloc((size_t)token_count, 1);
    if (!selected) return CCE_ERR_OOM;

    int count = 0;
    int initial = clamp_int(local.initial_tokens, 0, target);
    for (int i = 0; i < initial && count < target; ++i) {
        mark_token(selected, token_count, i, &count);
    }

    int recent = clamp_int(local.recent_tokens, 0, target);
    int recent_start = token_count - recent;
    if (recent_start < 0) recent_start = 0;
    for (int i = recent_start; i < token_count && count < target; ++i) {
        mark_token(selected, token_count, i, &count);
    }

    int stride = local.long_range_stride;
    if (stride > 0) {
        for (int i = stride; i < token_count - recent && count < target; i += stride) {
            mark_token(selected, token_count, i, &count);
        }
    }

    while (count < target && attention_scores) {
        int best = -1;
        float best_score = -3.402823466e+38f;
        for (int i = 0; i < token_count; ++i) {
            if (selected[i]) continue;
            if (attention_scores[i] > best_score) {
                best_score = attention_scores[i];
                best = i;
            }
        }
        if (best < 0) break;
        mark_token(selected, token_count, best, &count);
    }

    if (count < target) {
        int gap = token_count / (target + 1);
        if (gap <= 0) gap = 1;
        for (int i = gap; i < token_count && count < target; i += gap) {
            mark_token(selected, token_count, i, &count);
        }
        for (int i = 0; i < token_count && count < target; ++i) {
            mark_token(selected, token_count, i, &count);
        }
    }

    int out = 0;
    for (int i = 0; i < token_count && out < out_cap; ++i) {
        if (selected[i]) out_indices[out++] = i;
    }
    *out_count = out;
    free(selected);
    return CCE_OK;
}
