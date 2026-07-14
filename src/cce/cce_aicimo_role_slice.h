/*
 * cce_aicimo_role_slice.h — Role-slice routing for AICIMO.
 *
 * Provides Drole (structural role dispatch) and Dlex (lexical transformation)
 * decoupling on top of the canonical cce_aicimo.h API.
 *
 * The role-slice routing applies a deterministic bias from the role string
 * to the strength matrix before selecting the best adapter. This means:
 * - Same role + same router state => same adapter selection (deterministic)
 * - Different roles may select different adapters when strengths are close
 */
#ifndef CCE_AICIMO_ROLE_SLICE_H
#define CCE_AICIMO_ROLE_SLICE_H

#include "../../include/cce/cce_aicimo.h"

/* Route using only the Drole slice (structural role routing).
 * Applies a role-derived bias to the strength matrix before selection. */
cce_result aicimo_route_on_drole(const cce_aicimo_router *r,
                                  const float *input, size_t in_len,
                                  float *output, size_t out_cap,
                                  size_t *used_ops);

/* Full role-slice routing: Drole for dispatch, Dlex for transformation.
 * The role string deterministically biases adapter selection. */
cce_result aicimo_route_role_slice(const cce_aicimo_router *r,
                                    const float *input, size_t in_len,
                                    float *output, size_t out_cap,
                                    const char *role, size_t *used_ops);

#endif /* CCE_AICIMO_ROLE_SLICE_H */