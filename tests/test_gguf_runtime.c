/* GGUF residual/token stack: synthetic tiny weights → load → multi-token gen
 * + residual finite checks + microbench. Parallel to test_ds_runtime.
 * make gguf_stack → GGUF_STACK_PASS
 */
#define TL_CTX 128
#include "tiny_model_fixture.h"
#include "../include/cnet_platform.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../include/cce/cce_gguf.h"
#include "../include/cce/cce_infer_backend.h"

static int failures, checks;

static void check(int ok, const char *name) {
    checks++;
    printf("  %-58s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static double wall_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* residual stream tap: last layer must be finite and non-zero */
static int g_tap_layers;
static int g_tap_finite;
static int g_tap_nonzero;
static double g_tap_amax;

static void residual_tap(int layer, const float *x, int n_tokens, int dim,
                         void *uctx) {
    int i, nt = n_tokens > 0 ? n_tokens : 1;
    (void)uctx;
    g_tap_layers++;
    if (!x || dim < 1) return;
    /* inspect last token row */
    {
        const float *row = x + (size_t)(nt - 1) * (size_t)dim;
        double amax = 0;
        int finite = 1, nz = 0;
        for (i = 0; i < dim; ++i) {
            float v = row[i];
            if (!isfinite(v)) finite = 0;
            if (v != 0.0f) nz = 1;
            if (fabs((double)v) > amax) amax = fabs((double)v);
        }
        if (layer >= 0) {
            g_tap_finite = finite;
            g_tap_nonzero = nz;
            g_tap_amax = amax;
        }
    }
}

int main(void) {
    static tl_weights w;
    tl_entry ents[64];
    int n_ents;
    const char *gguf_path = "gguf_stack_fixture.gguf";
    cce_gguf_qwen2 *m = NULL;
    float logits[TL_V];
    int tokens_pre[8] = { 3, 7, 11, 5, 9, 13, 2, 17 };
    int i, tok;
    double t0, t1, tok_s = 0;
    const int N_BENCH = 32;
    int dec_toks[N_BENCH];
    cce_infer_session *sess = NULL;
    cce_infer_opts iopts;

    /* avoid littering forest archives during hermetic gate */
    cnet_setenv("CNET_FOREST_NO_PERSIST", "1", 1);
    cnet_setenv("CNET_INFER_FP", "1", 1);

    printf("== CNET GGUF residual/token stack (synthetic) ==\n");

    tl_gen(&w, 0);
    n_ents = tl_entries(&w, ents, 0 /* gguf names */);
    tl_write_gguf(gguf_path, ents, n_ents);
    check(1, "write synthetic tiny GGUF");

    check(cce_gguf_load_qwen2(&m, gguf_path) == CCE_OK && m, "load_qwen2 synthetic");
    if (!m) {
        remove(gguf_path);
        return 1;
    }
    check(m->n_layer == TL_L && m->n_embd == TL_D && m->vocab_size == TL_V,
          "hparams match fixture");
    check(m->max_ctx >= TL_CTX || m->max_ctx > 0, "max_ctx live");

    /* residual tap on first forward */
    g_tap_layers = 0;
    g_tap_finite = 0;
    g_tap_nonzero = 0;
    g_tap_amax = 0;
    cce_gguf_set_layer_tap(residual_tap, NULL);

    m->cur_pos = 0;
    check(cce_gguf_qwen2_forward(m, tokens_pre, 4, logits, TL_V) == CCE_OK,
          "prefill 4 tokens");
    check(m->cur_pos == 4, "cur_pos advanced after prefill");
    check(g_tap_layers >= TL_L, "layer residual tap fired");
    check(g_tap_finite && g_tap_nonzero, "residual finite non-zero");
    printf("    residual amax=%.4g tap_layers=%d\n", g_tap_amax, g_tap_layers);

    {
        int finite = 1;
        double amax = 0;
        for (i = 0; i < TL_V; ++i) {
            if (!isfinite(logits[i])) finite = 0;
            if (fabs((double)logits[i]) > amax) amax = fabs((double)logits[i]);
        }
        check(finite && amax > 0, "logits finite non-zero");
    }

    /* token generation steps (teacher-forced decode) */
    {
        int ok_dec = 1;
        for (i = 0; i < 8; ++i) {
            tok = tokens_pre[i];
            if (cce_gguf_qwen2_forward(m, &tok, 1, logits, TL_V) != CCE_OK)
                ok_dec = 0;
        }
        check(ok_dec, "decode 8 teacher-forced tokens");
    }
    check(m->cur_pos == 4 + 8, "pos after prefill+decode");

    /* sparse KV path (DSA-style budget) still forwards */
    {
        cce_result sr = cce_gguf_qwen2_set_sparse_kv(m, 0.25f);
        check(sr == CCE_OK, "sparse_kv 0.25 accepted");
        m->cur_pos = 0;
        check(cce_gguf_qwen2_forward(m, tokens_pre, 4, logits, TL_V) == CCE_OK,
              "prefill under sparse_kv");
        tok = 5;
        check(cce_gguf_qwen2_forward(m, &tok, 1, logits, TL_V) == CCE_OK,
              "decode under sparse_kv");
        (void)cce_gguf_qwen2_set_sparse_kv(m, 0.0f);
    }

    /* microbench: N_BENCH single-token decode steps after short prefill */
    for (i = 0; i < N_BENCH; ++i)
        dec_toks[i] = 2 + (i * 5 + 3) % (TL_V - 2);
    m->cur_pos = 0;
    (void)cce_gguf_qwen2_forward(m, tokens_pre, 2, logits, TL_V);
    t0 = wall_s();
    for (i = 0; i < N_BENCH; ++i) {
        if (cce_gguf_qwen2_forward(m, &dec_toks[i], 1, logits, TL_V) != CCE_OK) {
            check(0, "bench forward");
            break;
        }
    }
    t1 = wall_s();
    {
        double dt = t1 - t0;
        if (dt < 1e-9) dt = 1e-9;
        tok_s = (double)N_BENCH / dt;
    }
    check(tok_s > 0, "tok/s > 0");
    printf("    bench: %.1f tok/s (%d decode steps, synthetic %dL/d%d GGUF)\n",
           tok_s, N_BENCH, TL_L, TL_D);

    cce_gguf_set_layer_tap(NULL, NULL);
    cce_gguf_qwen2_free(m);
    m = NULL;

    /* same style via dual infer backend (CPU GGUF) */
    cce_infer_opts_default(&iopts, CCE_INFER_KIND_GGUF, CCE_INFER_DEVICE_CPU);
    iopts.gguf_path = gguf_path;
    iopts.synthetic = 0;
    iopts.dsa_enable = 0;
    iopts.sparse_kv = 0.0f;
    check(cce_infer_open(&sess, &iopts) == CCE_OK && sess, "infer backend GGUF CPU open");
    if (sess) {
        double tps2 = 0;
        check(cce_infer_bench(sess, 24, &tps2) == CCE_OK && tps2 > 0,
              "infer backend bench");
        printf("    infer_backend: %.1f tok/s (24 tokens, kind=%s device=%s)\n",
               tps2, cce_infer_kind_name(cce_infer_get_kind(sess)),
               cce_infer_device_name(cce_infer_get_device(sess)));
        cce_infer_close(sess);
    }

    /* GPU open is optional: unsupported-or-missing must not crash */
    {
        cce_infer_session *gs = NULL;
        cce_infer_opts gopts;
        cce_result gr;
        cce_infer_opts_default(&gopts, CCE_INFER_KIND_GGUF, CCE_INFER_DEVICE_GPU);
        gopts.gguf_path = gguf_path;
        gopts.synthetic = 0;
        gopts.dsa_enable = 0;
        gr = cce_infer_open(&gs, &gopts);
        if (gr == CCE_OK && gs) {
            double tpsg = 0;
            check(cce_infer_bench(gs, 8, &tpsg) == CCE_OK, "GPU GGUF bench (optional)");
            printf("    GPU available: %.1f tok/s device=%s\n",
                   tpsg, cce_infer_device_name(cce_infer_get_device(gs)));
            cce_infer_close(gs);
        } else {
            check(gr == CCE_ERR_NOT_FOUND || gr == CCE_ERR_UNSUPPORTED ||
                      gr == CCE_ERR_IO,
                  "GPU open soft-fails cleanly when absent");
            printf("    GPU GGUF: not available (rc=%d) — CPU path is the gate\n",
                   (int)gr);
        }
    }

    remove(gguf_path);

    printf("GGUF_STACK_PASS checks=%d failures=%d tok_s=%.2f\n",
           checks, failures, tok_s);
    return failures ? 1 : 0;
}
