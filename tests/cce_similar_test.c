/* cce_similar: SIMILAR_TO candidates + adversarial verify + evidence-gated
 * merge — "smaller" beyond byte-exact, honestly.
 *
 * Gates:
 *  - candidate scan surfaces EXACTLY the epsilon-perturbed pair (same-digest
 *    pairs excluded, unrelated same-shape pairs excluded by signature)
 *  - verify: epsilon-variant passes the probe battery, material variant fails
 *  - merge refuses unverified pairs (the evidence gate)
 *  - after merging the epsilon pair, the restored model is BIT-IDENTICAL to
 *    the canonical model (the fine-tune differed only in that matrix), and
 *    the measured end-to-end drift vs the original variant is within epsilon
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "../include/cce/cce_similar.h"
#include "../include/cce/cce_tier_runtime.h"
#include "tiny_model_fixture.h"

#ifdef _WIN32
#include <io.h>
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
#endif
    remove(dir);
}

int main(void) {
    printf("=== cce_similar: epsilon-equivalence behind evidence ===\n");

    static const int tokens[4] = { 3, 17, 9, 22 };

    tl_weights* wa = (tl_weights*)malloc(sizeof(tl_weights));
    tl_weights* we = (tl_weights*)malloc(sizeof(tl_weights));
    tl_weights* wm = (tl_weights*)malloc(sizeof(tl_weights));
    tl_gen(wa, 0);  /* canonical */
    tl_gen(we, 2);  /* epsilon fine-tune (gate.1 differs negligibly) */
    tl_gen(wm, 1);  /* material fine-tune (gate.1 differs a lot) */
    tl_write_st_dir("sim_a", wa);
    tl_write_st_dir("sim_e", we);
    tl_write_st_dir("sim_m", wm);

    wipe_store_dir("sim_store");
    cce_weight_store* s = NULL;
    CHECK(cce_weight_store_open(&s, "sim_store") == CCE_OK && s, "store opens");

    cce_anymodel *ma = NULL, *me = NULL, *mm = NULL;
    CHECK(cce_anymodel_open(&ma, "sim_a/model.safetensors") == CCE_OK && ma, "canonical opens");
    CHECK(cce_anymodel_open(&me, "sim_e/model.safetensors") == CCE_OK && me, "epsilon variant opens");
    CHECK(cce_anymodel_open(&mm, "sim_m/model.safetensors") == CCE_OK && mm, "material variant opens");
    if (!ma || !me || !mm) return 1;

    int nt, nn, nr;
    CHECK(cce_weight_store_ingest_model(s, ma, "A", "sim_a.manifest", &nt, &nn, &nr) == CCE_OK, "A ingests");
    CHECK(cce_weight_store_ingest_model(s, me, "E", "sim_e.manifest", &nt, &nn, &nr) == CCE_OK &&
          nn == 1, "epsilon variant costs one payload (the perturbed gate)");
    CHECK(cce_weight_store_ingest_model(s, mm, "M", "sim_m.manifest", &nt, &nn, &nr) == CCE_OK &&
          nn == 1, "material variant costs one payload");

    cce_spec_graph *ga = NULL, *ge = NULL, *gm = NULL;
    CHECK(cce_spec_graph_build(&ga, ma, "A") == CCE_OK && ga, "graph A");
    CHECK(cce_spec_graph_build(&ge, me, "E") == CCE_OK && ge, "graph E");
    CHECK(cce_spec_graph_build(&gm, mm, "M") == CCE_OK && gm, "graph M");
    if (!ga || !ge || !gm) return 1;

    /* 1. candidates A<->E: exactly the perturbed gate pair */
    cce_similar_pair pairs[16];
    int nc = cce_similar_candidates(ga, ge, 0.05f, pairs, 16);
    CHECK(nc == 1, "exactly one SIMILAR_TO candidate between A and its epsilon fine-tune");
    CHECK(nc == 1 && strcmp(pairs[0].name_a, "qwen2.blk.1.gate_proj") == 0 &&
          strcmp(pairs[0].name_b, "qwen2.blk.1.gate_proj") == 0,
          "the candidate is the perturbed matrix");
    printf("  candidate sig_dist = %g\n", nc == 1 ? (double)pairs[0].sig_dist : -1.0);

    /* 2. material variant is NOT a candidate at the same tau */
    {
        cce_similar_pair pm[16];
        int nm = cce_similar_candidates(ga, gm, 0.05f, pm, 16);
        CHECK(nm == 0, "materially-changed matrix does not pass the signature filter");
    }

    /* 3. adversarial verify: epsilon pair equivalent, material pair not */
    CHECK(cce_similar_verify(s, &pairs[0], 32, 1e-2f) == CCE_OK, "verify battery runs");
    printf("  epsilon pair: probes=%d max_rel_dev=%g equivalent=%d\n",
           pairs[0].probes, (double)pairs[0].max_rel_dev, pairs[0].equivalent);
    CHECK(pairs[0].probes == 32 && pairs[0].equivalent == 1 && pairs[0].max_rel_dev < 1e-2f,
          "epsilon variant verified equivalent with recorded evidence");

    cce_similar_pair hard;
    memset(&hard, 0, sizeof(hard));
    {
        int ia = cce_spec_graph_find(ga, "qwen2.blk.1.gate_proj");
        int im = cce_spec_graph_find(gm, "qwen2.blk.1.gate_proj");
        strncpy(hard.name_a, "qwen2.blk.1.gate_proj", sizeof(hard.name_a) - 1);
        strncpy(hard.name_b, "qwen2.blk.1.gate_proj", sizeof(hard.name_b) - 1);
        hard.digest_a = ga->nodes[ia].digest;
        hard.digest_b = gm->nodes[im].digest;
        CHECK(cce_similar_verify(s, &hard, 32, 1e-2f) == CCE_OK, "verify runs on material pair");
        printf("  material pair: max_rel_dev=%g equivalent=%d\n",
               (double)hard.max_rel_dev, hard.equivalent);
        CHECK(hard.equivalent == 0 && hard.max_rel_dev > 1e-2f,
              "material change fails the battery: NOT mergeable");
    }

    /* 4. the evidence gate: unverified / failed pairs cannot merge */
    {
        int rows = -1;
        CHECK(cce_similar_merge(s, &hard, "sim_m.manifest", "sim_m_merged.manifest", &rows)
              == CCE_ERR_UNSUPPORTED && rows == 0,
              "merge REFUSES a pair that failed verification");
        cce_similar_pair unverified = pairs[0];
        unverified.probes = 0;
        unverified.equivalent = 1; /* forged flag without evidence */
        CHECK(cce_similar_merge(s, &unverified, "sim_e.manifest", "sim_e_merged.manifest", &rows)
              == CCE_ERR_UNSUPPORTED,
              "merge REFUSES a pair without a probe battery behind it");
    }

    /* 5. merge the verified pair; measure everything */
    {
        int rows = 0;
        CHECK(cce_similar_merge(s, &pairs[0], "sim_e.manifest", "sim_e_merged.manifest", &rows) == CCE_OK &&
              rows == 1, "verified merge rewrites exactly one manifest row");

        float orig_e[TL_V], merged[TL_V], orig_a[TL_V];
        CHECK(cce_gguf_qwen2_forward(me->transformer, tokens, 4, orig_e, TL_V) == CCE_OK, "E forward");
        CHECK(cce_gguf_qwen2_forward(ma->transformer, tokens, 4, orig_a, TL_V) == CCE_OK, "A forward");

        cce_gguf_qwen2* mr = NULL;
        CHECK(cce_weight_store_restore_transformer(s, "sim_e_merged.manifest", "sim_merge.cce", &mr)
              == CCE_OK && mr, "merged model restores");
        if (mr) {
            CHECK(cce_gguf_qwen2_forward(mr, tokens, 4, merged, TL_V) == CCE_OK, "merged forward");
            /* E differed from A ONLY in the merged matrix -> merged == A exactly */
            float da = 0, de = 0;
            for (int v = 0; v < TL_V; v++) {
                float x = fabsf(merged[v] - orig_a[v]); if (x > da) da = x;
                float y = fabsf(merged[v] - orig_e[v]) / (1e-6f + fabsf(orig_e[v])); if (y > de) de = y;
            }
            printf("  merged vs canonical |diff|max=%g, merged vs variant rel drift=%g\n",
                   (double)da, (double)de);
            CHECK(da == 0.0f, "merged model is BIT-IDENTICAL to the canonical model");
            CHECK(de < 1e-2f, "end-to-end drift vs the original variant stays within epsilon");
            cce_gguf_qwen2_free(mr);
        }
        remove("sim_merge.cce");
    }

    cce_spec_graph_free(ga);
    cce_spec_graph_free(ge);
    cce_spec_graph_free(gm);
    cce_anymodel_free(ma);
    cce_anymodel_free(me);
    cce_anymodel_free(mm);
    cce_weight_store_close(s);
    wipe_store_dir("sim_store");
    tl_cleanup_st_dir("sim_a");
    tl_cleanup_st_dir("sim_e");
    tl_cleanup_st_dir("sim_m");
    remove("sim_a.manifest");
    remove("sim_e.manifest");
    remove("sim_m.manifest");
    remove("sim_e_merged.manifest");
    remove("sim_m_merged.manifest");
    free(wa); free(we); free(wm);

    printf("\n%d checks, %d failed -> %s\n", checks, fails, fails ? "FAIL" : "OK");
    return fails ? 1 : 0;
}
