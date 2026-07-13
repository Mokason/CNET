/* dense_stream_q: Arc A1, first slice of the Dense expert-streaming arc.
 *
 * Question: can a QUANTIZED (int8 weight-only PTQ) dense model stream from the
 * content-addressed weight store under a bounded resident cap, bit-identically,
 * with a real RAM/disk reduction?
 *
 * Flow (mirrors cce_tiers_test, but the specialists are int8-quantized first):
 *   1. build + load the tiny transformer fixture.
 *   2. int8-quantize every linear specialist (cce_gguf_qwen2_quantize_int8) and
 *      capture the QUANTIZED all-resident logits as the reference (refQ). Also
 *      capture the plain FP all-resident logits (refFP) from a second load.
 *   3. ingest the quantized model into the store; restore; attach the tier
 *      runtime with a small hot_cap; evict all (cold); stream a forward.
 *   4. GATE the quantized streaming and REPORT the size/RAM numbers.
 *
 * KEY UNKNOWN this slice resolves: does the store/restore/tier path PRESERVE
 * int8 end-to-end? The gate below measures it directly:
 *   - dmax(stream, refQ)  == 0  => quantized math survives streaming (ideal)
 *   - dmax(stream, refFP) == 0  => stream re-materialized FP (store dropped it)
 *   - bytes(int8 ingest) vs bytes(FP ingest): the compression the store keeps.
 * The specialist-level int8-vs-FP byte measurement quantifies the win the store
 * SHOULD capture even if it currently doesn't.
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
#include "tiny_model_fixture.h"

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
#ifdef _WIN32
    struct _finddata_t fd;
    char pat[600], path[700];
    snprintf(pat, sizeof(pat), "%s/*.spec", dir);
    intptr_t h = _findfirst(pat, &fd);
    if (h != -1) {
        do { snprintf(path, sizeof(path), "%s/%s", dir, fd.name); remove(path); }
        while (_findnext(h, &fd) == 0);
        _findclose(h);
    }
#else
    DIR* d = opendir(dir);
    if (d) {
        struct dirent* ent;
        char path[700];
        while ((ent = readdir(d)) != NULL) {
            size_t n = strlen(ent->d_name);
            if (n > 5 && strcmp(ent->d_name + n - 5, ".spec") == 0) {
                snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
                remove(path);
            }
        }
        closedir(d);
    }
#endif
    remove(dir);
}

/* Sum specialist weight-payload bytes across the forest: FP for every block,
 * plus the FP vs int8 subtotals for the blocks actually quantized (w_q set). */
static void measure_specialists(const cce_gguf_qwen2* t,
                                size_t* fp_all, size_t* fp_q, size_t* int8_q,
                                int* n_blocks, int* n_q_blocks) {
    size_t a = 0, fq = 0, iq = 0;
    int nb = 0, nq = 0;
    for (int b = 0; b < t->forest->num_branches; b++) {
        cce_cascade* cas = t->forest->branches[b].cascade;
        if (!cas) continue;
        for (int j = 0; j < cas->num_blocks; j++) {
            const cce_block* blk = &cas->blocks[j];
            size_t numel = blk->weights.numel;           /* in*out */
            int out_dim = (blk->weights.ndim >= 2) ? blk->weights.shape[1] : 0;
            a += numel * sizeof(float);
            nb++;
            if (blk->w_q) {                                /* int8-quantized */
                fq += numel * sizeof(float);
                iq += numel * sizeof(int8_t) + (size_t)out_dim * sizeof(float);
                nq++;
            }
        }
    }
    *fp_all = a; *fp_q = fq; *int8_q = iq; *n_blocks = nb; *n_q_blocks = nq;
}

int main(void) {
    LOGF = fopen("logs/dense_stream_q.log", "wb");

    LOG("=== dense_stream_q: quantized dense streaming under a bounded cap ===\n");

    const int N_SPECS = 7 * TL_L + 1;       /* 15: q,k,v,o,gate,up,down per layer + lm_head */
    const int HOT_CAP = 8;                   /* one layer's working set, << N_SPECS */
    static const int tokens[4] = { 3, 17, 9, 22 };

    tl_weights* w = (tl_weights*)malloc(sizeof(tl_weights));
    tl_gen(w, 0);
    tl_write_st_dir("dsq_a", w);

    /* ---------- reference FP all-resident logits (fresh KV) ---------- */
    float refFP[TL_V];
    wipe_store_dir("dsq_store_fp");
    cce_weight_store* sfp = NULL;
    CHECK(cce_weight_store_open(&sfp, "dsq_store_fp") == CCE_OK && sfp, "FP store opens");
    size_t bytes_fp = 0;
    {
        cce_anymodel* mb = NULL;
        CHECK(cce_anymodel_open(&mb, "dsq_a/model.safetensors") == CCE_OK && mb, "FP model opens");
        if (!mb) return 1;
        int nt = 0, nn = 0, nr = 0;
        CHECK(cce_weight_store_ingest_model(sfp, mb, "modelFP", "dsq_fp.manifest", &nt, &nn, &nr) == CCE_OK,
              "FP model ingests");
        CHECK(cce_gguf_qwen2_forward(mb->transformer, tokens, 4, refFP, TL_V) == CCE_OK,
              "FP all-resident forward");
        bytes_fp = cce_weight_store_bytes(sfp);
        cce_anymodel_free(mb);
    }

    /* ---------- quantize + reference INT8 all-resident logits (fresh KV) ---------- */
    float refQ[TL_V];
    size_t sp_fp_all = 0, sp_fp_q = 0, sp_int8_q = 0;
    int sp_nblocks = 0, sp_nq = 0, n_quantized = 0;
    wipe_store_dir("dsq_store_q");
    cce_weight_store* sq = NULL;
    CHECK(cce_weight_store_open(&sq, "dsq_store_q") == CCE_OK && sq, "int8 store opens");
    size_t bytes_q = 0;
    {
        cce_anymodel* ma = NULL;
        CHECK(cce_anymodel_open(&ma, "dsq_a/model.safetensors") == CCE_OK && ma, "int8 model opens");
        if (!ma) return 1;

        n_quantized = cce_gguf_qwen2_quantize_int8(ma->transformer);
        CHECK(n_quantized > 0, "int8 PTQ quantized the linear specialists");

        measure_specialists(ma->transformer, &sp_fp_all, &sp_fp_q, &sp_int8_q,
                            &sp_nblocks, &sp_nq);

        CHECK(cce_gguf_qwen2_forward(ma->transformer, tokens, 4, refQ, TL_V) == CCE_OK,
              "int8 all-resident forward (weight-only PTQ path)");

        int nt = 0, nn = 0, nr = 0;
        CHECK(cce_weight_store_ingest_model(sq, ma, "modelQ", "dsq_q.manifest", &nt, &nn, &nr) == CCE_OK,
              "int8 model ingests");
        bytes_q = cce_weight_store_bytes(sq);
        cce_anymodel_free(ma);
    }

    /* sanity: quantization actually perturbed the math vs FP */
    float dq_fp = 0;
    for (int v = 0; v < TL_V; v++) { float d = fabsf(refQ[v] - refFP[v]); if (d > dq_fp) dq_fp = d; }
    LOG("  int8-vs-FP all-resident logit dmax = %.6g  (quantization error)\n", (double)dq_fp);
    CHECK(dq_fp > 0.0f, "int8 PTQ changes the all-resident math (fixture non-degenerate)");

    /* ---------- restore + stream the QUANTIZED-ingested model, cold ---------- */
    cce_gguf_qwen2* m = NULL;
    CHECK(cce_weight_store_restore_transformer(sq, "dsq_q.manifest", "dsq_restore.cce", &m) == CCE_OK && m,
          "restores from int8 store");
    if (!m) return 1;

    cce_tier_runtime* rt = NULL;
    CHECK(cce_tier_attach(&rt, m, sq, "dsq_q.manifest", HOT_CAP) == CCE_OK && rt, "tier runtime attaches");
    if (!rt) return 1;
    CHECK(cce_tier_evict_all(rt) == CCE_OK, "evict all (start cold)");
    CHECK(cce_tier_resident(rt) == 0, "starts fully cold");

    float lg[TL_V];
    CHECK(cce_gguf_qwen2_forward(m, tokens, 4, lg, TL_V) == CCE_OK, "streaming forward runs");

    float dmax_q = 0, dmax_fp = 0;
    for (int v = 0; v < TL_V; v++) {
        float a = fabsf(lg[v] - refQ[v]);  if (a > dmax_q)  dmax_q  = a;
        float b = fabsf(lg[v] - refFP[v]); if (b > dmax_fp) dmax_fp = b;
    }

    LOG("\n--- residency mechanics (the streaming machinery) ---\n");
    LOG("  hot_cap=%d  N_SPECS=%d  high_water=%d  rehydrations=%d  resident=%d\n",
        HOT_CAP, N_SPECS, cce_tier_high_water(rt), cce_tier_rehydrations(rt), cce_tier_resident(rt));
    CHECK(cce_tier_high_water(rt) <= HOT_CAP, "resident specialists never exceeded the cap");
    CHECK(cce_tier_rehydrations(rt) == N_SPECS, "one streaming pass: each specialist fetched once");
    CHECK(cce_tier_resident(rt) <= HOT_CAP, "post-forward residency within cap");

    LOG("\n--- the KEY UNKNOWN: does the store preserve int8 end-to-end? ---\n");
    LOG("  dmax(stream, int8 all-resident refQ)  = %.6g\n", (double)dmax_q);
    LOG("  dmax(stream, FP   all-resident refFP) = %.6g\n", (double)dmax_fp);
    int preserved = (dmax_q == 0.0f) && (bytes_q < bytes_fp);
    if (preserved) {
        LOG("  VERDICT: store PRESERVES int8 -> quantized streaming is bit-identical.\n");
    } else {
        LOG("  VERDICT: store DOES NOT preserve int8 -> streaming re-materialized FP.\n");
        LOG("           drop site: serialize_cascade() in src/cce/cce_weight_store.c\n");
        LOG("           (serializes blk->weights.data FP32 only; blk->w_q / blk->w_scale\n");
        LOG("            are never written, and cce_weight_store_get restores FP only.)\n");
    }

    LOG("\n--- gate: QUANTIZED streaming is bit-identical to the int8 all-resident model ---\n");
    CHECK(preserved, "store PRESERVES int8 end-to-end (quantized-payload path in cce_weight_store)");
    CHECK(dmax_q == 0.0f, "capped QUANTIZED streaming logits BIT-IDENTICAL to int8 all-resident");

    LOG("\n--- on-disk store bytes: int8 ingest vs FP ingest of the same model ---\n");
    LOG("  store bytes (int8 ingest) = %zu\n", bytes_q);
    LOG("  store bytes (FP   ingest) = %zu\n", bytes_fp);
    LOG("  compression captured by store = %.3fx\n",
        bytes_q ? (double)bytes_fp / (double)bytes_q : 0.0);
    CHECK(bytes_q < bytes_fp,
          "store captures int8 compression (quantized payload smaller than FP ingest)");

    LOG("\n--- specialist working set: FP vs int8 (the win the store SHOULD capture) ---\n");
    LOG("  quantized blocks = %d of %d forest blocks (%d specialists; lm_head kept FP)\n",
        sp_nq, sp_nblocks, n_quantized);
    LOG("  quantized specialists  FP weight bytes = %zu\n", sp_fp_q);
    LOG("  quantized specialists int8 weight bytes = %zu (codes + per-col scale)\n", sp_int8_q);
    LOG("  per-specialist int8 compression = %.3fx\n",
        sp_int8_q ? (double)sp_fp_q / (double)sp_int8_q : 0.0);
    {
        /* projected whole-store size if the store kept int8 for quantized specs */
        size_t projected = bytes_fp - (sp_fp_q - sp_int8_q);
        LOG("  ALL-specialist FP weight bytes (forest) = %zu\n", sp_fp_all);
        LOG("  projected store bytes IF int8 preserved = %zu (%.3fx vs FP ingest)\n",
            projected, projected ? (double)bytes_fp / (double)projected : 0.0);
    }

    LOG("\n--- resident RAM cap vs full model ---\n");
    LOG("  full model = all %d specialists resident; capped stream <= %d specialists resident.\n",
        N_SPECS, HOT_CAP);
    LOG("  resident specialist cap fraction = %d/%d = %.2f of the forest.\n",
        HOT_CAP, N_SPECS, (double)HOT_CAP / (double)N_SPECS);

    /* teardown */
    CHECK(cce_tier_detach(rt) == CCE_OK, "detach rehydrates + releases");
    rt = NULL;
    cce_gguf_qwen2_free(m);
    m = NULL;
    remove("dsq_restore.cce");

    cce_weight_store_close(sq);
    cce_weight_store_close(sfp);
    wipe_store_dir("dsq_store_q");
    wipe_store_dir("dsq_store_fp");
    tl_cleanup_st_dir("dsq_a");
    remove("dsq_q.manifest");
    remove("dsq_fp.manifest");
    free(w);

    LOG("\n%d checks, %d failed -> %s\n", checks, fails, fails ? "FAIL" : "OK");
    if (LOGF) fclose(LOGF);
    return fails ? 1 : 0;
}
