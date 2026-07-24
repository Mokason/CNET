/* registry_lily — a cce_lily deep-base adapter hosted by registry_lora's certify
   gate. Teach via serve-in-the-loop, certify with the SAME policy/report used for
   cce_lora, serve on PASS. Teacher = the base on the full input; student = the
   base on a low-rank-compressed input (the adapter recovers the lost quality). */

#include "../include/cce/cce_ds_runtime.h"
#include "../include/cce/cce_lily.h"
#include "../include/router/registry_lily.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); g_fail++; } else printf("ok: %s\n", m); } while (0)

static uint32_t S = 3131;
static uint32_t rr(void) { S = S * 1664525u + 1013904223u; return S; }
static float uf(void) { return ((rr() >> 8) * (1.0f / 16777216.0f)) * 2.0f - 1.0f; }

static cce_ds_host *open_host(int n_layer) {
    cce_ds_hparams hp; cce_ds_host_opts opts; cce_ds_host *h = NULL;
    cce_ds_hparams_default_small(&hp);
    hp.n_layer = n_layer; hp.d_model = 128; hp.n_heads = 4;
    hp.qk_nope_head_dim = 16; hp.qk_rope_head_dim = 8; hp.v_head_dim = 16;
    hp.kv_lora_rank = 32; hp.n_expert = 8; hp.n_expert_used = 2; hp.n_ff_exp = 64; hp.vocab = 256;
    cce_ds_host_opts_default(&opts, "tmp_reg_lily.cce", NULL);
    opts.synthetic = 1; opts.max_ctx = 64; opts.dsa_enable = 1; opts.dsa_fraction = 0.25f;
    opts.cold_autoload = 1; opts.bind_cold = 0;
    if (cce_ds_host_open(&h, &hp, &opts) != CCE_OK) return NULL;
    return h;
}
static void fwd_final(cce_ds_host *h, const float *x, float *out) {
    cce_ds_host_reset(h); memcpy(h->residual, x, (size_t)h->d_model * sizeof(float));
    cce_ds_host_forward_token(h); memcpy(out, h->residual, (size_t)h->d_model * sizeof(float));
}

int main(void) {
    const int L = 3, r = 4, kc = 2;
    cce_ds_host *h = open_host(L);
    CHECK(h != NULL, "synthetic DS host opened");
    if (!h) return 1;
    const int d = h->d_model;

    float *U = malloc((size_t)d * kc * 4), *W = malloc((size_t)kc * d * 4);
    for (int i = 0; i < d * kc; i++) U[i] = uf() * 0.3f;
    for (int i = 0; i < kc * d; i++) W[i] = uf() * 0.3f;
    const float cs = 0.6f;
    #define COMPRESS(x, xd) do { float _t[8]; \
        for (int _k = 0; _k < kc; _k++) { float _a = 0; for (int _i = 0; _i < d; _i++) _a += (x)[_i] * U[_i*kc+_k]; _t[_k] = _a; } \
        for (int _o = 0; _o < d; _o++) { float _c = 0; for (int _k = 0; _k < kc; _k++) _c += cs * _t[_k] * W[_k*d+_o]; (xd)[_o] = (x)[_o] - _c; } } while (0)

    const size_t ntr = 256, nval = 128;
    float *Xf = malloc(ntr * d * 4), *Xd = malloc(ntr * d * 4);
    float *Vf = malloc(nval * d * 4), *Vd = malloc(nval * d * 4);
    for (size_t s = 0; s < ntr; s++) { float *xf = Xf + s*d, *xd = Xd + s*d; for (int i = 0; i < d; i++) xf[i] = uf() * 0.1f; COMPRESS(xf, xd); }
    for (size_t s = 0; s < nval; s++) { float *xf = Vf + s*d, *xd = Vd + s*d; for (int i = 0; i < d; i++) xf[i] = uf() * 0.1f; COMPRESS(xf, xd); }

    /* teacher per-layer residuals (train, on full input) + teacher final outputs (val, on full input) */
    float *teach_res = malloc(ntr * L * d * 4);
    cce_lily_collect(h, Xf, ntr, teach_res);
    float *val_teacher_out = malloc(nval * d * 4);
    for (size_t s = 0; s < nval; s++) fwd_final(h, Vf + s*d, val_teacher_out + s*d);

    cce_lily_train_opts inner = cce_lily_train_defaults(); inner.epochs = 300; inner.lr = 0.02f;

    /* ---- gate rejects an untrained (no-op) adapter ---- */
    {
        cce_lily zero; cce_lily_init(&zero, d, L, r, 2.0f, 5, 1);   /* B=0 => no change */
        registry_lora_cert_policy pol; memset(&pol, 0, sizeof pol);
        pol.argmax_mode = 0; pol.max_regressions = (int)nval / 10; pol.min_net_gain = 1;
        registry_lora_cert_report rep;
        int pass = registry_lily_certify(&zero, h, Vd, val_teacher_out, nval, &pol, &rep);
        printf("   (no-op adapter cert: fixes=%d regress=%d base_mse=%.4g adapter_mse=%.4g -> %s)\n",
               rep.fixes, rep.regressions, rep.base_mse, rep.adapter_mse, pass ? "PASS" : "FAIL");
        CHECK(pass == 0, "certify gate REJECTS a no-op adapter (no net fixes)");
        cce_lily_free(&zero);
    }

    /* ---- teach (serve-loop) + certify: a good adapter PASSES the same gate ---- */
    {
        cce_lily ly; cce_lily_init(&ly, d, L, r, 2.0f, 7, 1);
        registry_lora_cert_policy pol; memset(&pol, 0, sizeof pol);
        pol.argmax_mode = 0; pol.max_regressions = (int)nval / 10; pol.min_net_gain = 1;
        registry_lora_cert_report rep;
        int pass = registry_lily_teach_certify(&ly, h, Xd, teach_res, ntr, 10, &inner,
                                               Vd, val_teacher_out, nval, &pol, &rep);
        printf("   (serve-loop adapter cert: fixes=%d regress=%d base_mse=%.4g -> adapter_mse=%.4g -> %s)\n",
               rep.fixes, rep.regressions, rep.base_mse, rep.adapter_mse, pass ? "PASS" : "FAIL");
        CHECK(pass == 1, "teach(serve-loop)+certify PASSES a genuinely improving adapter");
        CHECK(rep.adapter_mse < rep.base_mse * 0.5, "certified adapter more than halves the deep-base error to teacher");
        CHECK(rep.regressions <= (int)nval / 10, "certified adapter stays within the regression budget");
        cce_lily_free(&ly);
    }

    free(U); free(W); free(Xf); free(Xd); free(Vf); free(Vd); free(teach_res); free(val_teacher_out);
    cce_ds_host_close(h);
    printf("\n%s (%d failures)\n", g_fail ? "TESTS FAILED" : "ALL TESTS PASSED", g_fail);
    return g_fail ? 1 : 0;
}
