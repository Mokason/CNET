#include "vd_coverage.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

int vd_cov_init(VdCoverage *c, const double *ref, size_t n_ref, size_t dim,
                size_t k, double tau) {
    if (!c) return -1;
    memset(c, 0, sizeof *c);
    if (!ref || n_ref == 0 || dim == 0 || k == 0) return -1;
    c->ref = (double *)malloc(n_ref * dim * sizeof(double));
    if (!c->ref) return -1;
    memcpy(c->ref, ref, n_ref * dim * sizeof(double));
    c->n_ref = n_ref;
    c->dim = dim;
    c->k = k < n_ref ? k : n_ref;
    c->tau = tau;
    return 0;
}

void vd_cov_free(VdCoverage *c) {
    if (!c) return;
    free(c->ref);
    memset(c, 0, sizeof *c);
}

double vd_cov_score(const VdCoverage *c, const double *x) {
    double best[64];
    size_t nb = 0, i, j, d;
    if (!c || !c->ref || !x) return NAN;
    if (c->k > 64) return NAN;   /* k is a protocol constant, kept small */
    for (d = 0; d < c->dim; d++)
        if (!isfinite(x[d]) || fabs(x[d]) > VD_COV_MAX_ABS) return NAN;

    /* Insertion into a k-sized max-at-end list: k is 8, so a heap would cost
       more than it saves and this keeps the order of operations obvious. */
    for (i = 0; i < c->n_ref; i++) {
        const double *r = c->ref + i * c->dim;
        double s = 0.0, dist;
        for (d = 0; d < c->dim; d++) {
            double diff = x[d] - r[d];
            s += diff * diff;
        }
        dist = sqrt(s);
        if (nb < c->k) {
            j = nb++;
            while (j > 0 && best[j - 1] > dist) { best[j] = best[j - 1]; j--; }
            best[j] = dist;
        } else if (dist < best[nb - 1]) {
            j = nb - 1;
            while (j > 0 && best[j - 1] > dist) { best[j] = best[j - 1]; j--; }
            best[j] = dist;
        }
    }
    if (nb == 0) return NAN;
    {
        double sum = 0.0;
        for (i = 0; i < nb; i++) sum += best[i];
        return sum / (double)nb;
    }
}

int vd_cov_admit(const VdCoverage *c, const double *x) {
    double s = vd_cov_score(c, x);
    if (!isfinite(s)) return 0;      /* unusable input never admits */
    return s <= c->tau ? 1 : 0;
}

static int dcmp(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

double vd_cov_quantile(double *scores, size_t n, double q) {
    size_t idx;
    if (!scores || n == 0) return NAN;
    if (q < 0.0) q = 0.0;
    if (q > 1.0) q = 1.0;
    qsort(scores, n, sizeof *scores, dcmp);
    idx = (size_t)(q * (double)(n - 1) + 0.5);
    if (idx >= n) idx = n - 1;
    return scores[idx];
}
