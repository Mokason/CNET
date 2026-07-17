/*
 * cce_aicimo_bridge.c — Bridge implementation for AICIMO adapter routing.
 *
 * Wraps the canonical cce_aicimo.h API in convenient single-call semantics.
 *
 * The functions below perform adapter routing through identity-initialized
 * blocks. The output buffer has the same dimensionality as the input.
 * This is NOT context expansion — it is adapter routing. The function names
 * retain "expand_context" for backward API compatibility only.
 */
#include "cce_aicimo_bridge.h"
#include "../../include/cce/cce_aicimo.h"

#include <string.h>

int cce_aicimo_expand_context(const float *input, size_t in_len,
                              float *output, size_t out_cap,
                              size_t base_dim) {
    if (!input || !output) return -1;
    if (in_len != base_dim || out_cap < in_len) return -1;

    cce_aicimo_router router;
    if (cce_aicimo_router_init(&router, 8, base_dim) != CCE_OK) return -1;

    size_t used = 0;
    cce_result rc = cce_aicimo_route(&router, input, in_len, output, out_cap, &used);

    cce_aicimo_router_free(&router);
    return (rc == CCE_OK) ? 0 : -1;
}

int cce_aicimo_expand_context_with_uncertainty(const float *input, size_t in_len,
                                               float *output, size_t out_cap,
                                               size_t base_dim,
                                               float *uncertainty) {
    if (!input || !output || !uncertainty) return -1;
    if (in_len != base_dim || out_cap < in_len) return -1;

    cce_aicimo_router router;
    if (cce_aicimo_router_init(&router, 8, base_dim) != CCE_OK) return -1;

    cce_result rc = cce_aicimo_route_with_uncertainty(&router, input, in_len,
                                                       output, out_cap, uncertainty);

    cce_aicimo_router_free(&router);
    return (rc == CCE_OK) ? 0 : -1;
}

int cce_aicimo_route_on_role(const float *input, size_t in_len,
                             float *output, size_t out_cap,
                             size_t base_dim, const char *role,
                             float *uncertainty) {
    if (!input || !output || !role || !uncertainty) return -1;
    if (in_len != base_dim || out_cap < in_len) return -1;

    cce_aicimo_router router;
    if (cce_aicimo_router_init(&router, 8, base_dim) != CCE_OK) return -1;

    /* Single shared decision: adapter and uncertainty come from the same
     * role-biased strength distribution. Previously the bridge issued a
     * second, unbiased route to obtain uncertainty; that reported the entropy
     * of a different distribution than the one that selected the adapter. */
    size_t selected = 0;
    cce_result rc = cce_aicimo_route_decision(&router, input, in_len,
                                               output, out_cap, role,
                                               &selected, uncertainty);

    cce_aicimo_router_free(&router);
    return (rc == CCE_OK) ? 0 : -1;
}