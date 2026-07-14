/*
 * cce_aicimo.h — Compatibility shim.
 *
 * The canonical public API now lives in include/cce/cce_aicimo.h.
 * This shim provides backward-compatible type and function name mappings
 * so existing code that includes "../src/cce/cce_aicimo.h" continues to
 * compile against the new API.
 */
#ifndef CCE_AICIMO_H_COMPAT
#define CCE_AICIMO_H_COMPAT

#include "../../include/cce/cce_aicimo.h"

#include <stdlib.h>
#include <string.h>

/* Backward-compatible type aliases */
typedef cce_aicimo_adapter AicimoAdapter;
typedef cce_aicimo_adapter_bank AicimoAdapterBank;
typedef cce_aicimo_strength_matrix AicimoStrengthMatrix;
typedef cce_aicimo_router AicimoRouter;

/* Backward-compatible function wrappers (old API returned int 0/-1) */

static inline int aicimo_router_init(AicimoRouter *r, size_t num_ops, size_t base_dim) {
    return (cce_aicimo_router_init(r, num_ops, base_dim) == CCE_OK) ? 0 : -1;
}

static inline int aicimo_router_init_variable(AicimoRouter *r, size_t num_ops, size_t base_dim) {
    return aicimo_router_init(r, num_ops, base_dim);
}

static inline void aicimo_router_free(AicimoRouter *r) {
    cce_aicimo_router_free(r);
}

static inline int aicimo_route(AicimoRouter *r, const float *input, size_t in_len,
                               float *output, size_t out_cap, size_t *used_ops) {
    return (cce_aicimo_route(r, input, in_len, output, out_cap, used_ops) == CCE_OK) ? 0 : -1;
}

/*
 * aicimo_compose — Apply routing multiple times.
 *
 * NOTE: This performs repeated adapter routing, NOT context expansion.
 * The output has the same dimensionality as the input. The "steps" count
 * controls how many times the routing is applied; with identity-initialized
 * adapters and default strengths, the output equals the input.
 */
static inline int aicimo_compose(AicimoRouter *r, const float *input, size_t in_len,
                                 float *output, size_t out_cap) {
    if (!r || !input || !output) return -1;
    if (in_len != r->base_dim || out_cap < in_len) return -1;

    float *tmp = (float *)malloc(in_len * sizeof(float));
    if (!tmp) return -1;

    memcpy(tmp, input, in_len * sizeof(float));

    for (size_t step = 0; step < 16; ++step) {
        size_t used = 0;
        if (cce_aicimo_route(r, tmp, in_len, output, out_cap, &used) != CCE_OK) {
            free(tmp);
            return -1;
        }
        memcpy(tmp, output, in_len * sizeof(float));
    }

    memcpy(output, tmp, in_len * sizeof(float));
    free(tmp);
    return 0;
}

#endif /* CCE_AICIMO_H_COMPAT */