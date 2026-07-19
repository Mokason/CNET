#include "../../include/cce/cce_deepseek_map.h"
#include "../../include/cce/cce_cascade.h"
#include "../../include/cce/cce_block.h"
#include "../../include/cce/cce_gguf.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>

void cce_ds_hparams_default_v3(cce_ds_hparams* hp) {
    if (!hp) return;
    memset(hp, 0, sizeof(*hp));
    hp->n_layer = 61;
    hp->d_model = 7168;
    hp->n_heads = 128;
    hp->n_kv_heads = 128;
    hp->qk_nope_head_dim = 128;
    hp->qk_rope_head_dim = 64;
    hp->v_head_dim = 128;
    hp->kv_lora_rank = 512;
    hp->q_lora_rank = 1536;
    hp->n_expert = 256;
    hp->n_expert_used = 8;
    hp->n_shared_expert = 1;
    hp->n_ff_exp = 2048;
    hp->vocab = 129280;
    hp->has_mtp = 1;
    hp->rope_theta = 10000.0f;
    snprintf(hp->arch, sizeof(hp->arch), "deepseek3");
}

void cce_ds_hparams_default_small(cce_ds_hparams* hp) {
    if (!hp) return;
    memset(hp, 0, sizeof(*hp));
    hp->n_layer = 2;
    hp->d_model = 256;
    hp->n_heads = 4;
    hp->n_kv_heads = 4;
    hp->qk_nope_head_dim = 32;
    hp->qk_rope_head_dim = 16;
    hp->v_head_dim = 32;
    hp->kv_lora_rank = 64;
    hp->q_lora_rank = 0;
    hp->n_expert = 4;
    hp->n_expert_used = 2;
    hp->n_shared_expert = 0;
    hp->n_ff_exp = 128;
    hp->vocab = 32000;
    hp->has_mtp = 0;
    hp->rope_theta = 10000.0f;
    snprintf(hp->arch, sizeof(hp->arch), "deepseek3");
}

const char* cce_ds_role_name(cce_ds_role role) {
    switch (role) {
    case CCE_DS_ROLE_TRUNK_EMBED: return "trunk.embed";
    case CCE_DS_ROLE_TRUNK_NORM: return "trunk.norm";
    case CCE_DS_ROLE_TRUNK_HEAD: return "trunk.head";
    case CCE_DS_ROLE_TRUNK_ROPE: return "trunk.rope";
    case CCE_DS_ROLE_TRUNK_MTP: return "trunk.mtp";
    case CCE_DS_ROLE_ATTN_NORM: return "attn.norm";
    case CCE_DS_ROLE_MLA_Q_DOWN: return "mla.q_down";
    case CCE_DS_ROLE_MLA_Q_NORM: return "mla.q_norm";
    case CCE_DS_ROLE_MLA_Q_UP: return "mla.q_up";
    case CCE_DS_ROLE_MLA_KV_DOWN: return "mla.kv_down";
    case CCE_DS_ROLE_MLA_KV_NORM: return "mla.kv_norm";
    case CCE_DS_ROLE_MLA_KV_UP: return "mla.kv_up";
    case CCE_DS_ROLE_MLA_O: return "mla.o";
    case CCE_DS_ROLE_FFN_NORM: return "ffn.norm";
    case CCE_DS_ROLE_FFN_ROUTER: return "ffn.route";
    case CCE_DS_ROLE_FFN_SHARED: return "ffn.shared";
    case CCE_DS_ROLE_FFN_EXPERT_GATE: return "ffn.expert.gate";
    case CCE_DS_ROLE_FFN_EXPERT_UP: return "ffn.expert.up";
    case CCE_DS_ROLE_FFN_EXPERT_DOWN: return "ffn.expert.down";
    default: return "none";
    }
}

int cce_ds_format_cnet(char* out, int out_n, cce_ds_role role,
                       int layer, int expert) {
    if (!out || out_n < 8) return -1;
    switch (role) {
    case CCE_DS_ROLE_TRUNK_EMBED:
        return snprintf(out, out_n, "trunk.embed");
    case CCE_DS_ROLE_TRUNK_NORM:
        return snprintf(out, out_n, "trunk.norm");
    case CCE_DS_ROLE_TRUNK_HEAD:
        return snprintf(out, out_n, "trunk.head");
    case CCE_DS_ROLE_TRUNK_ROPE:
        return snprintf(out, out_n, "trunk.rope");
    case CCE_DS_ROLE_TRUNK_MTP:
        return snprintf(out, out_n, "trunk.mtp");
    case CCE_DS_ROLE_ATTN_NORM:
        return snprintf(out, out_n, "L%02d.attn.norm", layer);
    case CCE_DS_ROLE_MLA_Q_DOWN:
        return snprintf(out, out_n, "L%02d.mla.q_dn", layer);
    case CCE_DS_ROLE_MLA_Q_NORM:
        return snprintf(out, out_n, "L%02d.mla.q_nm", layer);
    case CCE_DS_ROLE_MLA_Q_UP:
        return snprintf(out, out_n, "L%02d.mla.q_up", layer);
    case CCE_DS_ROLE_MLA_KV_DOWN:
        return snprintf(out, out_n, "L%02d.mla.kv_dn", layer);
    case CCE_DS_ROLE_MLA_KV_NORM:
        return snprintf(out, out_n, "L%02d.mla.kv_nm", layer);
    case CCE_DS_ROLE_MLA_KV_UP:
        return snprintf(out, out_n, "L%02d.mla.kv_up", layer);
    case CCE_DS_ROLE_MLA_O:
        return snprintf(out, out_n, "L%02d.mla.o", layer);
    case CCE_DS_ROLE_FFN_NORM:
        return snprintf(out, out_n, "L%02d.ffn.norm", layer);
    case CCE_DS_ROLE_FFN_ROUTER:
        return snprintf(out, out_n, "L%02d.ffn.route", layer);
    case CCE_DS_ROLE_FFN_SHARED:
        return snprintf(out, out_n, "L%02d.ffn.shared", layer);
    case CCE_DS_ROLE_FFN_EXPERT_GATE:
        return snprintf(out, out_n, "L%02d.ffn.e%03d.g", layer, expert);
    case CCE_DS_ROLE_FFN_EXPERT_UP:
        return snprintf(out, out_n, "L%02d.ffn.e%03d.u", layer, expert);
    case CCE_DS_ROLE_FFN_EXPERT_DOWN:
        return snprintf(out, out_n, "L%02d.ffn.e%03d.d", layer, expert);
    default:
        return snprintf(out, out_n, "unknown");
    }
}

int cce_ds_format_gguf(char* out, int out_n, cce_ds_role role,
                       int layer, int expert, const cce_ds_hparams* hp) {
    /* Import-side aliases (llama.cpp / DeepSeek GGUF convention).
     * Runtime never uses these as primary keys. */
    (void)hp;
    if (!out || out_n < 8) return -1;
    switch (role) {
    case CCE_DS_ROLE_TRUNK_EMBED:
        return snprintf(out, out_n, "token_embd.weight");
    case CCE_DS_ROLE_TRUNK_NORM:
        return snprintf(out, out_n, "output_norm.weight");
    case CCE_DS_ROLE_TRUNK_HEAD:
        return snprintf(out, out_n, "output.weight");
    case CCE_DS_ROLE_TRUNK_ROPE:
        return snprintf(out, out_n, "rope_freqs.weight");
    case CCE_DS_ROLE_TRUNK_MTP:
        return snprintf(out, out_n, "mtp.weight");
    case CCE_DS_ROLE_ATTN_NORM:
        return snprintf(out, out_n, "blk.%d.attn_norm.weight", layer);
    case CCE_DS_ROLE_MLA_Q_DOWN:
        return snprintf(out, out_n, "blk.%d.attn_q_a.weight", layer);
    case CCE_DS_ROLE_MLA_Q_NORM:
        return snprintf(out, out_n, "blk.%d.attn_q_a_norm.weight", layer);
    case CCE_DS_ROLE_MLA_Q_UP:
        return snprintf(out, out_n, "blk.%d.attn_q_b.weight", layer);
    case CCE_DS_ROLE_MLA_KV_DOWN:
        return snprintf(out, out_n, "blk.%d.attn_kv_a_mqa.weight", layer);
    case CCE_DS_ROLE_MLA_KV_NORM:
        return snprintf(out, out_n, "blk.%d.attn_kv_a_norm.weight", layer);
    case CCE_DS_ROLE_MLA_KV_UP:
        return snprintf(out, out_n, "blk.%d.attn_kv_b.weight", layer);
    case CCE_DS_ROLE_MLA_O:
        return snprintf(out, out_n, "blk.%d.attn_output.weight", layer);
    case CCE_DS_ROLE_FFN_NORM:
        return snprintf(out, out_n, "blk.%d.ffn_norm.weight", layer);
    case CCE_DS_ROLE_FFN_ROUTER:
        return snprintf(out, out_n, "blk.%d.ffn_gate_inp.weight", layer);
    case CCE_DS_ROLE_FFN_SHARED:
        return snprintf(out, out_n, "blk.%d.ffn_down_shexp.weight", layer);
    case CCE_DS_ROLE_FFN_EXPERT_GATE:
        /* Fused bank form preferred; per-expert names for contracts */
        return snprintf(out, out_n, "blk.%d.ffn_gate_exps.weight", layer);
    case CCE_DS_ROLE_FFN_EXPERT_UP:
        return snprintf(out, out_n, "blk.%d.ffn_up_exps.weight", layer);
    case CCE_DS_ROLE_FFN_EXPERT_DOWN:
        return snprintf(out, out_n, "blk.%d.ffn_down_exps.weight", layer);
    default:
        if (out_n > 0) out[0] = '\0';
        return 0;
    }
    (void)expert;
}

void cce_ds_hparams_to_mla(const cce_ds_hparams* hp, cce_mla_config* mla) {
    if (!hp || !mla) return;
    memset(mla, 0, sizeof(*mla));
    mla->n_heads = hp->n_heads;
    mla->n_kv_heads = hp->n_kv_heads > 0 ? hp->n_kv_heads : hp->n_heads;
    mla->qk_nope_head_dim = hp->qk_nope_head_dim;
    mla->qk_rope_head_dim = hp->qk_rope_head_dim;
    mla->v_head_dim = hp->v_head_dim;
    mla->kv_lora_rank = hp->kv_lora_rank;
    mla->q_lora_rank = hp->q_lora_rank;
    mla->d_model = hp->d_model;
    mla->rope_theta = hp->rope_theta > 0 ? hp->rope_theta : 10000.0f;
    mla->attn_scale = 0.0f;
    mla->use_absorb = 1;
}

static void leaf_set(cce_ds_leaf* L, cce_ds_role role, cce_ds_residency res,
                     int layer, int expert, int required,
                     const cce_ds_hparams* hp) {
    memset(L, 0, sizeof(*L));
    L->role = role;
    L->res = res;
    L->layer = layer;
    L->expert = expert;
    L->required = required;
    cce_ds_format_cnet(L->cnet, sizeof(L->cnet), role, layer, expert);
    cce_ds_format_gguf(L->gguf, sizeof(L->gguf), role, layer, expert, hp);
    L->gguf_b[0] = '\0';

    /* Dim contracts from hparams */
    switch (role) {
    case CCE_DS_ROLE_TRUNK_EMBED:
        L->dim_in = hp->vocab; L->dim_out = hp->d_model; break;
    case CCE_DS_ROLE_TRUNK_HEAD:
        L->dim_in = hp->d_model; L->dim_out = hp->vocab; break;
    case CCE_DS_ROLE_TRUNK_NORM:
    case CCE_DS_ROLE_ATTN_NORM:
    case CCE_DS_ROLE_FFN_NORM:
        L->dim_in = hp->d_model; L->dim_out = hp->d_model; break;
    case CCE_DS_ROLE_MLA_Q_DOWN:
        L->dim_in = hp->d_model; L->dim_out = hp->q_lora_rank; break;
    case CCE_DS_ROLE_MLA_Q_NORM:
        L->dim_in = hp->q_lora_rank; L->dim_out = hp->q_lora_rank; break;
    case CCE_DS_ROLE_MLA_Q_UP: {
        int q_in = hp->q_lora_rank > 0 ? hp->q_lora_rank : hp->d_model;
        int q_out = hp->n_heads * (hp->qk_nope_head_dim + hp->qk_rope_head_dim);
        L->dim_in = q_in; L->dim_out = q_out; break;
    }
    case CCE_DS_ROLE_MLA_KV_DOWN:
        L->dim_in = hp->d_model;
        L->dim_out = hp->kv_lora_rank + hp->qk_rope_head_dim;
        break;
    case CCE_DS_ROLE_MLA_KV_NORM:
        L->dim_in = hp->kv_lora_rank; L->dim_out = hp->kv_lora_rank; break;
    case CCE_DS_ROLE_MLA_KV_UP:
        L->dim_in = hp->kv_lora_rank;
        L->dim_out = hp->n_heads * (hp->qk_nope_head_dim + hp->v_head_dim);
        break;
    case CCE_DS_ROLE_MLA_O:
        L->dim_in = hp->n_heads * hp->v_head_dim;
        L->dim_out = hp->d_model;
        break;
    case CCE_DS_ROLE_FFN_ROUTER:
        L->dim_in = hp->d_model; L->dim_out = hp->n_expert; break;
    case CCE_DS_ROLE_FFN_EXPERT_GATE:
    case CCE_DS_ROLE_FFN_EXPERT_UP:
        L->dim_in = hp->d_model; L->dim_out = hp->n_ff_exp; break;
    case CCE_DS_ROLE_FFN_EXPERT_DOWN:
        L->dim_in = hp->n_ff_exp; L->dim_out = hp->d_model; break;
    default:
        L->dim_in = 0; L->dim_out = 0; break;
    }
}

static int map_capacity(const cce_ds_hparams* hp) {
    int n = 8; /* trunk */
    int per_layer = 12; /* attn norms + mla leaves + ffn norm/router */
    if (hp->n_expert > 0)
        per_layer += hp->n_expert * 3; /* g,u,d per expert contracts */
    if (hp->n_shared_expert > 0) per_layer += 3;
    return n + hp->n_layer * per_layer + 16;
}

cce_result cce_ds_map_build(cce_ds_map* map, const cce_ds_hparams* hp) {
    int cap, n = 0, L, e;
    cce_ds_leaf* leaves;

    if (!map || !hp || hp->n_layer < 1 || hp->d_model < 1)
        return CCE_ERR_INVALID_ARG;
    memset(map, 0, sizeof(*map));
    map->hp = *hp;
    cap = map_capacity(hp);
    leaves = (cce_ds_leaf*)calloc((size_t)cap, sizeof(cce_ds_leaf));
    if (!leaves) return CCE_ERR_OOM;

    /* ---- TRUNK ---- */
    leaf_set(&leaves[n++], CCE_DS_ROLE_TRUNK_EMBED, CCE_DS_RES_HOT, -1, -1, 1, hp);
    leaf_set(&leaves[n++], CCE_DS_ROLE_TRUNK_NORM,  CCE_DS_RES_HOT, -1, -1, 1, hp);
    leaf_set(&leaves[n++], CCE_DS_ROLE_TRUNK_HEAD,  CCE_DS_RES_HOT, -1, -1, 1, hp);
    leaf_set(&leaves[n++], CCE_DS_ROLE_TRUNK_ROPE,  CCE_DS_RES_HOT, -1, -1, 0, hp);
    if (hp->has_mtp)
        leaf_set(&leaves[n++], CCE_DS_ROLE_TRUNK_MTP, CCE_DS_RES_WARM, -1, -1, 0, hp);
    map->n_trunk = n;

    map->n_attn_per_layer = 0;
    map->n_ffn_per_layer = 0;

    for (L = 0; L < hp->n_layer; ++L) {
        int n0 = n;
        /* Attention / MLA — HOT (always needed each token) */
        leaf_set(&leaves[n++], CCE_DS_ROLE_ATTN_NORM, CCE_DS_RES_HOT, L, -1, 1, hp);
        if (hp->q_lora_rank > 0) {
            leaf_set(&leaves[n++], CCE_DS_ROLE_MLA_Q_DOWN, CCE_DS_RES_HOT, L, -1, 1, hp);
            leaf_set(&leaves[n++], CCE_DS_ROLE_MLA_Q_NORM, CCE_DS_RES_HOT, L, -1, 0, hp);
        }
        leaf_set(&leaves[n++], CCE_DS_ROLE_MLA_Q_UP,   CCE_DS_RES_HOT, L, -1, 1, hp);
        leaf_set(&leaves[n++], CCE_DS_ROLE_MLA_KV_DOWN,CCE_DS_RES_HOT, L, -1, 1, hp);
        leaf_set(&leaves[n++], CCE_DS_ROLE_MLA_KV_NORM,CCE_DS_RES_HOT, L, -1, 0, hp);
        leaf_set(&leaves[n++], CCE_DS_ROLE_MLA_KV_UP,  CCE_DS_RES_HOT, L, -1, 1, hp);
        leaf_set(&leaves[n++], CCE_DS_ROLE_MLA_O,      CCE_DS_RES_HOT, L, -1, 1, hp);
        map->n_attn_per_layer = n - n0;

        /* FFN */
        n0 = n;
        leaf_set(&leaves[n++], CCE_DS_ROLE_FFN_NORM, CCE_DS_RES_HOT, L, -1, 1, hp);
        if (hp->n_expert > 0) {
            leaf_set(&leaves[n++], CCE_DS_ROLE_FFN_ROUTER, CCE_DS_RES_HOT, L, -1, 1, hp);
            if (hp->n_shared_expert > 0)
                leaf_set(&leaves[n++], CCE_DS_ROLE_FFN_SHARED, CCE_DS_RES_WARM, L, -1, 0, hp);
            /* Expert leaves: COLD contracts (store-backed demand load).
             * GGUF alias points at fused banks; CNET name is per-expert. */
            for (e = 0; e < hp->n_expert; ++e) {
                leaf_set(&leaves[n++], CCE_DS_ROLE_FFN_EXPERT_GATE, CCE_DS_RES_COLD, L, e, 1, hp);
                leaf_set(&leaves[n++], CCE_DS_ROLE_FFN_EXPERT_UP,   CCE_DS_RES_COLD, L, e, 1, hp);
                leaf_set(&leaves[n++], CCE_DS_ROLE_FFN_EXPERT_DOWN, CCE_DS_RES_COLD, L, e, 1, hp);
                if (n >= cap) break;
            }
        } else {
            /* Dense FFN: reuse expert slots as gate/up/down with expert=-1
             * and warm residency — map as single shared FFN via routerless. */
            leaf_set(&leaves[n++], CCE_DS_ROLE_FFN_EXPERT_GATE, CCE_DS_RES_HOT, L, 0, 1, hp);
            snprintf(leaves[n-1].cnet, sizeof(leaves[n-1].cnet), "L%02d.ffn.gate", L);
            snprintf(leaves[n-1].gguf, sizeof(leaves[n-1].gguf), "blk.%d.ffn_gate.weight", L);
            leaf_set(&leaves[n++], CCE_DS_ROLE_FFN_EXPERT_UP, CCE_DS_RES_HOT, L, 0, 1, hp);
            snprintf(leaves[n-1].cnet, sizeof(leaves[n-1].cnet), "L%02d.ffn.up", L);
            snprintf(leaves[n-1].gguf, sizeof(leaves[n-1].gguf), "blk.%d.ffn_up.weight", L);
            leaf_set(&leaves[n++], CCE_DS_ROLE_FFN_EXPERT_DOWN, CCE_DS_RES_HOT, L, 0, 1, hp);
            snprintf(leaves[n-1].cnet, sizeof(leaves[n-1].cnet), "L%02d.ffn.down", L);
            snprintf(leaves[n-1].gguf, sizeof(leaves[n-1].gguf), "blk.%d.ffn_down.weight", L);
        }
        map->n_ffn_per_layer = n - n0;
        if (n >= cap) break;
    }

    map->leaves = leaves;
    map->n_leaves = n;
    map->capacity = cap;
    return CCE_OK;
}

void cce_ds_map_free(cce_ds_map* map) {
    if (!map) return;
    free(map->leaves);
    memset(map, 0, sizeof(*map));
}

const cce_ds_leaf* cce_ds_map_find_cnet(const cce_ds_map* map, const char* cnet) {
    int i;
    if (!map || !cnet) return NULL;
    for (i = 0; i < map->n_leaves; ++i)
        if (strcmp(map->leaves[i].cnet, cnet) == 0) return &map->leaves[i];
    return NULL;
}

const cce_ds_leaf* cce_ds_map_find_gguf(const cce_ds_map* map, const char* gguf) {
    int i;
    if (!map || !gguf) return NULL;
    for (i = 0; i < map->n_leaves; ++i)
        if (map->leaves[i].gguf[0] && strcmp(map->leaves[i].gguf, gguf) == 0)
            return &map->leaves[i];
    return NULL;
}

const cce_ds_leaf* cce_ds_map_find_role(const cce_ds_map* map, cce_ds_role role,
                                        int layer, int expert) {
    int i;
    if (!map) return NULL;
    for (i = 0; i < map->n_leaves; ++i) {
        const cce_ds_leaf* L = &map->leaves[i];
        if (L->role != role) continue;
        if (layer >= 0 && L->layer != layer) continue;
        if (expert >= 0 && L->expert != expert) continue;
        return L;
    }
    return NULL;
}

cce_result cce_ds_map_validate(const cce_ds_map* map, char* err, int err_n) {
    int i, j;
    if (!map || !map->leaves || map->n_leaves < 1) {
        if (err && err_n) snprintf(err, err_n, "empty map");
        return CCE_ERR_INVALID_ARG;
    }
    for (i = 0; i < map->n_leaves; ++i) {
        const cce_ds_leaf* A = &map->leaves[i];
        if (!A->cnet[0]) {
            if (err && err_n) snprintf(err, err_n, "leaf %d empty cnet", i);
            return CCE_ERR_INVALID_ARG;
        }
        if ((int)strlen(A->cnet) >= 64) {
            if (err && err_n) snprintf(err, err_n, "cnet too long: %s", A->cnet);
            return CCE_ERR_INVALID_ARG;
        }
        for (j = i + 1; j < map->n_leaves; ++j) {
            if (strcmp(A->cnet, map->leaves[j].cnet) == 0) {
                if (err && err_n)
                    snprintf(err, err_n, "duplicate cnet %s", A->cnet);
                return CCE_ERR_INVALID_ARG;
            }
        }
        if (A->required && A->dim_in <= 0 && A->role != CCE_DS_ROLE_TRUNK_ROPE &&
            A->role != CCE_DS_ROLE_TRUNK_MTP) {
            /* some roles have 0 dim by design; skip soft norms */
            if (A->role == CCE_DS_ROLE_MLA_Q_DOWN && map->hp.q_lora_rank <= 0)
                continue;
        }
    }
    return CCE_OK;
}

void cce_ds_map_residency_counts(const cce_ds_map* map,
                                 int* hot, int* warm, int* cold) {
    int i, h = 0, w = 0, c = 0;
    if (!map) { if (hot) *hot = 0; if (warm) *warm = 0; if (cold) *cold = 0; return; }
    for (i = 0; i < map->n_leaves; ++i) {
        if (map->leaves[i].res == CCE_DS_RES_HOT) h++;
        else if (map->leaves[i].res == CCE_DS_RES_WARM) w++;
        else c++;
    }
    if (hot) *hot = h;
    if (warm) *warm = w;
    if (cold) *cold = c;
}

cce_result cce_ds_map_bind_check(const cce_ds_map* map,
                                 cce_ds_has_tensor_fn has,
                                 void* ctx,
                                 cce_ds_bind_report* out) {
    int i;
    cce_ds_bind_report r = {1, 0, 0, 0, -1};
    if (!map) return CCE_ERR_INVALID_ARG;
    for (i = 0; i < map->n_leaves; ++i) {
        const cce_ds_leaf* L = &map->leaves[i];
        int present = 1;
        if (has && L->gguf[0])
            present = has(ctx, L->gguf) ? 1 : 0;
        if (present) {
            r.present++;
        } else if (L->required) {
            r.missing_required++;
            r.ok = 0;
            if (r.first_missing_leaf < 0) r.first_missing_leaf = i;
        } else {
            r.missing_optional++;
        }
    }
    if (out) *out = r;
    return r.ok ? CCE_OK : CCE_ERR_NOT_FOUND;
}

/* ================= Real forest bind + CNET pack (isolated) ================= */

int cce_ds_role_is_linear(cce_ds_role role) {
    switch (role) {
    case CCE_DS_ROLE_TRUNK_EMBED:
    case CCE_DS_ROLE_TRUNK_HEAD:
    case CCE_DS_ROLE_MLA_Q_DOWN:
    case CCE_DS_ROLE_MLA_Q_UP:
    case CCE_DS_ROLE_MLA_KV_DOWN:
    case CCE_DS_ROLE_MLA_KV_UP:
    case CCE_DS_ROLE_MLA_O:
    case CCE_DS_ROLE_FFN_ROUTER:
    case CCE_DS_ROLE_FFN_SHARED:
    case CCE_DS_ROLE_FFN_EXPERT_GATE:
    case CCE_DS_ROLE_FFN_EXPERT_UP:
    case CCE_DS_ROLE_FFN_EXPERT_DOWN:
        return 1;
    default:
        return 0; /* norms, rope, mtp tables — contract-only unless extended */
    }
}

void cce_ds_bind_opts_default(cce_ds_bind_opts* opts, const char* archive_path) {
    if (!opts) return;
    memset(opts, 0, sizeof(*opts));
    opts->archive_path = archive_path;
    opts->bind_hot = 1;
    opts->bind_warm = 1;
    opts->bind_cold = 0; /* experts demand-loaded later */
    opts->synthetic = 1;
    opts->seed = 0xC0E7u;
    opts->init_scale = 0.02f;
    opts->wire_connections = 1;
}

static int ds_res_allowed(const cce_ds_bind_opts* o, cce_ds_residency r) {
    if (r == CCE_DS_RES_HOT) return o->bind_hot;
    if (r == CCE_DS_RES_WARM) return o->bind_warm;
    return o->bind_cold;
}

static uint32_t ds_lcg(uint32_t* s) {
    *s = *s * 1664525u + 1013904223u;
    return *s;
}

static void ds_fill_synthetic(float* w, int in_d, int out_d, uint32_t seed) {
    /* cce_block layout: weights[i*out + o], shape [in,out] */
    int i, o;
    uint32_t s = seed;
    float scale = 0.02f / sqrtf((float)(in_d > 0 ? in_d : 1));
    for (i = 0; i < in_d; ++i)
        for (o = 0; o < out_d; ++o) {
            float u = (float)(ds_lcg(&s) >> 8) / (float)(1u << 24);
            w[(size_t)i * out_d + o] = (u * 2.0f - 1.0f) * scale;
        }
}

static cce_result ds_add_linear_cascade(cce_forest* forest, const char* name,
                                        int in_d, int out_d, const float* w_data,
                                        float init_scale) {
    cce_cascade* cas = NULL;
    cce_result rc;
    int idx = -1;
    cce_block* blk;
    size_t ne;

    if (!forest || !name || in_d < 1 || out_d < 1) return CCE_ERR_INVALID_ARG;
    if (cce_cascade_create(&cas, 2) != CCE_OK) return CCE_ERR_OOM;
    rc = cce_cascade_add_linear_head(cas, in_d, out_d, init_scale);
    if (rc != CCE_OK) {
        cce_cascade_destroy(cas);
        return rc;
    }
    blk = &cas->blocks[cas->num_blocks - 1];
    ne = (size_t)in_d * (size_t)out_d;
    if (w_data && blk->weights.data && blk->weights.numel >= ne)
        memcpy(blk->weights.data, w_data, ne * sizeof(float));
    /* Inference-only: drop Adam buffers (same as oracle GGUF path). */
    cce_tensor_free(&blk->momentum_weights);
    cce_tensor_free(&blk->momentum_bias);
    cce_tensor_free(&blk->second_moment_w);
    cce_tensor_free(&blk->second_moment_b);
    cce_block_freeze(blk);

    rc = cce_forest_add_cascade_branch(forest, cas, name, &idx);
    if (rc != CCE_OK) {
        cce_cascade_destroy(cas);
        return rc;
    }
    /* add_branch copies cascade then mark_cascade_moved zeros the shell */
    free(cas);
    return CCE_OK;
}

cce_result cce_ds_map_bind_forest(const cce_ds_map* map,
                                  cce_forest** out_forest,
                                  const cce_ds_bind_opts* opts,
                                  cce_ds_bind_result* result) {
    cce_ds_bind_opts local;
    cce_forest* forest = NULL;
    cce_ds_bind_result r;
    int i, maxb;
    cce_result rc;

    memset(&r, 0, sizeof(r));
    r.first_fail_leaf = -1;
    if (!map || !out_forest) return CCE_ERR_INVALID_ARG;
    if (!opts) {
        cce_ds_bind_opts_default(&local, "ds_forest.cce");
        opts = &local;
    }
    if (!opts->archive_path || !opts->archive_path[0])
        return CCE_ERR_INVALID_ARG;

    maxb = opts->max_branches > 0 ? opts->max_branches : map->n_leaves + 32;
    remove(opts->archive_path);
    rc = cce_forest_open(&forest, opts->archive_path, maxb);
    if (rc != CCE_OK || !forest) return CCE_ERR_IO;

    for (i = 0; i < map->n_leaves; ++i) {
        const cce_ds_leaf* L = &map->leaves[i];
        float* wbuf = NULL;
        int in_d = L->dim_in, out_d = L->dim_out;

        if (!cce_ds_role_is_linear(L->role) || in_d < 1 || out_d < 1) {
            r.skipped_norm++;
            continue;
        }
        if (!ds_res_allowed(opts, L->res)) {
            r.skipped_cold++;
            continue;
        }

        if (opts->load_weight) {
            rc = opts->load_weight(opts->load_ctx, L, &wbuf, &in_d, &out_d);
            if (rc != CCE_OK) {
                if (opts->synthetic) {
                    wbuf = (float*)malloc((size_t)in_d * out_d * sizeof(float));
                    if (!wbuf) { r.failed++; goto fail; }
                    ds_fill_synthetic(wbuf, in_d, out_d,
                                      opts->seed ^ (uint32_t)(i * 2654435761u));
                } else if (L->required) {
                    r.failed++;
                    r.first_fail_leaf = i;
                    snprintf(r.first_fail_cnet, sizeof r.first_fail_cnet, "%s",
                             L->cnet);
                    goto fail;
                } else {
                    r.skipped_missing++;
                    continue;
                }
            }
        } else if (opts->synthetic) {
            wbuf = (float*)malloc((size_t)in_d * out_d * sizeof(float));
            if (!wbuf) { r.failed++; goto fail; }
            ds_fill_synthetic(wbuf, in_d, out_d,
                              opts->seed ^ (uint32_t)(i * 2654435761u));
        } else {
            r.skipped_missing++;
            if (L->required) {
                r.failed++;
                r.first_fail_leaf = i;
                snprintf(r.first_fail_cnet, sizeof r.first_fail_cnet, "%s", L->cnet);
                goto fail;
            }
            continue;
        }

        rc = ds_add_linear_cascade(forest, L->cnet, in_d, out_d, wbuf,
                                   opts->init_scale > 0 ? opts->init_scale : 0.02f);
        free(wbuf);
        if (rc != CCE_OK) {
            r.failed++;
            r.first_fail_leaf = i;
            snprintf(r.first_fail_cnet, sizeof r.first_fail_cnet, "%s", L->cnet);
            goto fail;
        }
        r.bound++;
    }

    /* Structure: MLA up specializes down; FFN route specializes MLA o */
    if (opts->wire_connections && map->hp.n_layer > 0) {
        int L, b;
        for (L = 0; L < map->hp.n_layer; ++L) {
            char a[64], c[64];
            int ia = -1, ic = -1;
            snprintf(a, sizeof a, "L%02d.mla.kv_dn", L);
            snprintf(c, sizeof c, "L%02d.mla.kv_up", L);
            for (b = 0; b < forest->num_branches; ++b) {
                if (strcmp(forest->branches[b].name, a) == 0) ia = b;
                if (strcmp(forest->branches[b].name, c) == 0) ic = b;
            }
            if (ia >= 0 && ic >= 0) {
                cce_branch* br = &forest->branches[ic];
                if (br->num_connections < 8) {
                    snprintf(br->conn_names[br->num_connections], 64, "%s", a);
                    br->conn_types[br->num_connections] = 3; /* specializes */
                    br->num_connections++;
                }
            }
            snprintf(a, sizeof a, "L%02d.mla.o", L);
            snprintf(c, sizeof c, "L%02d.ffn.route", L);
            ia = ic = -1;
            for (b = 0; b < forest->num_branches; ++b) {
                if (strcmp(forest->branches[b].name, a) == 0) ia = b;
                if (strcmp(forest->branches[b].name, c) == 0) ic = b;
            }
            if (ia >= 0 && ic >= 0) {
                cce_branch* br = &forest->branches[ic];
                if (br->num_connections < 8) {
                    snprintf(br->conn_names[br->num_connections], 64, "%s", a);
                    br->conn_types[br->num_connections] = 2; /* composes */
                    br->num_connections++;
                }
            }
        }
    }

    *out_forest = forest;
    if (result) *result = r;
    return CCE_OK;

fail:
    cce_forest_close(forest);
    if (result) *result = r;
    return CCE_ERR_IO;
}

/* ---- CNET pack (.cnetpack) ---- */

#define CNPK_MAGIC 0x4B504E43u /* "CNPK" le */
#define CNPK_VER   1u

struct cce_ds_pack {
    FILE* f;
    char  path[512];
};

typedef struct {
    char     name[64];
    int32_t  dim_in;
    int32_t  dim_out;
    uint64_t off;   /* file offset of f32 payload */
    uint64_t nbytes;
} cnpk_entry;

cce_result cce_ds_pack_write(const char* path, const cce_ds_map* map,
                             cce_ds_load_weight_fn load, void* ctx) {
    FILE* f;
    uint32_t magic = CNPK_MAGIC, ver = CNPK_VER, n = 0;
    int i;
    if (!path || !map) return CCE_ERR_INVALID_ARG;
    f = fopen(path, "wb");
    if (!f) return CCE_ERR_IO;
    /* Audit b5c268a (LOW-MED): check every fwrite return so a mid-stream
     * disk-full / write-failure surfaces as CCE_ERR_IO rather than a
     * silently truncated .cnetpack that later loads with wrong numel. */
    #define PW_WRITE(ptr, sz, cnt) do { \
        if (fwrite((ptr), (sz), (cnt), f) != (cnt)) { \
            fclose(f); remove(path); return CCE_ERR_IO; \
        } \
    } while (0)
    PW_WRITE(&magic, 4, 1);
    PW_WRITE(&ver, 4, 1);
    PW_WRITE(&n, 4, 1); /* patch later */

    for (i = 0; i < map->n_leaves; ++i) {
        const cce_ds_leaf* L = &map->leaves[i];
        float* w = NULL;
        int in_d = L->dim_in, out_d = L->dim_out;
        uint64_t nbytes;
        cce_result rc;
        if (!cce_ds_role_is_linear(L->role) || in_d < 1 || out_d < 1) continue;
        if (load) {
            rc = load(ctx, L, &w, &in_d, &out_d);
            if (rc != CCE_OK || !w) continue;
        } else {
            w = (float*)malloc((size_t)in_d * out_d * sizeof(float));
            if (!w) { fclose(f); remove(path); return CCE_ERR_OOM; }
            ds_fill_synthetic(w, in_d, out_d, 0xC0FFEE01u ^ (uint32_t)i);
        }
        nbytes = (uint64_t)in_d * (uint64_t)out_d * sizeof(float);
        PW_WRITE(L->cnet, 1, 64);
        {
            int32_t di = in_d, dout = out_d;
            PW_WRITE(&di, 4, 1);
            PW_WRITE(&dout, 4, 1);
        }
        PW_WRITE(&nbytes, 8, 1);
        PW_WRITE(w, 1, (size_t)nbytes);
        free(w);
        n++;
    }
    if (fseek(f, 8, SEEK_SET) != 0) { fclose(f); remove(path); return CCE_ERR_IO; }
    PW_WRITE(&n, 4, 1);
    #undef PW_WRITE
    if (fclose(f) != 0) { remove(path); return CCE_ERR_IO; }
    return CCE_OK;
}

cce_result cce_ds_pack_open(cce_ds_pack** out, const char* path) {
    cce_ds_pack* p;
    uint32_t magic = 0, ver = 0;
    if (!out || !path) return CCE_ERR_INVALID_ARG;
    p = (cce_ds_pack*)calloc(1, sizeof(*p));
    if (!p) return CCE_ERR_OOM;
    p->f = fopen(path, "rb");
    if (!p->f) { free(p); return CCE_ERR_IO; }
    if (fread(&magic, 4, 1, p->f) != 1 || magic != CNPK_MAGIC ||
        fread(&ver, 4, 1, p->f) != 1 || ver != CNPK_VER) {
        fclose(p->f);
        free(p);
        return CCE_ERR_UNSUPPORTED;
    }
    snprintf(p->path, sizeof p->path, "%s", path);
    *out = p;
    return CCE_OK;
}

void cce_ds_pack_close(cce_ds_pack* p) {
    if (!p) return;
    if (p->f) fclose(p->f);
    free(p);
}

cce_result cce_ds_pack_load_weight(void* pack_ctx, const cce_ds_leaf* leaf,
                                   float** out_w, int* out_in, int* out_out) {
    cce_ds_pack* p = (cce_ds_pack*)pack_ctx;
    uint32_t n = 0, i;
    if (!p || !p->f || !leaf || !out_w) return CCE_ERR_INVALID_ARG;
    *out_w = NULL;
    fseek(p->f, 8, SEEK_SET);
    if (fread(&n, 4, 1, p->f) != 1) return CCE_ERR_IO;
    for (i = 0; i < n; ++i) {
        /* The on-disk name field is a fixed 64 bytes with no guaranteed
           terminator; the extra byte keeps strcmp inside the buffer. */
        char name[65];
        int32_t di, dout;
        uint64_t nbytes;
        if (fread(name, 1, 64, p->f) != 64) return CCE_ERR_IO;
        name[64] = '\0';
        if (fread(&di, 4, 1, p->f) != 1 || fread(&dout, 4, 1, p->f) != 1 ||
            fread(&nbytes, 8, 1, p->f) != 1)
            return CCE_ERR_IO;
        if (strcmp(name, leaf->cnet) == 0) {
            float* w = (float*)malloc((size_t)nbytes);
            if (!w) return CCE_ERR_OOM;
            if (fread(w, 1, (size_t)nbytes, p->f) != (size_t)nbytes) {
                free(w);
                return CCE_ERR_IO;
            }
            *out_w = w;
            if (out_in) *out_in = di;
            if (out_out) *out_out = dout;
            return CCE_OK;
        }
        fseek(p->f, (long)nbytes, SEEK_CUR);
    }
    return CCE_ERR_NOT_FOUND;
}

/* Optional GGUF import — CNET owns the reader; DeepSeek/llama.cpp not linked. */
cce_result cce_ds_gguf_load_weight(void* gguf_ctx, const cce_ds_leaf* leaf,
                                   float** out_w, int* out_in, int* out_out) {
    cce_gguf* g = (cce_gguf*)gguf_ctx;
    cce_tensor t = {0};
    cce_result rc;
    int in_d = 0, out_d = 0;
    int have_contract;
    float* w;
    size_t ne;
    if (!g || !leaf || !out_w) return CCE_ERR_INVALID_ARG;
    *out_w = NULL;
    if (!leaf->gguf[0]) return CCE_ERR_NOT_FOUND;
    rc = cce_gguf_load_tensor_by_name(g, leaf->gguf, &t);
    if (rc != CCE_OK || t.ndim != 2) {
        cce_tensor_free(&t);
        return CCE_ERR_NOT_FOUND;
    }
    /* GGUF/HF often [out,in]; cce_block wants [in,out]. Transpose on copy.
     *
     * Fail-closed dim selection (audit b5c268a): the old code fell through to
     * out_d=shape[0], in_d=shape[1] when neither orientation matched the
     * leaf contract, silently returning a wrong-shaped weight. Now we
     * require either the contract dims OR an explicit orientation match;
     * if neither holds we reject with CCE_ERR_UNSUPPORTED rather than
     * fabricate dims. */
    have_contract = (leaf->dim_in > 0 && leaf->dim_out > 0);
    if (have_contract) {
        if (t.shape[0] == leaf->dim_out && t.shape[1] == leaf->dim_in) {
            out_d = leaf->dim_out; in_d = leaf->dim_in;        /* [out,in] */
        } else if (t.shape[0] == leaf->dim_in && t.shape[1] == leaf->dim_out) {
            in_d = leaf->dim_in; out_d = leaf->dim_out;        /* [in,out] */
        } else {
            /* Neither orientation matches the contract → REJECT.
             * Returning wrong-shaped weights would later corrupt the forest
             * cascade dims and silently break MLA/MoE forward. */
            cce_tensor_free(&t);
            return CCE_ERR_UNSUPPORTED;
        }
    } else {
        /* No contract dims to validate against — keep the legacy best-effort
         * behavior (out=shape[0], in=shape[1]) so untyped leaves still load. */
        out_d = t.shape[0];
        in_d = t.shape[1];
        if (in_d < 1 || out_d < 1) {
            in_d = t.shape[0]; out_d = t.shape[1];
        }
        if (in_d < 1 || out_d < 1) {
            cce_tensor_free(&t);
            return CCE_ERR_UNSUPPORTED;
        }
    }
    if (t.numel != (size_t)in_d * (size_t)out_d) {
        /* shape/numel mismatch — corrupted or truncated tensor. */
        cce_tensor_free(&t);
        return CCE_ERR_UNSUPPORTED;
    }
    ne = (size_t)in_d * (size_t)out_d;
    w = (float*)malloc(ne * sizeof(float));
    if (!w) {
        cce_tensor_free(&t);
        return CCE_ERR_OOM;
    }
    if (t.shape[0] == out_d && t.shape[1] == in_d) {
        /* source [out,in] → dest [in,out] */
        int o, i;
        for (i = 0; i < in_d; ++i)
            for (o = 0; o < out_d; ++o)
                w[(size_t)i * out_d + o] = t.data[(size_t)o * in_d + i];
    } else {
        memcpy(w, t.data, ne * sizeof(float));
    }
    cce_tensor_free(&t);
    *out_w = w;
    if (out_in) *out_in = in_d;
    if (out_out) *out_out = out_d;
    return CCE_OK;
}
