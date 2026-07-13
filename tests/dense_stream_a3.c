/* dense_stream_a3: async readahead + learned hot-pinning (Arc A3, throughput).
 *
 * A1/A1.5 proved bounded-RAM bit-identical streaming; this slice makes it
 * FAST without changing the math:
 *   - the tier runtime RECORDS the specialist fetch order of the first cold
 *     pass (dense forwards are deterministic);
 *   - with readahead on, a background worker prefetches the next `depth`
 *     payloads in that order while the forward computes, wrapping around the
 *     sequence end (pass N's tail prefetches pass N+1's head — decode regime);
 *   - learned pinning keeps the most-fetched specialists resident, cutting
 *     rehydrations per pass by the pin count.
 *
 * Gates (all deterministic — the hit/wait split is timing-dependent, their
 * SUM is not):
 *   - pass 1 (learning) is honestly synchronous: 15 sync fetches, 0 prefetches.
 *   - pass 2 (after evict_all, which flushes staging — cold is COLD): exactly
 *     1 sync fetch (the pass head), 14 prefetch adoptions.
 *   - pass 3 (no evict between passes): 0 sync fetches — wrap-around staged
 *     the new pass's head during the previous tail.
 *   - pin_hot(4): rehydrations/pass drop to 11; pinned never refetched.
 *   - EVERY pass bit-identical to a synchronous twin driven through the
 *     IDENTICAL call sequence (KV state advances; readahead changes residency
 *     timing, never math).
 *   - residency high-water <= cap; staged payloads <= depth (RAM honesty).
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

static float logit_dmax(const float* a, const float* b, int n) {
    float m = 0;
    for (int v = 0; v < n; v++) { float d = fabsf(a[v] - b[v]); if (d > m) m = d; }
    return m;
}

int main(void) {
    LOGF = fopen("logs/dense_stream_a3.log", "wb");

    LOG("=== dense_stream_a3: async readahead + learned hot-pinning ===\n");

    const int N_SPECS = 7 * TL_L + 1;   /* 15 */
    const int HOT_CAP = 8;
    const int RA_DEPTH = 4;
    const int N_PIN = 4;
    static const int tokens[4] = { 3, 17, 9, 22 };

    tl_weights* w = (tl_weights*)malloc(sizeof(tl_weights));
    tl_gen(w, 0);
    tl_write_st_dir("dsa3_a", w);

    /* one int8 store both twins stream from */
    wipe_store_dir("dsa3_store");
    cce_weight_store* s = NULL;
    CHECK(cce_weight_store_open(&s, "dsa3_store") == CCE_OK && s, "store opens");
    {
        cce_anymodel* ma = NULL;
        CHECK(cce_anymodel_open(&ma, "dsa3_a/model.safetensors") == CCE_OK && ma, "model opens");
        if (!ma) return 1;
        CHECK(cce_gguf_qwen2_quantize_int8(ma->transformer) > 0, "int8 PTQ quantizes specialists");
        int nt = 0, nn = 0, nr = 0;
        CHECK(cce_weight_store_ingest_model(s, ma, "modelQ", "dsa3.manifest", &nt, &nn, &nr) == CCE_OK,
              "int8 model ingests");
        cce_anymodel_free(ma);
    }

    /* two restored twins from the SAME store: sync baseline vs readahead */
    cce_gguf_qwen2 *mS = NULL, *mR = NULL;
    CHECK(cce_weight_store_restore_transformer(s, "dsa3.manifest", "dsa3_rs.cce", &mS) == CCE_OK && mS,
          "sync twin restores");
    CHECK(cce_weight_store_restore_transformer(s, "dsa3.manifest", "dsa3_rr.cce", &mR) == CCE_OK && mR,
          "readahead twin restores");
    if (!mS || !mR) return 1;

    cce_tier_runtime *rtS = NULL, *rtR = NULL;
    CHECK(cce_tier_attach(&rtS, mS, s, "dsa3.manifest", HOT_CAP) == CCE_OK && rtS, "sync twin attaches");
    CHECK(cce_tier_attach(&rtR, mR, s, "dsa3.manifest", HOT_CAP) == CCE_OK && rtR, "readahead twin attaches");
    if (!rtS || !rtR) return 1;
    CHECK(cce_tier_evict_all(rtS) == CCE_OK && cce_tier_evict_all(rtR) == CCE_OK, "both start cold");

    cce_result ra_rc = cce_tier_set_readahead(rtR, RA_DEPTH);
    if (ra_rc == CCE_ERR_UNSUPPORTED) {
        LOG("  readahead unsupported on this platform (no threads) — nothing to gate\n");
        LOG("\n%d checks, %d failed -> OK (skipped)\n", checks, fails);
        return 0;
    }
    CHECK(ra_rc == CCE_OK, "readahead enables (depth 4) before anything is learned");

    float lgS[TL_V], lgR[TL_V];

    /* ---------- pass 1: the learning pass is honestly synchronous ---------- */
    LOG("\n--- pass 1 (cold): sequence learning, no prefetch yet ---\n");
    CHECK(cce_gguf_qwen2_forward(mS, tokens, 4, lgS, TL_V) == CCE_OK, "sync twin pass 1");
    CHECK(cce_gguf_qwen2_forward(mR, tokens, 4, lgR, TL_V) == CCE_OK, "readahead twin pass 1");
    CHECK(logit_dmax(lgS, lgR, TL_V) == 0.0f, "pass 1 logits BIT-IDENTICAL (learning changes nothing)");
    CHECK(cce_tier_seq_learned(rtR) == 1, "fetch sequence learned after one full pass");
    CHECK(cce_tier_sync_fetches(rtR) == N_SPECS, "pass 1 was fully synchronous (15 sync fetches)");
    CHECK(cce_tier_prefetch_hits(rtR) + cce_tier_prefetch_waits(rtR) == 0, "pass 1 adopted no prefetches");

    /* ---------- pass 2: evict_all flushes staging; head is the only sync fetch ---------- */
    LOG("\n--- pass 2 (after evict_all): readahead streams the pass ---\n");
    CHECK(cce_tier_evict_all(rtS) == CCE_OK && cce_tier_evict_all(rtR) == CCE_OK,
          "evict all (also flushes staged prefetches: cold is COLD)");
    int sync0 = cce_tier_sync_fetches(rtR);
    int pf0 = cce_tier_prefetch_hits(rtR) + cce_tier_prefetch_waits(rtR);
    CHECK(cce_gguf_qwen2_forward(mS, tokens, 4, lgS, TL_V) == CCE_OK, "sync twin pass 2");
    CHECK(cce_gguf_qwen2_forward(mR, tokens, 4, lgR, TL_V) == CCE_OK, "readahead twin pass 2");
    int sync_d = cce_tier_sync_fetches(rtR) - sync0;
    int pf_d = cce_tier_prefetch_hits(rtR) + cce_tier_prefetch_waits(rtR) - pf0;
    LOG("  pass 2: sync=%d prefetch-adopted=%d (hits=%d waits=%d total)\n",
        sync_d, pf_d, cce_tier_prefetch_hits(rtR), cce_tier_prefetch_waits(rtR));
    CHECK(logit_dmax(lgS, lgR, TL_V) == 0.0f, "pass 2 logits BIT-IDENTICAL under readahead");
    CHECK(sync_d == 1, "pass 2: exactly ONE sync fetch (the pass head; staging was flushed)");
    CHECK(pf_d == N_SPECS - 1, "pass 2: the other 14 specialists adopted from prefetch");

    /* ---------- pass 3: wrap-around — pass 2's tail staged pass 3's head ---------- */
    LOG("\n--- pass 3 (no evict): wrap-around readahead, zero sync fetches ---\n");
    sync0 = cce_tier_sync_fetches(rtR);
    pf0 = cce_tier_prefetch_hits(rtR) + cce_tier_prefetch_waits(rtR);
    int rh0 = cce_tier_rehydrations(rtR);
    CHECK(cce_gguf_qwen2_forward(mS, tokens, 4, lgS, TL_V) == CCE_OK, "sync twin pass 3");
    CHECK(cce_gguf_qwen2_forward(mR, tokens, 4, lgR, TL_V) == CCE_OK, "readahead twin pass 3");
    sync_d = cce_tier_sync_fetches(rtR) - sync0;
    pf_d = cce_tier_prefetch_hits(rtR) + cce_tier_prefetch_waits(rtR) - pf0;
    LOG("  pass 3: sync=%d prefetch-adopted=%d rehydrations=%d\n",
        sync_d, pf_d, cce_tier_rehydrations(rtR) - rh0);
    CHECK(logit_dmax(lgS, lgR, TL_V) == 0.0f, "pass 3 logits BIT-IDENTICAL under wrap-around readahead");
    CHECK(sync_d == 0, "pass 3: ZERO sync fetches (wrap-around staged the pass head)");
    CHECK(pf_d == N_SPECS, "pass 3: every specialist adopted from prefetch");
    CHECK(cce_tier_rehydrations(rtR) - rh0 == N_SPECS, "pass 3 still re-streams all 15 (cap < specs)");

    /* ---------- residency + RAM honesty under readahead ---------- */
    LOG("\n--- RAM honesty: cap held, staging bounded ---\n");
    LOG("  high_water=%d (cap %d)  staged_high_water=%d (depth %d)\n",
        cce_tier_high_water(rtR), HOT_CAP, cce_tier_staged_high_water(rtR), RA_DEPTH);
    CHECK(cce_tier_high_water(rtR) <= HOT_CAP, "resident specialists never exceeded the cap");
    CHECK(cce_tier_staged_high_water(rtR) <= RA_DEPTH, "staged payloads never exceeded the depth");
    LOG("  effective peak = cap %d + staged %d = %d payloads (of %d)\n",
        HOT_CAP, cce_tier_staged_high_water(rtR),
        HOT_CAP + cce_tier_staged_high_water(rtR), N_SPECS);

    /* ---------- learned pinning: the most-fetched stay resident ---------- */
    LOG("\n--- pass 4: pin_hot(%d) cuts rehydrations per pass ---\n", N_PIN);
    rh0 = cce_tier_rehydrations(rtR);
    CHECK(cce_tier_pin_hot(rtR, N_PIN) == CCE_OK, "pin_hot pins the 4 most-fetched specialists");
    CHECK(cce_tier_pinned(rtR) == N_PIN, "pinned count reported");
    int pin_rh = cce_tier_rehydrations(rtR) - rh0;
    LOG("  pinning rehydrated %d evicted picks (they were cold)\n", pin_rh);
    rh0 = cce_tier_rehydrations(rtR);
    CHECK(cce_gguf_qwen2_forward(mS, tokens, 4, lgS, TL_V) == CCE_OK, "sync twin pass 4");
    CHECK(cce_gguf_qwen2_forward(mR, tokens, 4, lgR, TL_V) == CCE_OK, "readahead+pinned twin pass 4");
    int rh_d = cce_tier_rehydrations(rtR) - rh0;
    LOG("  pass 4 rehydrations = %d (was %d/pass unpinned)\n", rh_d, N_SPECS);
    CHECK(logit_dmax(lgS, lgR, TL_V) == 0.0f, "pass 4 logits BIT-IDENTICAL under pinning (twin is unpinned)");
    CHECK(rh_d == N_SPECS - N_PIN, "pinned specialists are never refetched (11 rehydrations/pass)");
    CHECK(cce_tier_high_water(rtR) <= HOT_CAP, "cap still held with pinning (pinned live OUTSIDE the pool)");
    LOG("  effective RAM = cap %d + pinned %d + staged %d\n",
        HOT_CAP, cce_tier_pinned(rtR), cce_tier_staged_high_water(rtR));

    /* ---------- stall accounting (report; too small to assert on a fixture) ---------- */
    LOG("\n--- stall accounting (fixture-scale, report only) ---\n");
    LOG("  sync twin stall      = %.6fs over %d fetches\n",
        cce_tier_stall_seconds(rtS), cce_tier_sync_fetches(rtS));
    LOG("  readahead twin stall = %.6fs (sync=%d hits=%d waits=%d)\n",
        cce_tier_stall_seconds(rtR), cce_tier_sync_fetches(rtR),
        cce_tier_prefetch_hits(rtR), cce_tier_prefetch_waits(rtR));
    LOG("  (the throughput assertion runs on REAL payloads in dense_stream_real)\n");

    /* ---------- teardown: detach stops the worker and unpins ---------- */
    CHECK(cce_tier_detach(rtR) == CCE_OK, "readahead twin detaches (worker stopped, pins cleared)");
    CHECK(cce_tier_detach(rtS) == CCE_OK, "sync twin detaches");
    cce_gguf_qwen2_free(mS);
    cce_gguf_qwen2_free(mR);
    remove("dsa3_rs.cce");
    remove("dsa3_rr.cce");
    cce_weight_store_close(s);
    wipe_store_dir("dsa3_store");
    tl_cleanup_st_dir("dsa3_a");
    remove("dsa3.manifest");
    free(w);

    LOG("\n%d checks, %d failed -> %s\n", checks, fails, fails ? "FAIL" : "OK");
    if (LOGF) fclose(LOGF);
    return fails ? 1 : 0;
}
