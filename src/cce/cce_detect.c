/* Universal pre-run model structure detection. See include/cce/cce_detect.h.
 *
 * Detection philosophy: structure over labels. The container format comes
 * from magic bytes; the architecture family comes from which tensors are
 * actually present (attn_q/k/v/out vs fused qkv vs ssm_*), because arch
 * strings lie less often than they are missing, but shapes never lie.
 */

#include "../../include/cce/cce_detect.h"
#include "../../include/cce/cce_st_llama.h"
#include "../../include/cce/cce_qgkp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SUPRA_PACK_MAGIC_V   0x4B505553u /* 'S','U','P','K' (matches cce_safetensors.c) */
#define QWEN2_PACK_MAGIC_V   0x504b4751u /* 'Q','G','K','P' (matches cce_gguf.c) */

/* ---- names ---- */

const char* cce_detect_format_name(cce_container_format f) {
    switch (f) {
        case CCE_FMT_GGUF:        return "gguf";
        case CCE_FMT_SAFETENSORS: return "safetensors";
        case CCE_FMT_CCE_ARCHIVE: return "cce-archive";
        case CCE_FMT_CMDL:        return "cce-model";
        case CCE_FMT_SUPRA_PACK:  return "supra-pack";
        case CCE_FMT_QWEN2_PACK:  return "qwen2-pack";
        default:                  return "unknown";
    }
}

const char* cce_detect_family_name(cce_arch_family f) {
    switch (f) {
        case CCE_ARCH_FAMILY_LLAMA: return "llama-family (separate q/k/v/o)";
        case CCE_ARCH_FAMILY_GPT2:  return "gpt2-family (fused qkv)";
        case CCE_ARCH_FAMILY_MAMBA: return "mamba-family (state-space)";
        case CCE_ARCH_FAMILY_HYBRID:return "hybrid (attention + state-space)";
        case CCE_ARCH_FAMILY_MLP:   return "mlp-stack";
        case CCE_ARCH_FAMILY_CCE:   return "cce-native";
        default:                    return "unknown";
    }
}

static const char* ggml_type_name(uint32_t t) {
    switch (t) {
        case 0:  return "F32";
        case 1:  return "F16";
        case 2:  return "Q4_0";
        case 3:  return "Q4_1";
        case 6:  return "Q5_0";
        case 7:  return "Q5_1";
        case 8:  return "Q8_0";
        case 9:  return "Q8_1";
        case 10: return "Q2_K";
        case 11: return "Q3_K";
        case 12: return "Q4_K";
        case 13: return "Q5_K";
        case 14: return "Q6_K";
        case 15: return "Q8_K";
        case 30: return "BF16";
        default: return NULL;
    }
}

static void info_defaults(cce_model_info* i) {
    memset(i, 0, sizeof(*i));
    i->n_tensors = -1;
    i->n_layer = -1; i->hidden = -1; i->n_head = -1; i->n_kv_head = -1;
    i->vocab = -1; i->ffn = -1; i->ctx_len = -1;
    i->tied_embeddings = -1;
    i->attention_full_qkv = -1;
}

static void note_append(cce_model_info* i, const char* s) {
    size_t used = strlen(i->notes);
    if (used > 0 && used + 2 < sizeof(i->notes)) { i->notes[used++] = ';'; i->notes[used++] = ' '; i->notes[used] = 0; }
    strncat(i->notes, s, sizeof(i->notes) - strlen(i->notes) - 1);
}

/* ---- format sniff ---- */

static cce_container_format sniff_format(const char* path, long* file_size_out) {
    FILE* f = fopen(path, "rb");
    if (!f) return CCE_FMT_UNKNOWN;
    unsigned char hdr[9];
    size_t got = fread(hdr, 1, sizeof(hdr), f);
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fclose(f);
    if (file_size_out) *file_size_out = fsize;
    if (got < 4) return CCE_FMT_UNKNOWN;

    if (memcmp(hdr, "GGUF", 4) == 0) return CCE_FMT_GGUF;
    if (memcmp(hdr, "CCE1", 4) == 0) return CCE_FMT_CCE_ARCHIVE;
    if (memcmp(hdr, "CMDL", 4) == 0) return CCE_FMT_CMDL;

    uint32_t m32 = (uint32_t)hdr[0] | ((uint32_t)hdr[1] << 8) | ((uint32_t)hdr[2] << 16) | ((uint32_t)hdr[3] << 24);
    if (m32 == SUPRA_PACK_MAGIC_V) return CCE_FMT_SUPRA_PACK;
    if (m32 == QWEN2_PACK_MAGIC_V) return CCE_FMT_QWEN2_PACK;

    /* safetensors: 8-byte LE JSON-header length, then '{' */
    if (got >= 9) {
        uint64_t hlen = 0;
        for (int b = 0; b < 8; b++) hlen |= ((uint64_t)hdr[b]) << (b * 8);
        if (hlen > 0 && hlen <= 64ULL * 1024 * 1024 && hdr[8] == '{' &&
            (uint64_t)fsize >= 8ULL + hlen) {
            return CCE_FMT_SAFETENSORS;
        }
    }

    /* sharded HF checkpoint index (model.safetensors.index.json): plain JSON
       with a "weight_map". No binary magic, so it can only be claimed here,
       after every real magic above has failed. The loader auto-dispatches. */
    {
        size_t i = 0;
        while (i < got && (hdr[i] == ' ' || hdr[i] == '\t' || hdr[i] == '\r' || hdr[i] == '\n')) i++;
        if (i < got && hdr[i] == '{' && fsize > 0 && fsize <= 4L * 1024 * 1024) {
            char buf[8192];
            FILE* jf = fopen(path, "rb");
            if (jf) {
                size_t jn = fread(buf, 1, sizeof(buf) - 1, jf);
                fclose(jf);
                buf[jn] = 0;
                if (strstr(buf, "\"weight_map\"")) return CCE_FMT_SAFETENSORS;
            }
        }
    }
    return CCE_FMT_UNKNOWN;
}

/* ---- GGUF structural probe (metadata only) ---- */

/* blk.N. prefix -> N, else -1 */
static int gguf_blk_index(const char* name) {
    if (strncmp(name, "blk.", 4) != 0) return -1;
    const char* p = name + 4;
    if (*p < '0' || *p > '9') return -1;
    int idx = 0;
    while (*p >= '0' && *p <= '9') { idx = idx * 10 + (*p - '0'); p++; }
    return (*p == '.') ? idx : -1;
}

/* Verify the jamba-style hybrid layout the cce_hybrid runner actually supports:
 * every layer is EXACTLY one of a complete attention block or a complete ssm
 * block, there is >=1 of each, and the shared embedding + final norm exist.
 * Returns 1 when runnable; else fills `miss` with the first gap and returns 0. */
static int gguf_hybrid_complete(const cce_gguf* g, int* out_n_attn, int* out_n_ssm,
                                char* miss, size_t miss_cap) {
    int nt = cce_gguf_tensor_count(g);
    int max_blk = -1;
    for (int i = 0; i < nt; i++) {
        cce_gguf_tensor_meta m;
        if (cce_gguf_get_tensor_meta(g, i, &m) != CCE_OK) continue;
        int b = gguf_blk_index(m.name);
        if (b > max_blk) max_blk = b;
    }
    if (max_blk < 0) { snprintf(miss, miss_cap, "no blk.N tensors"); return 0; }

    static const char* attn_req[] = { "attn_k.weight", "attn_v.weight",
        "attn_output.weight", "ffn_gate.weight", "ffn_up.weight",
        "ffn_down.weight", "attn_norm.weight", "ffn_norm.weight" };
    static const char* ssm_req[] = { "ssm_conv1d.weight", "ssm_x.weight",
        "ssm_dt.weight", "ssm_a", "ssm_d", "ssm_out.weight", "attn_norm.weight" };

    int n_attn = 0, n_ssm = 0;
    char nm[160];
    for (int l = 0; l <= max_blk; l++) {
        snprintf(nm, sizeof(nm), "blk.%d.ssm_in.weight", l);
        int is_ssm = cce_gguf_find_tensor(g, nm) >= 0;
        snprintf(nm, sizeof(nm), "blk.%d.attn_q.weight", l);
        int is_attn = cce_gguf_find_tensor(g, nm) >= 0;
        if (is_ssm == is_attn) {
            snprintf(miss, miss_cap, "layer %d is neither a complete attn nor ssm block", l);
            return 0;
        }
        const char** req = is_attn ? attn_req : ssm_req;
        size_t nreq = is_attn ? sizeof(attn_req) / sizeof(attn_req[0])
                              : sizeof(ssm_req) / sizeof(ssm_req[0]);
        for (size_t r = 0; r < nreq; r++) {
            snprintf(nm, sizeof(nm), "blk.%d.%s", l, req[r]);
            if (cce_gguf_find_tensor(g, nm) < 0) {
                snprintf(miss, miss_cap, "%s layer %d missing %s",
                         is_attn ? "attn" : "ssm", l, req[r]);
                return 0;
            }
        }
        if (is_attn) n_attn++; else n_ssm++;
    }
    if (n_attn == 0 || n_ssm == 0) {
        snprintf(miss, miss_cap, "not hybrid (%d attn / %d ssm layers)", n_attn, n_ssm);
        return 0;
    }
    if (cce_gguf_find_tensor(g, "token_embd.weight") < 0) { snprintf(miss, miss_cap, "missing token_embd.weight"); return 0; }
    if (cce_gguf_find_tensor(g, "output_norm.weight") < 0) { snprintf(miss, miss_cap, "missing output_norm.weight"); return 0; }
    *out_n_attn = n_attn; *out_n_ssm = n_ssm;
    return 1;
}

static void probe_gguf(const char* path, cce_model_info* info) {
    cce_gguf* g = NULL;
    if (cce_gguf_load(path, &g) != CCE_OK || !g) {
        note_append(info, "gguf metadata parse failed");
        return;
    }

    const char* arch = cce_gguf_get_arch(g);
    if (arch && arch[0]) strncpy(info->arch, arch, sizeof(info->arch) - 1);
    info->n_layer  = cce_gguf_get_n_layer(g)      > 0 ? cce_gguf_get_n_layer(g)      : -1;
    info->hidden   = cce_gguf_get_hidden_size(g)  > 0 ? cce_gguf_get_hidden_size(g)  : -1;
    info->n_head   = cce_gguf_get_n_heads(g)      > 0 ? cce_gguf_get_n_heads(g)      : -1;
    info->n_kv_head= cce_gguf_get_n_kv_heads(g)   > 0 ? cce_gguf_get_n_kv_heads(g)   : -1;
    info->vocab    = cce_gguf_get_vocab_size(g)   > 0 ? cce_gguf_get_vocab_size(g)   : -1;
    info->ctx_len  = cce_gguf_get_context_length(g) > 0 ? cce_gguf_get_context_length(g) : -1;
    info->ffn      = cce_gguf_get_feed_forward_length(g) > 0 ? cce_gguf_get_feed_forward_length(g) : -1;
    info->n_tensors = cce_gguf_tensor_count(g);
    strncpy(info->naming, "gguf-blk", sizeof(info->naming) - 1);

    /* structural scan over tensor names */
    int has_q = 0, has_k = 0, has_v = 0, has_o = 0, has_qkv_fused = 0;
    int has_ssm = 0, has_gate = 0, has_output_w = 0, has_experts = 0;
    int emb_d0 = 0, emb_d1 = 0;
    uint64_t type_bytes[64] = {0};

    for (int i = 0; i < info->n_tensors; i++) {
        cce_gguf_tensor_meta m;
        if (cce_gguf_get_tensor_meta(g, i, &m) != CCE_OK) continue;
        if (m.ggml_type < 64) {
            if (UINT64_MAX - type_bytes[m.ggml_type] < m.nbytes)
                type_bytes[m.ggml_type] = UINT64_MAX;
            else
                type_bytes[m.ggml_type] += m.nbytes;
        }
        if (strstr(m.name, "attn_q.weight"))      has_q = 1;
        if (strstr(m.name, "attn_k.weight"))      has_k = 1;
        if (strstr(m.name, "attn_v.weight"))      has_v = 1;
        if (strstr(m.name, "attn_output.weight")) has_o = 1;
        if (strstr(m.name, "attn_qkv.weight"))    has_qkv_fused = 1;
        if (strstr(m.name, ".ssm_") || strstr(m.name, "ssm_in") || strstr(m.name, "ssm_conv1d")) has_ssm = 1;
        if (strstr(m.name, "ffn_gate"))           has_gate = 1;
        if (strstr(m.name, "ffn_up_exps") || strstr(m.name, "ffn_down_exps") ||
            strstr(m.name, "ffn_gate_exps") || strstr(m.name, "ffn_gate_inp") ||
            strstr(m.name, ".experts."))          has_experts = 1;
        if (strcmp(m.name, "output.weight") == 0) has_output_w = 1;
        if (strcmp(m.name, "token_embd.weight") == 0 && m.ndim == 2) {
            emb_d0 = m.shape[0]; emb_d1 = m.shape[1];
        }
    }

    info->is_moe = has_experts;

    /* derive hidden/vocab from the embedding when metadata is missing */
    if (emb_d0 > 0 && emb_d1 > 0) {
        if (info->hidden <= 0) info->hidden = (emb_d0 < emb_d1) ? emb_d0 : emb_d1;
        if (info->vocab <= 0)  info->vocab  = (emb_d0 == info->hidden) ? emb_d1 : emb_d0;
        info->tied_embeddings = has_output_w ? 0 : 1;
    }

    /* Dominant storage dtype by bytes, not tensor count. Quantized models often
       have many tiny F32 norm tensors, so count-weighting mislabels them. */
    uint32_t best_t = 0;
    uint64_t best_bytes = 0;
    for (uint32_t t = 0; t < 64; t++) {
        if (type_bytes[t] > best_bytes) {
            best_bytes = type_bytes[t];
            best_t = t;
        }
    }
    if (best_bytes > 0) {
        const char* tn = ggml_type_name(best_t);
        if (tn) strncpy(info->dtype, tn, sizeof(info->dtype) - 1);
        else snprintf(info->dtype, sizeof(info->dtype), "ggml_%u", best_t);
    }

    /* family from structure; arch string is only a label */
    if (has_ssm && (has_q || has_qkv_fused)) {
        /* jamba/zamba-style hybrid: interleaved attention + state-space. Its
           own family (NOT mamba) — the native mixed runner threads one
           residual stream through both mixer kinds. Advertise runnable only
           when the layout the loader supports is actually complete. */
        info->family = CCE_ARCH_FAMILY_HYBRID;
        int na = 0, ns = 0;
        char miss[128] = {0};
        if (!has_q && has_qkv_fused) {
            note_append(info, "hybrid with fused-qkv attention: the hybrid runner expects separate q/k/v/o");
        } else if (gguf_hybrid_complete(g, &na, &ns, miss, sizeof(miss))) {
            strncpy(info->naming, "hybrid-blk", sizeof(info->naming) - 1);
            if (info->n_layer <= 0) info->n_layer = na + ns;
            note_append(info, "interleaved attention+ssm (jamba-style): native mixed attention+ssm forward");
        } else {
            char note[200];
            snprintf(note, sizeof(note),
                     "hybrid attention+ssm but layout incomplete for the hybrid runner: %s", miss);
            note_append(info, note);
        }
    } else if (has_ssm) {
        info->family = CCE_ARCH_FAMILY_MAMBA;
        note_append(info, "state-space model: runs recurrently (O(1) state per token)");
    } else if (has_q && has_o) {
        info->family = CCE_ARCH_FAMILY_LLAMA;
        info->attention_full_qkv = (has_k && has_v) ? 1 : 0;
        if (!info->attention_full_qkv)
            note_append(info, "partial attention (q+o only): runs via the gemma-style fallback path");
        if (!has_gate)
            note_append(info, "no ffn_gate found: MLP may not be llama-gated");
    } else if (has_qkv_fused) {
        info->family = CCE_ARCH_FAMILY_GPT2;
        note_append(info, "fused-qkv gguf: cce gguf runner expects separate q/k/v/o");
    }

    /* cross-check declared arch vs detected structure */
    if (info->arch[0]) {
        int arch_says_mamba = strstr(info->arch, "mamba") != NULL || strstr(info->arch, "jamba") != NULL;
        if (arch_says_mamba && info->family != CCE_ARCH_FAMILY_MAMBA)
            note_append(info, "declared arch says mamba but no ssm tensors found");
        if (!arch_says_mamba && info->family == CCE_ARCH_FAMILY_MAMBA)
            note_append(info, "ssm tensors found but declared arch is not mamba");
    }

    cce_gguf_free(g);
}

/* ---- safetensors structural probe (header only) ---- */

static int max_layer_index(const char* name, const char* prefix) {
    /* returns layer index if name starts with prefix+digits+'.', else -1 */
    size_t plen = strlen(prefix);
    if (strncmp(name, prefix, plen) != 0) return -1;
    const char* p = name + plen;
    if (*p < '0' || *p > '9') return -1;
    int idx = 0;
    while (*p >= '0' && *p <= '9') { idx = idx * 10 + (*p - '0'); p++; }
    if (*p != '.') return -1;
    return idx;
}

static void probe_safetensors(const char* path, cce_model_info* info) {
    cce_safetensors* st = NULL;
    if (cce_safetensors_load(path, &st) != CCE_OK || !st) {
        note_append(info, "safetensors header parse failed");
        return;
    }

    int n = cce_safetensors_count(st);
    info->n_tensors = n;

    if (cce_safetensors_shard_count(st) > 0) {
        char note[64];
        snprintf(note, sizeof(note), "sharded checkpoint (%d shards)",
                 cce_safetensors_shard_count(st));
        note_append(info, note);
    }

    int has_qproj = 0, has_kproj = 0, has_vproj = 0, has_oproj = 0;
    int has_c_attn = 0, has_blocks_qkv = 0, has_mamba = 0, has_gate = 0, has_experts = 0;
    int has_lm_head = 0, has_tok_emb_supra = 0;
    int has_xproj = 0, has_extra_norms = 0;
    int layers_hf = -1, layers_gpt2 = -1, layers_blocks = -1, layers_backbone = -1, layers_flat = -1;
    int emb_d0 = 0, emb_d1 = 0;

    /* dominant dtype bookkeeping over the small fixed dtype strings */
    char dtypes[8][16]; int dtype_n[8]; int dtype_kinds = 0;

    static const char* emb_names[] = {
        "model.embed_tokens.weight", "tok_emb.weight", "wte.weight",
        "transformer.wte.weight", "backbone.embedding.weight",
        "backbone.embeddings.weight", "embeddings.word_embeddings.weight",
    };

    for (int i = 0; i < n; i++) {
        cce_safetensor_meta m;
        if (cce_safetensors_get_meta(st, i, &m) != CCE_OK) continue;

        if (strstr(m.name, "self_attn.q_proj")) has_qproj = 1;
        if (strstr(m.name, "self_attn.k_proj")) has_kproj = 1;
        if (strstr(m.name, "self_attn.v_proj")) has_vproj = 1;
        if (strstr(m.name, "self_attn.o_proj")) has_oproj = 1;
        if (strstr(m.name, "attn.c_attn"))      has_c_attn = 1;
        if (strstr(m.name, "attn.qkv"))         has_blocks_qkv = 1;
        if (strstr(m.name, "A_log") || strstr(m.name, ".ssm") || strstr(m.name, "mixer.")) has_mamba = 1;
        if (strstr(m.name, "mixer.x_proj"))     has_xproj = 1;
        if (strstr(m.name, "mlp.gate_proj"))    has_gate = 1;
        if (strstr(m.name, ".experts.") || strstr(m.name, "block_sparse_moe") ||
            strstr(m.name, "shared_expert"))     has_experts = 1;
        if (strstr(m.name, "self_attn.q_norm") || strstr(m.name, "self_attn.k_norm") ||
            strstr(m.name, "pre_feedforward_layernorm") || strstr(m.name, "post_feedforward_layernorm"))
            has_extra_norms = 1;
        if (strstr(m.name, "lm_head.weight") || strcmp(m.name, "head.weight") == 0 ||
            strcmp(m.name, "output.weight") == 0) has_lm_head = 1;
        if (strcmp(m.name, "tok_emb.weight") == 0) has_tok_emb_supra = 1;

        int li;
        if ((li = max_layer_index(m.name, "model.layers.")) >= 0 && li > layers_hf) layers_hf = li;
        if ((li = max_layer_index(m.name, "transformer.h.")) >= 0 && li > layers_gpt2) layers_gpt2 = li;
        if ((li = max_layer_index(m.name, "blocks.")) >= 0 && li > layers_blocks) layers_blocks = li;
        if ((li = max_layer_index(m.name, "backbone.layers.")) >= 0 && li > layers_backbone) layers_backbone = li;
        if ((li = max_layer_index(m.name, "layers.")) >= 0 && li > layers_flat) layers_flat = li;

        for (size_t e = 0; e < sizeof(emb_names)/sizeof(emb_names[0]); e++) {
            if (strcmp(m.name, emb_names[e]) == 0 && m.ndim == 2) {
                emb_d0 = m.shape[0]; emb_d1 = m.shape[1];
            }
        }

        int found = 0;
        for (int d = 0; d < dtype_kinds; d++) {
            if (strcmp(dtypes[d], m.dtype) == 0) { dtype_n[d]++; found = 1; break; }
        }
        if (!found && dtype_kinds < 8) {
            /* both are CCE_ST_MAX_DTYPE-sized */
            memcpy(dtypes[dtype_kinds], m.dtype, sizeof(dtypes[0]));
            dtypes[dtype_kinds][sizeof(dtypes[0]) - 1] = 0;
            dtype_n[dtype_kinds] = 1;
            dtype_kinds++;
        }
    }

    info->is_moe = has_experts;

    int best = -1, best_n = 0;
    for (int d = 0; d < dtype_kinds; d++) if (dtype_n[d] > best_n) { best_n = dtype_n[d]; best = d; }
    if (best >= 0) strncpy(info->dtype, dtypes[best], sizeof(info->dtype) - 1);

    /* embedding is [vocab, hidden] in HF convention; vocab is the larger dim */
    if (emb_d0 > 0 && emb_d1 > 0) {
        info->hidden = (emb_d0 < emb_d1) ? emb_d0 : emb_d1;
        info->vocab  = (emb_d0 == info->hidden) ? emb_d1 : emb_d0;
        info->tied_embeddings = has_lm_head ? 0 : 1;
    }

    /* family + naming from structure. The naming string is the loader
       fingerprint: only stamp it when the evidence the loader actually
       requires is present, so the registry can't promise a runner that
       would systematically fail. */
    if (has_mamba) {
        info->family = CCE_ARCH_FAMILY_MAMBA;
        if (layers_backbone >= 0 && has_xproj) {
            /* mamba-1 hf-backbone signature (x_proj exists only in mamba-1) */
            strncpy(info->naming, "hf-backbone", sizeof(info->naming) - 1);
            info->n_layer = layers_backbone + 1;
            note_append(info, "state-space model: runs recurrently (O(1) state per token)");
        } else {
            if (layers_backbone >= 0) info->n_layer = layers_backbone + 1;
            note_append(info, "ssm-style tensors but not the mamba-1 hf-backbone layout (mamba-2/hybrid?): no runner");
        }
    } else if (has_qproj && has_kproj && has_vproj && has_oproj) {
        info->family = CCE_ARCH_FAMILY_LLAMA;
        info->attention_full_qkv = 1;
        if (layers_hf >= 0) info->n_layer = layers_hf + 1;
        if (has_extra_norms) {
            note_append(info, "extra norm tensors (qwen3 q/k-norm or gemma sandwich): shared runner cannot express them");
        } else if (layers_hf >= 0 && has_gate) {
            strncpy(info->naming, "hf-model.layers", sizeof(info->naming) - 1);
        } else {
            if (layers_hf < 0) note_append(info, "q/k/v/o found but not under model.layers.N (prefixed/multimodal?): no runner");
            if (!has_gate) note_append(info, "no mlp.gate_proj: MLP not llama-gated, loader would refuse");
        }
    } else if (has_c_attn) {
        info->family = CCE_ARCH_FAMILY_GPT2;
        strncpy(info->naming, "gpt2-transformer.h", sizeof(info->naming) - 1);
        if (layers_gpt2 >= 0) info->n_layer = layers_gpt2 + 1;
        note_append(info, "gpt2-style safetensors: map via cce_safetensors population helpers");
    } else if (has_blocks_qkv && has_tok_emb_supra) {
        info->family = CCE_ARCH_FAMILY_GPT2;
        strncpy(info->naming, "supra-blocks", sizeof(info->naming) - 1);
        if (layers_blocks >= 0) info->n_layer = layers_blocks + 1;
        note_append(info, "loads from the containing directory");
    } else if (layers_flat >= 0) {
        info->family = CCE_ARCH_FAMILY_MLP;
        strncpy(info->naming, "flat-layers", sizeof(info->naming) - 1);
        info->n_layer = layers_flat + 1;
        note_append(info, "plain linear stack: map via cce_safetensors population helpers");
    }

    cce_safetensors_free(st);
}

/* ---- packed formats: headers are tiny fixed structs, read hparams directly ---- */

static void probe_supra_pack(const char* path, cce_model_info* info) {
    FILE* f = fopen(path, "rb");
    if (!f) return;
    uint32_t magic = 0, ver = 0;
    int hp[5];
    if (fread(&magic, 4, 1, f) == 1 && fread(&ver, 4, 1, f) == 1 &&
        fread(hp, sizeof(int), 5, f) == 5) {
        info->n_layer = hp[0]; info->hidden = hp[1];
        info->ctx_len = hp[2]; info->n_head = hp[3]; info->vocab = hp[4];
        info->family = CCE_ARCH_FAMILY_GPT2;
        strncpy(info->naming, "supra-pack", sizeof(info->naming) - 1);
        strncpy(info->dtype, "ternary", sizeof(info->dtype) - 1);
    } else {
        note_append(info, "supra-pack header truncated: hparams unreadable, not runnable");
    }
    fclose(f);
}

static void probe_qwen2_pack(const char* path, cce_model_info* info) {
    FILE* f = fopen(path, "rb");
    if (!f) return;
    uint32_t magic = 0, ver = 0;
    int hp[8];
    if (fread(&magic, 4, 1, f) != 1 || fread(&ver, 4, 1, f) != 1) {
        note_append(info, "qwen2-pack header truncated: hparams unreadable, not runnable");
        fclose(f);
        return;
    }
    if (ver == CCE_QGKP_VERSION_ENVELOPE) {
        cce_qgkp_info q;
        fclose(f);
        if (cce_qgkp_inspect(path, &q) != CCE_OK) {
            note_append(info, "QGKP v3 envelope header invalid");
            return;
        }
        info->n_layer = q.n_layer;
        info->hidden = q.hidden;
        info->ctx_len = q.context_length;
        info->family = CCE_ARCH_FAMILY_LLAMA;
        info->attention_full_qkv = 1;
        strncpy(info->arch, q.architecture, sizeof(info->arch) - 1);
        strncpy(info->naming, "qgkp-gguf-envelope", sizeof(info->naming) - 1);
        strncpy(info->dtype, q.quantization, sizeof(info->dtype) - 1);
        note_append(info, "lossless packet-trit GGUF envelope: execute via declared external backend");
        return;
    }
    if (
        fread(hp, sizeof(int), 8, f) == 8) {
        info->n_layer = hp[0]; info->hidden = hp[1];
        info->n_head = hp[2]; info->n_kv_head = hp[3];
        info->vocab = hp[5]; info->ctx_len = hp[6];
        info->family = CCE_ARCH_FAMILY_LLAMA;
        info->attention_full_qkv = 1;
        strncpy(info->naming, "qwen2-pack", sizeof(info->naming) - 1);
        strncpy(info->dtype, "ternary", sizeof(info->dtype) - 1);
    } else {
        note_append(info, "qwen2-pack header truncated: hparams unreadable, not runnable");
    }
    fclose(f);
}

/* ---- runner registry: the single arch->builder table ----
 *
 * Both cce_detect_file (the runnable/runner verdict) and cce_anymodel_open
 * (the actual dispatch) walk this table. Adding a runner = one row here.
 * precheck() may veto a structurally-matching row (e.g. missing config.json)
 * by returning a human-readable reason; NULL means good to go.
 */

static void parent_dir(const char* path, char* out, size_t cap) {
    strncpy(out, path, cap - 1);
    out[cap - 1] = 0;
    char* last = NULL;
    for (char* p = out; *p; p++) if (*p == '/' || *p == '\\') last = p;
    if (last) *last = 0;
    else { out[0] = '.'; out[1] = 0; }
}

static cce_result open_gguf_transformer(cce_anymodel* m, const char* path) {
    return cce_gguf_load_model(&m->transformer, path);
}
static cce_result open_st_llama(cce_anymodel* m, const char* path) {
    return cce_st_llama_load(&m->transformer, path);
}
static cce_result open_ssm(cce_anymodel* m, const char* path) {
    return cce_ssm_load(&m->ssm, path);
}
static cce_result open_hybrid(cce_anymodel* m, const char* path) {
    return cce_hybrid_load(&m->hybrid, path);
}
static cce_result open_supra_dir(cce_anymodel* m, const char* path) {
    char dir[512];
    parent_dir(path, dir, sizeof(dir));
    return cce_supra_a2a_load(&m->supra, dir);
}
static cce_result open_supra_pack(cce_anymodel* m, const char* path) {
    return cce_supra_a2a_load_packed(&m->supra, path);
}
static cce_result open_qwen2_pack(cce_anymodel* m, const char* path) {
    return cce_gguf_qwen2_load_packed(&m->transformer, path);
}
static cce_result open_cmdl(cce_anymodel* m, const char* path) {
    cce_result rc = cce_model_create(&m->model, "anymodel");
    if (rc != CCE_OK) return rc;
    rc = cce_model_load(m->model, path);
    if (rc != CCE_OK) { cce_model_destroy(m->model); m->model = NULL; }
    return rc;
}

/* HF safetensors carry no head counts; the loader needs the sibling config.json */
static const char* precheck_config_json(const char* path) {
    char cfg[512];
    strncpy(cfg, path, sizeof(cfg) - 1);
    cfg[sizeof(cfg) - 1] = 0;
    char* last = NULL;
    for (char* p = cfg; *p; p++) if (*p == '/' || *p == '\\') last = p;
    if (last) snprintf(last + 1, sizeof(cfg) - (size_t)(last + 1 - cfg), "config.json");
    else snprintf(cfg, sizeof(cfg), "config.json");
    FILE* f = fopen(cfg, "rb");
    if (f) { fclose(f); return NULL; }
    return "needs config.json (head counts) next to the weights file";
}

typedef struct {
    cce_container_format format;
    cce_arch_family family;
    const char* naming;                       /* NULL = any naming */
    const char* runner_name;
    cce_result (*open_fn)(cce_anymodel*, const char*);
    const char* (*precheck)(const char*);     /* NULL note = runnable */
} cce_runner_entry;

static const cce_runner_entry k_runner_registry[] = {
    { CCE_FMT_GGUF,        CCE_ARCH_FAMILY_LLAMA, NULL,              "cce_gguf_load_model",        open_gguf_transformer, NULL },
    { CCE_FMT_GGUF,        CCE_ARCH_FAMILY_MAMBA, NULL,              "cce_ssm_load",               open_ssm,              NULL },
    { CCE_FMT_GGUF,        CCE_ARCH_FAMILY_HYBRID,"hybrid-blk",      "cce_hybrid_load",            open_hybrid,           NULL },
    { CCE_FMT_SAFETENSORS, CCE_ARCH_FAMILY_MAMBA, "hf-backbone",     "cce_ssm_load",               open_ssm,              NULL },
    { CCE_FMT_SAFETENSORS, CCE_ARCH_FAMILY_LLAMA, "hf-model.layers", "cce_st_llama_load",          open_st_llama,         precheck_config_json },
    { CCE_FMT_SAFETENSORS, CCE_ARCH_FAMILY_GPT2,  "supra-blocks",    "cce_supra_a2a_load",         open_supra_dir,        NULL },
    { CCE_FMT_SUPRA_PACK,  CCE_ARCH_FAMILY_GPT2,  NULL,              "cce_supra_a2a_load_packed",  open_supra_pack,       NULL },
    { CCE_FMT_QWEN2_PACK,  CCE_ARCH_FAMILY_LLAMA, "qwen2-pack",     "cce_gguf_qwen2_load_packed", open_qwen2_pack,       NULL },
    /* CCE_FMT_CCE_ARCHIVE deliberately has NO row: cce_forest_open on a bare
       archive yields an empty forest (branch restore is owned by the loaders
       that wrote it, e.g. supra decomposed reload). Claiming runnable here
       would hand back a model with zero branches. */
    { CCE_FMT_CMDL,        CCE_ARCH_FAMILY_CCE,   NULL,              "cce_model_load",             open_cmdl,             NULL },
};

static const cce_runner_entry* registry_match(const cce_model_info* info) {
    for (size_t i = 0; i < sizeof(k_runner_registry) / sizeof(k_runner_registry[0]); i++) {
        const cce_runner_entry* e = &k_runner_registry[i];
        if (e->format != info->format) continue;
        if (e->family != info->family) continue;
        if (e->naming && strcmp(e->naming, info->naming) != 0) continue;
        return e;
    }
    return NULL;
}

static void apply_registry(const char* path, cce_model_info* info) {
    const cce_runner_entry* e = registry_match(info);
    if (!e) return; /* runnable stays 0; structural notes explain why */
    if (e->precheck) {
        const char* veto = e->precheck(path);
        if (veto) { note_append(info, veto); return; }
    }
    info->runnable = 1;
    strncpy(info->runner, e->runner_name, sizeof(info->runner) - 1);
}

/* ---- public API ---- */

cce_result cce_detect_file(const char* path, cce_model_info* out) {
    if (!path || !out) return CCE_ERR_INVALID_ARG;
    info_defaults(out);

    FILE* f = fopen(path, "rb");
    if (!f) return CCE_ERR_IO;
    fclose(f);

    long fsize = 0;
    out->format = sniff_format(path, &fsize);

    switch (out->format) {
        case CCE_FMT_GGUF:        probe_gguf(path, out); break;
        case CCE_FMT_SAFETENSORS: probe_safetensors(path, out); break;
        case CCE_FMT_SUPRA_PACK:  probe_supra_pack(path, out); break;
        case CCE_FMT_QWEN2_PACK:  probe_qwen2_pack(path, out); break;
        case CCE_FMT_CCE_ARCHIVE:
            out->family = CCE_ARCH_FAMILY_CCE;
            strncpy(out->naming, "cce-forest", sizeof(out->naming) - 1);
            note_append(out, "forest archive: reopen via its owning loader (standalone branch restore not implemented)");
            break;
        case CCE_FMT_CMDL:
            out->family = CCE_ARCH_FAMILY_CCE;
            strncpy(out->naming, "cce-model", sizeof(out->naming) - 1);
            break;
        default:
            note_append(out, "no known magic: not gguf/safetensors/cce");
            break;
    }

    apply_registry(path, out);
    return CCE_OK;
}

static void print_dim(const char* label, int v) {
    if (v >= 0) printf("  %-14s %d\n", label, v);
    else        printf("  %-14s ?\n", label);
}

void cce_detect_print(const cce_model_info* info, const char* path) {
    if (!info) return;
    printf("=== model structure: %s ===\n", path ? path : "(null)");
    printf("  %-14s %s\n", "format", cce_detect_format_name(info->format));
    printf("  %-14s %s\n", "family", cce_detect_family_name(info->family));
    if (info->arch[0])   printf("  %-14s %s\n", "declared arch", info->arch);
    if (info->naming[0]) printf("  %-14s %s\n", "naming", info->naming);
    if (info->dtype[0])  printf("  %-14s %s\n", "dtype", info->dtype);
    print_dim("tensors", info->n_tensors);
    print_dim("layers", info->n_layer);
    print_dim("hidden", info->hidden);
    print_dim("heads", info->n_head);
    print_dim("kv heads", info->n_kv_head);
    print_dim("vocab", info->vocab);
    print_dim("ffn", info->ffn);
    print_dim("ctx", info->ctx_len);
    if (info->tied_embeddings >= 0)
        printf("  %-14s %s\n", "tied emb", info->tied_embeddings ? "yes" : "no");
    if (info->attention_full_qkv >= 0)
        printf("  %-14s %s\n", "full q/k/v/o", info->attention_full_qkv ? "yes" : "no (partial)");
    printf("  %-14s %s\n", "model class", info->is_moe ? "moe" : "dense/non-moe");
    printf("  %-14s %s%s%s\n", "runnable",
           info->runnable ? "yes via " : "no",
           info->runnable ? info->runner : "",
           "");
    if (info->notes[0]) printf("  %-14s %s\n", "notes", info->notes);
}

/* ---- universal open ---- */

cce_result cce_anymodel_open(cce_anymodel** out, const char* path) {
    if (!out || !path) return CCE_ERR_INVALID_ARG;
    *out = NULL;

    cce_model_info info;
    cce_result rc = cce_detect_file(path, &info);
    if (rc != CCE_OK) return rc;
    if (!info.runnable) return CCE_ERR_UNSUPPORTED;

    const cce_runner_entry* e = registry_match(&info);
    if (!e) return CCE_ERR_UNSUPPORTED;

    cce_anymodel* m = (cce_anymodel*)calloc(1, sizeof(*m));
    if (!m) return CCE_ERR_OOM;
    m->info = info;

    rc = e->open_fn(m, path);
    if (rc != CCE_OK) { free(m); return rc; }
    *out = m;
    return CCE_OK;
}

void cce_anymodel_free(cce_anymodel* m) {
    if (!m) return;
    if (m->transformer) cce_gguf_qwen2_free(m->transformer);
    if (m->supra)       cce_supra_a2a_free(m->supra);
    if (m->ssm)         cce_ssm_free(m->ssm);
    if (m->hybrid)      cce_hybrid_free(m->hybrid);
    if (m->forest)      cce_forest_close(m->forest);
    if (m->model)       cce_model_destroy(m->model);
    free(m);
}
