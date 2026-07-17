/*
 * cce_aicimo.h — Canonical public API for AICIMO adapter routing in CCE.
 *
 * AICIMO = Adaptive Identity-Initialized Composable Meta-learning Operation.
 *
 * This header is the supported public interface for AICIMO adapter routing
 * within CCE. It lives under include/cce/ so all consumers include it as
 * <cce/cce_aicimo.h>.
 *
 * What AICIMO actually does:
 *   - Maintains a bank of identity-initialized adapters (zero-init + diagonal).
 *   - Routes input through the adapter with the highest accumulated strength
 *     score (GraphMoE-style selection).
 *   - Reports uncertainty derived from the actual route strength distribution
 *     (route entropy), NOT from a fixed formula or a magic constant.
 *
 * What AICIMO does NOT do:
 *   - It does NOT expand context window size. The output buffer has the same
 *     dimensionality as the input. Calling it "context expansion" is
 *     inaccurate; it is adapter routing through identity-initialized blocks.
 *   - It does NOT require a CceSpecialist type or any opaque wrapper.
 *
 * C11 only. No GPU, no network.
 */
#ifndef CCE_AICIMO_H
#define CCE_AICIMO_H

#include "cce_defs.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- core types ---- */

typedef struct {
    float *weights;     /* [in_dim * out_dim], row-major */
    size_t in_dim;
    size_t out_dim;
    int owns_data;
} cce_aicimo_adapter;

typedef struct {
    cce_aicimo_adapter *adapters;
    size_t count;
    size_t capacity;
} cce_aicimo_adapter_bank;

typedef struct {
    float *strength;    /* [num_ops * num_ops], row-major — GraphMoE style */
    size_t num_ops;
    int owns_data;
} cce_aicimo_strength_matrix;

typedef struct {
    cce_aicimo_adapter_bank bank;
    cce_aicimo_strength_matrix router;
    size_t base_dim;        /* dimension this router operates on */
} cce_aicimo_router;

/* ---- lifecycle ---- */

/*
 * Initialize a router with num_ops identity-initialized adapters, each
 * operating on base_dim vectors. Returns CCE_OK on success,
 * CCE_ERR_INVALID_ARG if r is NULL, num_ops is 0, or base_dim is 0,
 * CCE_ERR_OOM on allocation failure.
 */
cce_result cce_aicimo_router_init(cce_aicimo_router *r,
                                   size_t num_ops, size_t base_dim);

/* Free all owned resources. Safe to call with NULL (no-op). */
void cce_aicimo_router_free(cce_aicimo_router *r);

/* ---- routing ---- */

/*
 * Route input through the adapter with the highest accumulated strength.
 * On success: output[0..in_len-1] is filled, *used_ops = 1.
 * Returns CCE_OK or CCE_ERR_INVALID_ARG on bad arguments / dimension mismatch.
 */
cce_result cce_aicimo_route(const cce_aicimo_router *r,
                             const float *input, size_t in_len,
                             float *output, size_t out_cap,
                             size_t *used_ops);

/*
 * Route input, selecting the adapter based on a role string. Different roles
 * may bias different strength rows, enabling deterministic role-based dispatch.
 * On success: output filled, *selected_op set to the chosen adapter index.
 * Returns CCE_OK or CCE_ERR_INVALID_ARG.
 */
cce_result cce_aicimo_route_for_role(const cce_aicimo_router *r,
                                      const float *input, size_t in_len,
                                      float *output, size_t out_cap,
                                      const char *role,
                                      size_t *selected_op);

/*
 * Route input and compute uncertainty from the actual route strength
 * distribution (normalized Shannon entropy of row sums). The uncertainty
 * reflects how peaked or flat the routing decision is — NOT a fixed formula.
 * On success: output filled, *uncertainty in [0, 1].
 * Returns CCE_OK or CCE_ERR_INVALID_ARG.
 */
cce_result cce_aicimo_route_with_uncertainty(const cce_aicimo_router *r,
                                              const float *input, size_t in_len,
                                              float *output, size_t out_cap,
                                              float *uncertainty);

/*
 * Additive shared-decision API: perform one role-biased routing decision and
 * return BOTH the selected adapter index and the uncertainty derived from the
 * *same* strength distribution used to select it. Bridges must not need two
 * separate route calls to obtain adapter and uncertainty.
 *
 * All arguments are required. Returns CCE_OK on success, CCE_ERR_INVALID_ARG
 * on any NULL / dimension mismatch, or CCE_ERR_OOM.
 */
cce_result cce_aicimo_route_decision(const cce_aicimo_router *r,
                                      const float *input, size_t in_len,
                                      float *output, size_t out_cap,
                                      const char *role,
                                      size_t *selected_op,
                                      float *uncertainty);

/* ---- strength manipulation ---- */

/*
 * Set a single entry in the strength matrix at [row][col].
 * Returns CCE_OK or CCE_ERR_INVALID_ARG if r is NULL or indices are out of bounds.
 */
cce_result cce_aicimo_set_route_strength(cce_aicimo_router *r,
                                          size_t row, size_t col,
                                          float value);

#ifdef __cplusplus
}
#endif

#endif /* CCE_AICIMO_H */