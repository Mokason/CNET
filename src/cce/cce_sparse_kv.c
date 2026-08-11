#include "../../include/cce/cce_sparse_kv.h"

#include <stdio.h>
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

/* ---- streaming-aware budgeted index ------------------------------------ */

static void kv_ix_update_bytes(cce_kv_stream_index* ix) {
    size_t slot;
    if (!ix) return;
    slot = (size_t)(ix->k_slot_floats + ix->v_slot_floats) * sizeof(float);
    if (slot == 0) {
        /* abstract units: 1 per position */
        ix->bytes_full_kv = (size_t)ix->cursor;
        ix->bytes_index_kv = (size_t)ix->active_n;
        return;
    }
    ix->bytes_full_kv = (size_t)ix->cursor * slot;
    ix->bytes_index_kv = (size_t)ix->active_n * slot;
}

cce_result cce_kv_stream_index_init(cce_kv_stream_index* ix,
                                    const cce_specialist_kv_budget* budget,
                                    int legal_max) {
    if (!ix) return CCE_ERR_INVALID_ARG;
    memset(ix, 0, sizeof(*ix));
    if (legal_max <= 0) legal_max = CCE_KV_STREAM_IDX_MAX;
    if (legal_max > CCE_KV_STREAM_IDX_MAX) legal_max = CCE_KV_STREAM_IDX_MAX;
    ix->legal_max = legal_max;
    if (budget) {
        ix->budget = *budget;
    } else {
        cce_specialist_kv_budget_default(&ix->budget, legal_max);
    }
    if (ix->budget.max_tokens <= 0)
        ix->budget.max_tokens = clamp_int(legal_max / 5 + 1, 1, legal_max);
    if (ix->budget.max_tokens > CCE_KV_STREAM_IDX_MAX)
        ix->budget.max_tokens = CCE_KV_STREAM_IDX_MAX;
    return CCE_OK;
}

void cce_kv_stream_index_set_slot_sizes(cce_kv_stream_index* ix,
                                        int k_slot_floats, int v_slot_floats) {
    if (!ix) return;
    ix->k_slot_floats = k_slot_floats > 0 ? k_slot_floats : 0;
    ix->v_slot_floats = v_slot_floats > 0 ? v_slot_floats : 0;
    kv_ix_update_bytes(ix);
}

void cce_kv_stream_index_clear(cce_kv_stream_index* ix) {
    if (!ix) return;
    ix->cursor = 0;
    ix->active_n = 0;
    kv_ix_update_bytes(ix);
}

cce_result cce_kv_stream_index_rebuild(cce_kv_stream_index* ix,
                                       const float* attention_scores,
                                       int score_len) {
    int n, tc, cap;
    if (!ix) return CCE_ERR_INVALID_ARG;
    tc = ix->cursor;
    if (tc <= 0) {
        ix->active_n = 0;
        kv_ix_update_bytes(ix);
        return CCE_OK;
    }
    if (tc > CCE_KV_STREAM_IDX_MAX) tc = CCE_KV_STREAM_IDX_MAX;
    cap = ix->budget.max_tokens;
    if (cap > CCE_KV_STREAM_IDX_MAX) cap = CCE_KV_STREAM_IDX_MAX;
    if (cap > tc) cap = tc;
    /* Reuse selector — scores optional; if shorter than tc, pass NULL for fill */
    if (attention_scores && score_len < tc) attention_scores = NULL;
    if (cce_specialist_select_kv_tokens(attention_scores, tc, &ix->budget,
                                        ix->active, cap, &n) != CCE_OK)
        return CCE_ERR_INVALID_ARG;
    ix->active_n = n;
    ix->n_rebuilds++;
    if (attention_scores) ix->n_score_refresh++;
    kv_ix_update_bytes(ix);
    return CCE_OK;
}

cce_result cce_kv_stream_index_on_append(cce_kv_stream_index* ix, int pos,
                                         const float* attention_scores,
                                         int score_len) {
    if (!ix) return CCE_ERR_INVALID_ARG;
    if (pos < 0) return CCE_ERR_INVALID_ARG;
    /* streaming: positions should be contiguous 0..cursor */
    if (pos != ix->cursor) {
        /* allow restart or catch-up: set cursor to pos+1 after rebuild */
        if (pos + 1 > ix->legal_max) return CCE_ERR_INVALID_ARG;
        ix->cursor = pos + 1;
    } else {
        if (ix->cursor >= ix->legal_max) return CCE_ERR_INVALID_ARG;
        ix->cursor++;
    }
    ix->n_appends++;
    return cce_kv_stream_index_rebuild(ix, attention_scores, score_len);
}

cce_result cce_kv_stream_index_active(const cce_kv_stream_index* ix,
                                      int* out_indices, int out_cap,
                                      int* out_count) {
    int i, n;
    if (!ix || !out_indices || !out_count || out_cap <= 0)
        return CCE_ERR_INVALID_ARG;
    n = ix->active_n < out_cap ? ix->active_n : out_cap;
    for (i = 0; i < n; ++i) out_indices[i] = ix->active[i];
    *out_count = n;
    return CCE_OK;
}

int cce_kv_stream_index_contains(const cce_kv_stream_index* ix, int pos) {
    int lo, hi;
    if (!ix || pos < 0) return 0;
    lo = 0;
    hi = ix->active_n - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (ix->active[mid] == pos) return 1;
        if (ix->active[mid] < pos)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return 0;
}

float cce_kv_stream_index_mem_ratio(const cce_kv_stream_index* ix) {
    if (!ix || ix->bytes_full_kv == 0) return 1.0f;
    return (float)ix->bytes_index_kv / (float)ix->bytes_full_kv;
}

int cce_kv_stream_index_format(const cce_kv_stream_index* ix, char* buf,
                               size_t cap) {
    if (!ix || !buf || !cap) return -1;
    return snprintf(buf, cap,
                    "kv_stream_idx cursor=%d active=%d/%d budget=%d "
                    "mem_ratio=%.3f full_B=%zu idx_B=%zu appends=%llu rebuilds=%llu",
                    ix->cursor, ix->active_n, ix->legal_max, ix->budget.max_tokens,
                    cce_kv_stream_index_mem_ratio(ix), ix->bytes_full_kv,
                    ix->bytes_index_kv, (unsigned long long)ix->n_appends,
                    (unsigned long long)ix->n_rebuilds);
}
