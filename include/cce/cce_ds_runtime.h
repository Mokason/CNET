#ifndef CCE_DS_RUNTIME_H
#define CCE_DS_RUNTIME_H

/*
 * CNET DeepSeek-style isolated runtime host
 * =========================================
 * map → forest (HOT) + pack (weights) → residual forward with:
 *   - MLA per layer (latent KV from forest leaves or synthetic)
 *   - optional DSA top-k on MLA scores
 *   - MoE router + demand-load COLD experts from pack
 *
 * No DeepSeek repo / no llama.cpp. GGUF only via optional import helper.
 */

#include "cce_deepseek_map.h"
#include "cce_mla.h"
#include "cce_dsa.h"
#include "cce_forest.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cce_ds_host {
    cce_ds_map      map;
    cce_forest*     forest;
    cce_ds_pack*    pack;          /* optional; required for cold expert load */
    cce_mla*        mla;           /* [n_layer] */
    int             n_layer;
    int             d_model;
    int             max_ctx;
    int             pos;           /* next write position */
    float*          residual;      /* [d_model] */
    float*          scratch;       /* [d_model] */
    float*          logits;        /* [vocab] if head bound */
    int             vocab;
    /* knobs */
    int             dsa_enable;
    float           dsa_fraction;
    float           dsa_sleep_eps;  /* sleep after DSA softmax */
    int             dsa_speed;      /* 1: floor grid (CNET_DSA_PROFILE=speed) */
    int             cold_autoload; /* 1: ensure_expert on route */
    int             mla_quant_kv;  /* int8 latent KV side (FP8-class BW) */
    float           moe_sleep_eps; /* sleep on expert mix */
    int             dual_pipe;     /* batch-ensure experts then fire (overlap-ready) */
    /* telemetry (Forest sparse-activate) */
    int             tokens_fwd;
    int             experts_loaded;   /* cold→resident lifetime */
    int             experts_fired;    /* expert matmuls this process */
    int             experts_slept;    /* top-k slots zeroed by sleep */
    int             experts_attempted;
    int             dsa_support_sum;
    int             dsa_full_fallback; /* heads that fell back to dense */
    double          seconds_fwd;
} cce_ds_host;

typedef struct cce_ds_host_opts {
    const char* archive_path;   /* forest .cce */
    const char* pack_path;      /* .cnetpack or NULL for pure synthetic */
    int         max_ctx;
    int         synthetic;      /* 1 if no pack */
    uint32_t    seed;
    int         bind_cold;      /* bind all experts at open (heavy) */
    int         dsa_enable;
    float       dsa_fraction;
    float       dsa_sleep_eps;
    int         dsa_speed;
    int         cold_autoload;
    int         mla_quant_kv;
    float       moe_sleep_eps;
    int         dual_pipe;
} cce_ds_host_opts;

void cce_ds_host_opts_default(cce_ds_host_opts* o, const char* archive,
                              const char* pack);

/* Open synthetic or pack-backed host. Owns map/forest/mla. */
cce_result cce_ds_host_open(cce_ds_host** out, const cce_ds_hparams* hp,
                            const cce_ds_host_opts* opts);
void       cce_ds_host_close(cce_ds_host* h);
void       cce_ds_host_reset(cce_ds_host* h);

/* Prefill one residual vector (e.g. from embed); then decode steps. */
cce_result cce_ds_host_set_input(cce_ds_host* h, const float* x, int d);
cce_result cce_ds_host_forward_token(cce_ds_host* h); /* residual += layers */

/* Demand-load one COLD expert leaf from pack into forest (idempotent). */
cce_result cce_ds_host_ensure_expert(cce_ds_host* h, int layer, int expert);

/* GGUF → hparams (best-effort; fills small defaults for missing). */
cce_result cce_ds_hparams_from_gguf(const void* gguf /*cce_gguf**/, cce_ds_hparams* hp);

/* Import GGUF → .cnetpack (+ optional forest archive). Returns CCE_OK if pack written. */
cce_result cce_ds_import_gguf(const char* gguf_path, const char* pack_path,
                              const char* forest_path /*nullable*/,
                              int bind_cold);

/* Microbench: T tokens residual-only (synthetic input), fill tok/s. */
cce_result cce_ds_host_bench(cce_ds_host* h, int n_tokens, double* out_tok_s);

#ifdef __cplusplus
}
#endif

#endif /* CCE_DS_RUNTIME_H */
