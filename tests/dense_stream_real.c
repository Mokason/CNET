/* dense_stream_real: the dense expert-streaming pipeline on a REAL dense model
 * (not the synthetic fixture) — the proper end-to-end test.
 *
 * Loads a real HF checkpoint (default Qwen2.5-0.5B: 24 layers, hidden 896,
 * GQA 14:2, tied head, vocab 151936) via cce_anymodel_open, int8-quantizes every
 * linear specialist, ingests into the content-addressed weight store (A1.5
 * quantized-payload path), restores, attaches the tier runtime under a bounded
 * hot_cap << specialist count, and streams a forward. Verifies on REAL weights:
 *   - the model loads + forwards (finite, non-degenerate logits — a real argmax);
 *   - int8 PTQ is near-lossless (small logit dmax vs FP on real weights);
 *   - QUANTIZED streaming is BIT-IDENTICAL to the int8 all-resident model;
 *   - bounded RAM: resident specialists <= cap, one fetch each per pass;
 *   - real on-disk compression: int8 store bytes << FP store bytes.
 *
 * Build: make dense_stream_real  (needs a real checkpoint; path via argv[1])
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "../include/cce/cce_weight_store.h"
#include "../include/cce/cce_tier_runtime.h"
#include "../include/cce/cce_forest.h"
#include "../include/cce/cce_gguf.h"
#include "../include/cce/cce_detect.h"
#ifdef _WIN32
#include <io.h>
#else
#include <dirent.h>
#endif

static int checks = 0, fails = 0;
static FILE* LOGF = NULL;
#define LOG(...) do { printf(__VA_ARGS__); if (LOGF) fprintf(LOGF, __VA_ARGS__); } while (0)
#define CHECK(cond, msg) do { checks++; if (!(cond)) { fails++; LOG("  FAIL: %s\n", msg); } \
                              else { LOG("  ok:   %s\n", msg); } } while (0)

static void wipe_store_dir(const char* dir) {
#ifndef _WIN32
    DIR* d = opendir(dir);
    if (d) { struct dirent* e; char p[700];
        while ((e = readdir(d))) { size_t n = strlen(e->d_name);
            if (n > 5 && strcmp(e->d_name + n - 5, ".spec") == 0) { snprintf(p, sizeof p, "%s/%s", dir, e->d_name); remove(p); } }
        closedir(d); }
#endif
    remove(dir);
}

/* store weight-payload bytes: FP for every block, plus int8 subtotal for the quantized ones */
static void measure(const cce_gguf_qwen2* t, size_t* fp_all, size_t* int8_all,
                    size_t* fp_q, size_t* int8_q, int* nblk, int* nq) {
    size_t a = 0, iq = 0, fq = 0, iqq = 0; int nb = 0, q = 0;
    for (int b = 0; b < t->forest->num_branches; b++) {
        cce_cascade* cas = t->forest->branches[b].cascade; if (!cas) continue;
        for (int j = 0; j < cas->num_blocks; j++) {
            const cce_block* blk = &cas->blocks[j];
            size_t numel = blk->weights.numel; int out = (blk->weights.ndim >= 2) ? blk->weights.shape[1] : 0;
            a += numel * sizeof(float); nb++;
            if (blk->w_q) { size_t ib = numel + (size_t)out * sizeof(float);   /* int8 codes + per-out scale */
                iq += ib; fq += numel * sizeof(float); iqq += ib; q++; }
            else          { iq += numel * sizeof(float); }   /* non-quantized (e.g. the tied FP lm_head) stays FP */
        }
    }
    *fp_all = a; *int8_all = iq; *fp_q = fq; *int8_q = iqq; *nblk = nb; *nq = q;
}
static double dmax(const float* a, const float* b, int n) { double m = 0; for (int i = 0; i < n; i++) { double d = fabs((double)a[i] - b[i]); if (d > m) m = d; } return m; }
static int argmaxf(const float* v, int n) { int a = 0; for (int i = 1; i < n; i++) if (v[i] > v[a]) a = i; return a; }

int main(int argc, char** argv) {
    LOGF = fopen("logs/dense_stream_real.log", "wb");
    const char* path = (argc > 1) ? argv[1] : "/home/marble/AI/Models/Qwen2.5-0.5B/model.safetensors";
    const int HOT_CAP = (argc > 2) ? atoi(argv[2]) : 8;   /* << specialist count */
    static const int tokens[6] = { 785, 9639, 264, 3405, 11, 12095 };
    const int NT = 6;
    LOG("=== dense_stream_real: real dense model streamed under a bounded cap ===\n  model: %s\n", path);

    /* ---- FP reference: load, forward, ingest (for the size baseline) ---- */
    cce_anymodel* mfp = NULL;
    if (cce_anymodel_open(&mfp, path) != CCE_OK || !mfp || !mfp->transformer) {
        LOG("  FAIL: cce_anymodel_open could not build a transformer from %s\n", path);
        LOG("        (needs a standard HF dense qwen2/llama checkpoint under model.layers.N)\n");
        if (LOGF) fclose(LOGF); return 1;
    }
    cce_gguf_qwen2* tfp = mfp->transformer;
    int V = tfp->vocab_size, L = tfp->n_layer, D = tfp->n_embd;
    LOG("  loaded: n_layer=%d n_embd=%d n_head=%d n_kv_head=%d vocab=%d ffn=%d\n",
        L, tfp->n_embd, tfp->n_head, tfp->n_kv_head, V, tfp->feed_forward_length);
    CHECK(V > 1000 && L >= 2, "real transformer loaded with sane dims");

    float* refFP = (float*)malloc((size_t)V * sizeof(float));
    CHECK(cce_gguf_qwen2_forward(tfp, tokens, NT, refFP, V) == CCE_OK, "FP forward on real weights");
    int fp_arg = argmaxf(refFP, V);
    double mx = refFP[fp_arg], mean = 0; for (int i = 0; i < V; i++) mean += refFP[i]; mean /= V;
    LOG("  FP logits: argmax token=%d  max=%.3f  mean=%.3f  (non-degenerate => real forward works)\n", fp_arg, mx, mean);
    CHECK(mx > mean + 1.0 && fp_arg >= 0 && fp_arg < V, "FP logits are non-degenerate (real next-token distribution)");

    wipe_store_dir("dsr_store_fp");
    cce_weight_store* sfp = NULL; size_t bytes_fp = 0;
    if (cce_weight_store_open(&sfp, "dsr_store_fp") == CCE_OK) {
        int nt, nn, nr; cce_weight_store_ingest_model(sfp, mfp, "modelFP", "dsr_fp.manifest", &nt, &nn, &nr);
        bytes_fp = cce_weight_store_bytes(sfp); cce_weight_store_close(sfp);
    }
    cce_anymodel_free(mfp);

    /* ---- int8: load, quantize, forward (refQ), ingest ---- */
    cce_anymodel* ma = NULL;
    CHECK(cce_anymodel_open(&ma, path) == CCE_OK && ma && ma->transformer, "reopen real model for int8");
    cce_gguf_qwen2* t = ma->transformer;
    int nq = cce_gguf_qwen2_quantize_int8(t);
    LOG("  int8 PTQ: quantized %d specialists\n", nq);
    CHECK(nq > 0, "int8 PTQ quantized the linear specialists");
    float* refQ = (float*)malloc((size_t)V * sizeof(float));
    CHECK(cce_gguf_qwen2_forward(t, tokens, NT, refQ, V) == CCE_OK, "int8 all-resident forward");
    double dq_fp = dmax(refQ, refFP, V);
    LOG("  int8-vs-FP all-resident logit dmax = %.5g  (PTQ error on REAL weights)\n", dq_fp);
    CHECK(dq_fp > 0.0 && dq_fp < 5.0, "int8 PTQ is near-lossless on real weights (small, nonzero dmax)");

    size_t fp_all, int8_all, fp_q, int8_q; int nblk, nqb; measure(t, &fp_all, &int8_all, &fp_q, &int8_q, &nblk, &nqb);
    wipe_store_dir("dsr_store_q");
    cce_weight_store* sq = NULL; size_t bytes_q = 0;
    CHECK(cce_weight_store_open(&sq, "dsr_store_q") == CCE_OK && sq, "int8 store opens");
    int nt, nn, nr;
    CHECK(cce_weight_store_ingest_model(sq, ma, "modelQ", "dsr_q.manifest", &nt, &nn, &nr) == CCE_OK, "int8 model ingests");
    bytes_q = cce_weight_store_bytes(sq);
    cce_anymodel_free(ma);

    /* ---- restore + stream under a bounded cap ---- */
    cce_gguf_qwen2* m = NULL;
    CHECK(cce_weight_store_restore_transformer(sq, "dsr_q.manifest", "dsr_restore.cce", &m) == CCE_OK && m, "restore int8 model from store");
    cce_tier_runtime* rt = NULL;
    CHECK(cce_tier_attach(&rt, m, sq, "dsr_q.manifest", HOT_CAP) == CCE_OK && rt, "tier runtime attaches");
    CHECK(cce_tier_evict_all(rt) == CCE_OK && cce_tier_resident(rt) == 0, "evict all (start cold)");

    float* lg = (float*)malloc((size_t)V * sizeof(float));
    CHECK(cce_gguf_qwen2_forward(m, tokens, NT, lg, V) == CCE_OK, "streaming forward runs on real model");
    double d_q = dmax(lg, refQ, V), d_fp = dmax(lg, refFP, V);
    int hw = cce_tier_high_water(rt), rh = cce_tier_rehydrations(rt), res = cce_tier_resident(rt);

    LOG("\n  === REAL-MODEL streaming gate ===\n");
    LOG("  specialists=%d  hot_cap=%d  high_water=%d  rehydrations=%d  resident=%d\n", nblk, HOT_CAP, hw, rh, res);
    LOG("  dmax(stream, int8 all-resident) = %.5g   dmax(stream, FP) = %.5g\n", d_q, d_fp);
    LOG("  store bytes: int8=%zu  FP=%zu  -> %.2fx smaller (real model, on disk)\n",
        bytes_q, bytes_fp, bytes_q ? (double)bytes_fp / bytes_q : 0.0);
    LOG("  resident cap fraction = %d/%d = %.1f%% of the forest\n", HOT_CAP, nblk, nblk ? 100.0 * HOT_CAP / nblk : 0.0);

    CHECK(d_q == 0.0, "QUANTIZED streaming BIT-IDENTICAL to int8 all-resident (streaming = residency, not math)");
    CHECK(hw <= HOT_CAP, "resident specialists never exceeded the cap");
    CHECK(rh == nblk, "one streaming pass: each specialist fetched exactly once");
    CHECK(res <= HOT_CAP, "post-forward residency within cap");
    CHECK(bytes_q < bytes_fp, "real on-disk compression: int8 store smaller than FP store");
    double spec_ratio = int8_q ? (double)fp_q / int8_q : 0.0;
    LOG("  %d QUANTIZED specialists: FP=%zu int8=%zu -> %.2fx (int8 weight-only)\n", nqb, fp_q, int8_q, spec_ratio);
    LOG("  whole-forest %.2fx / whole-store %.2fx are diluted by the UNQUANTIZED FP tied lm_head + embedding\n",
        int8_all ? (double)fp_all / int8_all : 0.0, bytes_q ? (double)bytes_fp / bytes_q : 0.0);
    LOG("  (~%.0f MB, %ld params — 27%% of a 0.5B model; kept FP by design). Larger models -> whole approaches 4x.\n",
        (double)((long)V * D) * 4 / 1e6, (long)V * D);
    CHECK(spec_ratio > 3.5, "the QUANTIZED specialists compress ~4x on real weights (int8 weight-only, the streamable win)");

    free(refFP); free(refQ); free(lg);
    cce_tier_detach(rt); cce_weight_store_close(sq);
    wipe_store_dir("dsr_store_fp"); wipe_store_dir("dsr_store_q");
    remove("dsr_fp.manifest"); remove("dsr_q.manifest"); remove("dsr_restore.cce");
    LOG("\ndense_stream_real: %d passed, %d failed\n", checks - fails, fails);
    if (LOGF) fclose(LOGF);
    return fails ? 1 : 0;
}
