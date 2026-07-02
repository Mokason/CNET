/* test_hyperbolic — HONEST evaluation of the hyperbolic rank-prior idea.
 *
 * The module is verified correct (gradient check) and produces a coherent
 * Nickel-Kiela embedding of a capability hierarchy. BUT the premise behind
 * Idea B -- that hyperbolic geometry improves routing-PRIOR generalization over
 * a plain Euclidean baseline -- did NOT survive measurement: a scaling sweep
 * (31..1023-node trees, scratchpad/scale_probe.c) found Euclidean kNN prior
 * recovery is consistently better-or-equal at every depth. So hyperbolic is NOT
 * wired into the router; this test stands as the green record of that finding.
 *
 * Hierarchy: a complete binary tree of depth 5 (63 nodes, heap-indexed); each
 * task's prior is a smooth function of its position in the tree. */
#include <stdio.h>
#include <math.h>
#include <string.h>
#include "../include/hyperbolic.h"

#define N 63
#define DIM 2

static int g_pass = 0, g_fail = 0;
#define CHECK(c, msg) do { if (c) g_pass++; else { g_fail++; \
    printf("FAIL: %s (line %d)\n", msg, __LINE__); } } while (0)

static void build_tree_distances(double *D) {
    for (int s = 0; s < N; s++) {
        int dist[N]; for (int i = 0; i < N; i++) dist[i] = -1;
        int q[N], head = 0, tail = 0; dist[s] = 0; q[tail++] = s;
        while (head < tail) {
            int u = q[head++];
            int nb[3], nn = 0;
            if (u > 0) nb[nn++] = (u - 1) / 2;          /* parent */
            if (2 * u + 1 < N) nb[nn++] = 2 * u + 1;    /* children */
            if (2 * u + 2 < N) nb[nn++] = 2 * u + 2;
            for (int k = 0; k < nn; k++)
                if (dist[nb[k]] < 0) { dist[nb[k]] = dist[u] + 1; q[tail++] = nb[k]; }
        }
        for (int t = 0; t < N; t++) D[s * N + t] = (double)dist[t];
    }
}

/* fraction of nodes whose nearest embedded neighbor is a true tree edge (D==1) */
static double struct_rate(const double *coords, HypGeom g, const double *D) {
    int good = 0;
    for (int i = 0; i < N; i++) {
        int best = -1; double bd = 1e300;
        for (int j = 0; j < N; j++) {
            if (j == i) continue;
            double d = (g == HYP_GEOM_HYPERBOLIC)
                       ? poincare_dist(coords + i * DIM, coords + j * DIM, DIM)
                       : sqrt((coords[i*DIM]-coords[j*DIM])*(coords[i*DIM]-coords[j*DIM]) +
                              (coords[i*DIM+1]-coords[j*DIM+1])*(coords[i*DIM+1]-coords[j*DIM+1]));
            if (d < bd) { bd = d; best = j; }
        }
        if (best >= 0 && D[i * N + best] == 1.0) good++;
    }
    return (double)good / (double)N;
}

static void gradient_check(void) {
    const size_t dim = 3;
    double x[3] = { 0.10, -0.20, 0.30 }, y[3] = { -0.15, 0.25, 0.05 };
    double g[3]; poincare_dist_grad(x, y, dim, g);
    double h = 1e-6, maxerr = 0.0;
    for (size_t i = 0; i < dim; i++) {
        double xp[3], xm[3]; memcpy(xp, x, sizeof x); memcpy(xm, x, sizeof x);
        xp[i] += h; xm[i] -= h;
        double num = (poincare_dist(xp, y, dim) - poincare_dist(xm, y, dim)) / (2 * h);
        double e = fabs(num - g[i]); if (e > maxerr) maxerr = e;
    }
    printf("  gradient check: max |analytic - numeric| = %.2e\n", maxerr);
    CHECK(maxerr < 1e-5, "poincare_dist_grad matches finite differences");
}

int main(void) {
    static double D[N * N];
    build_tree_distances(D);
    gradient_check();

    double ch[N * DIM], ce[N * DIM];
    hyperbolic_embed(D, N, DIM, HYP_GEOM_HYPERBOLIC, 3000, 1.0, 7u, ch);
    hyperbolic_embed(D, N, DIM, HYP_GEOM_EUCLIDEAN,  3000, 0.30, 7u, ce);

    /* (1) tree-adjacency preservation */
    double sr_h = struct_rate(ch, HYP_GEOM_HYPERBOLIC, D);
    double sr_e = struct_rate(ce, HYP_GEOM_EUCLIDEAN,  D);
    double corr_h = hyperbolic_distortion(ch, N, DIM, HYP_GEOM_HYPERBOLIC, D);
    double corr_e = hyperbolic_distortion(ce, N, DIM, HYP_GEOM_EUCLIDEAN,  D);
    printf("  nearest-neighbor is a true tree edge:  hyperbolic=%.3f  euclidean=%.3f\n", sr_h, sr_e);
    printf("  distance correlation (info):           hyperbolic=%.3f  euclidean=%.3f\n", corr_h, corr_e);
    CHECK(sr_h > 0.70, "hyperbolic produces a coherent embedding of the hierarchy");

    /* (2) prior generalization across the WHOLE hierarchy. Each task's reliability
       prior is a smooth function of its position in the capability tree (nearby
       capabilities behave similarly): prior(i) = tree-distance(ref, i)/maxD. A
       held-out task's prior is predicted from its k=3 nearest embedded neighbors
       -- so recovery quality tracks how faithfully the geometry preserves the
       hierarchy, which is exactly where hyperbolic wins. */
    int ref = 62;
    double maxD = 0.0; for (int i = 0; i < N; i++) if (D[ref * N + i] > maxD) maxD = D[ref * N + i];
    double prior[N]; for (int i = 0; i < N; i++) prior[i] = D[ref * N + i] / maxD;

    double mae_h1 = 0, mae_e1 = 0, mae_h3 = 0, mae_e3 = 0;
    for (int held = 0; held < N; held++) {
        double ah[N * DIM], ae[N * DIM], ap[N], qh[DIM], qe[DIM];
        int m = 0;
        for (int i = 0; i < N; i++) {
            if (i == held) continue;
            ah[m*DIM]=ch[i*DIM]; ah[m*DIM+1]=ch[i*DIM+1];
            ae[m*DIM]=ce[i*DIM]; ae[m*DIM+1]=ce[i*DIM+1];
            ap[m] = prior[i]; m++;
        }
        qh[0]=ch[held*DIM]; qh[1]=ch[held*DIM+1];
        qe[0]=ce[held*DIM]; qe[1]=ce[held*DIM+1];
        /* k=1 == "borrow from the single most similar task" (the use case) */
        mae_h1 += fabs(hyperbolic_prior_knn(ah, ap, (size_t)m, qh, DIM, HYP_GEOM_HYPERBOLIC, 1) - prior[held]);
        mae_e1 += fabs(hyperbolic_prior_knn(ae, ap, (size_t)m, qe, DIM, HYP_GEOM_EUCLIDEAN,  1) - prior[held]);
        /* k=3 == smoothed average (reported for honesty) */
        mae_h3 += fabs(hyperbolic_prior_knn(ah, ap, (size_t)m, qh, DIM, HYP_GEOM_HYPERBOLIC, 3) - prior[held]);
        mae_e3 += fabs(hyperbolic_prior_knn(ae, ap, (size_t)m, qe, DIM, HYP_GEOM_EUCLIDEAN,  3) - prior[held]);
    }
    mae_h1 /= N; mae_e1 /= N; mae_h3 /= N; mae_e3 /= N;
    printf("  held-out prior recovery, k=1 (use case): hyperbolic=%.4f  euclidean=%.4f\n", mae_h1, mae_e1);
    printf("  held-out prior recovery, k=3 (smoothed):  hyperbolic=%.4f  euclidean=%.4f\n", mae_h3, mae_e3);
    CHECK(mae_e1 < 0.12, "euclidean kNN baseline recovers held-out priors well");
    /* The honest verdict: hyperbolic does NOT beat the euclidean baseline here. */
    CHECK(mae_e1 <= mae_h1 + 1e-9, "euclidean is at least as good as hyperbolic (Idea B not justified)");
    printf("  VERDICT: euclidean kNN ties-or-beats hyperbolic for prior recovery -> "
           "hyperbolic NOT wired into the router.\n");

    printf("\ntest_hyperbolic: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
