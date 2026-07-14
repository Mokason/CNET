#ifndef CCE_QWEN35_H
#define CCE_QWEN35_H

/* Native CPU execution core for Qwen3.5 (attention + Gated-DeltaNet hybrid).
 *
 * Qwen3.5 is NOT a pure transformer and NOT a Mamba/Jamba SSM. It interleaves
 * two mixer kinds over one residual stream, on a fixed layer schedule:
 *   - full-attention layers: joint Q+gate projection, per-head Q/K RMSNorm,
 *     (M)RoPE, causal GQA softmax attention, then a per-element sigmoid(gate)
 *     applied to the attention output, then the output projection.
 *   - Gated-DeltaNet recurrent linear-attention layers: mixed QKV projection,
 *     z gate, beta projection -> sigmoid, alpha projection + dt bias ->
 *     softplus -> multiply ssm_a (== -exp(A_log)), causal depthwise conv+SiLU,
 *     Q/K L2 norm, a recurrent Gated-Delta state update from Q/K/V, gated
 *     RMSNorm(out) * SiLU(z), then the output projection.
 * Default schedule: layer i is recurrent (DeltaNet) when (i+1) % interval != 0
 * and full attention otherwise (interval defaults to 4 -> a 3:1 recurrent:full
 * ratio), or an explicit per-layer recurrent mask.
 *
 * This header exposes the architecture-SPECIFIC compute cores that the generic
 * cce_hybrid (jamba/zamba mamba-1 SSM) runner deliberately does NOT provide:
 * the layer-schedule dispatch, the recurrent Gated-DeltaNet step, and the
 * Qwen3.5 gated causal attention. These are exercised by tests/cce_qwen35_test.c
 * against an independent scalar reference.
 *
 * HONEST BOUNDARY (see cce_qwen35_get_caps): this is a tested native execution
 * core + dispatch API operating on already-projected activations. It is NOT a
 * full arbitrary-quantized GGUF loader and NOT an end-to-end production model
 * runner. (M)RoPE and the causal short-conv+SiLU that precede the cores are the
 * caller's responsibility; the large q/k/v/o and in/out projections are applied
 * by the caller (e.g. via the existing cce_forest specialist cascades). The
 * recurrence here is the decode-time recurrent formulation, mathematically
 * equivalent to the chunked prefill kernel.
 *
 * Reference: llama.cpp src/models/qwen35.cpp (build_layer_attn /
 * build_layer_attn_linear) and transformers modeling_qwen3_5.py
 * (Qwen3_5Attention.forward, Qwen3_5GatedDeltaNet.forward,
 * torch_recurrent_gated_delta_rule).
 */

#include "cce_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Layer schedule / dispatch
 * ------------------------------------------------------------------------- */

typedef enum {
    CCE_QWEN35_LAYER_FULL_ATTN = 0, /* full (gated) attention layer */
    CCE_QWEN35_LAYER_DELTANET  = 1, /* Gated-DeltaNet recurrent linear-attn */
} cce_qwen35_layer_kind;

#define CCE_QWEN35_DEFAULT_FULL_ATTN_INTERVAL 4

/* Fill kinds_out[0..n_layer-1] with the default Qwen3.5 schedule: layer i is
 * CCE_QWEN35_LAYER_DELTANET when (i + 1) % full_attn_interval != 0, and
 * CCE_QWEN35_LAYER_FULL_ATTN otherwise. Requires n_layer >= 1 and
 * full_attn_interval >= 1. Returns CCE_ERR_INVALID_ARG on a NULL buffer or a
 * non-positive dimension. */
cce_result cce_qwen35_default_schedule(cce_qwen35_layer_kind *kinds_out,
                                       int n_layer, int full_attn_interval);

/* Fill kinds_out[0..n_layer-1] from an explicit recurrent mask: layer i is
 * DELTANET when recurrent_mask[i] != 0, else FULL_ATTN. This overrides the
 * default interval schedule. Requires non-NULL buffers and n_layer >= 1.
 * Returns CCE_ERR_INVALID_ARG otherwise. */
cce_result cce_qwen35_schedule_from_mask(cce_qwen35_layer_kind *kinds_out,
                                         const int *recurrent_mask, int n_layer);

/* 1 if the layer kind routes to the recurrent Gated-DeltaNet core, else 0. */
int cce_qwen35_layer_is_recurrent(cce_qwen35_layer_kind kind);

/* ---------------------------------------------------------------------------
 * Gated-DeltaNet recurrent core (persistent state)
 * ------------------------------------------------------------------------- */

/* Geometry + per-head gate parameters of one Gated-DeltaNet layer. num_k_heads
 * must be >= 1 and must divide num_v_heads (key/value head grouping: each value
 * head h reads key head h / (num_v_heads / num_k_heads)). dt_bias and ssm_a are
 * per-value-head [num_v_heads] and are COPIED into the state at init (not
 * aliased); ssm_a is the precomputed -exp(A_log) coefficient. */
typedef struct cce_qwen35_deltanet_config {
    int          num_v_heads;
    int          num_k_heads;
    int          head_k_dim;
    int          head_v_dim;
    float        l2_eps;   /* Q/K L2-norm epsilon (use 1e-6f) */
    const float *dt_bias;  /* [num_v_heads] softplus bias */
    const float *ssm_a;    /* [num_v_heads] == -exp(A_log) */
} cce_qwen35_deltanet_config;

/* Persistent recurrent state for one Gated-DeltaNet layer. Owns its parameter
 * copies and the [num_v_heads][head_k_dim][head_v_dim] delta state. */
typedef struct cce_qwen35_deltanet_state {
    int    num_v_heads;
    int    num_k_heads;
    int    head_k_dim;
    int    head_v_dim;
    float  l2_eps;
    float *dt_bias; /* owned [num_v_heads] */
    float *ssm_a;   /* owned [num_v_heads] */
    float *state;   /* owned [num_v_heads * head_k_dim * head_v_dim], row-major
                       per head as [head_k_dim][head_v_dim] */
    long   steps;   /* number of steps applied since the last reset */
} cce_qwen35_deltanet_state;

/* Validate cfg and allocate zeroed state. Returns CCE_ERR_INVALID_ARG on a bad
 * contract (NULL args, non-positive dims, num_k_heads not dividing num_v_heads,
 * NULL dt_bias/ssm_a) and CCE_ERR_OOM on allocation failure. */
cce_result cce_qwen35_deltanet_init(cce_qwen35_deltanet_state *st,
                                    const cce_qwen35_deltanet_config *cfg);

/* Zero the delta state and the step counter (parameters are kept). */
void cce_qwen35_deltanet_reset(cce_qwen35_deltanet_state *st);

/* Release owned buffers and zero the struct. Safe on a zeroed/failed state. */
void cce_qwen35_deltanet_free(cce_qwen35_deltanet_state *st);

/* Advance one token through the Gated-DeltaNet recurrence and read out this
 * token's core attention output. All inputs are the post-conv+SiLU, pre-core
 * activations for a single token:
 *   q, k   : [num_k_heads * head_k_dim]  (L2-normed and Q-scaled internally)
 *   v      : [num_v_heads * head_v_dim]
 *   alpha  : [num_v_heads]  raw a; g = ssm_a * softplus(alpha + dt_bias)
 *   beta   : [num_v_heads]  raw b; write gate = sigmoid(beta)
 *   out    : [num_v_heads * head_v_dim]  written; not read
 * The state (st->state) is decayed by exp(g), delta-updated from k/v, and read
 * with q. No caller-owned input buffer is mutated. Returns CCE_ERR_INVALID_ARG
 * on NULL args. */
cce_result cce_qwen35_deltanet_step(cce_qwen35_deltanet_state *st,
                                    const float *q, const float *k,
                                    const float *v, const float *alpha,
                                    const float *beta, float *out);

/* ---------------------------------------------------------------------------
 * Gated causal attention core (full-attention Qwen3.5 layers)
 * ------------------------------------------------------------------------- */

/* n_kv_head must be >= 1 and divide n_head (GQA grouping). */
typedef struct cce_qwen35_attn_config {
    int   n_head;
    int   n_kv_head;
    int   head_dim;
    float rms_eps; /* Q/K RMSNorm epsilon (use 1e-6f) */
} cce_qwen35_attn_config;

/* Gated causal multi-head attention over a single sequence of n_tokens.
 * Layouts (token-major, then head, then dim):
 *   q, gate : [n_tokens * n_head    * head_dim]
 *   k, v    : [n_tokens * n_kv_head * head_dim]
 *   q_norm_w, k_norm_w : [head_dim]  per-head RMSNorm weights
 *   out     : [n_tokens * n_head    * head_dim]  written
 * Per query head h: RMSNorm(q) and RMSNorm(k) over head_dim, causal softmax
 * with scale 1/sqrt(head_dim) over key head h/(n_head/n_kv_head), context from
 * v, then out = context * sigmoid(gate) (the Qwen3.5 output gate). (M)RoPE, if
 * used, must already be applied to q and k by the caller. No input buffer is
 * mutated. Returns CCE_ERR_INVALID_ARG on a bad contract, CCE_ERR_OOM on
 * scratch allocation failure. */
cce_result cce_qwen35_gated_attention(const cce_qwen35_attn_config *cfg,
                                      int n_tokens, const float *q,
                                      const float *gate, const float *k,
                                      const float *v, const float *q_norm_w,
                                      const float *k_norm_w, float *out);

/* ---------------------------------------------------------------------------
 * Honest capability statement
 * ------------------------------------------------------------------------- */

typedef struct cce_qwen35_caps {
    int layer_schedule_dispatch; /* 1: default + explicit-mask schedule */
    int deltanet_recurrent_core; /* 1: stateful Gated-DeltaNet step */
    int gated_attention_core;    /* 1: gated causal attention */
    int gguf_loader;             /* 0: no arbitrary-quantized GGUF loader here */
    int end_to_end_runner;       /* 0: no token-in/logits-out production runner */
    int applies_rope;            /* 0: (M)RoPE is the caller's responsibility */
    int applies_short_conv;      /* 0: conv+SiLU is the caller's responsibility */
    const char *summary;
} cce_qwen35_caps;

/* Fill out with the honest capability boundary of this module. */
void cce_qwen35_get_caps(cce_qwen35_caps *out);

#ifdef __cplusplus
}
#endif

#endif /* CCE_QWEN35_H */
