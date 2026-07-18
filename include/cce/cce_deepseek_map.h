#ifndef CCE_DEEPSEEK_MAP_H
#define CCE_DEEPSEEK_MAP_H

/*
 * CNET-native DeepSeek-style model map (forest-first, isolated)
 * ==============================================================
 * CNET does not depend on the DeepSeek repo, llama.cpp, or Python.
 * Forest vocabulary is the contract and the runtime identity:
 *
 *   TRUNK   — always-on spine (embed, final norm, lm_head, rope tables)
 *   BRANCH  — one decoder layer (layer index L)
 *   LEAF    — one forest-resident specialist cascade (linear/norm/router)
 *   FOREST  — whole model = set of named leaves under trunk + L*
 *   CONTRACT— role + dims + residency + requiredness for each leaf
 *
 * Optional interchange only:
 *   - GGUF tensor strings are *import aliases* (one-way), never runtime keys
 *   - CNET pack (.cnetpack) is the native leaf-weight container
 *
 * Name budget: cce_branch.name is 64 bytes — keep short, stable, parseable.
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

/* ---- Real forest bind (CNET-isolated) ----
 *
 * Materialize linear leaves as cascades in a cce_forest, named by cnet[].
 * Norm/rope leaves are contracts only (no 2D cascade) unless opts say so.
 *
 * Weight source priority:
 *   1. opts->load_weight callback (CNET pack, custom store, …)
 *   2. opts->synthetic != 0 → hermetic random weights (no external project)
 *   3. else fail if required linear leaf has no provider
 *
 * GGUF is NOT required. Use cce_ds_weight_provider_gguf only as optional import.
 */

typedef cce_result (*cce_ds_load_weight_fn)(void* ctx, const cce_ds_leaf* leaf,
                                            float** out_w, int* out_in, int* out_out);
/* out_w: row-major [out × in], caller frees with free() */

typedef struct cce_ds_bind_opts {
    const char* archive_path;   /* .cce path for forest open (required) */
    int         max_branches;   /* 0 → map->n_leaves + 16 */
    int         bind_hot;       /* 1 (default): bind HOT residency */
    int         bind_warm;      /* 1: bind WARM */
    int         bind_cold;      /* 0 default: skip COLD experts (demand-load later) */
    int         synthetic;      /* 1: fill missing weights with seeded random */
    uint32_t    seed;
    float       init_scale;     /* used only when creating empty then overwrite */
    cce_ds_load_weight_fn load_weight;
    void*       load_ctx;
    int         wire_connections; /* 1: layer leaf → specializes layer group */
} cce_ds_bind_opts;

typedef struct cce_ds_bind_result {
    int bound;              /* cascades successfully added */
    int skipped_norm;       /* contract-only 1D / non-linear */
    int skipped_cold;       /* residency filter */
    int skipped_missing;    /* no weight and not synthetic */
    int failed;
    int first_fail_leaf;    /* -1 ok */
    char first_fail_cnet[64];
} cce_ds_bind_result;

void cce_ds_bind_opts_default(cce_ds_bind_opts* opts, const char* archive_path);

/* Create forest and bind map leaves. *out_forest owned by caller (cce_forest_close). */
cce_result cce_ds_map_bind_forest(const cce_ds_map* map,
                                  cce_forest** out_forest,
                                  const cce_ds_bind_opts* opts,
                                  cce_ds_bind_result* result);

/* True if role is a 2D linear leaf (bindable cascade). */
int cce_ds_role_is_linear(cce_ds_role role);

/* ---- CNET native pack (isolated weight container, not GGUF) ----
 * Magic "CNPK" + version + per-leaf f32 matrices keyed by cnet name.
 */
cce_result cce_ds_pack_write(const char* path, const cce_ds_map* map,
                             cce_ds_load_weight_fn load, void* ctx);
/* load_weight for pack: ctx is FILE* opened by open helper, or pack handle */
typedef struct cce_ds_pack cce_ds_pack;
cce_result cce_ds_pack_open(cce_ds_pack** out, const char* path);
void       cce_ds_pack_close(cce_ds_pack* p);
cce_result cce_ds_pack_load_weight(void* pack_ctx, const cce_ds_leaf* leaf,
                                   float** out_w, int* out_in, int* out_out);

/* Optional GGUF import provider — implemented only if linked with cce_gguf.
 * Declared here; body in cce_deepseek_map.c uses weak/optional include.
 * Pass gguf* as ctx. Returns NOT_FOUND if tensor absent.
 */
cce_result cce_ds_gguf_load_weight(void* gguf_ctx, const cce_ds_leaf* leaf,
                                   float** out_w, int* out_in, int* out_out);

#ifdef __cplusplus
}
#endif

#endif /* CCE_DEEPSEEK_MAP_H */
