/* moe_forward: Arc B2 — routed, demand-loaded MoE FFN forward.
 *
 * PART 1 (hermetic): a synthetic qwen3moe gguf with small LCG weights.
 * cce_gguf_moe_ffn_forward is gated against an INDEPENDENT reference
 * implementation (plain loops over the generator arrays, llama.cpp
 * conventions: softmax over ALL experts -> top-k -> renormalize -> SwiGLU ->
 * weighted sum): same selection, same weights, same output. Mechanics:
 * exactly the routed experts are fetched, repeated tokens re-use residents,
 * residency never exceeds the cap, repeat forward is bit-identical.
 *
 * PART 2 (real, auto-skips): gemma-4-26B-A4B — routing + demand-loading on
 * 128 experts (8 fetched of 128), gemma4 side vectors loaded, forwards
 * finite and deterministic, BF16 last-layer path exercised.
 *
 * PART 3 (llama.cpp CROSS-EXECUTABLE PARITY, auto-skips): replay the
 * layer-0 MoE FFN on the attn_out tensors dumped from the REFERENCE
 * llama.cpp run (tools/moe_parity_dump.cpp -> logs/moe_parity). Gates per
 * token: identical top-8 expert selection, router logits + normalized
 * weights within fp tolerance, expert-input norm matches, and the combined
 * expert output within quant-noise tolerance of llama.cpp's (it computes
 * Q6_K/Q8_0 x q8 quantized matmuls; CNET dequants to F32).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#include "../include/cce/cce_gguf.h"
#include "../include/cce/cce_forest.h"
#include "tiny_model_fixture.h"

static int checks = 0, fails = 0;
static FILE* LOGF = NULL;

#define LOG(...) do { printf(__VA_ARGS__); if (LOGF) fprintf(LOGF, __VA_ARGS__); } while (0)
#define CHECK(cond, msg) do { checks++; if (!(cond)) { fails++; LOG("  FAIL: %s\n", msg); } \
                              else { LOG("  ok:   %s\n", msg); } } while (0)

/* ---------------- hermetic qwen3moe fixture (small LCG weights) ---------------- */

#define MF_L 2
#define MF_D 8
#define MF_F 16
#define MF_E 4
#define MF_K 2

static float g_gate[MF_L][MF_E][MF_F][MF_D];   /* bank rows [out][in] */
static float g_up  [MF_L][MF_E][MF_F][MF_D];
static float g_down[MF_L][MF_E][MF_D][MF_F];
static float g_rout[MF_L][MF_E][MF_D];

static uint32_t g_lcg = 0xC0FFEE42u;
static float lcg_f(void) {
    g_lcg = g_lcg * 1664525u + 1013904223u;
    return ((float)(g_lcg >> 8) / (float)(1u << 24)) - 0.5f;
}

static void mf_gen(void) {
    g_lcg = 0xC0FFEE42u;
    for (int l = 0; l < MF_L; l++) {
        for (int e = 0; e < MF_E; e++) {
            for (int o = 0; o < MF_F; o++)
                for (int i = 0; i < MF_D; i++) { g_gate[l][e][o][i] = lcg_f(); g_up[l][e][o][i] = lcg_f(); }
            for (int o = 0; o < MF_D; o++)
                for (int i = 0; i < MF_F; i++) g_down[l][e][o][i] = lcg_f();
            for (int i = 0; i < MF_D; i++) g_rout[l][e][i] = lcg_f();
        }
    }
}

static void mf_write_gguf(const char* path) {
    FILE* f = fopen(path, "wb");
    fwrite("GGUF", 1, 4, f);
    tl_gg_u32(f, 3);
    tl_gg_u64(f, 4 * MF_L);
    tl_gg_u64(f, 6);
    tl_gg_str(f, "general.architecture"); tl_gg_u32(f, 8); tl_gg_str(f, "qwen3moe");
    tl_gg_kv_u32(f, "qwen3moe.block_count", MF_L);
    tl_gg_kv_u32(f, "qwen3moe.embedding_length", MF_D);
    tl_gg_kv_u32(f, "qwen3moe.expert_count", MF_E);
    tl_gg_kv_u32(f, "qwen3moe.expert_used_count", MF_K);
    tl_gg_kv_u32(f, "qwen3moe.expert_feed_forward_length", MF_F);
    uint64_t off = 0;
    for (int l = 0; l < MF_L; l++) {
        char name[96];
        snprintf(name, sizeof name, "blk.%d.ffn_gate_exps.weight", l);
        tl_gg_str(f, name); tl_gg_u32(f, 3);
        tl_gg_u64(f, MF_D); tl_gg_u64(f, MF_F); tl_gg_u64(f, MF_E);
        tl_gg_u32(f, 0); tl_gg_u64(f, off); off += (uint64_t)MF_D * MF_F * MF_E * 4;
        snprintf(name, sizeof name, "blk.%d.ffn_up_exps.weight", l);
        tl_gg_str(f, name); tl_gg_u32(f, 3);
        tl_gg_u64(f, MF_D); tl_gg_u64(f, MF_F); tl_gg_u64(f, MF_E);
        tl_gg_u32(f, 0); tl_gg_u64(f, off); off += (uint64_t)MF_D * MF_F * MF_E * 4;
        snprintf(name, sizeof name, "blk.%d.ffn_down_exps.weight", l);
        tl_gg_str(f, name); tl_gg_u32(f, 3);
        tl_gg_u64(f, MF_F); tl_gg_u64(f, MF_D); tl_gg_u64(f, MF_E);
        tl_gg_u32(f, 0); tl_gg_u64(f, off); off += (uint64_t)MF_F * MF_D * MF_E * 4;
        snprintf(name, sizeof name, "blk.%d.ffn_gate_inp.weight", l);
        tl_gg_str(f, name); tl_gg_u32(f, 2);
        tl_gg_u64(f, MF_D); tl_gg_u64(f, MF_E);
        tl_gg_u32(f, 0); tl_gg_u64(f, off); off += (uint64_t)MF_D * MF_E * 4;
    }
    { long pos = ftell(f); int pad = (int)((32 - (pos % 32)) % 32);
      while (pad-- > 0) fputc(0, f); }
    for (int l = 0; l < MF_L; l++) {
        fwrite(g_gate[l], 4, (size_t)MF_E * MF_F * MF_D, f);
        fwrite(g_up[l],   4, (size_t)MF_E * MF_F * MF_D, f);
        fwrite(g_down[l], 4, (size_t)MF_E * MF_D * MF_F, f);
        fwrite(g_rout[l], 4, (size_t)MF_E * MF_D, f);
    }
    fclose(f);
}

/* INDEPENDENT reference (llama.cpp conventions, straight loops on arrays) */
static float ref_silu(float x) { return x / (1.0f + expf(-x)); }

static void mf_ref_forward(int l, const float* x, float* out, int* sel, float* w) {
    double z[MF_E];
    for (int e = 0; e < MF_E; e++) {
        z[e] = 0;
        for (int i = 0; i < MF_D; i++) z[e] += (double)g_rout[l][e][i] * x[i];
    }
    double mx = z[0];
    for (int e = 1; e < MF_E; e++) if (z[e] > mx) mx = z[e];
    double probs[MF_E], s = 0;
    for (int e = 0; e < MF_E; e++) { probs[e] = exp(z[e] - mx); s += probs[e]; }
    for (int e = 0; e < MF_E; e++) probs[e] /= s;
    char taken[MF_E] = {0};
    for (int k = 0; k < MF_K; k++) {
        int best = -1;
        for (int e = 0; e < MF_E; e++)
            if (!taken[e] && (best < 0 || probs[e] > probs[best])) best = e;
        taken[best] = 1; sel[k] = best; w[k] = (float)probs[best];
    }
    float ws = 0;
    for (int k = 0; k < MF_K; k++) ws += w[k];
    for (int k = 0; k < MF_K; k++) w[k] /= ws;
    memset(out, 0, MF_D * sizeof(float));
    for (int k = 0; k < MF_K; k++) {
        int e = sel[k];
        float h[MF_F];
        for (int o = 0; o < MF_F; o++) {
            float gg = 0, uu = 0;
            for (int i = 0; i < MF_D; i++) {
                gg += g_gate[l][e][o][i] * x[i];
                uu += g_up[l][e][o][i] * x[i];
            }
            h[o] = ref_silu(gg) * uu;
        }
        for (int o = 0; o < MF_D; o++) {
            float y = 0;
            for (int i = 0; i < MF_F; i++) y += g_down[l][e][o][i] * h[i];
            out[o] += w[k] * y;
        }
    }
}

static int sets_equal(const int* a, const int* b, int n) {
    int sa[64], sb[64];
    memcpy(sa, a, n * sizeof(int)); memcpy(sb, b, n * sizeof(int));
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++) {
            if (sa[j] < sa[i]) { int t = sa[i]; sa[i] = sa[j]; sa[j] = t; }
            if (sb[j] < sb[i]) { int t = sb[i]; sb[i] = sb[j]; sb[j] = t; }
        }
    return memcmp(sa, sb, n * sizeof(int)) == 0;
}

static double vdmax(const float* a, const float* b, int n) {
    double m = 0;
    for (int i = 0; i < n; i++) { double d = fabs((double)a[i] - b[i]); if (d > m) m = d; }
    return m;
}
/* ---------------- CNDT dump reader (llama.cpp parity fixtures) ---------------- */
static void* read_cndt(const char* dir, const char* name, int64_t ne[4], int* is_i32) {
    char p[600];
    snprintf(p, sizeof p, "%s/%s.bin", dir, name);
    FILE* f = fopen(p, "rb");
    if (!f) return NULL;
    char magic[4]; int32_t type = 0, ndim = 0;
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "CNDT", 4) != 0 ||
        fread(&type, 4, 1, f) != 1 || fread(&ndim, 4, 1, f) != 1 ||
        fread(ne, 8, 4, f) != 4) { fclose(f); return NULL; }
    size_t n = (size_t)ne[0] * ne[1] * ne[2] * ne[3];
    void* buf = malloc(n * 4);
    if (!buf || fread(buf, 4, n, f) != n) { free(buf); fclose(f); return NULL; }
    fclose(f);
    *is_i32 = (type == 26);
    return buf;
}

int main(int argc, char** argv) {
    LOGF = fopen("logs/moe_forward.log", "wb");
    LOG("=== moe_forward: Arc B2 — routed, demand-loaded MoE FFN forward ===\n");

    /* =================== PART 1: hermetic vs independent reference =================== */
    LOG("\n--- PART 1: hermetic (qwen3moe conventions, independent reference) ---\n");
    mf_gen();
    mf_write_gguf("mf_moe.gguf");

    cce_gguf_moe* mm = NULL;
    CHECK(cce_gguf_moe_open("mf_moe.gguf", &mm) == CCE_OK && mm, "hermetic MoE opens");
    if (!mm) return 1;
    remove("mf_rt.cce");
    cce_gguf_moe_rt* rt = NULL;
    CHECK(cce_gguf_moe_rt_open(mm, "mf_rt.cce", 8, &rt) == CCE_OK && rt, "runtime opens (cap 8)");
    if (!rt) return 1;
    CHECK(rt->expert_fetches == 0, "no experts loaded before the first token (routers only)");

    /* several tokens: forward must match the reference exactly */
    int all_sel = 1, all_w = 1, all_out = 1, all_run = 1;
    char touched[MF_L][MF_E];
    memset(touched, 0, sizeof(touched));
    int expect_fetch = 0;
    float x[MF_D], out[MF_D], ref[MF_D];
    int rsel[MF_K];
    float rw[MF_K];
    g_lcg = 0xBADD00D5u;
    for (int t = 0; t < 6; t++) {
        int l = t % MF_L;
        for (int i = 0; i < MF_D; i++) x[i] = 2.0f * lcg_f();
        mf_ref_forward(l, x, ref, rsel, rw);
        for (int k = 0; k < MF_K; k++)
            if (!touched[l][rsel[k]]) { touched[l][rsel[k]] = 1; expect_fetch++; }
        if (cce_gguf_moe_ffn_forward(rt, l, x, out) != CCE_OK) { all_run = 0; continue; }
        if (!sets_equal(rt->last_experts, rsel, MF_K)) all_sel = 0;
        /* weights matched by expert id */
        for (int k = 0; k < MF_K; k++)
            for (int j = 0; j < MF_K; j++)
                if (rt->last_experts[k] == rsel[j] &&
                    fabsf(rt->last_weights[k] - rw[j]) > 1e-5f) all_w = 0;
        if (vdmax(out, ref, MF_D) > 1e-4) all_out = 0;
    }
    CHECK(all_run, "6 tokens forward across both layers");
    CHECK(all_sel, "top-k expert SELECTION matches the independent reference on every token");
    CHECK(all_w, "renormalized routing weights match the reference (<= 1e-5)");
    CHECK(all_out, "combined expert output matches the reference (dmax <= 1e-4)");
    CHECK(rt->expert_fetches == expect_fetch,
          "exactly the ROUTED experts were fetched (demand loading, no waste)");
    CHECK(cce_forest_resident_count(rt->forest) <= 8, "resident experts bounded by the cap");

    /* determinism: same token twice -> bit-identical, no new fetches */
    {
        float out2[MF_D];
        int f0;
        for (int i = 0; i < MF_D; i++) x[i] = 0.3f * (float)(i - 3);
        CHECK(cce_gguf_moe_ffn_forward(rt, 0, x, out) == CCE_OK, "det token forwards");
        f0 = rt->expert_fetches;
        CHECK(cce_gguf_moe_ffn_forward(rt, 0, x, out2) == CCE_OK, "det token repeats");
        CHECK(memcmp(out, out2, sizeof out) == 0, "repeat forward BIT-IDENTICAL");
        CHECK(rt->expert_fetches == f0, "repeat token fetched nothing (residents reused)");
    }
    CHECK(cce_gguf_moe_ffn_forward(rt, MF_L, x, out) == CCE_ERR_INVALID_ARG, "layer out of range refused");

    cce_gguf_moe_rt_free(rt);
    cce_gguf_moe_free(mm);
    remove("mf_moe.gguf");
    remove("mf_rt.cce");

    /* =================== PART 2 + 3: real model =================== */
    const char* rpath = (argc > 1) ? argv[1]
                      : "/home/marble/Downloads/gemma-4-26B-A4B-it-UD-Q6_K_XL.gguf";
    const char* pdir = (argc > 2) ? argv[2] : "logs/moe_parity";
    FILE* rf = fopen(rpath, "rb");
    if (!rf) {
        LOG("\n--- PART 2/3 skipped: no real MoE checkpoint at %s ---\n", rpath);
    } else {
        fclose(rf);
        LOG("\n--- PART 2: real gemma4 MoE routing mechanics ---\n");
        cce_gguf_moe* rm = NULL;
        CHECK(cce_gguf_moe_open(rpath, &rm) == CCE_OK && rm, "real MoE opens");
        cce_gguf_moe_rt* rrt = NULL;
        remove("mf_rt_real.cce");
        if (rm) {
            CHECK(cce_gguf_moe_rt_open(rm, "mf_rt_real.cce", 16, &rrt) == CCE_OK && rrt,
                  "real runtime opens (cap 16 experts of 128/layer)");
        }
        if (rrt) {
            int D = rm->n_embd, K = rm->n_expert_used;
            CHECK(rrt->gate_inp_scale[0] && rrt->down_scale[0] && rrt->pre_norm[0],
                  "gemma4 side vectors loaded (router scale, down scale, pre-norm)");
            float* xx = (float*)malloc((size_t)D * sizeof(float));
            float* oo = (float*)malloc((size_t)D * sizeof(float));
            float* oo2 = (float*)malloc((size_t)D * sizeof(float));
            g_lcg = 0x5EED5EEDu;
            for (int i = 0; i < D; i++) xx[i] = 2.0f * lcg_f();
            CHECK(cce_gguf_moe_ffn_forward(rrt, 0, xx, oo) == CCE_OK, "layer-0 forward on 128 experts");
            CHECK(rrt->expert_fetches == K, "exactly 8 of 128 experts fetched");
            {
                int distinct = 1;
                float wsum = 0;
                for (int k = 0; k < K; k++) {
                    wsum += rrt->last_weights[k];
                    for (int j = k + 1; j < K; j++)
                        if (rrt->last_experts[k] == rrt->last_experts[j]) distinct = 0;
                }
                CHECK(distinct, "selected experts are distinct");
                CHECK(fabsf(wsum - 1.0f) < 1e-4f, "routing weights renormalize to 1");
            }
            {
                int finite = 1;
                double amax = 0;
                for (int i = 0; i < D; i++) {
                    if (!isfinite(oo[i])) finite = 0;
                    if (fabs((double)oo[i]) > amax) amax = fabs((double)oo[i]);
                }
                CHECK(finite && amax > 0, "real MoE output finite and non-degenerate");
            }
            CHECK(cce_gguf_moe_ffn_forward(rrt, 0, xx, oo2) == CCE_OK &&
                  memcmp(oo, oo2, (size_t)D * sizeof(float)) == 0 &&
                  rrt->expert_fetches == K,
                  "repeat token BIT-IDENTICAL with zero new fetches");
            {
                int f0 = rrt->expert_fetches;
                CHECK(cce_gguf_moe_ffn_forward(rrt, rm->n_layer - 1, xx, oo) == CCE_OK,
                      "last-layer forward (BF16 down bank) runs");
                int finite = 1;
                for (int i = 0; i < D; i++) if (!isfinite(oo[i])) finite = 0;
                CHECK(finite && rrt->expert_fetches == f0 + K, "BF16 layer finite, 8 more fetches");
            }
            CHECK(cce_forest_resident_count(rrt->forest) <= 16,
                  "resident experts bounded by the cap (16 of 3840)");

            /* ---------- PART 3: llama.cpp cross-executable parity ---------- */
            int64_t ne_in[4], ne_lg[4], ne_tk[4], ne_w[4], ne_we[4], ne_xe[4];
            int i32a = 0, i32b = 0, i32c = 0, i32d = 0, i32e = 0, i32f = 0;
            float* d_in = (float*)read_cndt(pdir, "attn_out-0", ne_in, &i32a);
            float* d_lg = (float*)read_cndt(pdir, "ffn_moe_logits-0", ne_lg, &i32b);
            /* the full expert ordering (the "ffn_moe_topk" node is a ggml view
               whose callback fires before its argsort source runs -> stale;
               the argsort op is the real selection, top-k = first k entries) */
            int*   d_tk = (int*)  read_cndt(pdir, "ffn_moe_argsort-0", ne_tk, &i32c);
            float* d_w  = (float*)read_cndt(pdir, "ffn_moe_weights_norm-0", ne_w, &i32d);
            float* d_we = (float*)read_cndt(pdir, "ffn_moe_weighted-0", ne_we, &i32e);
            float* d_xe = (float*)read_cndt(pdir, "ffn_norm_2-0", ne_xe, &i32f);
            if (!d_in || !d_lg || !d_tk || !d_w || !d_we) {
                LOG("\n--- PART 3 skipped: no llama.cpp dumps in %s ---\n", pdir);
                LOG("    (generate: bin/moe_parity_dump <model.gguf> %s \"<prompt>\")\n", pdir);
            } else {
                int T = (int)ne_in[1], E = rm->n_expert;
                LOG("\n--- PART 3: llama.cpp parity replay (%d tokens, layer 0) ---\n", T);
                CHECK(ne_in[0] == D && ne_lg[0] == E && ne_tk[0] == E && !i32a && i32c,
                      "dump shapes match the model");
                int sel_ok = 1, run_ok = 1;
                double lg_worst = 0, w_worst = 0, out_worst = 0, xe_worst = 0;
                for (int t = 0; t < T; t++) {
                    const float* xin = d_in + (size_t)t * D;
                    if (cce_gguf_moe_ffn_forward(rrt, 0, xin, oo) != CCE_OK) { run_ok = 0; continue; }
                    /* selection: identical top-8 set */
                    if (!sets_equal(rrt->last_experts, d_tk + (size_t)t * E, K)) {
                        sel_ok = 0;
                        LOG("  token %d SELECTION MISMATCH:\n    cnet :", t);
                        for (int k = 0; k < K; k++)
                            LOG(" %d(%.4f)", rrt->last_experts[k], rrt->last_weights[k]);
                        LOG("\n    llama:");
                        for (int k = 0; k < K; k++)
                            LOG(" %d(%.4f)", d_tk[(size_t)t * E + k], d_w[(size_t)t * K + k]);
                        LOG("\n");
                    }
                    /* router logits: F32 x F32 on both sides -> tight */
                    {
                        double m = 0;
                        const float* zl = d_lg + (size_t)t * E;
                        for (int e = 0; e < E; e++) {
                            double d = fabs((double)rrt->last_logits[e] - zl[e]);
                            if (d > m) m = d;
                        }
                        if (m > lg_worst) lg_worst = m;
                    }
                    /* weights matched by expert id */
                    for (int k = 0; k < K; k++)
                        for (int j = 0; j < K; j++)
                            if (rrt->last_experts[k] == d_tk[(size_t)t * E + j]) {
                                double d = fabs((double)rrt->last_weights[k] - d_w[(size_t)t * K + j]);
                                if (d > w_worst) w_worst = d;
                            }
                    /* combined output vs sum of llama.cpp's weighted expert slices */
                    {
                        double e2 = 0, r2 = 0;
                        for (int i = 0; i < D; i++) {
                            double r = 0;
                            for (int k = 0; k < K; k++)
                                r += (double)d_we[((size_t)t * K + k) * D + i];
                            double d = (double)oo[i] - r;
                            e2 += d * d; r2 += r * r;
                        }
                        double rl = r2 > 0 ? sqrt(e2 / r2) : sqrt(e2);
                        if (rl > out_worst) out_worst = rl;
                    }
                    /* expert-input norm (pre_ffw_norm_2 semantics) */
                    if (d_xe) {
                        double ss = 0;
                        for (int i = 0; i < D; i++) ss += (double)xin[i] * xin[i];
                        float inv = 1.0f / sqrtf((float)(ss / D) + 1e-6f);
                        double m = 0;
                        for (int i = 0; i < D; i++) {
                            double v = (double)xin[i] * inv * rrt->pre_norm[0][i];
                            double d = fabs(v - d_xe[(size_t)t * D + i]);
                            if (d > m) m = d;
                        }
                        if (m > xe_worst) xe_worst = m;
                    }
                }
                LOG("  worst over %d tokens: logits dmax=%.3g  weights dmax=%.3g  out relL2=%.3g  xe dmax=%.3g\n",
                    T, lg_worst, w_worst, out_worst, xe_worst);
                CHECK(run_ok, "replay forwards run on all tokens");
                CHECK(sel_ok, "PARITY: top-8 expert selection IDENTICAL to llama.cpp on every token");
                CHECK(lg_worst < 5e-3, "PARITY: router logits match llama.cpp (F32 fp-noise tolerance)");
                CHECK(w_worst < 1e-3, "PARITY: normalized routing weights match llama.cpp");
                CHECK(out_worst < 5e-2,
                      "PARITY: combined expert output within quant-noise tolerance of llama.cpp");
                CHECK(xe_worst < 1e-3, "PARITY: expert-input norm (pre_ffw_norm_2) semantics correct");
            }
            free(d_in); free(d_lg); free(d_tk); free(d_w); free(d_we); free(d_xe);
            free(xx); free(oo); free(oo2);
        }
        if (rrt) cce_gguf_moe_rt_free(rrt);
        if (rm) cce_gguf_moe_free(rm);
        remove("mf_rt_real.cce");
    }

    LOG("\n%d checks, %d failed -> %s\n", checks, fails, fails ? "FAIL" : "OK");
    if (LOGF) fclose(LOGF);
    return fails ? 1 : 0;
}
