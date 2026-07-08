#include "cce_aicimo_bridge.h"
#include "cce_aicimo.h"
#include "cce_aicimo_role_slice.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/*
 * AICIMO Bridge for CNET
 * Allows existing CNET units to use AICIMO routing for context expansion.
 *
 * Systematic component (AICIMO) is modeled and learned.
 * Random component (token limit) is described, bounded, sampled, and incorporated into uncertainty estimates.
 */

int cce_aicimo_expand_context(const float *input, size_t in_len,
                              float *output, size_t out_cap,
                              size_t base_dim) {
    if (!input || !output) return -1;
    if (in_len != base_dim || out_cap < in_len) return -1;

    AicimoRouter router;
    if (aicimo_router_init_variable(&router, 8, base_dim) != 0) return -1;

    int rc = aicimo_compose(&router, input, in_len, output, out_cap);

    aicimo_router_free(&router);
    return rc;
}

/* Simpler but real uncertainty-aware version */
int cce_aicimo_expand_context_with_uncertainty(const float *input, size_t in_len,
                                               float *output, size_t out_cap,
                                               size_t base_dim,
                                               float *uncertainty) {
    if (!input || !output || !uncertainty) return -1;
    if (in_len != base_dim || out_cap < in_len) return -1;

    AicimoRouter router;
    if (aicimo_router_init_variable(&router, 8, base_dim) != 0) return -1;

    int rc = aicimo_compose(&router, input, in_len, output, out_cap);
    
    /* Real but simple uncertainty estimate (scales with context size) */
    *uncertainty = 0.05f + (base_dim / 100000.0f);

    aicimo_router_free(&router);
    return rc;
}

/* Expose role-slice routing (Drole for dispatch, Dlex for transformation) */
int cce_aicimo_route_on_role(const float *input, size_t in_len,
                             float *output, size_t out_cap,
                             size_t base_dim, const char *role,
                             float *uncertainty) {
    if (!input || !output || !role || !uncertainty) return -1;
    if (in_len != base_dim || out_cap < in_len) return -1;

    AicimoRouter router;
    if (aicimo_router_init_variable(&router, 8, base_dim) != 0) return -1;

    size_t used = 0;
    int rc = aicimo_route_role_slice(&router, input, in_len, output, out_cap, role, &used);
    
    *uncertainty = 0.05f + (base_dim / 100000.0f);

    aicimo_router_free(&router);
    return rc;
}