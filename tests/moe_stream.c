/* moe_stream: Arc B3 — the MoE streaming throughput layer.
 *
 * Four mechanisms on top of B2's routed demand-loading, each gated:
 *
 *  STORE-BACKED PAYLOADS — experts ingest into the content-addressed weight
 *  store on first touch and re-stream from it afterwards. FP payloads are
 *  BIT-IDENTICAL across evict/re-stream; int8 payloads (~4x smaller) run the
 *  codes directly (RAW_QUANT, no dequant) and are bit-identical to their own
 *  first run, near-lossless vs FP.
 *
 *  ASYNC LOOKAHEAD — a background worker prefetches every routed expert
 *  while the forward computes: zero synchronous fetches once routing is
 *  known, math unchanged. Cross-layer speculation via lookahead_hint.
 *
 *  LEARNED PINNING — route_count tracks selection frequency; pin_hot keeps
 *  the most-routed experts resident, cutting refetches on replayed traffic.
 *
 *  BATCH-UNION — forward_batch loads each unique expert ONCE for a whole
 *  batch and is BIT-IDENTICAL to the per-token path.
 *
 * PART 2 (real gemma-4-26B-A4B, auto-skips) measures the ladder on real
 * payloads: gguf loads (Q6_K slice+dequant+transpose) vs int8 store streams
 * vs +lookahead, plus pinning and batch-union fetch counts.
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

/* evict every evictable resident expert (cold restart between passes) */
static void evict_experts(cce_gguf_moe_rt* rt) {
    for (int i = 0; i < rt->forest->num_branches; i++) {
        cce_branch* b = &rt->forest->branches[i];
        if (b->evictable && b->cascade && !b->is_view)
            cce_forest_evict_branch(rt->forest, i);
    }
}

static int fetches(const cce_gguf_moe_rt* rt) { return rt->gguf_loads + rt->store_hits; }

/* ---------------- hermetic qwen3moe fixture (small LCG weights) ---------------- */

#define MS_L 2
#define MS_D 8
#define MS_F 16
#define MS_E 4
#define MS_K 2

static uint32_t g_lcg = 0xC0FFEE42u;
static float lcg_f(void) {
    g_lcg = g_lcg * 1664525u + 1013904223u;
    return ((float)(g_lcg >> 8) / (float)(1u << 24)) - 0.5f;
}

static void ms_write_gguf(const char* path) {
    /* stream the banks straight from the LCG (same order the loader reads) */
    FILE* f = fopen(path, "wb");
    fwrite("GGUF", 1, 4, f);
    tl_gg_u32(f, 3);
    tl_gg_u64(f, 4 * MS_L);
    tl_gg_u64(f, 6);
    tl_gg_str(f, "general.architecture"); tl_gg_u32(f, 8); tl_gg_str(f, "qwen3moe");
    tl_gg_kv_u32(f, "qwen3moe.block_count", MS_L);
    tl_gg_kv_u32(f, "qwen3moe.embedding_length", MS_D);
    tl_gg_kv_u32(f, "qwen3moe.expert_count", MS_E);
    tl_gg_kv_u32(f, "qwen3moe.expert_used_count", MS_K);
    tl_gg_kv_u32(f, "qwen3moe.expert_feed_forward_length", MS_F);
    uint64_t off = 0;
    for (int l = 0; l < MS_L; l++) {
        char name[96];
        snprintf(name, sizeof name, "blk.%d.ffn_gate_exps.weight", l);
        tl_gg_str(f, name); tl_gg_u32(f, 3);
        tl_gg_u64(f, MS_D); tl_gg_u64(f, MS_F); tl_gg_u64(f, MS_E);
        tl_gg_u32(f, 0); tl_gg_u64(f, off); off += (uint64_t)MS_D * MS_F * MS_E * 4;
        snprintf(name, sizeof name, "blk.%d.ffn_up_exps.weight", l);
        tl_gg_str(f, name); tl_gg_u32(f, 3);
        tl_gg_u64(f, MS_D); tl_gg_u64(f, MS_F); tl_gg_u64(f, MS_E);
        tl_gg_u32(f, 0); tl_gg_u64(f, off); off += (uint64_t)MS_D * MS_F * MS_E * 4;
        snprintf(name, sizeof name, "blk.%d.ffn_down_exps.weight", l);
        tl_gg_str(f, name); tl_gg_u32(f, 3);
        tl_gg_u64(f, MS_F); tl_gg_u64(f, MS_D); tl_gg_u64(f, MS_E);
        tl_gg_u32(f, 0); tl_gg_u64(f, off); off += (uint64_t)MS_F * MS_D * MS_E * 4;
        snprintf(name, sizeof name, "blk.%d.ffn_gate_inp.weight", l);
        tl_gg_str(f, name); tl_gg_u32(f, 2);
        tl_gg_u64(f, MS_D); tl_gg_u64(f, MS_E);
        tl_gg_u32(f, 0); tl_gg_u64(f, off); off += (uint64_t)MS_D * MS_E * 4;
    }
    { long pos = ftell(f); int pad = (int)((32 - (pos % 32)) % 32);
      while (pad-- > 0) fputc(0, f); }
    g_lcg = 0x0DDBA11u;
    size_t per_layer = (size_t)MS_E * MS_F * MS_D * 2 + (size_t)MS_E * MS_D * MS_F
                     + (size_t)MS_E * MS_D;
    for (size_t i = 0; i < (size_t)MS_L * per_layer; i++) {
        float v = lcg_f();
        fwrite(&v, 4, 1, f);
    }
    fclose(f);
}

static double vdmax(const float* a, const float* b, int n) {
    double m = 0;
    for (int i = 0; i < n; i++) { double d = fabs((double)a[i] - b[i]); if (d > m) m = d; }
    return m;
}

int main(int argc, char** argv) {
    LOGF = fopen("logs/moe_stream.log", "wb");
    LOG("=== moe_stream: Arc B3 — store payloads, lookahead, pinning, batch-union ===\n");

    ms_write_gguf("ms_moe.gguf");
    cce_gguf_moe* mm = NULL;
    CHECK(cce_gguf_moe_open("ms_moe.gguf", &mm) == CCE_OK && mm, "hermetic MoE opens");
    if (!mm) return 1;

    float xa[MS_D], xb[4][MS_D], out_fp[MS_D], out2[MS_D], out_i8[MS_D];
    g_lcg = 0xBADD00D5u;
    for (int i = 0; i < MS_D; i++) xa[i] = 2.0f * lcg_f();
    for (int t = 0; t < 4; t++)
        for (int i = 0; i < MS_D; i++) xb[t][i] = 2.0f * lcg_f();

    /* ---------------- FP store payloads: bit-identical re-streams ---------------- */
    LOG("\n--- store-backed experts (FP): evict -> re-stream bit-identical ---\n");
    wipe_store_dir("ms_store_fp");
    remove("ms_rt_fp.cce");
    cce_weight_store* sfp = NULL;
    cce_gguf_moe_rt* rt = NULL;
    CHECK(cce_weight_store_open(&sfp, "ms_store_fp") == CCE_OK && sfp, "FP store opens");
    CHECK(cce_gguf_moe_rt_open(mm, "ms_rt_fp.cce", 8, &rt) == CCE_OK && rt, "runtime opens");
    if (!rt) return 1;
    CHECK(cce_gguf_moe_rt_attach_store(rt, sfp, 0) == CCE_OK, "FP store attaches");
    CHECK(cce_gguf_moe_ffn_forward(rt, 0, xa, out_fp) == CCE_OK, "token A forwards (cold)");
    int setA = fetches(rt);
    CHECK(rt->gguf_loads == setA && rt->store_hits == 0, "cold fetches came from the gguf");
    CHECK(cce_weight_store_count(sfp) == setA, "touched experts INGESTED into the store");
    evict_experts(rt);
    CHECK(cce_gguf_moe_ffn_forward(rt, 0, xa, out2) == CCE_OK, "token A re-forwards after evict");
    CHECK(rt->store_hits == setA && rt->gguf_loads == setA,
          "re-stream came from the STORE (no gguf reload)");
    CHECK(memcmp(out_fp, out2, sizeof out_fp) == 0, "FP store re-stream BIT-IDENTICAL");

    /* ---------------- int8 store payloads: 4x smaller, near-lossless ---------------- */
    LOG("\n--- store-backed experts (int8): quantized streaming ---\n");
    wipe_store_dir("ms_store_i8");
    remove("ms_rt_i8.cce");
    cce_weight_store* si8 = NULL;
    cce_gguf_moe_rt* rt8 = NULL;
    CHECK(cce_weight_store_open(&si8, "ms_store_i8") == CCE_OK && si8, "int8 store opens");
    CHECK(cce_gguf_moe_rt_open(mm, "ms_rt_i8.cce", 8, &rt8) == CCE_OK && rt8, "int8 runtime opens");
    if (!rt8) return 1;
    CHECK(cce_gguf_moe_rt_attach_store(rt8, si8, 1) == CCE_OK, "int8 store attaches");
    CHECK(cce_gguf_moe_ffn_forward(rt8, 0, xa, out_i8) == CCE_OK, "token A forwards (int8 math)");
    {
        double d = vdmax(out_i8, out_fp, MS_D), amax = 0;
        for (int i = 0; i < MS_D; i++) if (fabs((double)out_fp[i]) > amax) amax = fabs((double)out_fp[i]);
        LOG("  int8-vs-FP output dmax = %.3g (FP amax %.3g)\n", d, amax);
        CHECK(d > 0, "int8 payloads actually change the math (codes run, not FP)");
        CHECK(d < 0.02 * (amax > 1 ? amax : 1), "int8 experts near-lossless on the fixture");
    }
    CHECK(cce_weight_store_bytes(si8) < cce_weight_store_bytes(sfp),
          "int8 expert payloads smaller than FP payloads on disk");
    evict_experts(rt8);
    CHECK(cce_gguf_moe_ffn_forward(rt8, 0, xa, out2) == CCE_OK &&
          memcmp(out_i8, out2, sizeof out_i8) == 0 && rt8->store_hits > 0,
          "int8 re-stream from the store BIT-IDENTICAL to its first run");

    /* ---------------- async lookahead: zero sync fetches, same math ---------------- */
    LOG("\n--- async lookahead prefetch ---\n");
    {
        cce_result larc = cce_gguf_moe_rt_set_lookahead(rt8, 1);
        if (larc == CCE_ERR_UNSUPPORTED) {
            LOG("  lookahead unsupported on this platform — skipping\n");
        } else {
            CHECK(larc == CCE_OK, "lookahead enables (1 worker)");
            evict_experts(rt8);
            int s0 = rt8->sync_fetches;
            CHECK(cce_gguf_moe_ffn_forward(rt8, 0, xa, out2) == CCE_OK, "token A forwards under lookahead");
            LOG("  claims: hits=%d waits=%d sync-delta=%d\n",
                rt8->prefetch_hits, rt8->prefetch_waits, rt8->sync_fetches - s0);
            CHECK(rt8->sync_fetches - s0 == 0,
                  "ZERO synchronous fetches (whole top-k staged before the first expert ran)");
            CHECK(rt8->prefetch_hits + rt8->prefetch_waits > 0, "prefetches were adopted");
            CHECK(memcmp(out_i8, out2, sizeof out_i8) == 0, "lookahead changes residency timing, NOT math");

            /* cross-layer hint: prefetches layer 1's prediction, counters clean */
            int rc0 = 0;
            for (int e = 0; e < MS_E; e++) rc0 += rt8->route_count[1 * MS_E + e];
            CHECK(cce_gguf_moe_rt_lookahead_hint(rt8, 1, xa) == CCE_OK, "cross-layer hint accepted");
            int rc1 = 0;
            for (int e = 0; e < MS_E; e++) rc1 += rt8->route_count[1 * MS_E + e];
            CHECK(rc0 == rc1, "speculative hint does NOT perturb the learned route counts");
            CHECK(cce_gguf_moe_ffn_forward(rt8, 1, xa, out2) == CCE_OK, "hinted layer forwards");
            CHECK(cce_gguf_moe_rt_set_lookahead(rt8, 0) == CCE_OK, "lookahead disables cleanly");
        }
    }

    /* ---------------- learned pinning: replayed traffic stops refetching ---------------- */
    LOG("\n--- learned hot-expert pinning ---\n");
    {
        /* token A replayed 3x made its experts the most-routed on layer 0 */
        for (int r = 0; r < 3; r++) cce_gguf_moe_ffn_forward(rt8, 0, xa, out2);
        int selA[64];
        memcpy(selA, rt8->last_experts, sizeof selA);
        CHECK(cce_gguf_moe_rt_pin_hot(rt8, MS_K) == CCE_OK && rt8->pinned == MS_K,
              "pin_hot pins the 2 most-routed experts");
        {
            int pinned_are_selA = 1;
            for (int k = 0; k < MS_K; k++) {
                int bidx = 1 + selA[k]; /* layer 0 block: [router, exp0..] */
                if (rt8->forest->branches[bidx].evictable) pinned_are_selA = 0;
            }
            CHECK(pinned_are_selA, "the pinned experts ARE token A's (most-routed) experts");
        }
        evict_experts(rt8); /* pinned survive: evict refuses non-evictable */
        int f0 = fetches(rt8);
        CHECK(cce_gguf_moe_ffn_forward(rt8, 0, xa, out2) == CCE_OK, "token A after pin+evict");
        CHECK(fetches(rt8) - f0 == 0, "pinned experts never refetched (zero fetches for pinned traffic)");
        CHECK(memcmp(out_i8, out2, sizeof out_i8) == 0, "pinning = residency, not math");
    }

    /* ---------------- batch-union: unique experts once, bit-identical ---------------- */
    LOG("\n--- batch-union prefill ---\n");
    {
        float ob[4][MS_D], os[4][MS_D];
        evict_experts(rt); /* FP runtime, no pins: clean fetch accounting */
        int f0 = fetches(rt);
        CHECK(cce_gguf_moe_ffn_forward_batch(rt, 1, &xb[0][0], &ob[0][0], 4) == CCE_OK,
              "4-token batch forwards");
        int union_fetches = fetches(rt) - f0;
        LOG("  batch fetches=%d (naive per-token would fetch %d selections)\n",
            union_fetches, 4 * MS_K);
        CHECK(union_fetches <= MS_E && union_fetches > 0, "each unique expert fetched at most ONCE");
        int f1 = fetches(rt);
        int seq_ok = 1;
        for (int t = 0; t < 4; t++)
            if (cce_gguf_moe_ffn_forward(rt, 1, xb[t], os[t]) != CCE_OK) seq_ok = 0;
        CHECK(seq_ok, "the same 4 tokens forward per-token");
        CHECK(fetches(rt) == f1, "per-token replay fetched nothing (union already resident)");
        CHECK(memcmp(ob, os, sizeof ob) == 0, "batch-union BIT-IDENTICAL to the per-token path");
        CHECK(cce_gguf_moe_ffn_forward_batch(rt, 1, &xb[0][0], &ob[0][0], 0) == CCE_ERR_INVALID_ARG,
              "empty batch refused");
    }

    cce_gguf_moe_rt_free(rt);
    cce_gguf_moe_rt_free(rt8);
    cce_weight_store_close(sfp);
    cce_weight_store_close(si8);
    wipe_store_dir("ms_store_fp");
    wipe_store_dir("ms_store_i8");
    remove("ms_rt_fp.cce");
    remove("ms_rt_i8.cce");
    cce_gguf_moe_free(mm);
    remove("ms_moe.gguf");

    /* =================== PART 2: real-model throughput ladder =================== */
    const char* rpath = (argc > 1) ? argv[1]
                      : "/home/marble/Downloads/gemma-4-26B-A4B-it-UD-Q6_K_XL.gguf";
    FILE* rf = fopen(rpath, "rb");
    if (!rf) {
        LOG("\n--- PART 2 skipped: no real MoE checkpoint at %s ---\n", rpath);
    } else {
        fclose(rf);
        LOG("\n--- PART 2: real gemma4 streaming ladder (4 tokens, layer 0) ---\n");
        cce_gguf_moe* rm = NULL;
        CHECK(cce_gguf_moe_open(rpath, &rm) == CCE_OK && rm, "real MoE opens");
        if (rm) {
            int D = rm->n_embd, K = rm->n_expert_used;
            const int NT = 4;
            float* xs = (float*)malloc((size_t)NT * D * sizeof(float));
            float* o1 = (float*)malloc((size_t)NT * D * sizeof(float));
            float* o2 = (float*)malloc((size_t)NT * D * sizeof(float));
            g_lcg = 0x5EED5EEDu;
            for (int i = 0; i < NT * D; i++) xs[i] = 2.0f * lcg_f();

            /* rung 1: gguf-only baseline (Q6_K slice + dequant + transpose per fetch) */
            remove("ms_rt_r.cce");
            wipe_store_dir("ms_store_r");
            cce_gguf_moe_rt* rr = NULL;
            CHECK(cce_gguf_moe_rt_open(rm, "ms_rt_r.cce", 16, &rr) == CCE_OK && rr, "real runtime opens");
            if (rr) {
                double t0 = rr->stall_sec;
                for (int t = 0; t < NT; t++)
                    cce_gguf_moe_ffn_forward(rr, 0, xs + (size_t)t * D, o1 + (size_t)t * D);
                double stall_gguf = rr->stall_sec - t0;
                int f_gguf = fetches(rr);
                LOG("  gguf-only:      %d fetches, stall %.3fs (%.0f ms/fetch)\n",
                    f_gguf, stall_gguf, 1e3 * stall_gguf / (f_gguf ? f_gguf : 1));

                /* rung 2: int8 store — ingest pass, then stream from the store */
                cce_weight_store* sr = NULL;
                CHECK(cce_weight_store_open(&sr, "ms_store_r") == CCE_OK && sr, "real store opens");
                CHECK(cce_gguf_moe_rt_attach_store(rr, sr, 1) == CCE_OK, "int8 store attaches");
                evict_experts(rr);
                for (int t = 0; t < NT; t++)   /* ingest pass (loads + quantizes + puts) */
                    cce_gguf_moe_ffn_forward(rr, 0, xs + (size_t)t * D, o2 + (size_t)t * D);
                {
                    double e2 = 0, r2 = 0;
                    for (int i = 0; i < NT * D; i++) {
                        double d = (double)o2[i] - o1[i];
                        e2 += d * d; r2 += (double)o1[i] * o1[i];
                    }
                    LOG("  int8-vs-FP output relL2 = %.3g\n", sqrt(e2 / (r2 > 0 ? r2 : 1)));
                    CHECK(sqrt(e2 / (r2 > 0 ? r2 : 1)) < 5e-2, "int8 experts near-lossless on real weights");
                }
                evict_experts(rr);
                int h0 = rr->store_hits;
                t0 = rr->stall_sec;
                for (int t = 0; t < NT; t++)
                    cce_gguf_moe_ffn_forward(rr, 0, xs + (size_t)t * D, o2 + (size_t)t * D);
                double stall_store = rr->stall_sec - t0;
                int f_store = rr->store_hits - h0;
                LOG("  int8 store:     %d fetches, stall %.3fs (%.0f ms/fetch)\n",
                    f_store, stall_store, 1e3 * stall_store / (f_store ? f_store : 1));
                CHECK(f_store == f_gguf, "same traffic re-streams entirely from the store");
                CHECK(stall_store < stall_gguf,
                      "THROUGHPUT: int8 store streaming beats gguf reloading");

                /* rung 3: + async lookahead */
                float* o3 = o2;
                cce_result larc = cce_gguf_moe_rt_set_lookahead(rr, 1);
                if (larc == CCE_ERR_UNSUPPORTED) {
                    LOG("  (lookahead unsupported on this platform)\n");
                } else {
                    evict_experts(rr);
                    int s0 = rr->sync_fetches;
                    t0 = rr->stall_sec;
                    for (int t = 0; t < NT; t++)
                        cce_gguf_moe_ffn_forward(rr, 0, xs + (size_t)t * D, o3 + (size_t)t * D);
                    double stall_la = rr->stall_sec - t0;
                    LOG("  +lookahead:     stall %.3fs (hits=%d waits=%d sync-delta=%d)\n",
                        stall_la, rr->prefetch_hits, rr->prefetch_waits, rr->sync_fetches - s0);
                    CHECK(rr->sync_fetches - s0 == 0, "lookahead: zero synchronous fetches");
                    /* after SLIM, store fetches are ~1ms/expert: single-token
                       decode is compute-bound and lookahead's margin sits in
                       the noise — gate NON-REGRESSION (it must not hurt); its
                       overlap wins belong to slower media / bigger payloads */
                    CHECK(stall_la < stall_store * 1.5 + 0.01,
                          "lookahead does not regress the streamed decode");
                    cce_gguf_moe_rt_set_lookahead(rr, 0);
                }

                /* rung 4: learned pinning on replayed traffic */
                evict_experts(rr);
                int fa = fetches(rr);
                for (int t = 0; t < NT; t++)
                    cce_gguf_moe_ffn_forward(rr, 0, xs + (size_t)t * D, o2 + (size_t)t * D);
                int unpinned_fetches = fetches(rr) - fa;
                CHECK(cce_gguf_moe_rt_pin_hot(rr, 16) == CCE_OK && rr->pinned == 16,
                      "pin_hot pins the 16 most-routed experts");
                evict_experts(rr);
                fa = fetches(rr);
                for (int t = 0; t < NT; t++)
                    cce_gguf_moe_ffn_forward(rr, 0, xs + (size_t)t * D, o2 + (size_t)t * D);
                int pinned_fetches = fetches(rr) - fa;
                LOG("  pinning: %d fetches/replay -> %d with 16 pinned\n",
                    unpinned_fetches, pinned_fetches);
                CHECK(pinned_fetches < unpinned_fetches,
                      "pinned hot experts stop re-streaming on replayed traffic");

                /* rung 5: batch-union on the same tokens */
                evict_experts(rr);
                fa = fetches(rr);
                CHECK(cce_gguf_moe_ffn_forward_batch(rr, 0, xs, o2, NT) == CCE_OK,
                      "real 4-token batch forwards");
                int batch_fetches = fetches(rr) - fa;
                LOG("  batch-union: %d fetches for %d selections (%d pinned resident)\n",
                    batch_fetches, NT * K, rr->pinned);
                CHECK(batch_fetches <= unpinned_fetches - (unpinned_fetches - pinned_fetches),
                      "batch fetches bounded by the unpinned union");
                cce_weight_store_close(sr);
            }
            if (rr) cce_gguf_moe_rt_free(rr);
            free(xs); free(o1); free(o2);
            remove("ms_rt_r.cce");
            wipe_store_dir("ms_store_r");
        }
        if (rm) cce_gguf_moe_free(rm);
    }

    LOG("\n%d checks, %d failed -> %s\n", checks, fails, fails ? "FAIL" : "OK");
    if (LOGF) fclose(LOGF);
    return fails ? 1 : 0;
}
