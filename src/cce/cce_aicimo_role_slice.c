/*
 * cce_aicimo_role_slice.c — Role-slice routing implementation.
 *
 * Implements Drole (structural role embedding) vs Dlex (lexical) decoupling
 * on top of the canonical cce_aicimo.h API.
 *
 * Role-slice routing applies a deterministic bias derived from the role
 * string to the adapter selection process. This enables different roles
 * (e.g., "narrative", "analytical") to prefer different adapters when
 * the base strengths are close, while remaining fully deterministic:
 * same role + same router state => same selection.
 */
#include "cce_aicimo_role_slice.h"

#include <stdlib.h>
#include <string.h>

cce_result aicimo_route_on_drole(const cce_aicimo_router *r,
                                  const float *input, size_t in_len,
                                  float *output, size_t out_cap,
                                  size_t *used_ops) {
    if (!r || !input || !output || !used_ops) return CCE_ERR_INVALID_ARG;
    if (in_len != r->base_dim || out_cap < in_len) return CCE_ERR_INVALID_ARG;

    /* Drole routing: use standard strength-based selection (role-agnostic) */
    return cce_aicimo_route(r, input, in_len, output, out_cap, used_ops);
}

cce_result aicimo_route_role_slice(const cce_aicimo_router *r,
                                    const float *input, size_t in_len,
                                    float *output, size_t out_cap,
                                    const char *role, size_t *used_ops) {
    if (!r || !input || !output || !role || !used_ops) return CCE_ERR_INVALID_ARG;
    if (in_len != r->base_dim || out_cap < in_len) return CCE_ERR_INVALID_ARG;

    /* Full role-slice routing: use role-biased selection */
    size_t selected = 0;
    cce_result rc = cce_aicimo_route_for_role(r, input, in_len,
                                               output, out_cap, role, &selected);
    if (rc == CCE_OK) *used_ops = 1;
    return rc;
}