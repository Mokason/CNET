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
    int             synthetic;     /* 1: hermetic random weights (no real
                                   *   .cnetpack / GGUF model bound). Gates
                                   *   MTP random draft-weight arming. */
    float           moe_sleep_eps; /* sleep on expert mix */
    int             dual_pipe;     /* batch-ensure experts then fire (overlap-ready) */
    /* MTP speculative (Forest draft branches) */
    int             mtp_k;         /* max draft tokens per step (0=off) */
    int             mtp_draft_layers; /* 1 = shallow draft; 0 = linear-only */
    float*          mtp_embed;     /* [vocab * d_model] synthetic token embeds */
    float*          mtp_head_w;    /* trunk.head [d_model * vocab] */
    float*          mtp_draft_w;   /* draft.mtp.0 [d_model * vocab] */
    float*          mtp_draft_A;   /* optional residual mixer [d_model*d_model] */
    int             mtp_ready;
    /* EP place: expert e → device bucket (e % ep_places) */
    int             ep_places;     /* 1 or 2 (dual-GPU role tags) */
    int*            expert_place;  /* [n_expert] device id */
    /* telemetry (Forest sparse-activate) */
    int             tokens_fwd;
    int             experts_loaded;   /* cold→resident lifetime */
    int             experts_fired;    /* expert matmuls this process */
    int             experts_slept;    /* top-k slots zeroed by sleep */
    int             experts_attempted;
    int             dsa_support_sum;
    int             dsa_full_fallback; /* heads that fell back to dense */
    int             mtp_drafted;
    int             mtp_accepted;
    int             mtp_rejected;
    int             mtp_main_steps;
    int             mtp_draft_steps;
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
    int         mtp_k;           /* 0=off, 2..4 typical */
    int         mtp_draft_layers;/* 0=linear draft, 1=one residual layer */
    int         ep_places;       /* 1 or 2 expert placement buckets */
} cce_ds_host_opts;

/* Speculative-step stats (one call may accept multiple tokens). */
typedef struct cce_ds_mtp_stats {
    int drafted;
    int accepted;
    int rejected;
    int main_steps;
    int draft_steps;
} cce_ds_mtp_stats;

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

/* Interior-layer adaptation hook: called after each layer's residual update in
   the DS forward, so a per-layer low-rank adapter (cce_lily) can add its delta
   to the residual stream. NULL => off, zero overhead — byte-identical decode
   unless armed. CCE-free signature so the DS runtime needs no adapter
   dependency; installed by cce_lily_install_serving. */
typedef void (*CceLayerAdaptHook)(int layer, float* residual, int width, void* ctx);
extern CceLayerAdaptHook g_cce_layer_adapt_hook;
extern void*             g_cce_layer_adapt_ctx;

/* Synthetic weight amplitude for cce_ds_host_open(synthetic=1). Default 0.02
   keeps existing synthetic hosts bit-identical; raise it to drive the forward
   (and any compute-quality gap) into a measurable regime. Set before open. */
void cce_ds_set_synth_scale(float s);

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

/* Enable Forest MTP leaves (trunk.head, draft.mtp.0, embeds). Idempotent. */
cce_result cce_ds_host_enable_mtp(cce_ds_host* h, int k, int draft_layers);

/* One speculative step: draft up to k tokens (cheap), verify with main.
 * Returns tokens accepted (>=1 if progress). */
cce_result cce_ds_host_forward_spec(cce_ds_host* h, int k,
                                    cce_ds_mtp_stats* step /*nullable*/);

/* Bench n target tokens with MTP on; fills wall tok/s and aggregate stats. */
cce_result cce_ds_host_bench_mtp(cce_ds_host* h, int n_tokens, double* out_tok_s,
                                 cce_ds_mtp_stats* total /*nullable*/);

#ifdef __cplusplus
}
#endif

#endif /* CCE_DS_RUNTIME_H */
