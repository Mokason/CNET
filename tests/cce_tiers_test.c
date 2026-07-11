/* cce_tier_runtime: bounded-RAM inference from the weight store.
 *
 * Gates:
 *  - with hot_cap << specialist count, forward logits are BIT-IDENTICAL to
 *    the all-resident model (streaming changes residency, never math)
 *  - resident evictable cascades never exceed the cap (high-water)
 *  - one forward = one streaming pass (rehydrations == specialist count,
 *    no thrash within a pass; repeat forwards re-stream as documented)
 *  - detach rehydrates everything and the model runs standalone
 *  - attach refuses a manifest that cannot restore every branch
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "../include/cce/cce_weight_store.h"
#include "../include/cce/cce_tier_runtime.h"
#include "../include/cce/cce_forest.h"
#include "tiny_model_fixture.h"

#ifdef _WIN32
#include <io.h>
#else
#include <dirent.h>
#endif

static int checks = 0, fails = 0;
#define CHECK(cond, msg) do { checks++; if (!(cond)) { fails++; printf("  FAIL: %s\n", msg); } } while (0)

static void wipe_store_dir(const char* dir) {
#ifdef _WIN32
    struct _finddata_t fd;
    char pat[600], path[700];
    snprintf(pat, sizeof(pat), "%s/*.spec", dir);
    intptr_t h = _findfirst(pat, &fd);
    if (h != -1) {
        do {
            snprintf(path, sizeof(path), "%s/%s", dir, fd.name);
            remove(path);
        } while (_findnext(h, &fd) == 0);
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

int main(void) {
    printf("=== cce_tier_runtime: bounded RAM, identical math ===\n");

    const int N_SPECS = 7 * TL_L + 1; /* 15 */
    static const int tokens[4] = { 3, 17, 9, 22 };

    tl_weights* w = (tl_weights*)malloc(sizeof(tl_weights));
    tl_gen(w, 0);
    tl_write_st_dir("tr_a", w);

    wipe_store_dir("tr_store");
    cce_weight_store* s = NULL;
    CHECK(cce_weight_store_open(&s, "tr_store") == CCE_OK && s, "store opens");

    /* baseline: two consecutive forwards on the plain model (kv state grows) */
    float base1[TL_V], base2[TL_V];
    int nt, nn, nr;
    {
        cce_anymodel* ma = NULL;
        CHECK(cce_anymodel_open(&ma, "tr_a/model.safetensors") == CCE_OK && ma, "model opens");
        if (!ma) return 1;
        CHECK(cce_weight_store_ingest_model(s, ma, "modelA", "tr_a.manifest", &nt, &nn, &nr) == CCE_OK,
              "ingests");
        CHECK(cce_gguf_qwen2_forward(ma->transformer, tokens, 4, base1, TL_V) == CCE_OK, "baseline fwd 1");
        CHECK(cce_gguf_qwen2_forward(ma->transformer, tokens, 4, base2, TL_V) == CCE_OK, "baseline fwd 2");
        cce_anymodel_free(ma);
    }

    /* tiered model: restore, attach with a small cap, start cold */
    cce_gguf_qwen2* m = NULL;
    CHECK(cce_weight_store_restore_transformer(s, "tr_a.manifest", "tr_restore.cce", &m) == CCE_OK && m,
          "restores from store");
    if (!m) return 1;

    cce_tier_runtime* rt = NULL;
    CHECK(cce_tier_attach(&rt, m, s, "tr_a.manifest", 8) == CCE_OK && rt, "tier runtime attaches");
    if (!rt) return 1;
    CHECK(cce_tier_evict_all(rt) == CCE_OK, "evict all");
    CHECK(cce_tier_resident(rt) == 0, "starts fully cold");

    /* 1. streaming forward == baseline, bit for bit */
    {
        float lg[TL_V];
        CHECK(cce_gguf_qwen2_forward(m, tokens, 4, lg, TL_V) == CCE_OK, "streaming forward runs");
        float dmax = 0;
        for (int v = 0; v < TL_V; v++) { float d = fabsf(lg[v] - base1[v]); if (d > dmax) dmax = d; }
        CHECK(dmax == 0.0f, "capped streaming logits BIT-IDENTICAL to all-resident");
        CHECK(cce_tier_high_water(rt) <= 8, "resident specialists never exceeded the cap");
        CHECK(cce_tier_rehydrations(rt) == N_SPECS,
              "one streaming pass: each specialist fetched exactly once");
        CHECK(cce_tier_resident(rt) <= 8, "post-forward residency within cap");
        printf("  pass 1: high_water=%d rehydrations=%d resident=%d (of %d specs)\n",
               cce_tier_high_water(rt), cce_tier_rehydrations(rt), cce_tier_resident(rt), N_SPECS);
    }

    /* 2. second forward: kv continuity intact, re-streams evicted layers */
    {
        float lg[TL_V];
        CHECK(cce_gguf_qwen2_forward(m, tokens, 4, lg, TL_V) == CCE_OK, "second streaming forward");
        float dmax = 0;
        for (int v = 0; v < TL_V; v++) { float d = fabsf(lg[v] - base2[v]); if (d > dmax) dmax = d; }
        CHECK(dmax == 0.0f, "second pass also bit-identical (state + streaming compose)");
        CHECK(cce_tier_high_water(rt) <= 8, "cap still respected");
    }

    /* 3. detach: model becomes fully resident and standalone */
    {
        CHECK(cce_tier_detach(rt) == CCE_OK, "detach rehydrates + releases");
        rt = NULL;
        int resident = 0;
        for (int i = 0; i < m->forest->num_branches; i++)
            if (m->forest->branches[i].cascade) resident++;
        CHECK(resident == N_SPECS, "all specialists resident after detach");
        float lg[TL_V];
        CHECK(cce_gguf_qwen2_forward(m, tokens, 4, lg, TL_V) == CCE_OK,
              "detached model runs standalone");
    }
    cce_gguf_qwen2_free(m);
    m = NULL;
    remove("tr_restore.cce");

    /* 4. attach refuses an uncoverable manifest (honesty gate) */
    {
        cce_anymodel* ma = NULL;
        CHECK(cce_anymodel_open(&ma, "tr_a/model.safetensors") == CCE_OK && ma, "model reopens");
        FILE* bad = fopen("tr_bad.manifest", "wb");
        fprintf(bad, "CNET_MANIFEST v1\nmodel bad\nfamily transformer\n"
                     "spec qwen2.blk.0.q_proj 0123456789abcdef\nend\n");
        fclose(bad);
        cce_tier_runtime* rb = NULL;
        CHECK(cce_tier_attach(&rb, ma->transformer, s, "tr_bad.manifest", 8) == CCE_ERR_UNSUPPORTED &&
              rb == NULL,
              "manifest that cannot restore every branch is refused");
        cce_anymodel_free(ma);
        remove("tr_bad.manifest");
    }

    cce_weight_store_close(s);
    wipe_store_dir("tr_store");
    tl_cleanup_st_dir("tr_a");
    remove("tr_a.manifest");
    free(w);

    printf("\n%d checks, %d failed -> %s\n", checks, fails, fails ? "FAIL" : "OK");
    return fails ? 1 : 0;
}
