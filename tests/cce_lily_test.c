/* cce_lily test + benchmark. Correctness (apply==merge, train fits a planted
   multi-layer low-rank target) and the interconnection benchmark: shared-A
   (Lily) vs independent per-layer A, on shared vs independent ground truth. The
   hypothesis: when the true adaptation shares a subspace across layers, the
   shared-A form generalizes better (fewer params, less overfitting to noise). */

#include "../include/cce/cce_lily.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); g_fail++; } else printf("ok: %s\n", m); } while (0)

static uint32_t S = 777;
static uint32_t rr(void) { S = S * 1664525u + 1013904223u; return S; }
static float rn(void) {
    float u1 = (rr() >> 8) * (1.0f / 16777216.0f), u2 = (rr() >> 8) * (1.0f / 16777216.0f);
    if (u1 < 1e-7f) u1 = 1e-7f; return sqrtf(-2.0f * logf(u1)) * cosf(6.2831853f * u2);
}
static float uf(void) { return ((rr() >> 8) * (1.0f / 16777216.0f)) * 2.0f - 1.0f; }

static void gen_base(float *W, int L, int d) {
    for (int l = 0; l < L; l++) { float *Wl = W + (size_t)l * d * d;
        for (int i = 0; i < d; i++) for (int o = 0; o < d; o++)
            Wl[i * d + o] = (i == o ? 1.0f : 0.0f) + rn() * (0.1f / sqrtf((float)d)); }
}
static void plain_forward(const float *W, const float *x, float *y, int L, int d) {
    float *h = malloc((size_t)d * 4), *hn = malloc((size_t)d * 4);
    memcpy(h, x, (size_t)d * 4);
    for (int l = 0; l < L; l++) { const float *Wl = W + (size_t)l * d * d;
        for (int o = 0; o < d; o++) { float b = 0; for (int i = 0; i < d; i++) b += h[i] * Wl[i * d + o]; hn[o] = b; }
        memcpy(h, hn, (size_t)d * 4); }
    memcpy(y, h, (size_t)d * 4); free(h); free(hn);
}
static void randomize(cce_lily *ly, float bscale) {
    size_t na = (size_t)(ly->shared ? 1 : ly->layers) * ly->width * ly->rank, nb = (size_t)ly->layers * ly->rank * ly->width;
    for (size_t i = 0; i < na; i++) ly->A[i] = rn() * 0.3f;
    for (size_t i = 0; i < nb; i++) ly->B[i] = rn() * bscale;
}
static float maxabsdiff(const float *a, const float *b, int n) {
    float m = 0; for (int i = 0; i < n; i++) { float x = fabsf(a[i] - b[i]); if (x > m) m = x; } return m;
}

int main(void) {
    const int d = 24, L = 4;

    /* ---- correctness: apply(base + delta) == plain_forward(merged) ---- */
    {
        float *W = malloc((size_t)L * d * d * 4);
        gen_base(W, L, d);
        cce_lily ly; cce_lily_init(&ly, d, L, 4, 8.0f, 3, /*shared=*/1);
        randomize(&ly, 0.3f);
        float *Wm = malloc((size_t)L * d * d * 4); memcpy(Wm, W, (size_t)L * d * d * 4);
        cce_lily_merge(&ly, Wm);
        float md = 0, x[64], ya[64], yb[64];
        for (int t = 0; t < 32; t++) {
            for (int i = 0; i < d; i++) x[i] = uf();
            cce_lily_apply(&ly, W, x, ya);
            plain_forward(Wm, x, yb, L, d);
            float e = maxabsdiff(ya, yb, d); if (e > md) md = e;
        }
        printf("   (apply vs merged max diff = %.2e)\n", md);
        CHECK(md < 1e-3f, "apply(base+delta) == plain_forward(merged)");
        CHECK(cce_lily_param_count(&ly) == (size_t)4 * d * (L + 1), "shared param count = r*d*(L+1)");
        cce_lily_free(&ly); free(W); free(Wm);
    }

    /* ---- correctness: train recovers a planted shared multi-layer target ---- */
    {
        float *W = malloc((size_t)L * d * d * 4); gen_base(W, L, d);
        cce_lily truth; cce_lily_init(&truth, d, L, 2, 2.0f, 5, 1); randomize(&truth, 0.2f);
        const size_t n = 256; float *X = malloc(n * d * 4), *T = malloc(n * d * 4);
        for (size_t s = 0; s < n; s++) { float *xs = X + s * d; for (int i = 0; i < d; i++) xs[i] = uf();
            cce_lily_apply(&truth, W, xs, T + s * d); }
        cce_lily fit; cce_lily_init(&fit, d, L, 4, 2.0f, 9, 1);
        cce_lily_train_opts o = cce_lily_train_defaults(); o.epochs = 1500; o.lr = 0.02f;
        double m0 = cce_lily_eval_mse(&fit, W, X, T, n), m1 = cce_lily_train(&fit, W, X, T, n, &o);
        printf("   (train mse %.4g -> %.4g)\n", m0, m1);
        CHECK(m1 < 1e-3 && m1 < m0 * 1e-2, "train fits a planted shared multi-layer target");
        cce_lily_free(&truth); cce_lily_free(&fit); free(W); free(X); free(T);
    }

    /* ---- benchmark: shared (Lily) vs independent, on shared vs independent truth ---- */
    printf("\n=== interconnection benchmark (d=%d, L=%d, rank=4, true rank=2, noise=0.05) ===\n", d, L);
    for (int regime = 0; regime < 2; regime++) {
        int truth_shared = (regime == 0);
        printf("\n-- ground truth: %s adaptation across layers --\n", truth_shared ? "SHARED-subspace" : "INDEPENDENT-per-layer");
        float *W = malloc((size_t)L * d * d * 4); gen_base(W, L, d);
        cce_lily truth; cce_lily_init(&truth, d, L, 2, 2.0f, 100 + regime, truth_shared); randomize(&truth, 0.15f);

        const size_t ntr = 256, nte = 256;
        float *Xtr = malloc(ntr * d * 4), *Ttr = malloc(ntr * d * 4);
        float *Xte = malloc(nte * d * 4), *Tte = malloc(nte * d * 4);
        for (size_t s = 0; s < ntr; s++) { float *xs = Xtr + s * d; for (int i = 0; i < d; i++) xs[i] = uf();
            cce_lily_apply(&truth, W, xs, Ttr + s * d);
            for (int o = 0; o < d; o++) Ttr[s * d + o] += rn() * 0.05f; }               /* + noise */
        for (size_t s = 0; s < nte; s++) { float *xs = Xte + s * d; for (int i = 0; i < d; i++) xs[i] = uf();
            cce_lily_apply(&truth, W, xs, Tte + s * d); }                                 /* clean truth */

        printf("%-14s %9s %11s %12s\n", "adapter", "params", "train_mse", "heldout_mse");
        for (int shared = 1; shared >= 0; shared--) {
            cce_lily fit; cce_lily_init(&fit, d, L, 4, 2.0f, 42 + shared, shared);
            cce_lily_train_opts o = cce_lily_train_defaults(); o.epochs = 1200; o.lr = 0.02f;
            double tr = cce_lily_train(&fit, W, Xtr, Ttr, ntr, &o);
            double te = cce_lily_eval_mse(&fit, W, Xte, Tte, nte);
            printf("%-14s %9zu %11.5f %12.5f\n", shared ? "Lily(sharedA)" : "independent",
                   cce_lily_param_count(&fit), tr, te);
            cce_lily_free(&fit);
        }
        printf("  (dense per-layer would be %zu params)\n", (size_t)L * d * d);
        cce_lily_free(&truth); free(W); free(Xtr); free(Ttr); free(Xte); free(Tte);
    }
    printf("\ninterpretation: on SHARED-subspace truth the shared-A form should match\n"
           "held-out at fewer params (interconnection = regularizer); on INDEPENDENT\n"
           "truth it underfits and per-layer A is needed. The gate/orchestrator that\n"
           "hosts cce_lora hosts either unchanged.\n");

    printf("\n%s (%d failures)\n", g_fail ? "TESTS FAILED" : "ALL TESTS PASSED", g_fail);
    return g_fail ? 1 : 0;
}
