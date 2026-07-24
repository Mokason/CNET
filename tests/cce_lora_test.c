/* cce_lora unit test — M1 (apply/merge/init), M2 (train fits low-rank), M3
   (save/load + cascade bridge round-trip). Returns nonzero on any failure. */

#include "../include/cce/cce_lora.h"
#include "../include/cce/cce_tensor.h"
#include "../include/cce/cce_block.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); g_fail++; } \
                              else printf("ok: %s\n", msg); } while (0)

static uint32_t S = 12345;
static uint32_t rnd(void) { S = S * 1664525u + 1013904223u; return S; }
static float rf(void) { return ((rnd() >> 8) * (1.0f / 16777216.0f)) * 2.0f - 1.0f; } /* (-1,1) */

/* reference base linear: y[o] = b[o] + sum_i x[i]*W[i*out+o] */
static void base_linear(const float* W, const float* b, const float* x,
                        int in, int out, float* y) {
    for (int o = 0; o < out; o++) y[o] = b ? b[o] : 0.0f;
    for (int i = 0; i < in; i++) {
        float xi = x[i];
        for (int o = 0; o < out; o++) y[o] += xi * W[(size_t)i * out + o];
    }
}

static float maxabsdiff(const float* a, const float* b, int n) {
    float m = 0.0f;
    for (int i = 0; i < n; i++) { float d = fabsf(a[i] - b[i]); if (d > m) m = d; }
    return m;
}

int main(void) {
    const int in = 24, out = 10, rank = 4;

    /* ---------- M1: init makes delta exactly 0 ---------- */
    cce_lora lo;
    CHECK(cce_lora_init(&lo, in, out, rank, 8.0f, 7) == CCE_OK, "init ok");
    CHECK(cce_lora_param_count(&lo) == (size_t)rank * (in + out), "param count = rank*(in+out)");
    CHECK(cce_lora_dense_param_count(&lo) == (size_t)in * out, "dense param count = in*out");
    {
        cce_tensor x, y; int xs[1] = { in }, ys[1] = { out };
        cce_tensor_alloc(&x, xs, 1); cce_tensor_alloc(&y, ys, 1);
        for (int i = 0; i < in; i++) x.data[i] = rf();
        for (int o = 0; o < out; o++) y.data[o] = 3.14f + o;   /* base out */
        float before[64]; memcpy(before, y.data, out * sizeof(float));
        cce_lora_apply(&lo, &x, &y);
        CHECK(maxabsdiff(before, y.data, out) == 0.0f, "untrained apply is a no-op (B=0)");
        cce_tensor_free(&x); cce_tensor_free(&y);
    }

    /* ---------- M1: apply(base) == base(merged W) for arbitrary A,B ---------- */
    for (size_t i = 0; i < lo.A.numel; i++) lo.A.data[i] = rf() * 0.5f;
    for (size_t i = 0; i < lo.B.numel; i++) lo.B.data[i] = rf() * 0.5f;
    {
        float *W = malloc((size_t)in * out * sizeof(float));
        float *b = malloc((size_t)out * sizeof(float));
        for (int i = 0; i < in * out; i++) W[i] = rf();
        for (int o = 0; o < out; o++) b[o] = rf();

        cce_tensor Wm; int ws[2] = { in, out }; cce_tensor_alloc(&Wm, ws, 2);
        memcpy(Wm.data, W, (size_t)in * out * sizeof(float));
        cce_lora_merge(&lo, &Wm);                      /* Wm = W + delta */

        cce_tensor x, y; int xs[1] = { in }, ys[1] = { out };
        cce_tensor_alloc(&x, xs, 1); cce_tensor_alloc(&y, ys, 1);
        float ymerged[64], ydelta[64];
        float md = 0.0f;
        for (int t = 0; t < 32; t++) {
            for (int i = 0; i < in; i++) x.data[i] = rf();
            base_linear(Wm.data, b, x.data, in, out, ymerged);      /* merged path */
            base_linear(W, b, x.data, in, out, ydelta);             /* base ... */
            memcpy(y.data, ydelta, out * sizeof(float));
            cce_lora_apply(&lo, &x, &y);                            /* ... + apply delta */
            float d = maxabsdiff(ymerged, y.data, out);
            if (d > md) md = d;
        }
        CHECK(md < 1e-3f, "apply-delta == merged-W (max diff < 1e-3)");
        printf("   (max diff = %.2e)\n", md);
        cce_tensor_free(&Wm); cce_tensor_free(&x); cce_tensor_free(&y);
        free(W); free(b);
    }
    cce_lora_free(&lo);

    /* ---------- M2: train recovers a planted low-rank map ---------- */
    {
        const int k = 3;   /* true rank of the target correction */
        /* Wstar = U V, U:[in,k], V:[k,out] */
        float *U = malloc((size_t)in * k * sizeof(float));
        float *V = malloc((size_t)k * out * sizeof(float));
        float *Wstar = calloc((size_t)in * out, sizeof(float));
        for (int i = 0; i < in * k; i++) U[i] = rf();
        for (int i = 0; i < k * out; i++) V[i] = rf();
        for (int i = 0; i < in; i++) for (int o = 0; o < out; o++) {
            float s = 0; for (int j = 0; j < k; j++) s += U[i * k + j] * V[j * out + o];
            Wstar[(size_t)i * out + o] = s;
        }
        const size_t n = 256;
        float *X = malloc(n * in * sizeof(float));
        float *R = malloc(n * out * sizeof(float));
        for (size_t s = 0; s < n; s++) {
            float* xs = X + s * in; for (int i = 0; i < in; i++) xs[i] = rf();
            base_linear(Wstar, NULL, xs, in, out, R + s * out);      /* residual = x Wstar */
        }
        cce_lora ltr;
        cce_lora_init(&ltr, in, out, rank, 8.0f, 3);                 /* rank 4 >= true 3 */
        cce_lora_train_opts opt = cce_lora_train_defaults();
        opt.epochs = 800; opt.lr = 0.02f;
        double mse0 = cce_lora_eval_mse(&ltr, X, R, n);
        double mse1 = cce_lora_train(&ltr, X, R, n, &opt);
        printf("   (train mse %.4g -> %.4g)\n", mse0, mse1);
        CHECK(mse1 < 1e-3 && mse1 < mse0 * 1e-2, "train fits planted low-rank residual");

        /* held-out generalisation: merge into zero W, compare map to Wstar on new x */
        cce_tensor Wz; int ws[2] = { in, out }; cce_tensor_alloc(&Wz, ws, 2);
        memset(Wz.data, 0, (size_t)in * out * sizeof(float));
        cce_lora_merge(&ltr, &Wz);
        float ytrue[64], ypred[64], md = 0.0f;
        for (int t = 0; t < 64; t++) {
            float xh[64]; for (int i = 0; i < in; i++) xh[i] = rf();
            base_linear(Wstar, NULL, xh, in, out, ytrue);
            base_linear(Wz.data, NULL, xh, in, out, ypred);
            float d = maxabsdiff(ytrue, ypred, out); if (d > md) md = d;
        }
        printf("   (held-out max diff = %.3e)\n", md);
        CHECK(md < 5e-2f, "recovered map generalises to held-out inputs");

        /* ---------- M3: save/load round-trip ---------- */
        const char* path = "/tmp/cce_lora_rt.bin";
        CHECK(cce_lora_save(&ltr, path) == CCE_OK, "save ok");
        cce_lora lrd;
        CHECK(cce_lora_load(&lrd, path) == CCE_OK, "load ok");
        CHECK(lrd.in_dim == in && lrd.out_dim == out && lrd.rank == rank, "load meta matches");
        CHECK(maxabsdiff(ltr.A.data, lrd.A.data, (int)ltr.A.numel) == 0.0f, "A round-trips exactly");
        CHECK(maxabsdiff(ltr.B.data, lrd.B.data, (int)ltr.B.numel) == 0.0f, "B round-trips exactly");

        /* ---------- M3: cascade bridge preserves the delta ---------- */
        cce_cascade* cas = NULL;
        CHECK(cce_lora_to_cascade(&ltr, &cas) == CCE_OK && cas != NULL, "to_cascade ok");
        cce_lora lfc;
        CHECK(cce_lora_from_cascade(cas, in, out, rank, &lfc) == CCE_OK, "from_cascade ok");
        {
            cce_tensor x, ya, yb; int xs[1] = { in }, ys[1] = { out };
            cce_tensor_alloc(&x, xs, 1); cce_tensor_alloc(&ya, ys, 1); cce_tensor_alloc(&yb, ys, 1);
            float md2 = 0.0f;
            for (int t = 0; t < 32; t++) {
                for (int i = 0; i < in; i++) x.data[i] = rf();
                memset(ya.data, 0, out * sizeof(float)); memset(yb.data, 0, out * sizeof(float));
                cce_lora_apply(&ltr, &x, &ya);
                cce_lora_apply(&lfc, &x, &yb);
                float d = maxabsdiff(ya.data, yb.data, out); if (d > md2) md2 = d;
            }
            printf("   (cascade round-trip max diff = %.2e)\n", md2);
            CHECK(md2 < 1e-4f, "cascade bridge preserves the delta");
            cce_tensor_free(&x); cce_tensor_free(&ya); cce_tensor_free(&yb);
        }
        cce_cascade_destroy(cas);
        cce_lora_free(&lfc); cce_lora_free(&lrd); cce_lora_free(&ltr);
        cce_tensor_free(&Wz);
        free(U); free(V); free(Wstar); free(X); free(R);
    }

    /* ---------- M4: attach to a real LINEAR_HEAD block ---------- */
    {
        const int k = 3;
        cce_block head;
        cce_block_init_linear(&head, in, out, 0.0f);
        head.type = CCE_BLOCK_LINEAR_HEAD;                    /* pure linear (no sigmoid) */
        for (size_t i = 0; i < head.weights.numel; i++) head.weights.data[i] = rf() * 0.3f;
        for (int o = 0; o < out; o++) head.bias.data[o] = rf() * 0.1f;

        /* teacher = frozen head + a planted low-rank correction */
        float *U = malloc((size_t)in * k * sizeof(float));
        float *V = malloc((size_t)k * out * sizeof(float));
        for (int i = 0; i < in * k; i++) U[i] = rf() * 0.5f;
        for (int i = 0; i < k * out; i++) V[i] = rf() * 0.5f;

        const size_t n = 256;
        float *X = malloc(n * in * sizeof(float));
        float *BO = malloc(n * out * sizeof(float));   /* base head out */
        float *TO = malloc(n * out * sizeof(float));   /* teacher out   */
        cce_tensor xt, yt; int xs[1] = { in }, ys[1] = { out };
        cce_tensor_alloc(&xt, xs, 1); cce_tensor_alloc(&yt, ys, 1);
        for (size_t s = 0; s < n; s++) {
            float* x = X + s * in; for (int i = 0; i < in; i++) x[i] = rf();
            memcpy(xt.data, x, in * sizeof(float));
            memset(yt.data, 0, out * sizeof(float));
            cce_block_forward(&head, &xt, &yt);               /* base head */
            memcpy(BO + s * out, yt.data, out * sizeof(float));
            /* teacher = base + planted low-rank map applied to x */
            float corr[64];
            for (int o = 0; o < out; o++) corr[o] = 0.0f;
            for (int i = 0; i < in; i++) for (int j = 0; j < k; j++) {
                float ui = U[i * k + j];
                for (int o = 0; o < out; o++) corr[o] += x[i] * ui * V[j * out + o];
            }
            for (int o = 0; o < out; o++) TO[s * out + o] = BO[s * out + o] + corr[o];
        }

        cce_lora adp;
        cce_lora_init(&adp, in, out, rank, 8.0f, 5);
        cce_lora_train_opts opt = cce_lora_train_defaults();
        opt.epochs = 800; opt.lr = 0.02f;
        double mse = cce_lora_fit_residual(&adp, X, BO, TO, n, &opt);
        printf("   (fit_residual mse = %.4g)\n", mse);
        CHECK(mse >= 0 && mse < 1e-3, "fit_residual learns teacher correction");

        /* head_forward(NULL) == frozen base; head_forward(adapter) ~= teacher */
        float mdNull = 0.0f, mdAdp = 0.0f;
        for (int t = 0; t < 64; t++) {
            for (int i = 0; i < in; i++) xt.data[i] = rf();
            /* expected base + expected teacher for this fresh x */
            float base[64], corr[64];
            memset(yt.data, 0, out * sizeof(float));
            cce_block_forward(&head, &xt, &yt); memcpy(base, yt.data, out * sizeof(float));
            for (int o = 0; o < out; o++) corr[o] = 0.0f;
            for (int i = 0; i < in; i++) for (int j = 0; j < k; j++) {
                float ui = U[i * k + j];
                for (int o = 0; o < out; o++) corr[o] += xt.data[i] * ui * V[j * out + o];
            }
            memset(yt.data, 0, out * sizeof(float));
            cce_lora_head_forward(&head, NULL, &xt, &yt);
            float dn = maxabsdiff(base, yt.data, out); if (dn > mdNull) mdNull = dn;
            memset(yt.data, 0, out * sizeof(float));
            cce_lora_head_forward(&head, &adp, &xt, &yt);
            float teacher[64]; for (int o = 0; o < out; o++) teacher[o] = base[o] + corr[o];
            float da = maxabsdiff(teacher, yt.data, out); if (da > mdAdp) mdAdp = da;
        }
        printf("   (head_forward NULL diff=%.2e  adapter diff=%.3e)\n", mdNull, mdAdp);
        CHECK(mdNull == 0.0f, "head_forward(NULL) is the exact frozen base (zero overhead)");
        CHECK(mdAdp < 5e-2f, "head_forward(adapter) reproduces the teacher on held-out x");

        cce_tensor_free(&xt); cce_tensor_free(&yt);
        cce_block_free(&head);
        cce_lora_free(&adp);
        free(U); free(V); free(X); free(BO); free(TO);
    }

    printf("\n%s (%d failures)\n", g_fail ? "TESTS FAILED" : "ALL TESTS PASSED", g_fail);
    return g_fail ? 1 : 0;
}
