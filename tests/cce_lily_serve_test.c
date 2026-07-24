/* cce_lily interior-layer serve hook, verified against the real DS residual
   forward (synthetic runtime). Proves: the hook fires once per layer; a bound
   adapter adds exactly (alpha/r)B_L(A_L·residual) at each layer; a zero-delta
   adapter and an uninstalled hook leave decode byte-identical. */

#include "../include/cce/cce_ds_runtime.h"
#include "../include/cce/cce_lily.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); g_fail++; } else printf("ok: %s\n", m); } while (0)

static uint32_t S = 4242;
static uint32_t rr(void) { S = S * 1664525u + 1013904223u; return S; }
static float rn(void) {
    float u1 = (rr() >> 8) * (1.0f / 16777216.0f), u2 = (rr() >> 8) * (1.0f / 16777216.0f);
    if (u1 < 1e-7f) u1 = 1e-7f; return sqrtf(-2.0f * logf(u1)) * cosf(6.2831853f * u2);
}

static cce_ds_host *open_host(int n_layer) {
    cce_ds_hparams hp; cce_ds_host_opts opts; cce_ds_host *h = NULL;
    cce_ds_hparams_default_small(&hp);
    hp.n_layer = n_layer; hp.d_model = 128; hp.n_heads = 4;
    hp.qk_nope_head_dim = 16; hp.qk_rope_head_dim = 8; hp.v_head_dim = 16;
    hp.kv_lora_rank = 32; hp.n_expert = 4; hp.n_expert_used = 2; hp.n_ff_exp = 64; hp.vocab = 256;
    cce_ds_host_opts_default(&opts, "tmp_lily_serve.cce", NULL);
    opts.synthetic = 1; opts.max_ctx = 64; opts.dsa_enable = 1; opts.dsa_fraction = 0.25f;
    opts.cold_autoload = 1; opts.bind_cold = 0;
    if (cce_ds_host_open(&h, &hp, &opts) != CCE_OK) return NULL;
    return h;
}
static void seed(cce_ds_host *h) {
    cce_ds_host_reset(h);
    for (int i = 0; i < h->d_model; i++) h->residual[i] = 0.02f * sinf(0.07f * (float)i);
}
static void run(cce_ds_host *h, float *out) {
    seed(h); cce_ds_host_forward_token(h);
    memcpy(out, h->residual, (size_t)h->d_model * sizeof(float));
}

/* probe: count hook firings + record layer order */
static int g_calls = 0, g_layers[16];
static void probe(int layer, float *r, int w, void *ctx) { (void)r; (void)w; (void)ctx;
    if (g_calls < 16) g_layers[g_calls] = layer; g_calls++; }

int main(void) {
    /* ---- the hook fires once per layer, in order ---- */
    {
        cce_ds_host *h = open_host(3);
        CHECK(h != NULL, "synthetic DS host (3 layers) opened");
        if (!h) return 1;
        g_calls = 0; g_cce_layer_adapt_hook = probe; g_cce_layer_adapt_ctx = NULL;
        seed(h); cce_ds_host_forward_token(h);
        g_cce_layer_adapt_hook = NULL;
        CHECK(g_calls == 3 && g_layers[0] == 0 && g_layers[1] == 1 && g_layers[2] == 2,
              "hook fires once per layer, layers 0..n-1 in order");
        cce_ds_host_close(h);
    }

    /* ---- exact single-layer delta on the real residual ---- */
    {
        cce_ds_host *h = open_host(1);
        if (!h) { printf("FAIL open\n"); return 1; }
        const int d = h->d_model;
        float *R_off = malloc((size_t)d * 4), *R_on = malloc((size_t)d * 4);
        run(h, R_off);                                   /* baseline, no hook */

        cce_lily ly; cce_lily_init(&ly, d, 1, 8, 4.0f, 7, /*shared=*/1);
        for (size_t i = 0; i < (size_t)d * 8; i++) ly.A[i] = rn() * 0.2f;
        for (size_t i = 0; i < (size_t)8 * d; i++) ly.B[i] = rn() * 0.1f;

        cce_lily_install_serving(&ly);
        run(h, R_on);                                    /* base + layer-0 delta */
        cce_lily_uninstall_serving();

        /* reference: delta on R_off = (alpha/r) B (A·R_off) */
        const int r = 8; const float sc = 4.0f / (float)r;
        float tmp[8]; for (int k = 0; k < r; k++) { float a = 0; for (int i = 0; i < d; i++) a += R_off[i] * ly.A[i * r + k]; tmp[k] = a; }
        float md = 0;
        for (int o = 0; o < d; o++) {
            float ref = R_off[o]; for (int k = 0; k < r; k++) ref += sc * tmp[k] * ly.B[k * d + o];
            float e = fabsf(R_on[o] - ref); if (e > md) md = e;
        }
        printf("   (interior-hook vs reference delta max diff = %.2e)\n", md);
        CHECK(md < 1e-4f, "served residual == base + (alpha/r)B(A·residual) at the layer");

        cce_lily_free(&ly); free(R_off); free(R_on); cce_ds_host_close(h);
    }

    /* ---- safety: zero-delta and uninstalled hook == byte-identical decode ---- */
    {
        cce_ds_host *h = open_host(3);
        if (!h) { printf("FAIL open\n"); return 1; }
        const int d = h->d_model;
        float *R_off = malloc((size_t)d * 4), *R_t = malloc((size_t)d * 4);
        run(h, R_off);

        cce_lily zero; cce_lily_init(&zero, d, 3, 8, 4.0f, 1, 1);  /* B=0 => delta 0 */
        cce_lily_install_serving(&zero);
        run(h, R_t);
        CHECK(memcmp(R_off, R_t, (size_t)d * 4) == 0, "zero-delta adapter: decode is byte-identical");

        /* now a nonzero adapter DOES change decode (per-layer, compounding) */
        cce_lily nz; cce_lily_init(&nz, d, 3, 8, 4.0f, 2, 1);
        for (size_t i = 0; i < (size_t)d * 8; i++) nz.A[i] = rn() * 0.2f;
        for (size_t i = 0; i < (size_t)3 * 8 * d; i++) nz.B[i] = rn() * 0.1f;
        cce_lily_install_serving(&nz);
        run(h, R_t);
        float md = 0; for (int o = 0; o < d; o++) { float e = fabsf(R_t[o] - R_off[o]); if (e > md) md = e; }
        CHECK(md > 1e-6f, "nonzero adapter changes decode (delta applied at every layer)");
        printf("   (3-layer nonzero adapter shifts residual by up to %.3e)\n", md);

        cce_lily_uninstall_serving();
        run(h, R_t);
        CHECK(memcmp(R_off, R_t, (size_t)d * 4) == 0, "after uninstall: decode is the exact frozen base");

        cce_lily_free(&zero); cce_lily_free(&nz); free(R_off); free(R_t); cce_ds_host_close(h);
    }

    printf("\n%s (%d failures)\n", g_fail ? "TESTS FAILED" : "ALL TESTS PASSED", g_fail);
    return g_fail ? 1 : 0;
}
