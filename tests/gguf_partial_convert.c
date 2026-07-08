/* gguf_partial_convert: convert linear weight tensors from a (hybrid) GGUF
 * into a CNET CCE forest archive of specialists.
 *
 *   CNET_ORACLE_INT8=1 ./bin/gguf_partial_convert in.gguf out.cce
 *
 * This extracts as many 2D ".weight" tensors as possible using the GGUF
 * helpers (dequant for Q8_0 etc. happens inside). With CNET_ORACLE_INT8=1
 * each specialist is int8-quantized on load and the FP copy is dropped
 * immediately (memory friendly for 9B+ models).
 *
 * The resulting .cce can be opened as a forest and its branches used as
 * frozen CCE primitives (compositional, router, etc.). No full model
 * forward is constructed (the source is a hybrid attn+SSM arch that the
 * current runners do not implement).
 *
 * Build: same as detect_cli (make will pick it up if renamed into place).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/cce/cce_gguf.h"
#include "../include/cce/cce_forest.h"

static void sanitize_name(const char* in, char* out, size_t cap) {
    /* turn "blk.3.attn_q.weight" into "qwythos.blk.3.attn_q.weight" or similar */
    snprintf(out, cap, "qwythos.%s", in);
    /* dots are used in other examples; keep them */
}

static int try_add_bias_for(const cce_gguf* g, const char* wname, char* bias_out, size_t cap) {
    /* If wname ends with ".weight", try the same stem + ".bias" */
    const char* suffix = strstr(wname, ".weight");
    if (!suffix) return 0;
    size_t stem_len = suffix - wname;
    if (stem_len + 6 >= cap) return 0;
    memcpy(bias_out, wname, stem_len);
    strcpy(bias_out + stem_len, ".bias");
    if (cce_gguf_find_tensor(g, bias_out) >= 0) return 1;
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <input.gguf> <output.cce>\n", argv[0]);
        fprintf(stderr, "  env CNET_ORACLE_INT8=1 recommended for large models\n");
        return 1;
    }
    const char* in_path = argv[1];
    const char* out_path = argv[2];

    cce_gguf* g = NULL;
    cce_result rc = cce_gguf_load(in_path, &g);
    if (rc != CCE_OK || !g) {
        fprintf(stderr, "error: cce_gguf_load failed (rc=%d) for %s\n", (int)rc, in_path);
        return 2;
    }

    int nt = cce_gguf_tensor_count(g);
    printf("GGUF loaded: %d tensors, arch=%s, layers=%d, hidden=%d\n",
           nt,
           cce_gguf_get_arch(g) ? cce_gguf_get_arch(g) : "unknown",
           cce_gguf_get_n_layer(g),
           cce_gguf_get_hidden_size(g));
    fflush(stdout);

    remove(out_path);
    cce_forest* forest = NULL;
    /* generous branch capacity for a full model (bumped for 400+ weights) */
    if (cce_forest_open(&forest, out_path, 2048) != CCE_OK || !forest) {
        fprintf(stderr, "error: cce_forest_open failed for %s\n", out_path);
        cce_gguf_free(g);
        return 3;
    }

    int added = 0;
    int attempted = 0;
    char brname[256];
    char biasname[256];

    /* Manifest for names (very useful for oracle-int8 runs) */
    char man_path[512];
    snprintf(man_path, sizeof(man_path), "%s.manifest.txt", out_path);
    FILE* man = fopen(man_path, "w");
    if (man) {
        fprintf(man, "# CNET partial GGUF convert manifest\n");
        fprintf(man, "# source: %s\n", in_path);
        fprintf(man, "# format: branch_name\n");
    }

    for (int i = 0; i < nt; i++) {
        cce_gguf_tensor_meta m;
        if (cce_gguf_get_tensor_meta(g, i, &m) != CCE_OK) continue;
        if (m.ndim != 2) continue;                 /* only matrices for linear specialists */
        if (strstr(m.name, "weight") == NULL) continue;

        attempted++;
        sanitize_name(m.name, brname, sizeof(brname));

        const char* bias = NULL;
        if (try_add_bias_for(g, m.name, biasname, sizeof(biasname))) {
            bias = biasname;
        }

        int r = cce_gguf_add_linear_branch(forest, g, m.name, bias, brname, 0.02f);
        if (r >= 0) {
            added++;
            if (man) fprintf(man, "%s\n", brname);
            if ((added % 5) == 0) {
                printf("  ... added %d branches (processed %d/%d tensors)\n", added, i+1, nt);
                fflush(stdout);
            }
        } else {
            /* only complain if the tensor was present (real failure, not arch variant) */
            if (cce_gguf_find_tensor(g, m.name) >= 0) {
                fprintf(stderr, "WARN: present tensor %s failed to become branch %s\n", m.name, brname);
            }
        }
    }

    /* Also opportunistically pull a few top-level special tensors if they look like heads/embed */
    const char* specials[] = {
        "token_embd.weight",
        "output.weight",
        "output_norm.weight",
        "model.embed_tokens.weight",
        NULL
    };
    for (int si=0; specials[si]; si++) {
        if (cce_gguf_find_tensor(g, specials[si]) >= 0) {
            char br[128];
            snprintf(br, sizeof(br), "qwythos.%s", specials[si]);
            if (cce_gguf_add_linear_branch(forest, g, specials[si], NULL, br, 0.0f) >= 0) {
                added++;
                if (man) fprintf(man, "%s\n", br);
            }
        }
    }

    if (man) {
        fprintf(man, "# total_added=%d attempted=%d\n", added, attempted);
        fclose(man);
    }

    // Export int8 payloads (w_q + w_scale + optional bias) to a compact sidecar.
    // This is what gives actual on-disk size reduction in CNET int8 form.
    // Only happens when the blocks have int8 data (i.e. ORACLE_INT8 path or manual quant).
    {
        bool any_int8 = false;
        for (int i = 0; i < forest->num_branches; i++) {
            cce_cascade* cas = forest->branches[i].cascade;
            if (cas && cas->num_blocks > 0 && cas->blocks[0].w_q) {
                any_int8 = true;
                break;
            }
        }
        if (any_int8) {
            char exp[512];
            snprintf(exp, sizeof(exp), "%s.int8data", out_path);
            FILE* ef = fopen(exp, "wb");
            if (ef) {
                uint32_t magic = 0x3843544e; // "NTC8"
                uint32_t ver = 1;
                uint32_t nb = forest->num_branches;
                fwrite(&magic, 4, 1, ef);
                fwrite(&ver, 4, 1, ef);
                fwrite(&nb, 4, 1, ef);
                for (int i = 0; i < nb; i++) {
                    const char* nm = forest->branches[i].name;
                    uint32_t nlen = (uint32_t)strlen(nm);
                    fwrite(&nlen, 4, 1, ef);
                    fwrite(nm, 1, nlen, ef);

                    cce_cascade* cas = forest->branches[i].cascade;
                    int32_t idim = 0, odim = 0;
                    int8_t hasb = 0;
                    if (cas && cas->num_blocks > 0) {
                        cce_block* b = &cas->blocks[0];
                        if (b->weights.ndim == 2) {
                            idim = b->weights.shape[0];
                            odim = b->weights.shape[1];
                        }
                        if (b->bias.data && b->bias.numel > 0) hasb = 1;
                    }
                    fwrite(&idim, 4, 1, ef);
                    fwrite(&odim, 4, 1, ef);
                    fwrite(&hasb, 1, 1, ef);

                    cce_block* b = (cas && cas->num_blocks > 0) ? &cas->blocks[0] : NULL;
                    if (odim > 0 && b && b->w_scale) {
                        fwrite(b->w_scale, sizeof(float), odim, ef);
                    }
                    size_t csz = (size_t)idim * odim;
                    if (csz > 0 && b && b->w_q) {
                        fwrite(b->w_q, 1, csz, ef);
                    }
                    if (hasb && odim > 0 && b && b->bias.data) {
                        fwrite(b->bias.data, sizeof(float), odim, ef);
                    }
                }
                fclose(ef);
                printf("Int8 data exported: %s\n", exp);
            }
        }
    }

    cce_forest_close(forest);
    cce_gguf_free(g);

    printf("Done: added %d linear specialists (attempted %d) -> %s\n", added, attempted, out_path);
    fflush(stdout);
    printf("Manifest: %s.manifest.txt\n", out_path);
    printf("Use with CCE forest APIs, or set CNET_ORACLE_INT8=1 before running for int8 path.\n");
    printf("Note: this is a *partial* conversion (linear weight matrices only).\n");
    printf("No full model forward / runner was built (the GGUF may use hybrid/SSM layers).\n");
    return 0;
}