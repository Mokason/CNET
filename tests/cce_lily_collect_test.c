/* cce_lily training-data collection loop through the deep forward, end to end:
   run the real DS residual forward over a batch, CAPTURE the per-layer residuals
   (the collection loop), plant a per-layer teacher correction on them, then
   distill-train a Lily adapter on the collected data (no base autograd) and
   verify it recovers the planted per-layer correction on held-out residuals. */

#include "../include/cce/cce_ds_runtime.h"
#include "../include/cce/cce_lily.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); g_fail++; } else printf("ok: %s\n", m); } while (0)

static uint32_t S = 9001;
static uint32_t rr(void) { S = S * 1664525u + 1013904223u; return S; }
static float uf(void) { return ((rr() >> 8) * (1.0f / 16777216.0f)) * 2.0f - 1.0f; }

static cce_ds_host *open_host(int n_layer) {
    cce_ds_hparams hp; cce_ds_host_opts opts; cce_ds_host *h = NULL;
    cce_ds_hparams_default_small(&hp);
    hp.n_layer = n_layer; hp.d_model = 128; hp.n_heads = 4;
    hp.qk_nope_head_dim = 16; hp.qk_rope_head_dim = 8; hp.v_head_dim = 16;
    hp.kv_lora_rank = 32; hp.n_expert = 4; hp.n_expert_used = 2; hp.n_ff_exp = 64; hp.vocab = 256;
    cce_ds_host_opts_default(&opts, "tmp_lily_collect.cce", NULL);
    opts.synthetic = 1; opts.max_ctx = 64; opts.dsa_enable = 1; opts.dsa_fraction = 0.25f;
    opts.cold_autoload = 1; opts.bind_cold = 0;
    if (cce_ds_host_open(&h, &hp, &opts) != CCE_OK) return NULL;
    return h;
}

int main(void) {
    const int L = 3, k = 2, r = 4;
    cce_ds_host *h = open_host(L);
    CHECK(h != NULL, "synthetic DS host opened");
    if (!h) return 1;
    const int d = h->d_model;

    /* inputs (initial residuals) */
    const size_t ntr = 256, nte = 128;
    float *Xtr = malloc(ntr * d * 4), *Xte = malloc(nte * d * 4);
    for (size_t i = 0; i < ntr * d; i++) Xtr[i] = uf() * 0.05f;
    for (size_t i = 0; i < nte * d; i++) Xte[i] = uf() * 0.05f;

    /* ---- the collection loop: capture per-layer residuals from the real forward ---- */
    float *Rtr = malloc(ntr * L * d * 4), *Rte = malloc(nte * L * d * 4);
    CHECK(cce_lily_collect(h, Xtr, ntr, Rtr) == CCE_OK, "collect train residuals through DS forward");
    CHECK(cce_lily_collect(h, Xte, nte, Rte) == CCE_OK, "collect held-out residuals through DS forward");
    /* the forward actually evolved the residual (attn+ffn), not an echo of input */
    {
        float md = 0; const float *rL = Rtr + (size_t)(L - 1) * d;   /* sample 0, last layer */
        for (int i = 0; i < d; i++) { float e = fabsf(rL[i] - Xtr[i]); if (e > md) md = e; }
        printf("   (captured residual differs from input by up to %.3e — the forward ran;\n"
               "    small because the synthetic test model's attn+ffn are near-identity)\n", md);
        CHECK(md > 1e-7f, "collection captured the live forward's (evolved) residual, not the input");
    }

    /* ---- plant a per-layer teacher correction on the collected residuals ----
       corr_L(x) = cs * V_L (U x), U shared [d,k], V_L [k,d]. target = base + corr. */
    const float cs = 0.15f;
    float *U = malloc((size_t)d * k * 4), *V = malloc((size_t)L * k * d * 4);
    for (int i = 0; i < d * k; i++) U[i] = uf() * 0.25f;
    for (int i = 0; i < L * k * d; i++) V[i] = uf() * 0.2f;
    /* build training targets: Ttr = Rtr + corr(Rtr) */
    float *Ttr = malloc(ntr * L * d * 4);
    float tk[8];
    for (size_t sIdx = 0; sIdx < ntr; sIdx++) for (int l = 0; l < L; l++) {
        const float *ri = Rtr + ((size_t)sIdx * L + l) * d;
        float *to = Ttr + ((size_t)sIdx * L + l) * d;
        for (int kk = 0; kk < k; kk++) { float a = 0; for (int i = 0; i < d; i++) a += ri[i] * U[i * k + kk]; tk[kk] = a; }
        for (int o = 0; o < d; o++) { float c = 0; for (int kk = 0; kk < k; kk++) c += cs * tk[kk] * V[((size_t)l * k + kk) * d + o];
            to[o] = ri[o] + c; }
    }

    /* ---- distill-train the adapter on the collected data (no base autograd) ---- */
    cce_lily fit; cce_lily_init(&fit, d, L, r, 2.0f, 11, /*shared=*/1);
    cce_lily_train_opts o = cce_lily_train_defaults(); o.epochs = 1200; o.lr = 0.02f;
    double mse = cce_lily_train_residual(&fit, Rtr, Ttr, ntr, &o);
    printf("   (distill mse -> %.5g)\n", mse);
    CHECK(mse >= 0 && mse < 1e-4, "distill-train fits the per-layer residual correction");

    /* ---- verify recovery on HELD-OUT collected residuals ---- */
    float sc = 2.0f / (float)r, md = 0;
    for (size_t sIdx = 0; sIdx < nte; sIdx++) for (int l = 0; l < L; l++) {
        const float *ri = Rte + ((size_t)sIdx * L + l) * d;
        const float *A = fit.A;                         /* shared */
        const float *B = fit.B + (size_t)l * r * d;
        float ftk[8]; for (int kk = 0; kk < r; kk++) { float a = 0; for (int i = 0; i < d; i++) a += ri[i] * A[i * r + kk]; ftk[kk] = a; }
        float utk[8]; for (int kk = 0; kk < k; kk++) { float a = 0; for (int i = 0; i < d; i++) a += ri[i] * U[i * k + kk]; utk[kk] = a; }
        for (int oo = 0; oo < d; oo++) {
            float fit_delta = 0; for (int kk = 0; kk < r; kk++) fit_delta += sc * ftk[kk] * B[kk * d + oo];
            float true_corr = 0; for (int kk = 0; kk < k; kk++) true_corr += cs * utk[kk] * V[((size_t)l * k + kk) * d + oo];
            float e = fabsf(fit_delta - true_corr); if (e > md) md = e;
        }
    }
    printf("   (held-out fit-delta vs planted-correction max diff = %.3e)\n", md);
    CHECK(md < 5e-3f, "adapter recovered the planted per-layer correction on held-out residuals");

    free(Xtr); free(Xte); free(Rtr); free(Rte); free(Ttr); free(U); free(V);
    cce_lily_free(&fit); cce_ds_host_close(h);
    printf("\n%s (%d failures)\n", g_fail ? "TESTS FAILED" : "ALL TESTS PASSED", g_fail);
    return g_fail ? 1 : 0;
}
