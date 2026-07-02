#ifndef HYPERBOLIC_H
#define HYPERBOLIC_H

/* Hyperbolic (Poincaré-ball) embedding for GENERALIZING routing priors across
 * SIMILAR tasks -- the advisory-layer upgrade SONA gestures at.
 *
 * CNET's learned routing prior (CircuitRankArtifact) is keyed by an EXACT
 * task signature, so a never-seen-before task gets no prior at all. The fix is
 * to embed the capability/task hierarchy in a metric space and let a new task
 * borrow priors from its nearest neighbors. Because that hierarchy is a TREE
 * (primitive -> plan -> chunk; tags form a type tree), the right geometry is
 * HYPERBOLIC, not Euclidean: trees embed into a Poincaré ball with low
 * distortion at tiny dimension, but cannot embed into R^d without distortion
 * (the cophenetic-distortion theorem). This module makes that concrete and
 * measurable, with a geom switch so the win over Euclidean can be verified.
 *
 * It is an ADVISORY scorer only: it produces a prior to bias consideration
 * order. It never decides eligibility, never prunes, never certifies. */

#include <stddef.h>

typedef enum {
    HYP_GEOM_HYPERBOLIC = 0,   /* Poincaré ball (||x|| < 1) */
    HYP_GEOM_EUCLIDEAN  = 1    /* R^dim baseline (for the honest comparison) */
} HypGeom;

/* Poincaré-ball distance between x and y (each dim-vector with ||.|| < 1). */
double poincare_dist(const double *x, const double *y, size_t dim);

/* Euclidean gradient of poincare_dist(x,y) with respect to x (dim values into
   grad_x). The Riemannian gradient is (1-||x||^2)^2/4 times this; embedding
   uses that, but the raw gradient is exposed for gradient-checking. */
void poincare_dist_grad(const double *x, const double *y, size_t dim, double *grad_x);

/* Embed n items, given a symmetric target distance matrix (n*n, row-major, zero
   diagonal), into `geom` space of dimension dim via the Nickel-Kiela
   neighborhood-ranking loss: for each node, pull its nearest neighbor(s) close
   and push sampled non-neighbors away under a softmax over distances (Riemannian
   SGD for hyperbolic, with burn-in). This preserves local hierarchy structure
   and stays stable for deep trees (stress-matching raw distances does not).
   out_coords (n*dim, caller-allocated) receives the embedding. Deterministic
   given seed. Returns the final loss, or -1.0 on bad args. */
double hyperbolic_embed(const double *target_dist, size_t n, size_t dim,
                        HypGeom geom, int epochs, double lr, unsigned seed,
                        double *out_coords);

/* Pearson correlation between embedded pairwise distances and the target
   distances over all i<j (a cophenetic-style fidelity score; 1.0 = perfect).
   Higher = the embedding preserves the hierarchy better. */
double hyperbolic_distortion(const double *coords, size_t n, size_t dim,
                             HypGeom geom, const double *target_dist);

/* Generalize a per-anchor scalar prior to a query point: distance-weighted
   k-nearest-neighbor average (weight = 1/(dist+eps)) in `geom` space.
   Returns the generalized prior, or 0.0 if there are no anchors. */
double hyperbolic_prior_knn(const double *anchor_coords, const double *anchor_prior,
                            size_t n_anchor, const double *query, size_t dim,
                            HypGeom geom, size_t k);

#endif
