/*
 * cce_aicimo_bridge.h — Bridge API for AICIMO adapter routing.
 *
 * This is the consumer-facing interface for CNET units that want to use
 * AICIMO adapter routing. It wraps the canonical API in cce_aicimo.h
 * with convenient single-call semantics.
 *
 * IMPORTANT: These functions perform adapter routing, NOT context expansion.
 * The output buffer has the same dimensionality as the input. The name
 * "expand_context" is retained for backward API compatibility but the
 * behavior is accurately described as adapter routing.
 */
#ifndef CCE_AICIMO_BRIDGE_H
#define CCE_AICIMO_BRIDGE_H

#include <stddef.h>

/*
 * Route input through AICIMO adapter routing.
 * Output has the same dimensionality as input (adapter routing, not expansion).
 * Returns 0 on success, -1 on error.
 */
int cce_aicimo_expand_context(const float *input, size_t in_len,
                              float *output, size_t out_cap,
                              size_t base_dim);

/*
 * Route input and report uncertainty from the actual route strength
 * distribution. Uncertainty varies with router state, not a fixed formula.
 * Returns 0 on success, -1 on error.
 */
int cce_aicimo_expand_context_with_uncertainty(const float *input, size_t in_len,
                                               float *output, size_t out_cap,
                                               size_t base_dim,
                                               float *uncertainty);

/*
 * Route input using role-based adapter selection.
 * Returns 0 on success, -1 on error.
 */
int cce_aicimo_route_on_role(const float *input, size_t in_len,
                             float *output, size_t out_cap,
                             size_t base_dim, const char *role,
                             float *uncertainty);

#endif /* CCE_AICIMO_BRIDGE_H */