/* cce_lily with a REAL teacher residual stream — distilled through the live DS
   forward, no planted target.

   Teacher = the model on the FULL input. Student = the SAME model on a
   low-rank-COMPRESSED input (a cheaper/degraded path). Both residual streams are
   captured from the real forward; a Lily adapter is distilled from the teacher's
   per-layer residuals; then serving the compressed-path student WITH the adapter
   moves its final output back toward the teacher's — the adapter recovers the
   quality lost to compression.

   (The other natural teacher — same model at more experts / dense attention — is
   the deployment target, but this synthetic CI model's forward is near-identity
   and invariant to those compute knobs, so it shows no gap there; input
   compression gives a genuine, non-planted gap the real forward does exhibit.) */

#include "../include/cce/cce_ds_runtime.h"
#include "../include/cce/cce_lily.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); g_fail++; } else printf("ok: %s\n", m); } while (0)

static uint32_t S = 24601;
static uint32_t rr(void) { S = S * 1664525u + 1013904223u; return S; }
static float uf(void) { return ((rr() >> 8) * (1.0f / 16777216.0f)) * 2.0f - 1.0f; }

static cce_ds_host *open_host(int n_layer) {
    cce_ds_hparams hp; cce_ds_host_opts opts; cce_ds_host *h = NULL;
    cce_ds_hparams_default_small(&hp);
    hp.n_layer = n_layer; hp.d_model = 128; hp.n_heads = 4;
    hp.qk_nope_head_dim = 16; hp.qk_rope_head_dim = 8; hp.v_head_dim = 16;
    hp.kv_lora_rank = 32; hp.n_expert = 8; hp.n_expert_used = 2; hp.n_ff_exp = 64; hp.vocab = 256;
    cce_ds_host_opts_default(&opts, "tmp_lily_teacher.cce", NULL);
    opts.synthetic = 1; opts.max_ctx = 64; opts.dsa_enable = 1; opts.dsa_fraction = 0.25f;
    opts.cold_autoload = 1; opts.bind_cold = 0;
    if (cce_ds_host_open(&h, &hp, &opts) != CCE_OK) return NULL;
    return h;
}
static void forward_final(cce_ds_host *h, const float *x, float *out) {
    cce_ds_host_reset(h);
    memcpy(h->residual, x, (size_t)h->d_model * sizeof(float));
    cce_ds_host_forward_token(h);
    memcpy(out, h->residual, (size_t)h->d_model * sizeof(float));
}
static double l1(const float *a, const float *b, int n) { double s = 0; for (int i = 0; i < n; i++) s += fabs((double)a[i] - b[i]); return s / n; }

int main(void) {
    const int L = 3, r = 4, kc = 2;   /* compression removes a rank-kc component */
    cce_ds_host *h = open_host(L);
    CHECK(h != NULL, "synthetic DS host opened");
    if (!h) return 1;
    const int d = h->d_model;

    /* a fixed low-rank compressor: x_deg = x - cs * W (U^T x) */
    float *U = malloc((size_t)d * kc * 4), *W = malloc((size_t)kc * d * 4);
    for (int i = 0; i < d * kc; i++) U[i] = uf() * 0.3f;
    for (int i = 0; i < kc * d; i++) W[i] = uf() * 0.3f;
    const float cs = 0.6f;
    #define COMPRESS(x, xd) do { float _t[8]; \
        for (int _k = 0; _k < kc; _k++) { float _a = 0; for (int _i = 0; _i < d; _i++) _a += (x)[_i] * U[_i*kc+_k]; _t[_k] = _a; } \
        for (int _o = 0; _o < d; _o++) { float _c = 0; for (int _k = 0; _k < kc; _k++) _c += cs * _t[_k] * W[_k*d+_o]; (xd)[_o] = (x)[_o] - _c; } } while (0)

    const size_t ntr = 256, nte = 64;
    float *Xf = malloc(ntr * d * 4), *Xd = malloc(ntr * d * 4);
    float *Xtf = malloc(nte * d * 4), *Xtd = malloc(nte * d * 4);
    for (size_t s = 0; s < ntr; s++) { float *xf = Xf + s*d, *xd = Xd + s*d;
        for (int i = 0; i < d; i++) xf[i] = uf() * 0.1f; COMPRESS(xf, xd); }
    for (size_t s = 0; s < nte; s++) { float *xf = Xtf + s*d, *xd = Xtd + s*d;
        for (int i = 0; i < d; i++) xf[i] = uf() * 0.1f; COMPRESS(xf, xd); }

    /* ---- capture BOTH residual streams from the real forward ---- */
    float *teach_res = malloc(ntr * L * d * 4), *stud_res = malloc(ntr * L * d * 4);
    CHECK(cce_lily_collect(h, Xf, ntr, teach_res) == CCE_OK, "capture TEACHER residual stream (full input) from real forward");
    CHECK(cce_lily_collect(h, Xd, ntr, stud_res) == CCE_OK, "capture STUDENT residual stream (compressed input) from real forward");
    double gap0 = l1(stud_res, teach_res, (int)(ntr * L * d));
    printf("   (per-layer teacher-vs-student residual gap = %.3e, non-planted)\n", gap0);
    CHECK(gap0 > 1e-6, "the compressed path yields a real per-layer residual gap to distill");

    /* ---- distill a Lily adapter from the teacher's residual stream ---- */
    cce_lily fit; cce_lily_init(&fit, d, L, r, 2.0f, 13, 1);
    cce_lily_train_opts o = cce_lily_train_defaults(); o.epochs = 1500; o.lr = 0.02f;
    double mse = cce_lily_train_residual(&fit, stud_res, teach_res, ntr, &o);
    printf("   (distill mse -> %.5g)\n", mse);
    CHECK(mse >= 0 && mse < gap0 * gap0, "distill-train closes the residual gap");

    /* ---- verify serving. Two credit-assignment modes ---- */
    const size_t nb = (size_t)L * r * d * sizeof(float);
    float *saveB = malloc(nb); memcpy(saveB, fit.B, nb);
    float *Rt = malloc((size_t)d*4), *Rs = malloc((size_t)d*4), *Ra = malloc((size_t)d*4);

    /* mode 1: apply the distilled delta at EVERY layer (free-running) */
    double gb = 0, gall = 0;
    for (size_t s = 0; s < nte; s++) {
        forward_final(h, Xtf + s*d, Rt);
        forward_final(h, Xtd + s*d, Rs);
        cce_lily_install_serving(&fit); forward_final(h, Xtd + s*d, Ra); cce_lily_uninstall_serving();
        gb += l1(Rs, Rt, d); gall += l1(Ra, Rt, d);
    }
    gb /= nte; gall /= nte;
    printf("   (final gap to teacher — student %.3e | all-layer serve %.3e)\n", gb, gall);
    printf("   FINDING: per-layer distillation trains each layer to close the FULL gap; applying\n"
           "   all L deltas free-running OVER-corrects (they compound: %+.0f%%). Correct credit\n"
           "   needs serve-in-the-loop training, or adapting only the output-feeding last layer:\n",
           100.0 * (gb - gall) / (gb + 1e-30));

    /* mode 2: serve ONLY the last layer's delta (feeds the output directly, no compounding) */
    memset(fit.B, 0, (size_t)(L - 1) * r * d * sizeof(float));   /* zero all but last layer */
    double glast = 0;
    for (size_t s = 0; s < nte; s++) {
        forward_final(h, Xtf + s*d, Rt);
        cce_lily_install_serving(&fit); forward_final(h, Xtd + s*d, Ra); cce_lily_uninstall_serving();
        glast += l1(Ra, Rt, d);
    }
    glast /= nte;
    memcpy(fit.B, saveB, nb);
    printf("   (last-layer-only serve %.3e — closed %.0f%%)\n", glast, 100.0 * (gb - glast) / (gb + 1e-30));
    CHECK(glast < gb * 0.5, "correct-credit (last-layer) adapter recovers >=50%% of the teacher gap");
    free(saveB);

    free(U); free(W); free(Xf); free(Xd); free(Xtf); free(Xtd);
    free(teach_res); free(stud_res); free(Rt); free(Rs); free(Ra);
    cce_lily_free(&fit); cce_ds_host_close(h);
    printf("\n%s (%d failures)\n", g_fail ? "TESTS FAILED" : "ALL TESTS PASSED", g_fail);
    return g_fail ? 1 : 0;
}
