#ifndef CCE_DEEPSEEK_MAP_H
#define CCE_DEEPSEEK_MAP_H

/*
 * CNET-native DeepSeek GGUF tensor map
 * =====================================
 * We do NOT follow Python/HF module paths as the source of truth.
 * Forest vocabulary is the contract:
 *
 *   TRUNK   — always-on spine (embed, final norm, lm_head, rope tables)
 *   BRANCH  — one decoder layer (layer index L)
 *   LEAF    — one forest-resident specialist cascade (linear/norm/router)
 *   FOREST  — whole model = set of named leaves under trunk + L*
 *   CONTRACT— role + dims + residency + requiredness for each leaf
 *
 * GGUF tensor names (blk.N.attn_kv_a_mqa.weight, …) are *aliases* only —
 * import-side. Runtime always addresses leaves by CNET names.
 *
 * Name budget: cce_branch.name is 64 bytes — keep short, stable, parseable.
 *
 * Examples:
 *   trunk.embed
 *   trunk.norm
 *   trunk.head
 *   L03.attn.norm
 *   L03.mla.kv_dn      # latent compress W^{DKV}|k^R
 *   L03.mla.kv_up      # W^{UK}|W^{UV}
 *   L03.ffn.route
 *   L03.ffn.e012.g     # expert 12 gate
 */

#include "cce_defs.h"
#include "cce_forest.h"
#include "cce_mla.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Roles (contracts on leaves) ---- */
typedef enum cce_ds_role {
    CCE_DS_ROLE_NONE = 0,
    /* Trunk */
    CCE_DS_ROLE_TRUNK_EMBED = 1,
    CCE_DS_ROLE_TRUNK_NORM,
    CCE_DS_ROLE_TRUNK_HEAD,
    CCE_DS_ROLE_TRUNK_ROPE,
    CCE_DS_ROLE_TRUNK_MTP,       /* multi-token prediction head (optional) */
    /* Per-layer attention (MLA) */
    CCE_DS_ROLE_ATTN_NORM,
    CCE_DS_ROLE_MLA_Q_DOWN,      /* q_lora W^{DQ} */
    CCE_DS_ROLE_MLA_Q_NORM,
    CCE_DS_ROLE_MLA_Q_UP,        /* W^{UQ}|W^{QR} fused or split */
    CCE_DS_ROLE_MLA_KV_DOWN,     /* W^{DKV}|k^R  — cached latent path */
    CCE_DS_ROLE_MLA_KV_NORM,
    CCE_DS_ROLE_MLA_KV_UP,       /* W^{UK}|W^{UV} */
    CCE_DS_ROLE_MLA_O,
    /* Per-layer FFN / MoE */
    CCE_DS_ROLE_FFN_NORM,
    CCE_DS_ROLE_FFN_ROUTER,
    CCE_DS_ROLE_FFN_SHARED,      /* shared expert (if any) */
    CCE_DS_ROLE_FFN_EXPERT_GATE,
    CCE_DS_ROLE_FFN_EXPERT_UP,
    CCE_DS_ROLE_FFN_EXPERT_DOWN,
    CCE_DS_ROLE_COUNT
} cce_ds_role;

/* Residency policy for the leaf in the forest */
typedef enum cce_ds_residency {
    CCE_DS_RES_HOT  = 0,  /* always resident (trunk, routers, norms, MLA core) */
    CCE_DS_RES_WARM = 1,  /* mmap / pin_hot candidates */
    CCE_DS_RES_COLD = 2   /* demand-load experts (store-backed) */
} cce_ds_residency;

/* Hparams needed to expand the full map (from GGUF metadata or hand-set). */
typedef struct cce_ds_hparams {
    int n_layer;
    int d_model;
    int n_heads;
    int n_kv_heads;
    int qk_nope_head_dim;
    int qk_rope_head_dim;
    int v_head_dim;
    int kv_lora_rank;
    int q_lora_rank;       /* 0 = no Q compression */
    int n_expert;          /* 0 = dense FFN */
    int n_expert_used;
    int n_shared_expert;   /* DeepSeek shared experts */
    int n_ff_exp;          /* expert intermediate */
    int vocab;
    int has_mtp;
    float rope_theta;
    char arch[32];         /* "deepseek2", "deepseek3", … */
} cce_ds_hparams;

/* One leaf contract: forest name + GGUF alias + role + placement. */
typedef struct cce_ds_leaf {
    cce_ds_role      role;
    cce_ds_residency res;
    int              layer;     /* -1 trunk */
    int              expert;    /* -1 non-expert */
    int              required;  /* 1 = refuse load if missing */
    char             cnet[64];  /* forest branch name (canonical) */
    char             gguf[96];  /* primary GGUF tensor name (alias) */
    char             gguf_b[96];/* optional bias / second half */
    /* Dim contract: 0 = derive from hparams at bind time */
    int              dim_in;
    int              dim_out;
} cce_ds_leaf;

/* Full map for a model. leaves[] owned by this struct when built. */
typedef struct cce_ds_map {
    cce_ds_hparams hp;
    cce_ds_leaf*   leaves;
    int            n_leaves;
    int            n_trunk;
    int            n_attn_per_layer;
    int            n_ffn_per_layer;
    int            capacity;
} cce_ds_map;

/* Defaults resembling DeepSeek-V3 scale (for map shape tests, not weights). */
void cce_ds_hparams_default_v3(cce_ds_hparams* hp);
void cce_ds_hparams_default_small(cce_ds_hparams* hp); /* hermetic toy */

/* Build the full leaf table from hparams. Caller cce_ds_map_free. */
cce_result cce_ds_map_build(cce_ds_map* map, const cce_ds_hparams* hp);
void       cce_ds_map_free(cce_ds_map* map);

/* Lookup */
const cce_ds_leaf* cce_ds_map_find_cnet(const cce_ds_map* map, const char* cnet);
const cce_ds_leaf* cce_ds_map_find_gguf(const cce_ds_map* map, const char* gguf);
const cce_ds_leaf* cce_ds_map_find_role(const cce_ds_map* map, cce_ds_role role,
                                        int layer, int expert);

/* Fill CNET name for a role (snprintf into out[64]). */
int cce_ds_format_cnet(char* out, int out_n, cce_ds_role role,
                       int layer, int expert);

/* Fill primary GGUF alias (DeepSeek/llama.cpp convention). */
int cce_ds_format_gguf(char* out, int out_n, cce_ds_role role,
                       int layer, int expert, const cce_ds_hparams* hp);

/* Role string for logs/contracts. */
const char* cce_ds_role_name(cce_ds_role role);

/* Derive MLA config from hparams (feeds cce_mla). */
void cce_ds_hparams_to_mla(const cce_ds_hparams* hp, cce_mla_config* mla);

/* Validate map internal consistency (unique cnet names, dim > 0 for required). */
cce_result cce_ds_map_validate(const cce_ds_map* map, char* err, int err_n);

/* Count leaves by residency (for pin_hot / store planning). */
void cce_ds_map_residency_counts(const cce_ds_map* map,
                                 int* hot, int* warm, int* cold);

/*
 * Bind plan: walk map and report which GGUF tensors would attach to which
 * forest leaves. Does not load weights — pure naming contract check.
 * If gguf_has_tensor(name) is provided, missing required → CCE_ERR_NOT_FOUND.
 */
typedef int (*cce_ds_has_tensor_fn)(void* ctx, const char* gguf_name);

typedef struct cce_ds_bind_report {
    int ok;
    int present;
    int missing_optional;
    int missing_required;
    int first_missing_leaf; /* index into map->leaves, or -1 */
} cce_ds_bind_report;

cce_result cce_ds_map_bind_check(const cce_ds_map* map,
                                 cce_ds_has_tensor_fn has,
                                 void* ctx,
                                 cce_ds_bind_report* out);

#ifdef __cplusplus
}
#endif

#endif /* CCE_DEEPSEEK_MAP_H */
