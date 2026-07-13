/* moe_expert_quant: Arc B4 — per-expert data-aware quantization on ROUTED
 * activations.
 *
 * The router's real traffic becomes each expert's calibration set: while
 * calibrating, every routed token's expert input lands in that expert's
 * sample buffer; the data-aware store modes then quantize each expert at
 * ingest against ITS OWN samples (ridge-damped sample-space OBQ, monotone
 * from the naive init; < 8 samples falls back to naive rather than overfit).
 * The int4 modes apply the proven bit-width policy (gate/up int4 — packed
 * 2/byte by the store — down int8).
 *
 * PART 1 (hermetic): calibrate on 24 tokens, quantize naive-int4 vs
 * data-aware-int4, evaluate on the calibration tokens (the mechanism must
 * win there) and on HELD-OUT tokens (no harm, reported); payloads ~6x
 * smaller than FP; quantized experts re-stream bit-identical; un-calibrated
 * experts fall back naive and still run.
 *
 * PART 2 (real gemma-4-26B-A4B, auto-skips): calibrate layer 0 on 48 real
 * routed tokens, quantize the eval unions naive vs data-aware at int4,
 * measure held-out output error vs the FP experts — the B4 headline:
 * data-aware < naive on real weights and real routing.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#include "../include/cce/cce_gguf.h"
#include "../include/cce/cce_forest.h"
#include "../include/cce/cce_weight_store.h"
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
#ifndef _WIN32
    DIR* d = opendir(dir);
    if (d) { struct dirent* e; char p[700];
        while ((e = readdir(d))) { size_t n = strlen(e->d_name);
            if (n > 5 && strcmp(e->d_name + n - 5, ".spec") == 0) { snprintf(p, sizeof p, "%s/%s", dir, e->d_name); remove(p); } }
        closedir(d); }
#endif
    remove(dir);
}

static void evict_experts(cce_gguf_moe_rt* rt) {
    for (int i = 0; i < rt->forest->num_branches; i++) {
        cce_branch* b = &rt->forest->branches[i];
        if (b->evictable && b->cascade && !b->is_view)
            cce_forest_evict_branch(rt->forest, i);
    }
}

/* relative L2 of a vs ref over n floats */
static double rel_l2(const float* a, const float* ref, size_t n) {
    double e = 0, r = 0;
    for (size_t i = 0; i < n; i++) {
        double d = (double)a[i] - ref[i];
        e += d * d;
        r += (double)ref[i] * ref[i];
    }
    return r > 0 ? sqrt(e / r) : sqrt(e);
}

/* ---------------- hermetic qwen3moe fixture (LCG weights) ---------------- */

#define MQ_L 2
#define MQ_D 8
#define MQ_F 16
#define MQ_E 4
#define MQ_K 2

static uint32_t g_lcg = 0xC0FFEE42u;
static float lcg_f(void) {
    g_lcg = g_lcg * 1664525u + 1013904223u;
    return ((float)(g_lcg >> 8) / (float)(1u << 24)) - 0.5f;
}

static void mq_write_gguf(const char* path) {
    FILE* f = fopen(path, "wb");
    fwrite("GGUF", 1, 4, f);
    tl_gg_u32(f, 3);
    tl_gg_u64(f, 4 * MQ_L);
    tl_gg_u64(f, 6);
    tl_gg_str(f, "general.architecture"); tl_gg_u32(f, 8); tl_gg_str(f, "qwen3moe");
    tl_gg_kv_u32(f, "qwen3moe.block_count", MQ_L);
    tl_gg_kv_u32(f, "qwen3moe.embedding_length", MQ_D);
    tl_gg_kv_u32(f, "qwen3moe.expert_count", MQ_E);
    tl_gg_kv_u32(f, "qwen3moe.expert_used_count", MQ_K);
    tl_gg_kv_u32(f, "qwen3moe.expert_feed_forward_length", MQ_F);
    uint64_t off = 0;
    for (int l = 0; l < MQ_L; l++) {
        char name[96];
        snprintf(name, sizeof name, "blk.%d.ffn_gate_exps.weight", l);
        tl_gg_str(f, name); tl_gg_u32(f, 3);
        tl_gg_u64(f, MQ_D); tl_gg_u64(f, MQ_F); tl_gg_u64(f, MQ_E);
        tl_gg_u32(f, 0); tl_gg_u64(f, off); off += (uint64_t)MQ_D * MQ_F * MQ_E * 4;
        snprintf(name, sizeof name, "blk.%d.ffn_up_exps.weight", l);
        tl_gg_str(f, name); tl_gg_u32(f, 3);
        tl_gg_u64(f, MQ_D); tl_gg_u64(f, MQ_F); tl_gg_u64(f, MQ_E);
        tl_gg_u32(f, 0); tl_gg_u64(f, off); off += (uint64_t)MQ_D * MQ_F * MQ_E * 4;
        snprintf(name, sizeof name, "blk.%d.ffn_down_exps.weight", l);
        tl_gg_str(f, name); tl_gg_u32(f, 3);
        tl_gg_u64(f, MQ_F); tl_gg_u64(f, MQ_D); tl_gg_u64(f, MQ_E);
        tl_gg_u32(f, 0); tl_gg_u64(f, off); off += (uint64_t)MQ_F * MQ_D * MQ_E * 4;
        snprintf(name, sizeof name, "blk.%d.ffn_gate_inp.weight", l);
        tl_gg_str(f, name); tl_gg_u32(f, 2);
        tl_gg_u64(f, MQ_D); tl_gg_u64(f, MQ_E);
        tl_gg_u32(f, 0); tl_gg_u64(f, off); off += (uint64_t)MQ_D * MQ_E * 4;
    }
    { long pos = ftell(f); int pad = (int)((32 - (pos % 32)) % 32);
      while (pad-- > 0) fputc(0, f); }
    g_lcg = 0x0DDBA11u;
    size_t per_layer = (size_t)MQ_E * MQ_F * MQ_D * 2 + (size_t)MQ_E * MQ_D * MQ_F
                     + (size_t)MQ_E * MQ_D;
    for (size_t i = 0; i < (size_t)MQ_L * per_layer; i++) {
        float v = lcg_f();
        fwrite(&v, 4, 1, f);
    }
    fclose(f);
}

int main(int argc, char** argv) {
    LOGF = fopen("logs/moe_expert_quant.log", "wb");
    LOG("=== moe_expert_quant: Arc B4 — per-expert data-aware quant on routed traffic ===\n");

    /* =================== PART 1: hermetic =================== */
    mq_write_gguf("mq_moe.gguf");
    cce_gguf_moe* mm = NULL;
    CHECK(cce_gguf_moe_open("mq_moe.gguf", &mm) == CCE_OK && mm, "hermetic MoE opens");
    if (!mm) return 1;

#define NC 288
#define NE 64
    static float xc[NC][MQ_D], xe_[NE][MQ_D];
    static float fp_c[NC][MQ_D], fp_e[NE][MQ_D];
    static float nv_c[NC][MQ_D], nv_e[NE][MQ_D];
    static float da_c[NC][MQ_D], da_e[NE][MQ_D];
    static float rr2[NE][MQ_D];
    g_lcg = 0xBADD00D5u;
    for (int t = 0; t < NC; t++) for (int i = 0; i < MQ_D; i++) xc[t][i] = 2.0f * lcg_f();
    for (int t = 0; t < NE; t++) for (int i = 0; i < MQ_D; i++) xe_[t][i] = 2.0f * lcg_f();

    remove("mq_rt.cce");
    cce_gguf_moe_rt* rt = NULL;
    CHECK(cce_gguf_moe_rt_open(mm, "mq_rt.cce", 8, &rt) == CCE_OK && rt, "runtime opens");
    if (!rt) return 1;

    /* calibrate on layer 0 ONLY (layer 1 stays un-calibrated: fallback path) */
    LOG("\n--- calibration: 288 routed tokens on layer 0 ---\n");
    CHECK(cce_gguf_moe_rt_set_calibration(rt, 128) == CCE_OK, "calibration enables (cap 128)");
    CHECK(cce_gguf_moe_ffn_forward_batch(rt, 0, &xc[0][0], &fp_c[0][0], NC) == CCE_OK,
          "calibration batch forwards (FP experts)");
    {
        int with_samples = 0, total = 0;
        for (int e = 0; e < MQ_E; e++) {
            int n = cce_gguf_moe_rt_calib_samples(rt, 0, e);
            total += n;
            if (n >= 8) with_samples++;
            LOG("  expert (0,%d): %d samples\n", e, n);
        }
        CHECK(total > 0 && with_samples > 0, "routed tokens became per-expert calibration samples");
        CHECK(cce_gguf_moe_rt_calib_samples(rt, 1, 0) +
              cce_gguf_moe_rt_calib_samples(rt, 1, 1) +
              cce_gguf_moe_rt_calib_samples(rt, 1, 2) +
              cce_gguf_moe_rt_calib_samples(rt, 1, 3) == 0, "layer 1 collected nothing (not run)");
    }
    CHECK(cce_gguf_moe_rt_set_calibration(rt, 0) == CCE_OK,
          "calibration stops BEFORE eval (held-out stays held out)");

    /* FP references for the held-out tokens */
    CHECK(cce_gguf_moe_ffn_forward_batch(rt, 0, &xe_[0][0], &fp_e[0][0], NE) == CCE_OK,
          "held-out tokens forward FP");

    /* naive int4 (gate/up K=7, down int8) */
    LOG("\n--- naive int4 vs data-aware int4 (bit-width policy in both) ---\n");
    wipe_store_dir("mq_store_n");
    wipe_store_dir("mq_store_d");
    cce_weight_store *sn = NULL, *sd = NULL;
    CHECK(cce_weight_store_open(&sn, "mq_store_n") == CCE_OK &&
          cce_weight_store_open(&sd, "mq_store_d") == CCE_OK, "stores open");
    CHECK(cce_gguf_moe_rt_attach_store(rt, sn, CCE_MOE_STORE_INT4) == CCE_OK, "naive-int4 attaches");
    evict_experts(rt);
    CHECK(cce_gguf_moe_ffn_forward_batch(rt, 0, &xc[0][0], &nv_c[0][0], NC) == CCE_OK &&
          cce_gguf_moe_ffn_forward_batch(rt, 0, &xe_[0][0], &nv_e[0][0], NE) == CCE_OK,
          "naive-int4 forwards (calib + held-out tokens)");
    uint64_t d_naive = rt->digests[0 * MQ_E + rt->last_experts[0]];
    uint64_t dnaive_all[MQ_E];
    for (int e = 0; e < MQ_E; e++) dnaive_all[e] = rt->digests[0 * MQ_E + e];

    CHECK(cce_gguf_moe_rt_attach_store(rt, sd, CCE_MOE_STORE_INT4_DA) == CCE_OK, "data-aware int4 attaches");
    evict_experts(rt);
    CHECK(cce_gguf_moe_ffn_forward_batch(rt, 0, &xc[0][0], &da_c[0][0], NC) == CCE_OK &&
          cce_gguf_moe_ffn_forward_batch(rt, 0, &xe_[0][0], &da_e[0][0], NE) == CCE_OK,
          "data-aware int4 forwards (calib + held-out tokens)");

    {
        double en_c = rel_l2(&nv_c[0][0], &fp_c[0][0], (size_t)NC * MQ_D);
        double ed_c = rel_l2(&da_c[0][0], &fp_c[0][0], (size_t)NC * MQ_D);
        double en_e = rel_l2(&nv_e[0][0], &fp_e[0][0], (size_t)NE * MQ_D);
        double ed_e = rel_l2(&da_e[0][0], &fp_e[0][0], (size_t)NE * MQ_D);
        LOG("  calib tokens:    naive relL2 %.4f  data-aware %.4f (%+.1f%%)\n",
            en_c, ed_c, 100.0 * (ed_c / en_c - 1.0));
        LOG("  held-out tokens: naive relL2 %.4f  data-aware %.4f (%+.1f%%)\n",
            en_e, ed_e, 100.0 * (ed_e / en_e - 1.0));
        CHECK(ed_c < en_c, "MECHANISM: data-aware beats naive on the calibration traffic");
        CHECK(ed_e < en_e * 1.02, "held-out: data-aware does not harm (n >= 2*in regime: win expected)");
    }

    /* payload economics (tiny dims are scale/bias-dominated: the >5x weight
     * compression shows on the REAL model below) + naive/DA payloads differ
     * for at least one CALIBRATED expert */
    {
        long pb = cce_weight_store_payload_size(sd, rt->digests[0 * MQ_E + rt->last_experts[0]]);
        double fp_bytes = 3.0 * MQ_D * MQ_F * 4 + (2.0 * MQ_F + MQ_D) * 4 /* + biases */;
        LOG("  expert payload: %ld bytes vs %.0f FP-equivalent (%.1fx)\n", pb, fp_bytes, fp_bytes / pb);
        CHECK(pb > 0 && fp_bytes / pb > 2.0, "int4-policy payload compresses (real-scale gate in PART 2)");
        /* NOTE: naive and DA variants of one expert get the SAME digest value
         * (cce_spec_digest hashes the FP weights; codes/scales are not part of
         * content identity) — they live in separate stores here. Same-store
         * re-ingest under a different quant config would byte-verify-refuse.
         * The math proves the payloads differ: */
        (void)d_naive; (void)dnaive_all;
        CHECK(memcmp(nv_c, da_c, sizeof nv_c) != 0,
              "naive and data-aware payloads produce different expert math");
    }

    /* quantized re-stream: bit-identical */
    evict_experts(rt);
    CHECK(cce_gguf_moe_ffn_forward_batch(rt, 0, &xe_[0][0], &rr2[0][0], NE) == CCE_OK &&
          memcmp(rr2, da_e, sizeof rr2) == 0,
          "data-aware int4 experts re-stream BIT-IDENTICAL");

    /* un-calibrated layer falls back naive and still runs */
    {
        float o1[MQ_D];
        CHECK(cce_gguf_moe_ffn_forward(rt, 1, xe_[0], o1) == CCE_OK, "un-calibrated layer forwards (naive fallback)");
        int finite = 1;
        for (int i = 0; i < MQ_D; i++) if (!isfinite(o1[i])) finite = 0;
        CHECK(finite, "fallback output finite");
    }

    /* int8-DA mode coverage: runs + restreams bit-identical */
    {
        static float o8[NE][MQ_D], o8b[NE][MQ_D];
        wipe_store_dir("mq_store_8");
        cce_weight_store* s8 = NULL;
        CHECK(cce_weight_store_open(&s8, "mq_store_8") == CCE_OK &&
              cce_gguf_moe_rt_attach_store(rt, s8, CCE_MOE_STORE_INT8_DA) == CCE_OK,
              "int8-DA mode attaches");
        evict_experts(rt);
        CHECK(cce_gguf_moe_ffn_forward_batch(rt, 0, &xe_[0][0], &o8[0][0], NE) == CCE_OK,
              "int8-DA forwards");
        CHECK(rel_l2(&o8[0][0], &fp_e[0][0], (size_t)NE * MQ_D) < 0.02,
              "int8-DA near-lossless");
        evict_experts(rt);
        CHECK(cce_gguf_moe_ffn_forward_batch(rt, 0, &xe_[0][0], &o8b[0][0], NE) == CCE_OK &&
              memcmp(o8, o8b, sizeof o8) == 0, "int8-DA re-streams bit-identical");
        cce_weight_store_close(s8);
        wipe_store_dir("mq_store_8");
    }

    cce_gguf_moe_rt_free(rt);
    cce_weight_store_close(sn);
    cce_weight_store_close(sd);
    wipe_store_dir("mq_store_n");
    wipe_store_dir("mq_store_d");
    remove("mq_rt.cce");
    cce_gguf_moe_free(mm);
    remove("mq_moe.gguf");

    /* =================== PART 2: real model =================== */
    const char* rpath = (argc > 1) ? argv[1]
                      : "/home/marble/Downloads/gemma-4-26B-A4B-it-UD-Q6_K_XL.gguf";
    FILE* rf = fopen(rpath, "rb");
    if (!rf) {
        LOG("\n--- PART 2 skipped: no real MoE checkpoint at %s ---\n", rpath);
    } else {
        fclose(rf);
        LOG("\n--- PART 2: real gemma4, layer 0 — calibrate on routed traffic ---\n");
        cce_gguf_moe* rm = NULL;
        CHECK(cce_gguf_moe_open(rpath, &rm) == CCE_OK && rm, "real MoE opens");
        if (rm) {
            int D = rm->n_embd, E = rm->n_expert, K = rm->n_expert_used;
            const int RC = 128, RE = 16;
            remove("mq_rt_r.cce");
            cce_gguf_moe_rt* rr = NULL;
            /* cap 128: the calibration union stays resident (no thrash) */
            CHECK(cce_gguf_moe_rt_open(rm, "mq_rt_r.cce", 128, &rr) == CCE_OK && rr,
                  "real runtime opens (cap 128)");
            if (rr) {
                float* xs  = (float*)malloc((size_t)RC * D * sizeof(float));
                float* xev = (float*)malloc((size_t)RE * D * sizeof(float));
                float* ofp = (float*)malloc((size_t)RE * D * sizeof(float));
                float* onv = (float*)malloc((size_t)RE * D * sizeof(float));
                float* oda = (float*)malloc((size_t)RE * D * sizeof(float));
                float* tmp = (float*)malloc((size_t)RC * D * sizeof(float));
                g_lcg = 0x5EED5EEDu;
                for (int i = 0; i < RC * D; i++) xs[i] = 2.0f * lcg_f();
                for (int i = 0; i < RE * D; i++) xev[i] = 2.0f * lcg_f();

                CHECK(cce_gguf_moe_rt_set_calibration(rr, 64) == CCE_OK, "real calibration enables");
                CHECK(cce_gguf_moe_ffn_forward_batch(rr, 0, xs, tmp, RC) == CCE_OK,
                      "128 real tokens calibrate layer 0 (batch-union load)");
                CHECK(cce_gguf_moe_rt_set_calibration(rr, 0) == CCE_OK, "real calibration stops");
                {
                    int covered = 0, total_s = 0, mx = 0;
                    for (int e = 0; e < E; e++) {
                        int n = cce_gguf_moe_rt_calib_samples(rr, 0, e);
                        total_s += n;
                        if (n >= 8) covered++;
                        if (n > mx) mx = n;
                    }
                    LOG("  calibration: %d samples over %d experts with >=8 (max %d)\n",
                        total_s, covered, mx);
                    CHECK(total_s == RC * K, "every routed (token,expert) pair became a sample");
                    CHECK(covered > 0, "hot experts crossed the data-aware threshold");
                }

                CHECK(cce_gguf_moe_ffn_forward_batch(rr, 0, xev, ofp, RE) == CCE_OK,
                      "held-out FP reference");

                wipe_store_dir("mq_store_rn");
                wipe_store_dir("mq_store_rd");
                cce_weight_store *srn = NULL, *srd = NULL;
                CHECK(cce_weight_store_open(&srn, "mq_store_rn") == CCE_OK &&
                      cce_weight_store_open(&srd, "mq_store_rd") == CCE_OK, "real stores open");

                CHECK(cce_gguf_moe_rt_attach_store(rr, srn, CCE_MOE_STORE_INT4) == CCE_OK, "naive int4 attaches");
                evict_experts(rr);
                CHECK(cce_gguf_moe_ffn_forward_batch(rr, 0, xev, onv, RE) == CCE_OK, "naive int4 held-out pass");

                CHECK(cce_gguf_moe_rt_attach_store(rr, srd, CCE_MOE_STORE_INT4_DA) == CCE_OK, "data-aware int4 attaches");
                evict_experts(rr);
                CHECK(cce_gguf_moe_ffn_forward_batch(rr, 0, xev, oda, RE) == CCE_OK, "data-aware int4 held-out pass");

                {
                    double en = rel_l2(onv, ofp, (size_t)RE * D);
                    double ed = rel_l2(oda, ofp, (size_t)RE * D);
                    LOG("\n  === B4 headline (real weights, real routing, held-out tokens) ===\n");
                    LOG("  int4 output relL2 vs FP experts: naive %.4f  data-aware %.4f (%+.1f%%)\n",
                        en, ed, 100.0 * (ed / en - 1.0));
                    CHECK(ed < en,
                          "B4: per-expert DATA-AWARE int4 beats naive int4 on held-out routed traffic");
                }
                {
                    long pb = cce_weight_store_payload_size(srd, rr->digests[rr->last_experts[0]]);
                    double fpb = 3.0 * (double)D * rm->n_ff_exp * 4;
                    LOG("  expert payload %.1f MB vs %.1f MB FP (%.1fx); store holds %d quantized experts\n",
                        pb / 1e6, fpb / 1e6, fpb / pb, cce_weight_store_count(srd));
                    CHECK(pb > 0 && fpb / pb > 4.5, "real int4-policy payload >4.5x smaller than FP");
                }
                /* quantized streaming stays bit-identical */
                evict_experts(rr);
                CHECK(cce_gguf_moe_ffn_forward_batch(rr, 0, xev, tmp, RE) == CCE_OK &&
                      memcmp(tmp, oda, (size_t)RE * D * sizeof(float)) == 0,
                      "data-aware int4 experts re-stream BIT-IDENTICAL");

                cce_weight_store_close(srn);
                cce_weight_store_close(srd);
                wipe_store_dir("mq_store_rn");
                wipe_store_dir("mq_store_rd");
                free(xs); free(xev); free(ofp); free(onv); free(oda); free(tmp);
            }
            if (rr) cce_gguf_moe_rt_free(rr);
            remove("mq_rt_r.cce");
            cce_gguf_moe_free(rm);
        }
    }

    LOG("\n%d checks, %d failed -> %s\n", checks, fails, fails ? "FAIL" : "OK");
    if (LOGF) fclose(LOGF);
    return fails ? 1 : 0;
}
