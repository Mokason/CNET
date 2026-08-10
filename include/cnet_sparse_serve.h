/* Sparse adaptive serve policy (Brain board laws → CNET).
 *
 * Among coverage-gated certified units that ADMIT the input, pick by
 * center geometry (mean of certified rows). Fail closed if none admit.
 * Does not lower floors. Does not replace CNU1.
 */
#ifndef CNET_SPARSE_SERVE_H
#define CNET_SPARSE_SERVE_H

#include <stddef.h>

#include "cnet_export.h"
#include "hybrid_ai.h"
#include "nn.h"
#include "router.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CNET_SPARSE_NAME_MAX 64
#define CNET_SPARSE_MAX_CAND 32

typedef struct {
    char unit[CNET_SPARSE_NAME_MAX];
    double score; /* lower better; 0 = exact certified row */
    size_t n_rows;
    int admits; /* 1 if input in certified set */
} CnetSparseCandidate;

/* Rank up to `cap` coverage records for this port shape.
 * Only records that admit `in` are returned (fail-closed filter).
 * Score = L2 to mean of certified rows (0 if exact row match).
 * Returns 0 on success (n_out may be 0 = abstain). -1 on bad args. */
CNET_API int cnet_sparse_rank_covered(const HybridAi *h, Port in_port, Port out_port,
                                      const double *in, size_t in_len,
                                      CnetSparseCandidate *out, size_t cap,
                                      size_t *n_out);

/* Best unit name among admitting coverage, or -1 to abstain.
 * name_out receives unit name when return is 0. */
CNET_API int cnet_sparse_pick_covered_unit(const HybridAi *h, Port in_port,
                                           Port out_port, const double *in,
                                           size_t in_len, char *name_out,
                                           size_t name_cap, double *score_out);

/* Mean of coverage rows for unit (center). Returns 0 and writes center[in_dim]
 * when unit has active coverage; -1 otherwise. Caller supplies center[in_dim]. */
CNET_API int cnet_sparse_unit_center(const HybridAi *h, const char *unit,
                                     double *center, size_t in_dim);

/* Same-domain residual ACCUM (Brain LINK_ACCUM law):
 *   y = peak(x) + alpha * residual(x)   // same x, same out dim
 * Fail-closed:
 *   - peak (and residual if set) must be CERT in registry
 *   - in/out dims must match
 *   - if coverage exists for the unit, input must be admitted
 * alpha default if <=0: 0.65 (board LTM residual)
 * Returns 0 on success, -1 abstain/refuse, -2 bad args.
 * *used_residual_out = 1 if residual hop ran.
 */
CNET_API int cnet_sparse_run_accum(const HybridAi *h, const PrimitiveRegistry *reg,
                                   Port in_port, Port out_port,
                                   const char *peak_unit, const char *resid_unit,
                                   const double *x, size_t in_len, double *y,
                                   size_t out_cap, double alpha,
                                   int *used_residual_out);

#ifdef __cplusplus
}
#endif

#endif
