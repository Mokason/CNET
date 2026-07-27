/* Continuous-domain coverage gate for a PORT_RAW visual specialist.
 *
 * Exact training-row membership is the wrong question for continuous features:
 * no held-out proposal is ever an exact training row, so a strict row gate
 * refuses everything, and the legacy "admit anything" gate refuses nothing.
 * Neither is an honest statement about whether the head has seen this kind of
 * input before.
 *
 * This is a conformal k-NN distance gate. The score is the mean Euclidean
 * distance from a feature vector to its k nearest members of a bounded
 * reference set drawn from the data the head was certified over; the threshold
 * is a quantile of that score measured on a calibration split that is disjoint
 * from both the fitting data and the final holdout. Admission is
 * `score <= tau`.
 *
 * It is deliberately reproducible with no fitted state beyond the reference
 * rows and tau, both of which already have a capsule transport: the rows travel
 * as HybridCoverage rows, tau travels in the frontend asset. A fresh runtime
 * recomputes the identical score.
 *
 * Protocol: plans/cnet_vision_portable_specialist_20260728.md
 */
#ifndef VD_COVERAGE_H
#define VD_COVERAGE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Any coordinate beyond this magnitude is refused before scoring: it cannot be
   a real PCA projection of a HOG descriptor, and letting it reach the distance
   computation would only produce a meaninglessly large score. */
#define VD_COV_MAX_ABS 1.0e6

typedef struct {
    double *ref;       /* n_ref * dim, owned */
    size_t n_ref, dim, k;
    double tau;
} VdCoverage;

/* Copies ref. Returns 0 on success. k is clamped to n_ref. */
int vd_cov_init(VdCoverage *c, const double *ref, size_t n_ref, size_t dim,
                size_t k, double tau);
void vd_cov_free(VdCoverage *c);

/* Mean distance to the k nearest reference rows, or NaN if x is unusable
   (NULL, non-finite, or beyond VD_COV_MAX_ABS). */
double vd_cov_score(const VdCoverage *c, const double *x);

/* 1 = admit, 0 = refuse. A NaN score always refuses. */
int vd_cov_admit(const VdCoverage *c, const double *x);

/* Quantile of a scratch array of scores; sorts in place. q in [0,1]. */
double vd_cov_quantile(double *scores, size_t n, double q);

#ifdef __cplusplus
}
#endif

#endif
