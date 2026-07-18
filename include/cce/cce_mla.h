#ifndef CCE_MLA_H
#define CCE_MLA_H

/*
 * DeepSeek Multi-head Latent Attention (MLA) — CCE implementation.
 *
 * DeepSeek-V2/V3 MLA (paper + inference demos):
 *
 *   h → c^{KV} = W^{DKV} h          (kv_lora_rank)
 *       k^R    = RoPE(W^{KR} h)     (qk_rope_head_dim)  — shared across heads
 *   Cache only (c^{KV}, k^R) per token  ≪ full K/V heads.
 *
 *   q^C, q^R from W^{UQ}/W^{QR} (optional q_lora down-proj first)
 *   k^C, v   = W^{UK/UV} c^{KV}     (up-project latent on demand)
 *   Attention on concat(q^C,q^R) · concat(k^C,k^R), values = v
 *
 * Inference absorb: fold W^{UK} into q (no materialize of full K history)
 * when the nope path has no intermediate nonlinearity — we support both
 * explicit up-project and absorbed-dot paths.
 *
 * Single-user: huge KV memory win for long context without multi-tenant EP.
 */

#include "cce_defs.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cce_mla_config {
    int   n_heads;
    int   n_kv_heads;         /* usually == n_heads for MLA */
    int   qk_nope_head_dim;   /* content (non-RoPE) part of Q/K head */
    int   qk_rope_head_dim;   /* RoPE part; K rope shared MQA-style */
    int   v_head_dim;
    int   kv_lora_rank;       /* d_c — latent KV rank (cached) */
    int   q_lora_rank;        /* 0 = no Q compression */
    int   d_model;            /* hidden size */
    float rope_theta;         /* default 10000 */
    float attn_scale;         /* 0 → 1/sqrt(qk_nope+qk_rope) */
    int   use_absorb;         /* 1: absorbed q·c path for nope scores */
} cce_mla_config;

/* Host weight views (row-major). Not owned unless cce_mla_weights_alloc. */
typedef struct cce_mla_weights {
    /* Required */
    const float* w_dkv;   /* [d_model × (kv_lora_rank + qk_rope_head_dim)] */
    const float* w_uk;    /* [kv_lora_rank × (n_heads * qk_nope_head_dim)] */
    const float* w_uv;    /* [kv_lora_rank × (n_heads * v_head_dim)] */
    const float* w_kr;    /* optional if w_dkv packs k_rope; else [d_model × qk_rope] */
    /* Q path */
    const float* w_dq;    /* optional [d_model × q_lora_rank] if q_lora_rank>0 */
    const float* w_uq;    /* [q_in × (n_heads * qk_nope)] q_in = q_lora or d_model */
    const float* w_qr;    /* [q_in × (n_heads * qk_rope)] */
    const float* w_o;     /* [ (n_heads * v_head_dim) × d_model ] */
    /* Optional norms */
    const float* kv_norm; /* [kv_lora_rank] RMS weight or NULL */
    const float* q_norm;  /* [q_lora_rank] or NULL */
    float        rms_eps;
    int          owns;    /* free buffers on destroy */
} cce_mla_weights;

/* Compressed KV cache: per position only latent + shared rope key. */
typedef struct cce_mla_cache {
    int    max_ctx;
    int    cur_len;
    int    kv_lora_rank;
    int    qk_rope_head_dim;
    float* c_kv;   /* [max_ctx × kv_lora_rank] */
    float* k_pe;   /* [max_ctx × qk_rope_head_dim] */
} cce_mla_cache;

typedef struct cce_mla {
    cce_mla_config  cfg;
    cce_mla_weights w;
    cce_mla_cache   cache;
    /* scratch (owned) */
    float* q_nope;   /* [n_heads * qk_nope] */
    float* q_pe;     /* [n_heads * qk_rope] */
    float* k_nope;   /* [n_heads * qk_nope] for one pos up-project */
    float* v;        /* [n_heads * v_head_dim] */
    float* scores;   /* [max_ctx] */
    float* attn_out; /* [n_heads * v_head_dim] */
    float* out;      /* [d_model] */
    float* tmp;      /* [max(d_model, q_lora, kv_lora+rope)] */
} cce_mla;

/* Defaults matching DeepSeek-V3-ish small demo (not full 671B). */
void cce_mla_config_default(cce_mla_config* cfg, int d_model, int n_heads);

/* Bytes cached per token per layer (c_kv + k_pe). */
size_t cce_mla_cache_bytes_per_token(const cce_mla_config* cfg);

/* Compare to dense MHA cache: 2 * n_kv_heads * max(k_dim,v_dim) floats. */
size_t cce_mha_cache_bytes_per_token(int n_kv_heads, int head_dim);

cce_result cce_mla_init(cce_mla* m, const cce_mla_config* cfg,
                        const cce_mla_weights* w, int max_ctx);
void       cce_mla_free(cce_mla* m);
void       cce_mla_reset_cache(cce_mla* m);

/* Allocate + fill random orthonormal-ish synthetic weights (hermetic tests). */
cce_result cce_mla_weights_alloc_synthetic(cce_mla_weights* w,
                                           const cce_mla_config* cfg,
                                           uint32_t seed);
void       cce_mla_weights_free(cce_mla_weights* w);

/*
 * One-token MLA forward (append to cache, causal attend, write y[d_model]).
 * x: [d_model] current residual. pos: absolute position for RoPE.
 */
cce_result cce_mla_forward_token(cce_mla* m, const float* x, int pos,
                                 float* y);

/*
 * Prefill: T tokens x[T*d_model] → y[T*d_model], fills cache 0..T-1.
 */
cce_result cce_mla_forward_seq(cce_mla* m, const float* x, int T, float* y);

/* Apply RoPE in-place to pair-dim vector of length rope_dim at position pos. */
void cce_mla_apply_rope(float* x, int rope_dim, int pos, float theta);

/* Matvec: y[out] = W[in×out]^T-style row-major W as [out][in] * x[in]
 * i.e. y[o] = sum_i W[o*in + i] * x[i] */
void cce_mla_matvec(const float* W, const float* x, float* y, int in, int out);

#ifdef __cplusplus
}
#endif

#endif /* CCE_MLA_H */
