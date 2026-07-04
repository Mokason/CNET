/* HF-llama-style safetensors loader. See include/cce/cce_st_llama.h.
 *
 * Mapping (HF torch [out,in] -> CCE [in,out], same transpose as the GGUF path):
 *   model.embed_tokens.weight                    -> tok_emb ([V,D] rows)
 *   model.layers.N.self_attn.{q,k,v,o}_proj      -> qwen2.blk.N.{q,k,v,o}_proj
 *   model.layers.N.mlp.{gate,up,down}_proj       -> qwen2.blk.N.{gate,up,down}_proj
 *   model.layers.N.input_layernorm.weight        -> attn_norm[N]
 *   model.layers.N.post_attention_layernorm      -> ffn_norm[N]
 *   model.norm.weight                            -> output_norm
 *   lm_head.weight (or tied embed)               -> qwen2.lm_head
 * Optional per-projection biases (qwen2-style HF exports) are picked up
 * automatically when present.
 */

#include "../../include/cce/cce_st_llama.h"
#include "../../include/cce/cce_safetensors.h"
#include "../../include/cce/cce_block.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- minimal config.json int extraction (flat "key": number scan) ---- */

static char* read_small_file(const char* path, size_t cap) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    char* buf = (char*)malloc(cap + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, cap, f);
    fclose(f);
    buf[n] = 0;
    return buf;
}

static int config_get_int(const char* json, const char* key, int* out) {
    char pat[96];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char* p = strstr(json, pat);
    if (!p) return 0;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == ':') p++;
    if (*p != '-' && (*p < '0' || *p > '9')) return 0;
    *out = (int)strtol(p, NULL, 10);
    return 1;
}

static int config_get_float(const char* json, const char* key, float* out) {
    char pat[96];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char* p = strstr(json, pat);
    if (!p) return 0;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == ':') p++;
    if (*p != '-' && *p != '.' && (*p < '0' || *p > '9')) return 0;
    *out = (float)strtod(p, NULL); /* handles "1e-05" scientific notation */
    return 1;
}

static void sibling_path(const char* model_path, const char* fname, char* out, size_t cap) {
    strncpy(out, model_path, cap - 1);
    out[cap - 1] = 0;
    char* last = NULL;
    for (char* q = out; *q; q++) if (*q == '/' || *q == '\\') last = q;
    if (last) snprintf(last + 1, cap - (size_t)(last + 1 - out), "%s", fname);
    else snprintf(out, cap, "%s", fname);
}

/* ---- branch helper (mirrors cce_gguf_add_linear_branch) ---- */

static cce_result st_add_linear_branch(cce_forest* forest, const cce_safetensors* st,
                                       const char* wname, const char* bname,
                                       const char* brname) {
    int wi = cce_safetensors_find(st, wname);
    if (wi < 0) return CCE_ERR_NOT_FOUND;
    cce_safetensor_meta meta;
    if (cce_safetensors_get_meta(st, wi, &meta) != CCE_OK || meta.ndim != 2) return CCE_ERR_UNSUPPORTED;
    int out_d = meta.shape[0], in_d = meta.shape[1];

    cce_cascade* cas = NULL;
    if (cce_cascade_create(&cas, 2) != CCE_OK) return CCE_ERR_OOM;
    cce_result rc = cce_cascade_add_linear_head(cas, in_d, out_d, 0.0f);
    if (rc != CCE_OK) { cce_cascade_destroy(cas); return rc; }

    cce_block* blk = &cas->blocks[cas->num_blocks - 1];
    rc = cce_safetensors_populate_block(blk, st, wname,
                                        (bname && cce_safetensors_find(st, bname) >= 0) ? bname : NULL, 1);
    if (rc != CCE_OK) { cce_cascade_destroy(cas); return rc; }

    int idx = -1;
    rc = cce_forest_add_cascade_branch(forest, cas, brname, &idx);
    if (rc != CCE_OK) { cce_cascade_destroy(cas); return rc; }
    free(cas); /* forest owns its shallow copy; add_branch zeroed our shell */
    return CCE_OK;
}

static cce_result st_load_named(const cce_safetensors* st, const char* name, cce_tensor* out) {
    int idx = cce_safetensors_find(st, name);
    if (idx < 0) return CCE_ERR_NOT_FOUND;
    return cce_safetensors_load_as_tensor(st, idx, out);
}

/* ---- loader ---- */

cce_result cce_st_llama_load(cce_gguf_qwen2** out, const char* path) {
    if (!out || !path) return CCE_ERR_INVALID_ARG;
    *out = NULL;

    cce_safetensors* st = NULL;
    cce_result rc = cce_safetensors_load(path, &st);
    if (rc != CCE_OK) return rc;

    /* head counts come from the sibling config.json; refuse rather than guess */
    char cfgpath[512];
    sibling_path(path, "config.json", cfgpath, sizeof(cfgpath));
    char* cfg = read_small_file(cfgpath, 64 * 1024);
    if (!cfg) { cce_safetensors_free(st); return CCE_ERR_NOT_FOUND; }

    int H = 0, KV = 0, max_pos = 0, bos = -1, eos = -1, head_dim_cfg = 0;
    float rope_theta = 0.0f, rms_eps = 0.0f;
    int ok_h = config_get_int(cfg, "num_attention_heads", &H);
    if (!config_get_int(cfg, "num_key_value_heads", &KV)) KV = H;
    config_get_int(cfg, "max_position_embeddings", &max_pos);
    config_get_int(cfg, "bos_token_id", &bos);
    config_get_int(cfg, "eos_token_id", &eos);
    config_get_int(cfg, "head_dim", &head_dim_cfg);
    config_get_float(cfg, "rope_theta", &rope_theta);
    config_get_float(cfg, "rms_norm_eps", &rms_eps);
    free(cfg);
    if (!ok_h || H <= 0) { cce_safetensors_free(st); return CCE_ERR_NOT_FOUND; }

    /* structures the shared forward cannot express: refuse, don't garble.
       qwen3-style per-head q/k norms and gemma-style sandwich norms would be
       silently dropped by this mapping -> wrong numerics. */
    {
        int nt = cce_safetensors_count(st);
        for (int i = 0; i < nt; i++) {
            cce_safetensor_meta mm;
            if (cce_safetensors_get_meta(st, i, &mm) != CCE_OK) continue;
            if (strstr(mm.name, "self_attn.q_norm") || strstr(mm.name, "self_attn.k_norm") ||
                strstr(mm.name, "pre_feedforward_layernorm") || strstr(mm.name, "post_feedforward_layernorm")) {
                cce_safetensors_free(st);
                return CCE_ERR_UNSUPPORTED;
            }
        }
    }

    /* layer count from the tensor names actually present */
    int L = 0;
    char name[192], bias[192], brname[128];
    for (;; L++) {
        snprintf(name, sizeof(name), "model.layers.%d.self_attn.q_proj.weight", L);
        if (cce_safetensors_find(st, name) < 0) break;
        if (L > 4096) { cce_safetensors_free(st); return CCE_ERR_UNSUPPORTED; }
    }
    if (L == 0) { cce_safetensors_free(st); return CCE_ERR_NOT_FOUND; }

    /* dims from tensor shapes (shapes never lie; config only supplies head counts) */
    cce_safetensor_meta qm, km, gm;
    cce_safetensors_get_meta(st, cce_safetensors_find(st, "model.layers.0.self_attn.q_proj.weight"), &qm);
    int ki = cce_safetensors_find(st, "model.layers.0.self_attn.k_proj.weight");
    int gi = cce_safetensors_find(st, "model.layers.0.mlp.gate_proj.weight");
    if (ki < 0 || gi < 0 || qm.ndim != 2) { cce_safetensors_free(st); return CCE_ERR_UNSUPPORTED; }
    cce_safetensors_get_meta(st, ki, &km);
    cce_safetensors_get_meta(st, gi, &gm);

    int D  = qm.shape[1];
    int HD = head_dim_cfg > 0 ? head_dim_cfg : qm.shape[0] / H;
    if (HD <= 0 || qm.shape[0] != H * HD) { cce_safetensors_free(st); return CCE_ERR_UNSUPPORTED; }
    if (km.shape[0] != KV * HD) { cce_safetensors_free(st); return CCE_ERR_UNSUPPORTED; }
    /* the shared forward assumes the q projection width equals the hidden size
       (attn buffers are [n_tokens, D]); H*head_dim != D (e.g. qwen3) would
       garble silently -> refuse */
    if (H * HD != D) { cce_safetensors_free(st); return CCE_ERR_UNSUPPORTED; }
    int FFN = gm.shape[0];

    cce_gguf_qwen2* m = (cce_gguf_qwen2*)calloc(1, sizeof(*m));
    if (!m) { cce_safetensors_free(st); return CCE_ERR_OOM; }
    m->n_layer = L; m->n_embd = D; m->n_head = H; m->n_kv_head = KV;
    m->head_dim = HD; m->feed_forward_length = FFN;
    m->bos_token_id = bos; m->eos_token_id = eos;
    m->rope_freq_base = rope_theta; /* 0 -> forward default 10000 */
    m->rms_eps = rms_eps;           /* 0 -> forward default 1e-6 */
    strncpy(m->tokenizer_model, "hf", sizeof(m->tokenizer_model) - 1);

    const char* tmpf = "st_llama_forest.cce";
    remove(tmpf);
    if (cce_forest_open(&m->forest, tmpf, 8 * L + 8) != CCE_OK) { free(m); cce_safetensors_free(st); return CCE_ERR_IO; }

    static const char* hf_proj[7]  = { "self_attn.q_proj", "self_attn.k_proj", "self_attn.v_proj",
                                       "self_attn.o_proj", "mlp.gate_proj", "mlp.up_proj", "mlp.down_proj" };
    static const char* br_proj[7]  = { "q_proj", "k_proj", "v_proj", "o_proj",
                                       "gate_proj", "up_proj", "down_proj" };
    for (int l = 0; l < L; l++) {
        for (int p = 0; p < 7; p++) {
            snprintf(name, sizeof(name), "model.layers.%d.%s.weight", l, hf_proj[p]);
            snprintf(bias, sizeof(bias), "model.layers.%d.%s.bias", l, hf_proj[p]);
            snprintf(brname, sizeof(brname), "qwen2.blk.%d.%s", l, br_proj[p]);
            rc = st_add_linear_branch(m->forest, st, name, bias, brname);
            if (rc != CCE_OK) { cce_gguf_qwen2_free(m); cce_safetensors_free(st); return rc; }
        }
    }

    const char* head_name = (cce_safetensors_find(st, "lm_head.weight") >= 0)
                            ? "lm_head.weight" : "model.embed_tokens.weight";
    rc = st_add_linear_branch(m->forest, st, head_name, NULL, "qwen2.lm_head");
    if (rc != CCE_OK) { cce_gguf_qwen2_free(m); cce_safetensors_free(st); return rc; }

    /* norms + embedding (the forward checks .data for the optional gemma extras,
       so calloc'd zero tensors mean "absent") */
    m->attn_norm = (cce_tensor*)calloc(L, sizeof(cce_tensor));
    m->ffn_norm  = (cce_tensor*)calloc(L, sizeof(cce_tensor));
    m->attn_q_norm = (cce_tensor*)calloc(L, sizeof(cce_tensor));
    m->post_attention_norm = (cce_tensor*)calloc(L, sizeof(cce_tensor));
    m->post_ffw_norm = (cce_tensor*)calloc(L, sizeof(cce_tensor));
    m->layer_output_scale = (cce_tensor*)calloc(L, sizeof(cce_tensor));
    if (!m->attn_norm || !m->ffn_norm || !m->attn_q_norm ||
        !m->post_attention_norm || !m->post_ffw_norm || !m->layer_output_scale) {
        cce_gguf_qwen2_free(m); cce_safetensors_free(st); return CCE_ERR_OOM;
    }
    for (int l = 0; l < L; l++) {
        snprintf(name, sizeof(name), "model.layers.%d.input_layernorm.weight", l);
        if (st_load_named(st, name, &m->attn_norm[l]) != CCE_OK || m->attn_norm[l].numel != (size_t)D) {
            cce_gguf_qwen2_free(m); cce_safetensors_free(st); return CCE_ERR_UNSUPPORTED;
        }
        snprintf(name, sizeof(name), "model.layers.%d.post_attention_layernorm.weight", l);
        if (st_load_named(st, name, &m->ffn_norm[l]) != CCE_OK || m->ffn_norm[l].numel != (size_t)D) {
            cce_gguf_qwen2_free(m); cce_safetensors_free(st); return CCE_ERR_UNSUPPORTED;
        }
    }

    if (st_load_named(st, "model.embed_tokens.weight", &m->tok_emb) != CCE_OK || m->tok_emb.ndim != 2) {
        cce_gguf_qwen2_free(m); cce_safetensors_free(st); return CCE_ERR_NOT_FOUND;
    }
    m->vocab_size = m->tok_emb.shape[0];
    if (st_load_named(st, "model.norm.weight", &m->output_norm) != CCE_OK ||
        m->output_norm.numel != (size_t)D) {
        cce_gguf_qwen2_free(m); cce_safetensors_free(st); return CCE_ERR_NOT_FOUND;
    }
    /* manual-head fallback tensor (used when the lm_head cascade is missing) */
    st_load_named(st, head_name, &m->output);

    m->ctx_len = max_pos;
    m->max_ctx = (m->ctx_len > 0) ? m->ctx_len : 2048;
    if (m->max_ctx > 8192) m->max_ctx = 8192; /* same cap as the GGUF loader */
    m->cur_pos = 0;

    /* uniform per-layer geometry (the forward is geometry-driven now);
       slot totals equal the old L*KV*HD layout for uniform models */
    if (cce_gguf_qwen2_geom_uniform(m) != CCE_OK) {
        cce_gguf_qwen2_free(m); cce_safetensors_free(st);
        return CCE_ERR_UNSUPPORTED;
    }
    m->k_cache = (float*)calloc((size_t)m->max_ctx * m->k_slot_floats, sizeof(float));
    m->v_cache = (float*)calloc((size_t)m->max_ctx * m->v_slot_floats, sizeof(float));
    if (!m->k_cache || !m->v_cache) { cce_gguf_qwen2_free(m); cce_safetensors_free(st); return CCE_ERR_OOM; }
    (void)KV; (void)HD;

    cce_safetensors_free(st);
    *out = m;
    return CCE_OK;
}
