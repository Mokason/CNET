/* bench_cce_lora — M5. Compares a DENSE output-layer delta (full in*out, trained
   by the same Adam) against rank-r cce_lora adapters on the SAME (input,
   residual) data, isolating the effect of rank. Two regimes:

     (1) low-rank correction  (intrinsic rank k=4 + noise)  — the skill regime
     (2) near-full-rank        (k=min(in,out))              — the honest boundary

   Reports, per student: trained params, teach time (CPU ms), stored bytes
   (fp32 and trit-packed 1.6-bit estimate), held-out MSE, and a port_validate-
   style pass-rate (fraction of held-out rows within L-inf tol). Decision: the
   smallest rank whose held-out MSE <= dense at fewer params and less time. */

#include "../include/cce/cce_lora.h"
#include "../include/cce/cce_tensor.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint32_t S = 987654321u;
static uint32_t rnd(void) { S = S * 1664525u + 1013904223u; return S; }
static float rf(void) { return ((rnd() >> 8) * (1.0f / 16777216.0f)) * 2.0f - 1.0f; }
static float rn(float sd) { /* ~gaussian via sum of 4 uniforms */
    float s = 0; for (int i = 0; i < 4; i++) s += rf(); return s * 0.5f * sd;
}

static void base_linear(const float* W, const float* x, int in, int out, float* y) {
    for (int o = 0; o < out; o++) y[o] = 0.0f;
    for (int i = 0; i < in; i++) { float xi = x[i];
        for (int o = 0; o < out; o++) y[o] += xi * W[(size_t)i * out + o]; }
}

/* dense delta W:[in,out] fit to residuals by full-batch Adam (same optimizer as lora) */
static double dense_fit(float* W, const float* X, const float* R, size_t n,
                        int in, int out, int epochs, float lr) {
    size_t np = (size_t)in * out;
    float *g = calloc(np, sizeof(float)), *m = calloc(np, sizeof(float)), *v = calloc(np, sizeof(float));
    float *y = malloc((size_t)out * sizeof(float));
    const float b1 = 0.9f, b2 = 0.999f, eps = 1e-8f; double last = 0;
    for (int ep = 1; ep <= epochs; ep++) {
        memset(g, 0, np * sizeof(float)); double se = 0;
        for (size_t s = 0; s < n; s++) {
            const float* x = X + s * in; const float* r = R + s * out;
            base_linear(W, x, in, out, y);
            for (int o = 0; o < out; o++) { float e = y[o] - r[o]; se += (double)e * e;
                for (int i = 0; i < in; i++) g[(size_t)i * out + o] += x[i] * e; }
        }
        float invn = 1.0f / (float)n;
        for (size_t i = 0; i < np; i++) {
            float gg = g[i] * invn;
            m[i] = b1 * m[i] + (1 - b1) * gg; v[i] = b2 * v[i] + (1 - b2) * gg * gg;
            float mh = m[i] / (1 - powf(b1, (float)ep)), vh = v[i] / (1 - powf(b2, (float)ep));
            W[i] -= lr * mh / (sqrtf(vh) + eps);
        }
        last = se / (double)(n * out);
    }
    free(g); free(m); free(v); free(y); return last;
}
static double dense_mse(const float* W, const float* X, const float* R, size_t n, int in, int out) {
    float* y = malloc((size_t)out * sizeof(float)); double se = 0;
    for (size_t s = 0; s < n; s++) { base_linear(W, X + s * in, in, out, y);
        const float* r = R + s * out; for (int o = 0; o < out; o++) { double e = y[o] - r[o]; se += e * e; } }
    free(y); return se / (double)(n * out);
}
/* L-inf pass-rate: fraction of rows whose every output is within tol of target */
static double passrate(const float* pred, const float* R, size_t n, int out, float tol) {
    size_t ok = 0;
    for (size_t s = 0; s < n; s++) { int good = 1;
        for (int o = 0; o < out; o++) if (fabsf(pred[s * out + o] - R[s * out + o]) > tol) { good = 0; break; }
        ok += good; }
    return (double)ok / (double)n;
}

static void gen(int in, int out, int k, size_t ntr, size_t nte, float noise,
                float** Xtr, float** Rtr, float** Xte, float** Rte) {
    float* U = malloc((size_t)in * k * sizeof(float));
    float* V = malloc((size_t)k * out * sizeof(float));
    for (int i = 0; i < in * k; i++) U[i] = rf();
    for (int i = 0; i < k * out; i++) V[i] = rf();
    float* Wstar = calloc((size_t)in * out, sizeof(float));
    for (int i = 0; i < in; i++) for (int o = 0; o < out; o++) { float s = 0;
        for (int j = 0; j < k; j++) s += U[i * k + j] * V[j * out + o];
        Wstar[(size_t)i * out + o] = s / sqrtf((float)k); }
    size_t n = ntr + nte;
    float* X = malloc(n * in * sizeof(float));
    float* R = malloc(n * out * sizeof(float));
    for (size_t s = 0; s < n; s++) { float* x = X + s * in;
        for (int i = 0; i < in; i++) x[i] = rf();
        base_linear(Wstar, x, in, out, R + s * out);
        for (int o = 0; o < out; o++) R[s * out + o] += rn(noise); }
    *Xtr = X; *Rtr = R; *Xte = X + ntr * in; *Rte = R + ntr * out;
    free(U); free(V); free(Wstar);
}

static void predict_lora(const cce_lora* lo, const float* X, size_t n, int in, int out, float* P) {
    cce_tensor x, y; int xs[1] = { in }, ys[1] = { out };
    cce_tensor_alloc(&x, xs, 1); cce_tensor_alloc(&y, ys, 1);
    for (size_t s = 0; s < n; s++) { memcpy(x.data, X + s * in, in * sizeof(float));
        memset(y.data, 0, out * sizeof(float)); cce_lora_apply(lo, &x, &y);
        memcpy(P + s * out, y.data, out * sizeof(float)); }
    cce_tensor_free(&x); cce_tensor_free(&y);
}
static void predict_dense(const float* W, const float* X, size_t n, int in, int out, float* P) {
    for (size_t s = 0; s < n; s++) base_linear(W, X + s * in, in, out, P + s * out);
}

static void run_regime(const char* name, int in, int out, int k,
                       size_t ntr, size_t nte, float noise, int epochs, float tol) {
    printf("\n=== regime: %s  (in=%d out=%d, intrinsic rank k=%d, ntrain=%zu, noise=%.3f) ===\n",
           name, in, out, k, ntr, noise);
    float *Xtr, *Rtr, *Xte, *Rte;
    gen(in, out, k, ntr, nte, noise, &Xtr, &Rtr, &Xte, &Rte);
    float* P = malloc(nte * out * sizeof(float));

    printf("%-10s %10s %9s %11s %11s %10s %8s\n",
           "student", "params", "teach_ms", "bytes_fp32", "bytes_trit", "held_mse", "pass%");

    /* dense baseline */
    float* W = calloc((size_t)in * out, sizeof(float));
    clock_t t0 = clock();
    dense_fit(W, Xtr, Rtr, ntr, in, out, epochs, 0.02f);
    double dms = (clock() - t0) * 1000.0 / CLOCKS_PER_SEC;
    double dmse = dense_mse(W, Xte, Rte, nte, in, out);
    predict_dense(W, Xte, nte, in, out, P);
    double dpass = passrate(P, Rte, nte, out, tol);
    size_t dparams = (size_t)in * out;
    printf("%-10s %10zu %9.1f %11zu %11s %10.5f %7.1f%%\n",
           "dense", dparams, dms, dparams * 4, "-", dmse, dpass * 100.0);
    free(W);

    int ranks[] = { 2, 4, 8, 16, 32 };
    int best_r = -1;
    for (size_t ri = 0; ri < sizeof(ranks) / sizeof(ranks[0]); ri++) {
        int r = ranks[ri]; if (r > in || r > out) continue;
        cce_lora lo; cce_lora_init(&lo, in, out, r, (float)(2 * r), 7 + r);
        cce_lora_train_opts opt = cce_lora_train_defaults();
        opt.epochs = epochs; opt.lr = 0.02f;
        clock_t r0 = clock();
        cce_lora_train(&lo, Xtr, Rtr, ntr, &opt);
        double rms = (clock() - r0) * 1000.0 / CLOCKS_PER_SEC;
        double rmse = cce_lora_eval_mse(&lo, Xte, Rte, nte);
        predict_lora(&lo, Xte, nte, in, out, P);
        double rpass = passrate(P, Rte, nte, out, tol);
        size_t rparams = cce_lora_param_count(&lo);
        size_t btrit = (rparams * 8 + 4) / 5 + (size_t)(in + out) * 4; /* 1.6-bit + fp scales */
        char lbl[24]; snprintf(lbl, sizeof(lbl), "rank-%d", r);
        printf("%-10s %10zu %9.1f %11zu %11zu %10.5f %7.1f%%\n",
               lbl, rparams, rms, rparams * 4, btrit, rmse, rpass * 100.0);
        if (best_r < 0 && rmse <= dmse * 1.05 && rparams < dparams && rms < dms) best_r = r;
        cce_lora_free(&lo);
    }
    if (best_r > 0) {
        double shrink = (double)((size_t)in * out) / (double)((size_t)best_r * (in + out));
        printf("--> smallest rank matching dense (<=1.05x mse) at fewer params AND less time: rank-%d (%.1fx fewer params)\n",
               best_r, shrink);
    } else {
        printf("--> no rank matched dense under all three criteria (expected in the near-full-rank regime)\n");
    }
    free(P); free(Xtr); /* Rtr shares buffer via gen (X,R own the whole n) */
    free(Rtr);
}

int main(void) {
    const int in = 256, out = 128;
    run_regime("low-rank correction", in, out, 4, 512, 256, 0.03f, 400, 0.15f);
    run_regime("near-full-rank",      in, out, 96, 512, 256, 0.03f, 400, 0.15f);
    printf("\n(interpretation: rank-r wins when the correction is approximately low-rank —\n"
           " the skill-teaching regime; dense is needed only for near-full-rank corrections.)\n");
    return 0;
}
