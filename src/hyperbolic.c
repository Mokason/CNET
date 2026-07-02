/* Hyperbolic (Poincaré-ball) embedding -- see include/hyperbolic.h.
 * Distance gradient follows Nickel & Kiela 2017; embedding minimizes stress by
 * (Riemannian) gradient descent. Self-contained; depends only on libm. */
#include "../include/hyperbolic.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define BALL_EPS 1e-5      /* keep points strictly inside the unit ball */

static double dot(const double *a, const double *b, size_t d) {
    double s = 0.0; for (size_t i = 0; i < d; i++) s += a[i] * b[i]; return s;
}
static double sqnorm(const double *a, size_t d) { return dot(a, a, d); }

double poincare_dist(const double *x, const double *y, size_t dim) {
    double sx = sqnorm(x, dim), sy = sqnorm(y, dim);
    double dxy = 0.0;
    for (size_t i = 0; i < dim; i++) { double t = x[i] - y[i]; dxy += t * t; }
    double denom = (1.0 - sx) * (1.0 - sy);
    if (denom < 1e-12) denom = 1e-12;
    double arg = 1.0 + 2.0 * dxy / denom;
    if (arg < 1.0) arg = 1.0;
    return acosh(arg);
}

void poincare_dist_grad(const double *x, const double *y, size_t dim, double *grad_x) {
    double sx = sqnorm(x, dim), sy = sqnorm(y, dim), xy = dot(x, y, dim);
    double dxy = sx - 2.0 * xy + sy;                 /* ||x-y||^2 */
    double alpha = 1.0 - sx, beta = 1.0 - sy;
    if (alpha < 1e-12) alpha = 1e-12;
    if (beta  < 1e-12) beta  = 1e-12;
    double gamma = 1.0 + 2.0 / (alpha * beta) * dxy;
    double g2 = gamma * gamma - 1.0;
    if (g2 <= 1e-12) { for (size_t i = 0; i < dim; i++) grad_x[i] = 0.0; return; }
    double c = 4.0 / (beta * sqrt(g2));
    double coeff = (sy - 2.0 * xy + 1.0) / (alpha * alpha);   /* other point's norm */
    for (size_t i = 0; i < dim; i++)
        grad_x[i] = c * (coeff * x[i] - y[i] / alpha);
}

static void euclid_dist_grad(const double *x, const double *y, size_t dim, double *grad_x) {
    double d = 0.0; for (size_t i = 0; i < dim; i++) { double t = x[i]-y[i]; d += t*t; }
    d = sqrt(d);
    if (d < 1e-12) { for (size_t i = 0; i < dim; i++) grad_x[i] = 0.0; return; }
    for (size_t i = 0; i < dim; i++) grad_x[i] = (x[i] - y[i]) / d;
}

static double dist_geom(const double *x, const double *y, size_t dim, HypGeom g) {
    if (g == HYP_GEOM_HYPERBOLIC) return poincare_dist(x, y, dim);
    double d = 0.0; for (size_t i = 0; i < dim; i++) { double t = x[i]-y[i]; d += t*t; }
    return sqrt(d);
}

static void project_to_ball(double *x, size_t dim) {
    double n = sqrt(sqnorm(x, dim));
    double maxn = 1.0 - BALL_EPS;
    if (n > maxn) { double s = maxn / n; for (size_t i = 0; i < dim; i++) x[i] *= s; }
}

/* tiny deterministic LCG in [-1,1) */
static double lcg(unsigned *st) {
    *st = *st * 1103515245u + 12345u;
    return ((double)((*st >> 8) & 0xFFFFFF) / (double)0x1000000) * 2.0 - 1.0;
}

/* one geom-aware SGD step pulling a single point along coef * d(point,other) */
static void apply_step(double *pt, const double *other, size_t dim, HypGeom geom,
                       double lr, double coef) {
    double g[64];
    if (geom == HYP_GEOM_HYPERBOLIC) poincare_dist_grad(pt, other, dim, g);
    else                            euclid_dist_grad(pt, other, dim, g);
    double scale = 1.0;
    if (geom == HYP_GEOM_HYPERBOLIC) {
        double a = 1.0 - sqnorm(pt, dim);              /* Riemannian rescale (1-||x||^2)^2/4 */
        scale = a * a / 4.0;
    }
    for (size_t k = 0; k < dim; k++) pt[k] -= lr * scale * coef * g[k];
    if (geom == HYP_GEOM_HYPERBOLIC) project_to_ball(pt, dim);
}

/* Neighborhood-ranking embedding (Nickel & Kiela 2017): for each node u and each
   nearest-neighbor v (positive), pull v close and push sampled non-neighbors away
   under a softmax over distances. Preserves the hierarchy's local structure --
   which is what prior-kNN needs -- and, unlike stress, stays numerically stable
   for deep trees (it never forces points to the boundary to match a raw distance). */
double hyperbolic_embed(const double *target_dist, size_t n, size_t dim,
                        HypGeom geom, int epochs, double lr, unsigned seed,
                        double *out_coords) {
    if (!target_dist || !out_coords || n < 2 || dim == 0 || dim > 64 || epochs <= 0) return -1.0;

    unsigned st = seed ? seed : 1u;
    for (size_t i = 0; i < n * dim; i++) out_coords[i] = 0.001 * lcg(&st);

    size_t n_neg = (n - 2 < 10) ? (n - 2) : 10;
    if (n_neg < 1) n_neg = 1;
    int burn = epochs / 10; if (burn < 1) burn = 1;

    double loss = 0.0;
    for (int ep = 0; ep < epochs; ep++) {
        double cur_lr = (ep < burn) ? lr * 0.1 : lr;   /* burn-in stabilizes early epochs */
        loss = 0.0;
        for (size_t u = 0; u < n; u++) {
            double mind = 1e300;                         /* nearest target distance for u */
            for (size_t j = 0; j < n; j++)
                if (j != u && target_dist[u * n + j] < mind) mind = target_dist[u * n + j];

            for (size_t v = 0; v < n; v++) {
                if (v == u) continue;
                if (target_dist[u * n + v] > mind + 1e-9) continue;   /* v is a nearest neighbor */

                size_t S[64]; size_t ns = 0; S[ns++] = v;             /* S = {v} + negatives */
                int guard = 0;
                while (ns < 1 + n_neg && guard < 2000) {
                    guard++;
                    size_t c = (size_t)((lcg(&st) * 0.5 + 0.5) * (double)n);
                    if (c >= n) c = n - 1;
                    if (c == u || target_dist[u * n + c] <= mind + 1e-9) continue;
                    int dup = 0; for (size_t a = 0; a < ns; a++) if (S[a] == c) { dup = 1; break; }
                    if (!dup) S[ns++] = c;
                }

                double dd[64] = {0.0}, p[64], Z = 0.0, mn = 1e300;
                for (size_t a = 0; a < ns; a++) {
                    dd[a] = dist_geom(out_coords + u * dim, out_coords + S[a] * dim, dim, geom);
                    if (dd[a] < mn) mn = dd[a];
                }
                for (size_t a = 0; a < ns; a++) { p[a] = exp(-(dd[a] - mn)); Z += p[a]; }
                for (size_t a = 0; a < ns; a++) p[a] /= Z;
                loss += (dd[0] - mn) + log(Z);                        /* -log softmax of the positive */

                /* dL/dd(u,S[a]) = [a==0] - p[a]; update u (accumulated) and each S[a] */
                double *xu = out_coords + u * dim;
                /* update neighbors first (uses pre-step xu), then u */
                double coef0[64];
                for (size_t a = 0; a < ns; a++) coef0[a] = ((a == 0) ? 1.0 : 0.0) - p[a];
                for (size_t a = 0; a < ns; a++)
                    apply_step(out_coords + S[a] * dim, xu, dim, geom, cur_lr, coef0[a]);
                for (size_t a = 0; a < ns; a++)
                    apply_step(xu, out_coords + S[a] * dim, dim, geom, cur_lr, coef0[a]);
            }
        }
    }
    return loss;
}

double hyperbolic_distortion(const double *coords, size_t n, size_t dim,
                             HypGeom geom, const double *target_dist) {
    if (!coords || !target_dist || n < 2) return 0.0;
    /* Pearson correlation between embedded and target distances over i<j */
    double sx = 0, sy = 0, sxx = 0, syy = 0, sxy = 0; size_t m = 0;
    for (size_t i = 0; i < n; i++)
        for (size_t j = i + 1; j < n; j++) {
            double de = dist_geom(coords + i * dim, coords + j * dim, dim, geom);
            double dt = target_dist[i * n + j];
            sx += de; sy += dt; sxx += de * de; syy += dt * dt; sxy += de * dt; m++;
        }
    if (m == 0) return 0.0;
    double cov = sxy - sx * sy / (double)m;
    double vx = sxx - sx * sx / (double)m;
    double vy = syy - sy * sy / (double)m;
    if (vx <= 0 || vy <= 0) return 0.0;
    return cov / sqrt(vx * vy);
}

double hyperbolic_prior_knn(const double *anchor_coords, const double *anchor_prior,
                            size_t n_anchor, const double *query, size_t dim,
                            HypGeom geom, size_t k) {
    if (!anchor_coords || !anchor_prior || !query || n_anchor == 0) return 0.0;
    if (k == 0 || k > n_anchor) k = n_anchor;

    /* distances to all anchors */
    double *d = (double *)malloc(n_anchor * sizeof(double));
    size_t *idx = (size_t *)malloc(n_anchor * sizeof(size_t));
    if (!d || !idx) { free(d); free(idx); return 0.0; }
    for (size_t i = 0; i < n_anchor; i++) {
        d[i] = dist_geom(query, anchor_coords + i * dim, dim, geom);
        idx[i] = i;
    }
    /* partial selection sort for the k smallest */
    for (size_t a = 0; a < k; a++) {
        size_t best = a;
        for (size_t b = a + 1; b < n_anchor; b++) if (d[idx[b]] < d[idx[best]]) best = b;
        size_t t = idx[a]; idx[a] = idx[best]; idx[best] = t;
    }
    double num = 0.0, den = 0.0;
    for (size_t a = 0; a < k; a++) {
        double w = 1.0 / (d[idx[a]] + 1e-9);
        num += w * anchor_prior[idx[a]];
        den += w;
    }
    free(d); free(idx);
    return den > 0 ? num / den : 0.0;
}
