/* Real-checkpoint loading for the QAT trainer — the ONLY translation unit
 * that binds it to $(CCE). Split out of cce_transformer_qat.c so the trainer
 * core, and therefore the hermetic gradcheck gate, links without safetensors
 * / gguf / kv_page and builds on any platform.
 *
 * Moved verbatim; public API unchanged (same header, same symbol).
 * Spec: docs/superpowers/specs/2026-08-16-qat-modern-block-design.md section 3
 */

#include "../../include/cce/cce_transformer_qat.h"
#include "../../include/cce/cce_transformer_qat_internal.h"
#include "../../include/cce/cce_safetensors.h"   /* cce_supra_decomposed, head_fp, forest */

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

/* Copy `in*out` FP values from src into the shadow of a trainer P, asserting the
   element count matches (STE moments/grad left at their fresh-zero state). */
static cce_result copy_into_P(P* p, const float* src, int in, int out) {
    if (!p || !src) return CCE_ERR_INVALID_ARG;
    if (p->in != in || p->out != out) return CCE_ERR_INVALID_ARG;
    memcpy(p->w, src, (size_t)in * out * sizeof(float));
    return CCE_OK;
}

/* The pure-linear projection for a named branch lives in the LAST block of its
   cascade (single-block LINEAR_HEAD cascades in the decomposed model), weights
   stored [in][out] row-major — same orientation as the trainer's P. */
static const cce_block* supra_last_block(cce_forest* f, const char* name) {
    cce_cascade* c = cce_forest_get_resident(f, name);
    if (!c || c->num_blocks < 1) return NULL;
    return &c->blocks[c->num_blocks - 1];
}

cce_result cce_transformer_qat_load_decomposed(cce_transformer_qat* t, void* decomposed_model) {
    if (!t || !decomposed_model) return CCE_ERR_INVALID_ARG;
    cce_supra_decomposed* m = (cce_supra_decomposed*)decomposed_model;
    const cce_transformer_qat_config* c = &t->cfg;
    int L = c->n_layer, D = c->n_embd, M = c->mlp_hidden, V = c->vocab, B = c->block_size;

    /* dims must line up exactly (direct memcpy, no reshape) */
    if (m->n_layer != L || m->n_embd != D || m->n_head != c->n_head ||
        m->vocab_size != V || m->block_size != B) return CCE_ERR_INVALID_ARG;
    if (!m->forest) return CCE_ERR_INVALID_ARG;

    /* mlp_hidden must match the real up-projection out width */
    {
        const cce_block* up0 = supra_last_block(m->forest, "gpt.block0.mlp_up");
        if (!up0 || up0->weights.ndim != 2 || up0->weights.shape[1] != M)
            return CCE_ERR_INVALID_ARG;
    }

    /* --- embeddings (tok_emb QAT-able, pos_emb frozen FP) --- */
    if (!m->tok_emb.data || m->tok_emb.numel != (size_t)V * D) return CCE_ERR_INVALID_ARG;
    if (!m->pos_emb.data || m->pos_emb.numel != (size_t)B * D) return CCE_ERR_INVALID_ARG;
    if (copy_into_P(&t->tok_emb, m->tok_emb.data, V, D) != CCE_OK) return CCE_ERR_INVALID_ARG;
    if (copy_into_P(&t->pos_emb, m->pos_emb.data, B, D) != CCE_OK) return CCE_ERR_INVALID_ARG;

    /* --- per-layer LayerNorm params + linear projections --- */
    for (int l = 0; l < L; ++l) {
        char name[128];
        /* LN1 / LN2 (weights + biases), each [D] -> P[1][D] */
        if (m->ln1_w[l].numel != (size_t)D || m->ln1_b[l].numel != (size_t)D ||
            m->ln2_w[l].numel != (size_t)D || m->ln2_b[l].numel != (size_t)D)
            return CCE_ERR_INVALID_ARG;
        if (copy_into_P(&t->ln1_w[l], m->ln1_w[l].data, 1, D) != CCE_OK) return CCE_ERR_INVALID_ARG;
        if (copy_into_P(&t->ln1_b[l], m->ln1_b[l].data, 1, D) != CCE_OK) return CCE_ERR_INVALID_ARG;
        if (copy_into_P(&t->ln2_w[l], m->ln2_w[l].data, 1, D) != CCE_OK) return CCE_ERR_INVALID_ARG;
        if (copy_into_P(&t->ln2_b[l], m->ln2_b[l].data, 1, D) != CCE_OK) return CCE_ERR_INVALID_ARG;

        /* fused QKV [D][3D] + bias [3D] */
        snprintf(name, sizeof(name), "gpt.block%d.qkv", l);
        const cce_block* b = supra_last_block(m->forest, name);
        if (!b || b->weights.ndim != 2 || !b->bias.data) return CCE_ERR_INVALID_ARG;
        if (copy_into_P(&t->qkv_w[l], b->weights.data, b->weights.shape[0], b->weights.shape[1]) != CCE_OK ||
            copy_into_P(&t->qkv_b[l], b->bias.data, 1, b->weights.shape[1]) != CCE_OK) return CCE_ERR_INVALID_ARG;

        /* attn output projection [D][D] + bias [D] */
        snprintf(name, sizeof(name), "gpt.block%d.attn_proj", l);
        b = supra_last_block(m->forest, name);
        if (!b || b->weights.ndim != 2 || !b->bias.data) return CCE_ERR_INVALID_ARG;
        if (copy_into_P(&t->proj_w[l], b->weights.data, b->weights.shape[0], b->weights.shape[1]) != CCE_OK ||
            copy_into_P(&t->proj_b[l], b->bias.data, 1, b->weights.shape[1]) != CCE_OK) return CCE_ERR_INVALID_ARG;

        /* MLP up [D][M] + bias [M] */
        snprintf(name, sizeof(name), "gpt.block%d.mlp_up", l);
        b = supra_last_block(m->forest, name);
        if (!b || b->weights.ndim != 2 || !b->bias.data) return CCE_ERR_INVALID_ARG;
        if (copy_into_P(&t->up_w[l], b->weights.data, b->weights.shape[0], b->weights.shape[1]) != CCE_OK ||
            copy_into_P(&t->up_b[l], b->bias.data, 1, b->weights.shape[1]) != CCE_OK) return CCE_ERR_INVALID_ARG;

        /* MLP down [M][D] + bias [D] */
        snprintf(name, sizeof(name), "gpt.block%d.mlp_down", l);
        b = supra_last_block(m->forest, name);
        if (!b || b->weights.ndim != 2 || !b->bias.data) return CCE_ERR_INVALID_ARG;
        if (copy_into_P(&t->down_w[l], b->weights.data, b->weights.shape[0], b->weights.shape[1]) != CCE_OK ||
            copy_into_P(&t->down_b[l], b->bias.data, 1, b->weights.shape[1]) != CCE_OK) return CCE_ERR_INVALID_ARG;
    }

    /* --- final LayerNorm --- */
    if (m->ln_f_w.numel != (size_t)D || m->ln_f_b.numel != (size_t)D) return CCE_ERR_INVALID_ARG;
    if (copy_into_P(&t->lnf_w, m->ln_f_w.data, 1, D) != CCE_OK) return CCE_ERR_INVALID_ARG;
    if (copy_into_P(&t->lnf_b, m->ln_f_b.data, 1, D) != CCE_OK) return CCE_ERR_INVALID_ARG;

    /* --- logits head [D][V] + bias [V] (borrowed via the public FP accessor) --- */
    {
        const float* hw = NULL; const float* hb = NULL; int hin = 0, hout = 0;
        if (cce_supra_head_fp(m, &hw, &hb, &hin, &hout) != CCE_OK) return CCE_ERR_INVALID_ARG;
        if (hin != D || hout != V || !hw) return CCE_ERR_INVALID_ARG;
        if (copy_into_P(&t->head_w, hw, D, V) != CCE_OK) return CCE_ERR_INVALID_ARG;
        if (hb) { if (copy_into_P(&t->head_b, hb, 1, V) != CCE_OK) return CCE_ERR_INVALID_ARG; }
        else    { memset(t->head_b.w, 0, (size_t)V * sizeof(float)); }
    }

    return CCE_OK;
}
