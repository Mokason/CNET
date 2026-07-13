/* dense_stream_trit: packed 4-bit/ternary STORAGE for the dense streaming arc.
 *
 * A1.5 taught the weight store to keep int8 payloads (4x). This slice extends
 * the quantized-payload path to the PACKED formats:
 *   - ternary: 5 trits/byte base-3 (w_trit) = 1.6 bit/weight  (~20x on codes)
 *   - int4:    2 codes/byte nibbles          = 4.0 bit/weight (~8x on codes)
 *
 * Two layers of gates:
 *
 * PART 1 (store unit, real-ish dims 512x100): one FP cascade quantized four
 * ways (fp / int8 / packed-trit / int4) lands in ONE store under four DISTINCT
 * digests (per-kind salts; cce_spec_digest hashes FP only, so the salts are
 * what keep the variants apart), each payload has the EXACT expected byte
 * size (trit codes at exactly 1.6 bit/weight, int4 at 4.0), and each restores
 * REPRESENTATION-bit-exact (memcmp of w_trit / codes / scales).
 *
 * PART 2 (model streaming, tiny fixture): a ternary-packed model and an int4
 * model each ingest, restore, and STREAM under a bounded resident cap with
 * logits BIT-IDENTICAL to their all-resident twins — the restored blocks run
 * the forward's trit / int8 paths (w_trit > w_q > FP precedence).
 *
 * NOTE: quantize+pack happens at the BLOCK level (cce_block_quantize_ternary +
 * cce_block_pack_trits), which leaves the FP tensor in place. The gguf-level
 * cce_gguf_qwen2_pack_trits helper frees FP weights to reclaim RAM, which
 * destroys the shape AND the FP-anchored digest — pack-for-RAM and
 * pack-for-ingest are different operations; ingest needs FP present.
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

/* naive per-out absmax int4 PTQ (grid [-7,7]); codes ride in the int8 w_q array
 * like dense_stream_a2 — the store detects the [-7,7] range and packs 2/byte. */
static int block_quantize_int4(cce_block* blk) {
    if (blk->type != CCE_BLOCK_LINEAR && blk->type != CCE_BLOCK_LINEAR_HEAD) return 0;
    if (!blk->weights.data || blk->weights.ndim != 2 || blk->w_q || blk->w_trit) return 0;
    int in_dim = blk->weights.shape[0], out_dim = blk->weights.shape[1];
    int8_t* q  = (int8_t*)malloc((size_t)in_dim * out_dim);
    float*  sc = (float*)calloc((size_t)out_dim, sizeof(float));
    if (!q || !sc) { free(q); free(sc); return 0; }
    for (int i = 0; i < in_dim; ++i) {
        const float* wr = &blk->weights.data[(size_t)i * out_dim];
        for (int o = 0; o < out_dim; ++o) { float a = fabsf(wr[o]); if (a > sc[o]) sc[o] = a; }
    }
    for (int o = 0; o < out_dim; ++o) sc[o] = (sc[o] > 0.0f) ? sc[o] / 7.0f : 1.0f;
    for (int i = 0; i < in_dim; ++i) {
        const float* wr = &blk->weights.data[(size_t)i * out_dim];
        int8_t* qr = &q[(size_t)i * out_dim];
        for (int o = 0; o < out_dim; ++o) {
            long c = lroundf(wr[o] / sc[o]);
            if (c >  7) c =  7;
            if (c < -7) c = -7;
            qr[o] = (int8_t)c;
        }
    }
    blk->w_q = q; blk->w_scale = sc;
    return 1;
}

/* forest-wide block-level quantizers (skip lm_head/mtp like the gguf helpers) */
static int forest_ternary_pack(cce_gguf_qwen2* m) {
    int n = 0;
    for (int b = 0; b < m->forest->num_branches; ++b) {
        const char* bn = m->forest->branches[b].name;
        if (strstr(bn, "lm_head") || strstr(bn, "mtp.")) continue;
        cce_cascade* cas = m->forest->branches[b].cascade;
        if (!cas) continue;
        for (int j = 0; j < cas->num_blocks; ++j) {
            cce_block* blk = &cas->blocks[j];
            if (cce_block_quantize_ternary(blk) != CCE_OK) continue;
            if (cce_block_pack_trits(blk) == CCE_OK) n++;
        }
    }
    return n;
}

static int forest_int4(cce_gguf_qwen2* m) {
    int n = 0;
    for (int b = 0; b < m->forest->num_branches; ++b) {
        const char* bn = m->forest->branches[b].name;
        if (strstr(bn, "lm_head") || strstr(bn, "mtp.")) continue;
        cce_cascade* cas = m->forest->branches[b].cascade;
        if (!cas) continue;
        for (int j = 0; j < cas->num_blocks; ++j)
            n += block_quantize_int4(&cas->blocks[j]);
    }
    return n;
}

/* deterministic weight fill */
static uint32_t lcg_state = 0x1234567u;
static float lcg_uniform(void) {
    lcg_state = lcg_state * 1664525u + 1013904223u;
    return ((float)(lcg_state >> 8) / (float)(1u << 24)) * 2.0f - 1.0f;
}

static cce_cascade* make_unit_cascade(int in_dim, int out_dim) {
    cce_cascade* cas = NULL;
    if (cce_cascade_create(&cas, 1) != CCE_OK) return NULL;
    if (cce_cascade_add_linear(cas, in_dim, out_dim, 0.0f) != CCE_OK) {
        cce_cascade_destroy(cas); return NULL;
    }
    cce_block* blk = &cas->blocks[0];
    lcg_state = 0x1234567u;   /* same fill for every variant */
    for (size_t k = 0; k < blk->weights.numel; ++k) blk->weights.data[k] = lcg_uniform();
    if (blk->bias.data && blk->bias.numel > 0)
        for (size_t k = 0; k < blk->bias.numel; ++k) blk->bias.data[k] = lcg_uniform();
    return cas;
}

int main(void) {
    LOGF = fopen("logs/dense_stream_trit.log", "wb");

    LOG("=== dense_stream_trit: packed ternary (1.6-bit) + int4 storage & streaming ===\n");

    /* =================== PART 1: store unit at 512x100 =================== */
    LOG("\n--- PART 1: one FP cascade, four precision variants, one store ---\n");
    const int U_IN = 512, U_OUT = 100, U_BPR = (U_OUT + 4) / 5;

    wipe_store_dir("dst_store_u");
    cce_weight_store* su = NULL;
    CHECK(cce_weight_store_open(&su, "dst_store_u") == CCE_OK && su, "unit store opens");

    cce_cascade* c_fp = make_unit_cascade(U_IN, U_OUT);
    cce_cascade* c_i8 = make_unit_cascade(U_IN, U_OUT);
    cce_cascade* c_tr = make_unit_cascade(U_IN, U_OUT);
    cce_cascade* c_i4 = make_unit_cascade(U_IN, U_OUT);
    CHECK(c_fp && c_i8 && c_tr && c_i4, "four identical FP cascades built");
    if (!c_fp || !c_i8 || !c_tr || !c_i4) return 1;

    CHECK(cce_block_quantize_int8(&c_i8->blocks[0]) == CCE_OK, "variant int8 quantizes");
    CHECK(cce_block_quantize_ternary(&c_tr->blocks[0]) == CCE_OK, "variant ternary quantizes");
    CHECK(cce_block_pack_trits(&c_tr->blocks[0]) == CCE_OK, "variant ternary packs to trits");
    CHECK(c_tr->blocks[0].w_trit && !c_tr->blocks[0].w_q, "packed variant holds w_trit (w_q freed)");
    CHECK(c_tr->blocks[0].w_trit_bpr == U_BPR, "bytes-per-row = ceil(out/5)");
    CHECK(block_quantize_int4(&c_i4->blocks[0]) == 1, "variant int4 quantizes (codes in [-7,7])");

    uint64_t d_fp = 0, d_i8 = 0, d_tr = 0, d_i4 = 0;
    size_t b0, p_fp, p_i8, p_tr, p_i4;
    b0 = cce_weight_store_bytes(su);
    CHECK(cce_weight_store_put(su, c_fp, &d_fp, NULL) == CCE_OK, "FP variant puts");
    p_fp = cce_weight_store_bytes(su) - b0; b0 += p_fp;
    CHECK(cce_weight_store_put(su, c_i8, &d_i8, NULL) == CCE_OK, "int8 variant puts");
    p_i8 = cce_weight_store_bytes(su) - b0; b0 += p_i8;
    CHECK(cce_weight_store_put(su, c_tr, &d_tr, NULL) == CCE_OK, "trit variant puts");
    p_tr = cce_weight_store_bytes(su) - b0; b0 += p_tr;
    CHECK(cce_weight_store_put(su, c_i4, &d_i4, NULL) == CCE_OK, "int4 variant puts");
    p_i4 = cce_weight_store_bytes(su) - b0;

    LOG("  digests: fp=%016llx i8=%016llx tr=%016llx i4=%016llx\n",
        (unsigned long long)d_fp, (unsigned long long)d_i8,
        (unsigned long long)d_tr, (unsigned long long)d_i4);
    CHECK(d_fp != d_i8 && d_fp != d_tr && d_fp != d_i4 &&
          d_i8 != d_tr && d_i8 != d_i4 && d_tr != d_i4,
          "four precision variants of one FP cascade -> four DISTINCT digests");
    CHECK(cce_weight_store_count(su) == 4, "one store holds all four payloads");

    /* exact expected payload bytes (format gate) */
    int has_bias = (c_fp->blocks[0].bias.data && c_fp->blocks[0].bias.numel > 0) ? 1 : 0;
    size_t bias_b = has_bias ? (size_t)U_OUT * 4 : 0;
    size_t e_fp = 16 + 16 + (size_t)U_IN * U_OUT * 4 + bias_b;
    size_t e_i8 = 16 + 20 + (size_t)U_IN * U_OUT + (size_t)U_OUT * 4 + bias_b;
    size_t e_tr = 16 + 20 + 4 + (size_t)U_IN * U_BPR + (size_t)U_OUT * 4 + bias_b;
    size_t e_i4 = 16 + 20 + ((size_t)U_IN * U_OUT + 1) / 2 + (size_t)U_OUT * 4 + bias_b;
    LOG("  payload bytes: fp=%zu (want %zu)  i8=%zu (want %zu)  tr=%zu (want %zu)  i4=%zu (want %zu)\n",
        p_fp, e_fp, p_i8, e_i8, p_tr, e_tr, p_i4, e_i4);
    CHECK(p_fp == e_fp, "FP payload has the exact v1 byte size (no regression)");
    CHECK(p_i8 == e_i8, "int8 payload has the exact v2 byte size (no regression)");
    CHECK(p_tr == e_tr, "trit payload has the exact v3 byte size");
    CHECK(p_i4 == e_i4, "int4 payload has the exact v3 byte size");

    double trit_bits = 8.0 * (double)((size_t)U_IN * U_BPR) / (double)((size_t)U_IN * U_OUT);
    double int4_bits = 8.0 * (double)(((size_t)U_IN * U_OUT + 1) / 2) / (double)((size_t)U_IN * U_OUT);
    LOG("  code bit-rates: trit %.3f bit/weight, int4 %.3f bit/weight\n", trit_bits, int4_bits);
    LOG("  whole-payload compression vs FP: trit %.2fx, int4 %.2fx, int8 %.2fx\n",
        (double)p_fp / (double)p_tr, (double)p_fp / (double)p_i4, (double)p_fp / (double)p_i8);
    CHECK(trit_bits == 1.6, "trit codes at EXACTLY 1.6 bit/weight (5 trits/byte, out%%5==0)");
    CHECK(int4_bits == 4.0, "int4 codes at EXACTLY 4.0 bit/weight (2 codes/byte)");
    CHECK((double)p_fp / (double)p_tr >= 18.0, "trit payload ~20x smaller than FP (>=18x incl. scale+bias)");
    CHECK((double)p_fp / (double)p_i4 >= 7.0, "int4 payload ~8x smaller than FP (>=7x incl. scale+bias)");

    /* restore each variant: representation must be bit-exact */
    {
        cce_cascade* r = NULL;
        CHECK(cce_weight_store_get(su, d_fp, &r) == CCE_OK && r, "FP variant restores");
        if (r) {
            cce_block* rb = &r->blocks[0];
            CHECK(!rb->w_q && !rb->w_trit, "restored FP variant carries no quant arrays");
            CHECK(memcmp(rb->weights.data, c_fp->blocks[0].weights.data,
                         (size_t)U_IN * U_OUT * 4) == 0, "restored FP weights bit-exact");
            cce_cascade_destroy(r);
        }
    }
    {
        cce_cascade* r = NULL;
        CHECK(cce_weight_store_get(su, d_i8, &r) == CCE_OK && r, "int8 variant restores");
        if (r) {
            cce_block* rb = &r->blocks[0];
            CHECK(rb->w_q && rb->w_scale && !rb->w_trit, "restored int8 variant carries w_q + scale");
            CHECK(rb->w_q && memcmp(rb->w_q, c_i8->blocks[0].w_q, (size_t)U_IN * U_OUT) == 0,
                  "restored int8 codes bit-exact");
            CHECK(rb->w_scale && memcmp(rb->w_scale, c_i8->blocks[0].w_scale, (size_t)U_OUT * 4) == 0,
                  "restored int8 scales bit-exact");
            cce_cascade_destroy(r);
        }
    }
    {
        cce_cascade* r = NULL;
        CHECK(cce_weight_store_get(su, d_tr, &r) == CCE_OK && r, "trit variant restores");
        if (r) {
            cce_block* rb = &r->blocks[0];
            CHECK(rb->w_trit && rb->w_scale && !rb->w_q,
                  "restored trit variant carries w_trit + scale (no w_q -> trit forward path)");
            CHECK(rb->w_trit_bpr == U_BPR, "restored bytes-per-row intact");
            CHECK(rb->w_trit && memcmp(rb->w_trit, c_tr->blocks[0].w_trit, (size_t)U_IN * U_BPR) == 0,
                  "restored packed trits bit-exact");
            CHECK(rb->w_scale && memcmp(rb->w_scale, c_tr->blocks[0].w_scale, (size_t)U_OUT * 4) == 0,
                  "restored trit scales bit-exact");
            cce_cascade_destroy(r);
        }
    }
    {
        cce_cascade* r = NULL;
        CHECK(cce_weight_store_get(su, d_i4, &r) == CCE_OK && r, "int4 variant restores");
        if (r) {
            cce_block* rb = &r->blocks[0];
            CHECK(rb->w_q && rb->w_scale && !rb->w_trit, "restored int4 variant unpacks to w_q (int8 forward)");
            CHECK(rb->w_q && memcmp(rb->w_q, c_i4->blocks[0].w_q, (size_t)U_IN * U_OUT) == 0,
                  "restored int4 codes bit-exact through the nibble roundtrip");
            CHECK(rb->w_scale && memcmp(rb->w_scale, c_i4->blocks[0].w_scale, (size_t)U_OUT * 4) == 0,
                  "restored int4 scales bit-exact");
            cce_cascade_destroy(r);
        }
    }

    cce_cascade_destroy(c_fp); cce_cascade_destroy(c_i8);
    cce_cascade_destroy(c_tr); cce_cascade_destroy(c_i4);
    cce_weight_store_close(su);
    wipe_store_dir("dst_store_u");

    /* =================== PART 2: packed models STREAM bit-identically =================== */
    LOG("\n--- PART 2: packed models stream under a bounded cap, bit-identical ---\n");

    const int N_SPECS = 7 * TL_L + 1;   /* 15: q,k,v,o,gate,up,down per layer + lm_head */
    const int HOT_CAP = 8;
    static const int tokens[4] = { 3, 17, 9, 22 };

    tl_weights* w = (tl_weights*)malloc(sizeof(tl_weights));
    tl_gen(w, 0);
    tl_write_st_dir("dst_a", w);

    /* FP reference + FP store bytes */
    float refFP[TL_V];
    wipe_store_dir("dst_store_fp");
    cce_weight_store* sfp = NULL;
    CHECK(cce_weight_store_open(&sfp, "dst_store_fp") == CCE_OK && sfp, "FP store opens");
    size_t bytes_fp = 0;
    {
        cce_anymodel* mb = NULL;
        CHECK(cce_anymodel_open(&mb, "dst_a/model.safetensors") == CCE_OK && mb, "FP model opens");
        if (!mb) return 1;
        int nt = 0, nn = 0, nr = 0;
        CHECK(cce_weight_store_ingest_model(sfp, mb, "modelFP", "dst_fp.manifest", &nt, &nn, &nr) == CCE_OK,
              "FP model ingests");
        CHECK(cce_gguf_qwen2_forward(mb->transformer, tokens, 4, refFP, TL_V) == CCE_OK,
              "FP all-resident forward");
        bytes_fp = cce_weight_store_bytes(sfp);
        cce_anymodel_free(mb);
    }

    /* ---- ternary-packed model: all-resident ref, ingest, stream ---- */
    float refT[TL_V];
    wipe_store_dir("dst_store_t");
    cce_weight_store* st = NULL;
    CHECK(cce_weight_store_open(&st, "dst_store_t") == CCE_OK && st, "trit store opens");
    size_t bytes_t = 0;
    {
        cce_anymodel* ma = NULL;
        CHECK(cce_anymodel_open(&ma, "dst_a/model.safetensors") == CCE_OK && ma, "trit model opens");
        if (!ma) return 1;
        int n_packed = forest_ternary_pack(ma->transformer);
        LOG("  ternary-packed specialists: %d\n", n_packed);
        CHECK(n_packed > 0, "block-level ternary+pack quantized the linear specialists");
        CHECK(cce_gguf_qwen2_forward(ma->transformer, tokens, 4, refT, TL_V) == CCE_OK,
              "trit all-resident forward (packed 1.6-bit path)");
        float d = 0;
        for (int v = 0; v < TL_V; v++) { float a = fabsf(refT[v] - refFP[v]); if (a > d) d = a; }
        LOG("  trit-vs-FP all-resident logit dmax = %.6g (quantization error)\n", (double)d);
        CHECK(d > 0.0f, "ternary PTQ changes the all-resident math (fixture non-degenerate)");
        int nt = 0, nn = 0, nr = 0;
        CHECK(cce_weight_store_ingest_model(st, ma, "modelT", "dst_t.manifest", &nt, &nn, &nr) == CCE_OK,
              "trit model ingests");
        bytes_t = cce_weight_store_bytes(st);
        cce_anymodel_free(ma);
    }
    {
        cce_gguf_qwen2* m = NULL;
        CHECK(cce_weight_store_restore_transformer(st, "dst_t.manifest", "dst_restore_t.cce", &m) == CCE_OK && m,
              "restores from trit store");
        if (!m) return 1;
        cce_tier_runtime* rt = NULL;
        CHECK(cce_tier_attach(&rt, m, st, "dst_t.manifest", HOT_CAP) == CCE_OK && rt, "trit tier runtime attaches");
        if (!rt) return 1;
        CHECK(cce_tier_evict_all(rt) == CCE_OK, "trit: evict all (start cold)");
        CHECK(cce_tier_resident(rt) == 0, "trit: starts fully cold");
        float lg[TL_V];
        CHECK(cce_gguf_qwen2_forward(m, tokens, 4, lg, TL_V) == CCE_OK, "trit streaming forward runs");
        float dmax = 0;
        for (int v = 0; v < TL_V; v++) { float a = fabsf(lg[v] - refT[v]); if (a > dmax) dmax = a; }
        LOG("  dmax(stream, trit all-resident) = %.6g\n", (double)dmax);
        LOG("  hot_cap=%d N_SPECS=%d high_water=%d rehydrations=%d resident=%d\n",
            HOT_CAP, N_SPECS, cce_tier_high_water(rt), cce_tier_rehydrations(rt), cce_tier_resident(rt));
        CHECK(dmax == 0.0f, "capped PACKED-TERNARY streaming logits BIT-IDENTICAL to all-resident");
        CHECK(cce_tier_high_water(rt) <= HOT_CAP, "trit: residency never exceeded the cap");
        CHECK(cce_tier_rehydrations(rt) == N_SPECS, "trit: one pass, each specialist fetched once");
        CHECK(cce_tier_detach(rt) == CCE_OK, "trit: detach rehydrates + releases");
        cce_gguf_qwen2_free(m);
        remove("dst_restore_t.cce");
    }

    /* ---- int4 model: all-resident ref, ingest, stream ---- */
    float refI4[TL_V];
    wipe_store_dir("dst_store_i4");
    cce_weight_store* si = NULL;
    CHECK(cce_weight_store_open(&si, "dst_store_i4") == CCE_OK && si, "int4 store opens");
    size_t bytes_i4 = 0;
    {
        cce_anymodel* ma = NULL;
        CHECK(cce_anymodel_open(&ma, "dst_a/model.safetensors") == CCE_OK && ma, "int4 model opens");
        if (!ma) return 1;
        int n_q = forest_int4(ma->transformer);
        LOG("  int4-quantized specialists: %d\n", n_q);
        CHECK(n_q > 0, "naive int4 PTQ quantized the linear specialists");
        CHECK(cce_gguf_qwen2_forward(ma->transformer, tokens, 4, refI4, TL_V) == CCE_OK,
              "int4 all-resident forward (codes ride the int8 path)");
        float d = 0;
        for (int v = 0; v < TL_V; v++) { float a = fabsf(refI4[v] - refFP[v]); if (a > d) d = a; }
        CHECK(d > 0.0f, "int4 PTQ changes the all-resident math");
        int nt = 0, nn = 0, nr = 0;
        CHECK(cce_weight_store_ingest_model(si, ma, "modelI4", "dst_i4.manifest", &nt, &nn, &nr) == CCE_OK,
              "int4 model ingests");
        bytes_i4 = cce_weight_store_bytes(si);
        cce_anymodel_free(ma);
    }
    {
        cce_gguf_qwen2* m = NULL;
        CHECK(cce_weight_store_restore_transformer(si, "dst_i4.manifest", "dst_restore_i4.cce", &m) == CCE_OK && m,
              "restores from int4 store");
        if (!m) return 1;
        cce_tier_runtime* rt = NULL;
        CHECK(cce_tier_attach(&rt, m, si, "dst_i4.manifest", HOT_CAP) == CCE_OK && rt, "int4 tier runtime attaches");
        if (!rt) return 1;
        CHECK(cce_tier_evict_all(rt) == CCE_OK, "int4: evict all (start cold)");
        float lg[TL_V];
        CHECK(cce_gguf_qwen2_forward(m, tokens, 4, lg, TL_V) == CCE_OK, "int4 streaming forward runs");
        float dmax = 0;
        for (int v = 0; v < TL_V; v++) { float a = fabsf(lg[v] - refI4[v]); if (a > dmax) dmax = a; }
        LOG("  dmax(stream, int4 all-resident) = %.6g\n", (double)dmax);
        CHECK(dmax == 0.0f, "capped PACKED-INT4 streaming logits BIT-IDENTICAL to all-resident");
        CHECK(cce_tier_high_water(rt) <= HOT_CAP, "int4: residency never exceeded the cap");
        CHECK(cce_tier_detach(rt) == CCE_OK, "int4: detach rehydrates + releases");
        cce_gguf_qwen2_free(m);
        remove("dst_restore_i4.cce");
    }

    LOG("\n--- on-disk store bytes for the same model ---\n");
    LOG("  FP=%zu  int4=%zu  trit=%zu  (trit %.3fx vs FP, int4 %.3fx vs FP)\n",
        bytes_fp, bytes_i4, bytes_t,
        bytes_t ? (double)bytes_fp / (double)bytes_t : 0.0,
        bytes_i4 ? (double)bytes_fp / (double)bytes_i4 : 0.0);
    LOG("  NOTE: the tiny fixture (out dims 4..24) pads trit rows (ceil(out/5)) and\n");
    LOG("        amortizes scale/bias poorly; PART 1 shows the exact 1.6-bit rate at real dims.\n");
    CHECK(bytes_t < bytes_i4, "trit store smaller than int4 store");
    CHECK(bytes_i4 < bytes_fp, "int4 store smaller than FP store");

    /* teardown */
    cce_weight_store_close(st);
    cce_weight_store_close(si);
    cce_weight_store_close(sfp);
    wipe_store_dir("dst_store_t");
    wipe_store_dir("dst_store_i4");
    wipe_store_dir("dst_store_fp");
    tl_cleanup_st_dir("dst_a");
    remove("dst_t.manifest");
    remove("dst_i4.manifest");
    remove("dst_fp.manifest");
    free(w);

    LOG("\n%d checks, %d failed -> %s\n", checks, fails, fails ? "FAIL" : "OK");
    if (LOGF) fclose(LOGF);
    return fails ? 1 : 0;
}
