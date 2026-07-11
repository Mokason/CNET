/* cce_specgraph: specialist identity + wiring graph for decomposed models.
 *
 * Gates:
 *  - node inventory + roles + dims for a tiny llama opened via cce_anymodel
 *  - digest stability (reload => identical) and cross-container identity
 *    (same weights via gguf and safetensors => identical digests)
 *  - locality of change (perturb ONE matrix => exactly one node changes)
 *  - digest-equal => fingerprint-equal invariant
 *  - DATA_FLOWS wiring counts for transformer and ssm families
 *  - flat-file save/load round-trip
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "../include/cce/cce_specgraph.h"
#include "tiny_model_fixture.h"

static int checks = 0, fails = 0;
#define CHECK(cond, msg) do { checks++; if (!(cond)) { fails++; printf("  FAIL: %s\n", msg); } } while (0)

static int graphs_digest_equal(const cce_spec_graph* a, const cce_spec_graph* b,
                               int* n_diff, char diff_name[96]) {
    if (a->n_nodes != b->n_nodes) return 0;
    int nd = 0;
    for (int i = 0; i < a->n_nodes; i++) {
        int j = cce_spec_graph_find(b, a->nodes[i].name);
        if (j < 0) return 0;
        if (a->nodes[i].digest != b->nodes[j].digest ||
            a->nodes[i].fingerprint != b->nodes[j].fingerprint) {
            nd++;
            if (diff_name) strncpy(diff_name, a->nodes[i].name, 95);
        }
    }
    if (n_diff) *n_diff = nd;
    return 1;
}

int main(void) {
    printf("=== cce_specgraph: specialist identity + wiring ===\n");

    tl_weights* w = (tl_weights*)malloc(sizeof(tl_weights));
    tl_gen(w, 0);
    tl_write_st_dir("sg_a", w);
    tl_entry ents[64];
    int n_gg = tl_entries(w, ents, 0);
    tl_write_gguf("sg_a.gguf", ents, n_gg);

    /* 1. build from anymodel (st) */
    cce_anymodel* ma = NULL;
    CHECK(cce_anymodel_open(&ma, "sg_a/model.safetensors") == CCE_OK && ma && ma->transformer,
          "tiny llama opens via anymodel");
    if (!ma) return 1;

    cce_spec_graph* ga = NULL;
    CHECK(cce_spec_graph_build(&ga, ma, "modelA") == CCE_OK && ga, "graph builds");
    if (!ga) return 1;
    CHECK(ga->n_nodes == 7 * TL_L + 1, "one node per specialist (7L+1)");
    CHECK(ga->n_edges == 7 * TL_L + 3 * (TL_L - 1) + 1, "transformer DATA_FLOWS wiring count");

    int qi = cce_spec_graph_find(ga, "qwen2.blk.0.q_proj");
    CHECK(qi >= 0 && strcmp(ga->nodes[qi].role, "q_proj") == 0, "role parsed from qualified name");
    CHECK(qi >= 0 && ga->nodes[qi].in_dim == TL_D && ga->nodes[qi].out_dim == TL_H * TL_HD,
          "node dims from cascade");
    int hi = cce_spec_graph_find(ga, "qwen2.lm_head");
    CHECK(hi >= 0 && ga->nodes[hi].in_dim == TL_D && ga->nodes[hi].out_dim == TL_V, "head node dims");
    int has_sig = 0;
    for (int s = 0; s < CCE_SPEC_SIG_DIM; s++) if (ga->nodes[qi].sig[s] != 0.0f) has_sig = 1;
    CHECK(has_sig && ga->nodes[qi].fingerprint != 0, "behavioral fingerprint ran");

    /* 2. digest stability across reloads */
    {
        cce_anymodel* m2 = NULL;
        CHECK(cce_anymodel_open(&m2, "sg_a/model.safetensors") == CCE_OK && m2, "reopens");
        cce_spec_graph* g2 = NULL;
        CHECK(cce_spec_graph_build(&g2, m2, "modelA") == CCE_OK && g2, "rebuilds");
        int nd = -1;
        CHECK(g2 && graphs_digest_equal(ga, g2, &nd, NULL) && nd == 0,
              "reload => identical digests + fingerprints");
        cce_spec_graph_free(g2);
        cce_anymodel_free(m2);
    }

    /* 3. cross-container identity: same weights via gguf */
    {
        cce_anymodel* mg = NULL;
        CHECK(cce_anymodel_open(&mg, "sg_a.gguf") == CCE_OK && mg && mg->transformer,
              "same weights open via gguf");
        cce_spec_graph* gg = NULL;
        CHECK(cce_spec_graph_build(&gg, mg, "modelA-gguf") == CCE_OK && gg, "gguf graph builds");
        int nd = -1;
        char diff[96] = {0};
        int same = gg && graphs_digest_equal(ga, gg, &nd, diff) && nd == 0;
        if (!same) {
            cce_block* sa = &ma->transformer->forest->branches[0].cascade->blocks[0];
            cce_block* sb = &mg->transformer->forest->branches[0].cascade->blocks[0];
            printf("  cross-container differences=%d last=%s first: "
                   "type=%d/%d shape=%dx%d/%dx%d w0=%.9g/%.9g b0=%.9g/%.9g\n",
                   nd, diff, (int)sa->type, (int)sb->type,
                   sa->weights.shape[0], sa->weights.shape[1],
                   sb->weights.shape[0], sb->weights.shape[1],
                   sa->weights.data[0], sb->weights.data[0],
                   sa->bias.data ? sa->bias.data[0] : 0.0f,
                   sb->bias.data ? sb->bias.data[0] : 0.0f);
        }
        CHECK(same,
              "gguf and safetensors produce IDENTICAL specialist digests");
        cce_spec_graph_free(gg);
        cce_anymodel_free(mg);
    }

    /* 4. locality: perturb exactly one matrix -> exactly one node changes */
    {
        tl_weights* wv = (tl_weights*)malloc(sizeof(tl_weights));
        tl_gen(wv, 1);
        tl_write_st_dir("sg_b", wv);
        cce_anymodel* mb = NULL;
        CHECK(cce_anymodel_open(&mb, "sg_b/model.safetensors") == CCE_OK && mb, "variant opens");
        cce_spec_graph* gb = NULL;
        CHECK(cce_spec_graph_build(&gb, mb, "modelB") == CCE_OK && gb, "variant graph builds");
        int nd = -1;
        char diff[96] = {0};
        CHECK(gb && graphs_digest_equal(ga, gb, &nd, diff) && nd == 1,
              "exactly one node differs after a one-matrix fine-tune");
        CHECK(strcmp(diff, "qwen2.blk.1.gate_proj") == 0, "the changed node is the changed matrix");

        /* digest-equal => fingerprint-equal invariant over the shared nodes */
        int inv_ok = 1;
        for (int i = 0; i < ga->n_nodes && gb; i++) {
            int j = cce_spec_graph_find(gb, ga->nodes[i].name);
            if (j >= 0 && ga->nodes[i].digest == gb->nodes[j].digest &&
                ga->nodes[i].fingerprint != gb->nodes[j].fingerprint) inv_ok = 0;
        }
        CHECK(inv_ok, "digest-equal => fingerprint-equal (checkable invariant)");
        cce_spec_graph_free(gb);
        cce_anymodel_free(mb);
        tl_cleanup_st_dir("sg_b");
        free(wv);
    }

    /* 5. save/load round-trip */
    {
        CHECK(cce_spec_graph_save(ga, "sg_a.graph") == CCE_OK, "graph saves");
        cce_spec_graph* gl = NULL;
        CHECK(cce_spec_graph_load(&gl, "sg_a.graph") == CCE_OK && gl, "graph loads");
        int nd = -1;
        CHECK(gl && gl->n_nodes == ga->n_nodes && gl->n_edges == ga->n_edges &&
              graphs_digest_equal(ga, gl, &nd, NULL) && nd == 0 &&
              strcmp(gl->model_name, "modelA") == 0,
              "round-trip preserves nodes, edges, identities");
        cce_spec_graph_free(gl);
        remove("sg_a.graph");
    }

    /* 6. ssm family wiring */
    {
        tm_weights* tw = (tm_weights*)malloc(sizeof(tm_weights));
        tm_gen(tw);
        tm_write_st("sg_mamba.safetensors", tw);
        cce_anymodel* ms = NULL;
        CHECK(cce_anymodel_open(&ms, "sg_mamba.safetensors") == CCE_OK && ms && ms->ssm,
              "tiny mamba opens via anymodel");
        cce_spec_graph* gs = NULL;
        CHECK(ms && cce_spec_graph_build(&gs, ms, "mambaA") == CCE_OK && gs, "ssm graph builds");
        if (gs) {
            CHECK(gs->n_nodes == 4 * TM_L + 1, "ssm node inventory (4L+1)");
            CHECK(gs->n_edges == 5 * TM_L + (TM_L - 1) + 1, "ssm DATA_FLOWS wiring count");
            cce_spec_graph_free(gs);
        }
        if (ms) cce_anymodel_free(ms);
        remove("sg_mamba.safetensors");
        free(tw);
    }

    cce_spec_graph_free(ga);
    cce_anymodel_free(ma);
    tl_cleanup_st_dir("sg_a");
    remove("sg_a.gguf");
    free(w);

    printf("\n%d checks, %d failed -> %s\n", checks, fails, fails ? "FAIL" : "OK");
    return fails ? 1 : 0;
}
