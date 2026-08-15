/* merge_family: the fine-tune-family merge pipeline (model-merge scope M0,
 * docs/superpowers/specs/2026-07-03-model-merge-scope.md).
 *
 * Enterprise shape: one base model + N fine-tunes served from ONE
 * content-addressed store. Composes three already-gated mechanisms into a
 * single pipeline and gates the pipeline itself:
 *
 *   1. ingest base + 3 fine-tunes -> one store, 4 manifests; each fine-tune
 *      costs exactly its changed matrices (storage accounting vs naive 4x)
 *   2. every family member restores + tier-streams from the shared store
 *      BIT-identical to its standalone self, under one bounded HOT cap
 *   3. epsilon fine-tune layers merge to the canonical digest ONLY behind
 *      the adversarial probe battery; material ones are refused — inside
 *      the same pipeline, not just in isolation
 *
 * Hermetic: tiny-llama fixture, no model files, no network.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "../include/cce/cce_weight_store.h"
#include "../include/cce/cce_specgraph.h"
#include "../include/cce/cce_similar.h"
#include "../include/cce/cce_tier_runtime.h"
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

static const int TOKENS[4] = { 3, 17, 9, 22 };

/* standalone forward straight from a safetensors dir */
static int standalone_logits(const char* dir, float* out /*[TL_V]*/) {
    char path[512];
    snprintf(path, sizeof(path), "%s/model.safetensors", dir);
    cce_anymodel* m = NULL;
    if (cce_anymodel_open(&m, path) != CCE_OK || !m || !m->transformer) return 0;
    int ok = cce_gguf_qwen2_forward(m->transformer, TOKENS, 4, out, TL_V) == CCE_OK;
    cce_anymodel_free(m);
    return ok;
}

/* restore from manifest, tier-stream under hot_cap, forward; returns high water */
static int streamed_logits(cce_weight_store* s, const char* manifest,
                           int hot_cap, float* out, int* high_water) {
    cce_gguf_qwen2* mr = NULL;
    char tmp[64];
    snprintf(tmp, sizeof(tmp), "mf_restore.cce");
    if (cce_weight_store_restore_transformer(s, manifest, tmp, &mr) != CCE_OK || !mr) return 0;
    cce_tier_runtime* rt = NULL;
    int ok = 0;
    if (cce_tier_attach(&rt, mr, s, manifest, hot_cap) == CCE_OK && rt) {
        cce_tier_evict_all(rt);            /* start cold: force streaming */
        ok = cce_gguf_qwen2_forward(mr, TOKENS, 4, out, TL_V) == CCE_OK;
        *high_water = cce_tier_high_water(rt);
        cce_tier_detach(rt);
    }
    cce_gguf_qwen2_free(mr);
    remove(tmp);
    return ok;
}

int main(void) {
    printf("=== merge_family: base + N fine-tunes from ONE store (M0) ===\n");

    /* ---- the family: base + 3 fine-tunes touching DIFFERENT matrices ---- */
    tl_weights* base = (tl_weights*)malloc(sizeof(tl_weights));
    tl_weights* ft_mat = (tl_weights*)malloc(sizeof(tl_weights));   /* material gate[1] */
    tl_weights* ft_eps = (tl_weights*)malloc(sizeof(tl_weights));   /* epsilon gate[1] */
    tl_weights* ft_two = (tl_weights*)malloc(sizeof(tl_weights));   /* material q[0] + down[1] */
    tl_gen(base, 0);
    tl_gen(ft_mat, 1);
    tl_gen(ft_eps, 2);
    tl_gen(ft_two, 0);
    for (int i = 0; i < TL_H * TL_HD; i++)
        for (int j = 0; j < TL_D; j++)
            ft_two->q_w[0][i][j] += 0.02f * (float)(i + 2 * j + 1);
    for (int i = 0; i < TL_D; i++)
        for (int j = 0; j < TL_FFN; j++)
            ft_two->down_w[1][i][j] -= 0.015f * (float)(i + j + 2);

    tl_write_st_dir("mf_base", base);
    tl_write_st_dir("mf_mat", ft_mat);
    tl_write_st_dir("mf_eps", ft_eps);
    tl_write_st_dir("mf_two", ft_two);

    wipe_store_dir("mf_store");
    cce_weight_store* s = NULL;
    CHECK(cce_weight_store_open(&s, "mf_store") == CCE_OK && s, "family store opens");
    if (!s) return 1;

    /* ---- 1. ingest the family; each member costs its changed matrices ---- */
    size_t bytes_base = 0;
    {
        struct { const char* dir; const char* name; const char* manifest; int expect_new; }
        fam[4] = {
            { "mf_base", "base",   "mf_base.manifest", -1 },  /* -1: first ingest */
            { "mf_mat",  "ft-mat", "mf_mat.manifest",   1 },
            { "mf_eps",  "ft-eps", "mf_eps.manifest",   1 },
            { "mf_two",  "ft-two", "mf_two.manifest",   2 },
        };
        for (int k = 0; k < 4; k++) {
            char path[512];
            snprintf(path, sizeof(path), "%s/model.safetensors", fam[k].dir);
            cce_anymodel* m = NULL;
            CHECK(cce_anymodel_open(&m, path) == CCE_OK && m, "family member opens");
            if (!m) return 1;
            int nt = 0, nn = 0, nr = 0;
            CHECK(cce_weight_store_ingest_model(s, m, fam[k].name, fam[k].manifest,
                                                &nt, &nn, &nr) == CCE_OK, "member ingests");
            if (fam[k].expect_new >= 0) {
                char msg[128];
                snprintf(msg, sizeof(msg), "%s costs exactly %d new payload(s)",
                         fam[k].name, fam[k].expect_new);
                CHECK(nn == fam[k].expect_new, msg);
            } else {
                bytes_base = cce_weight_store_bytes(s);
            }
            cce_anymodel_free(m);
        }
    }

    /* ---- 2. storage accounting: family store vs naive one-store-per-model ---- */
    {
        size_t family = cce_weight_store_bytes(s);
        size_t naive = 4 * bytes_base;   /* each member standalone is ~a base-sized store */
        double ratio = (double)family / (double)naive;
        printf("  storage: naive 4x separate = %zu B | family store = %zu B  (%.1f%%, %.2fx smaller)\n",
               naive, family, 100.0 * ratio, (double)naive / (double)family);
        CHECK(family > bytes_base, "fine-tune diffs do cost something (honest accounting)");
        CHECK(ratio < 0.35, "family store under 35% of naive 4x storage");
    }

    /* ---- 3. every member serves from the ONE store, bit-identical, bounded RAM ---- */
    {
        const char* dirs[4] = { "mf_base", "mf_mat", "mf_eps", "mf_two" };
        const char* mans[4] = { "mf_base.manifest", "mf_mat.manifest",
                                "mf_eps.manifest", "mf_two.manifest" };
        for (int k = 0; k < 4; k++) {
            float solo[TL_V], streamed[TL_V];
            int hw = 0;
            CHECK(standalone_logits(dirs[k], solo), "standalone forward");
            CHECK(streamed_logits(s, mans[k], 8, streamed, &hw), "store-streamed forward");
            float dmax = 0;
            for (int v = 0; v < TL_V; v++) {
                float d = fabsf(streamed[v] - solo[v]);
                if (d > dmax) dmax = d;
            }
            char msg[128];
            snprintf(msg, sizeof(msg), "%s: streamed == standalone (bit-identical, hot_cap 8, high water %d)",
                     dirs[k], hw);
            CHECK(dmax == 0.0f && hw <= 8, msg);
        }
    }

    /* ---- 4. epsilon member merges to canonical BEHIND the battery ---- */
    {
        char pa[512], pe[512];
        snprintf(pa, sizeof(pa), "mf_base/model.safetensors");
        snprintf(pe, sizeof(pe), "mf_eps/model.safetensors");
        cce_anymodel *ma = NULL, *me = NULL;
        cce_anymodel_open(&ma, pa);
        cce_anymodel_open(&me, pe);
        cce_spec_graph *ga = NULL, *ge = NULL;
        CHECK(ma && me &&
              cce_spec_graph_build(&ga, ma, "base") == CCE_OK &&
              cce_spec_graph_build(&ge, me, "ft-eps") == CCE_OK, "family graphs build");

        cce_similar_pair pairs[16];
        int nc = ga && ge ? cce_similar_candidates(ga, ge, 0.05f, pairs, 16) : 0;
        CHECK(nc == 1, "one SIMILAR_TO candidate between base and the epsilon member");
        if (nc == 1) {
            CHECK(cce_similar_verify(s, &pairs[0], 32, 1e-2f) == CCE_OK &&
                  pairs[0].equivalent == 1,
                  "probe battery verifies the epsilon pair equivalent");
            printf("  epsilon pair: probes=%d max_rel_dev=%g\n",
                   pairs[0].probes, (double)pairs[0].max_rel_dev);

            int rows = 0;
            CHECK(cce_similar_merge(s, &pairs[0], "mf_eps.manifest",
                                    "mf_eps_merged.manifest", &rows) == CCE_OK && rows == 1,
                  "verified pair merges the epsilon manifest row to canonical");

            /* the merged member's ONLY difference was the merged matrix ->
               its store-served forward is now bit-identical to BASE */
            float merged[TL_V], base_l[TL_V];
            int hw = 0;
            CHECK(standalone_logits("mf_base", base_l), "base standalone forward");
            CHECK(streamed_logits(s, "mf_eps_merged.manifest", 8, merged, &hw),
                  "merged member streams from the store");
            float dmax = 0;
            for (int v = 0; v < TL_V; v++) {
                float d = fabsf(merged[v] - base_l[v]);
                if (d > dmax) dmax = d;
            }
            CHECK(dmax == 0.0f, "post-merge member is bit-identical to the canonical base");
        }

        /* refusal stays law inside the pipeline: the material member neither
           passes the signature filter NOR merges when forced */
        {
            char pm[512];
            snprintf(pm, sizeof(pm), "mf_mat/model.safetensors");
            cce_anymodel* mm = NULL;
            cce_anymodel_open(&mm, pm);
            cce_spec_graph* gm = NULL;
            CHECK(mm && cce_spec_graph_build(&gm, mm, "ft-mat") == CCE_OK, "material graph builds");
            cce_similar_pair pmx[16];
            int nm = ga && gm ? cce_similar_candidates(ga, gm, 0.05f, pmx, 16) : 1;
            CHECK(nm == 0, "material member is NOT a merge candidate");
            if (ga && gm) {
                int ia = cce_spec_graph_find(ga, "qwen2.blk.1.gate_proj");
                int im = cce_spec_graph_find(gm, "qwen2.blk.1.gate_proj");
                if (ia >= 0 && im >= 0) {
                    cce_similar_pair hard;
                    memset(&hard, 0, sizeof hard);
                    strncpy(hard.name_a, "qwen2.blk.1.gate_proj", sizeof(hard.name_a) - 1);
                    strncpy(hard.name_b, "qwen2.blk.1.gate_proj", sizeof(hard.name_b) - 1);
                    hard.digest_a = ga->nodes[ia].digest;
                    hard.digest_b = gm->nodes[im].digest;
                    CHECK(cce_similar_verify(s, &hard, 32, 1e-2f) == CCE_OK &&
                          hard.equivalent == 0,
                          "battery rejects the material pair");
                    int rows = -1;
                    CHECK(cce_similar_merge(s, &hard, "mf_mat.manifest",
                                            "mf_mat_merged.manifest", &rows) != CCE_OK,
                          "unverified merge is REFUSED inside the pipeline");
                }
            }
            if (gm) cce_spec_graph_free(gm);
            if (mm) cce_anymodel_free(mm);
        }

        if (ga) cce_spec_graph_free(ga);
        if (ge) cce_spec_graph_free(ge);
        if (ma) cce_anymodel_free(ma);
        if (me) cce_anymodel_free(me);
    }

    /* ---- cleanup ---- */
    cce_weight_store_close(s);
    wipe_store_dir("mf_store");
    tl_cleanup_st_dir("mf_base");
    tl_cleanup_st_dir("mf_mat");
    tl_cleanup_st_dir("mf_eps");
    tl_cleanup_st_dir("mf_two");
    remove("mf_base.manifest"); remove("mf_mat.manifest");
    remove("mf_eps.manifest");  remove("mf_two.manifest");
    remove("mf_eps_merged.manifest"); remove("mf_mat_merged.manifest");
    free(base); free(ft_mat); free(ft_eps); free(ft_two);

    printf("\n%d checks, %d failed -> %s\n", checks, fails, fails ? "FAIL" : "OK");
    return fails ? 1 : 0;
}
