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

#include <time.h>
static double now_sec(void) {
#ifdef _WIN32
    return (double)clock() / (double)CLOCKS_PER_SEC;
#else
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
#endif
}

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

    /* ---- A3: async readahead + learned pinning — THROUGHPUT on real payloads ----
     * The pass above was the learning pass (fetch order recorded). Each timed
     * pass below resets cur_pos so it replays the IDENTICAL prefill: logits
     * must stay bit-identical to refQ, and wall/stall time becomes comparable
     * across passes. Stall = forward-thread time blocked on store fetches;
     * readahead overlaps those fetches with compute, so stall must DROP. */
    {
        const int K = 3;
        CHECK(cce_tier_seq_learned(rt) == 1, "A3: fetch sequence learned by the first streaming pass");

        double stall0 = cce_tier_stall_seconds(rt);
        double t0 = now_sec();
        int ok_sync = 1, id_sync = 1;
        for (int k = 0; k < K; k++) {
            CHECK(cce_tier_evict_all(rt) == CCE_OK, "A3: cold start (sync pass)");
            m->cur_pos = 0;
            if (cce_gguf_qwen2_forward(m, tokens, NT, lg, V) != CCE_OK) ok_sync = 0;
            if (dmax(lg, refQ, V) != 0.0) id_sync = 0;
        }
        double wall_sync = now_sec() - t0, stall_sync = cce_tier_stall_seconds(rt) - stall0;
        CHECK(ok_sync && id_sync, "A3: sync passes run, logits stay bit-identical to int8 all-resident");

        cce_result rarc = cce_tier_set_readahead(rt, 6);
        if (rarc == CCE_ERR_UNSUPPORTED) {
            LOG("  A3: readahead unsupported on this platform — skipping throughput gate\n");
        } else {
            CHECK(rarc == CCE_OK, "A3: readahead enables (depth 6)");
            int sync0 = cce_tier_sync_fetches(rt);
            stall0 = cce_tier_stall_seconds(rt);
            t0 = now_sec();
            int ok_ra = 1, id_ra = 1;
            for (int k = 0; k < K; k++) {
                CHECK(cce_tier_evict_all(rt) == CCE_OK, "A3: cold start (readahead pass)");
                m->cur_pos = 0;
                if (cce_gguf_qwen2_forward(m, tokens, NT, lg, V) != CCE_OK) ok_ra = 0;
                if (dmax(lg, refQ, V) != 0.0) id_ra = 0;
            }
            double wall_ra = now_sec() - t0, stall_ra = cce_tier_stall_seconds(rt) - stall0;
            int sync_d = cce_tier_sync_fetches(rt) - sync0;
            CHECK(ok_ra && id_ra, "A3: READAHEAD passes bit-identical to int8 all-resident (real model)");
            CHECK(sync_d <= K, "A3: at most one sync fetch per cold pass (the pass head); rest prefetched");
            CHECK(cce_tier_staged_high_water(rt) <= 6, "A3: staged payloads bounded by depth (RAM honesty)");

            LOG("\n  === A3 throughput (real payloads, %d specialists, %d passes each) ===\n", nblk, K);
            LOG("  sync:      wall %.3fs  stall %.3fs  (%.1f ms/pass stalled on fetches)\n",
                wall_sync, stall_sync, 1e3 * stall_sync / K);
            LOG("  readahead: wall %.3fs  stall %.3fs  (%.1f ms/pass; hits=%d waits=%d sync=%d)\n",
                wall_ra, stall_ra, 1e3 * stall_ra / K,
                cce_tier_prefetch_hits(rt), cce_tier_prefetch_waits(rt), sync_d);
            LOG("  stall cut %.1f%%  wall cut %.1f%%\n",
                stall_sync > 0 ? 100.0 * (1.0 - stall_ra / stall_sync) : 0.0,
                wall_sync > 0 ? 100.0 * (1.0 - wall_ra / wall_sync) : 0.0);
            CHECK(stall_ra < stall_sync,
                  "A3 THROUGHPUT: readahead cuts forward-thread fetch stall on real payloads");

            /* learned pinning on top: score = fetches x payload bytes, so the
               huge FP tied lm_head payload (the single most expensive fetch
               of every pass) gets pinned first, then the largest FFN blocks */
            const int N_PIN = 32;
            CHECK(cce_tier_pin_hot(rt, N_PIN) == CCE_OK && cce_tier_pinned(rt) == N_PIN,
                  "A3: pin_hot pins the 32 costliest specialists (fetches x bytes)");
            int rh0 = cce_tier_rehydrations(rt);
            stall0 = cce_tier_stall_seconds(rt);
            t0 = now_sec();
            int ok_pin = 1, id_pin = 1;
            for (int k = 0; k < K; k++) {
                CHECK(cce_tier_evict_all(rt) == CCE_OK, "A3: cold start (pinned pass; pinned stay)");
                m->cur_pos = 0;
                if (cce_gguf_qwen2_forward(m, tokens, NT, lg, V) != CCE_OK) ok_pin = 0;
                if (dmax(lg, refQ, V) != 0.0) id_pin = 0;
            }
            double wall_pin = now_sec() - t0, stall_pin = cce_tier_stall_seconds(rt) - stall0;
            int rh_pass = (cce_tier_rehydrations(rt) - rh0) / K;
            LOG("  +pinned%d:  wall %.3fs  stall %.3fs  rehydrations/pass %d (was %d)\n",
                N_PIN, wall_pin, stall_pin, rh_pass, nblk);
            CHECK(ok_pin && id_pin, "A3: PINNED passes bit-identical (pinning = residency, not math)");
            CHECK(rh_pass == nblk - N_PIN, "A3: pinned specialists never refetched (rehydrations/pass drop)");
            CHECK(stall_pin < stall_ra,
                  "A3: cost-weighted pinning cuts stall further (the big FP head stops re-streaming)");
            LOG("  effective RAM = cap %d + pinned %d + staged<=%d of %d specialists\n",
                HOT_CAP, N_PIN, 6, nblk);
        }
    }

    free(refFP); free(refQ); free(lg);
    cce_tier_detach(rt); cce_weight_store_close(sq);
    wipe_store_dir("dsr_store_fp"); wipe_store_dir("dsr_store_q");
    remove("dsr_fp.manifest"); remove("dsr_q.manifest"); remove("dsr_restore.cce");
    LOG("\ndense_stream_real: %d passed, %d failed\n", checks - fails, fails);
    if (LOGF) fclose(LOGF);
    return fails ? 1 : 0;
}
