#ifndef CCE_SSM_H
#define CCE_SSM_H

/* Mamba-1 style selective state-space model runner for CCE.
 *
 * Same decomposition philosophy as the transformer loaders: every large
 * linear projection (in_proj / x_proj / dt_proj / out_proj / lm_head)
 * becomes an independent named specialist cascade in a cce_forest
 * ("mamba.blk.N.in_proj", ..., "mamba.lm_head"); small per-channel
 * parameters (A_log, D, conv1d, norms, dt bias) stay as plain tensors.
 * The selective scan is the glue between specialists, exactly as the
 * attention glue sits between the qwen2 specialists.
 *
 * Inference is recurrent (O(1) state per token): a depthwise conv ring
 * buffer [E x d_conv] and an SSM state [E x d_state] per layer. This is
 * the natural mode for CCE-style incremental generation and needs no
 * sequence-length-bound buffers.
 *
 * Loaders (both fill the same struct; dims are derived from tensor shapes,
 * no config file needed):
 *   - HF safetensors:  backbone.layers.N.mixer.{in_proj,conv1d,x_proj,
 *                      dt_proj,A_log,D,out_proj}, backbone.norm_f,
 *                      backbone.embedding(s).weight, lm_head (tied if absent)
 *   - GGUF:            blk.N.{attn_norm,ssm_in,ssm_conv1d,ssm_x,ssm_dt,
 *                      ssm_a,ssm_d,ssm_out}, token_embd, output_norm,
 *                      output (tied if absent)
 *     NOTE: GGUF records dims in ggml ne order (innermost first, the reverse
 *     of torch) — confirmed against the real gguf in Models/ — and the loader
 *     normalizes accordingly. The mapping is verified equivalent to the
 *     safetensors mapping on shared weights; it has not been validated
 *     against real llama.cpp mamba exports yet (none available in-repo).
 */

#include "cce_defs.h"
#include "cce_tensor.h"
#include "cce_forest.h"
#include "cce_cascade.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cce_ssm_model {
    cce_forest* forest;      /* linear specialists */

    /* cached branch cascades, one per layer (owned by forest) */
    cce_cascade** in_cas;    /* [L] D -> 2E */
    cce_cascade** x_cas;     /* [L] E -> R + 2N */
    cce_cascade** dt_cas;    /* [L] R -> E (bias = dt_proj.bias) */
    cce_cascade** out_cas;   /* [L] E -> D */
    cce_cascade*  head_cas;  /* D -> V */

    /* per-layer small parameters */
    cce_tensor* norm;        /* [L] pre-mixer RMSNorm weight [D] */
    cce_tensor* conv_w;      /* [L] depthwise conv weight, E*K elems ([E][K]) */
    cce_tensor* conv_b;      /* [L] conv bias [E] (numel 0 if absent) */
    cce_tensor* A_log;       /* [L] [E][N] */
    cce_tensor* Dvec;        /* [L] skip weight [E] */

    cce_tensor tok_emb;      /* [V][D] */
    cce_tensor norm_f;       /* final RMSNorm weight [D] */

    int n_layer;
    int d_model;             /* D */
    int d_inner;             /* E */
    int d_state;             /* N */
    int d_conv;              /* K */
    int dt_rank;             /* R */
    int vocab_size;
    float norm_eps;

    int bos_token_id;
    int eos_token_id;

    /* recurrent state */
    float* conv_state;       /* [L][E][K] shift buffer, newest at K-1 */
    float* ssm_state;        /* [L][E][N] */
    int cur_pos;
} cce_ssm_model;

/* Load from a .safetensors (HF mamba naming) or .gguf (llama.cpp mamba naming)
 * file; the container is sniffed from the magic bytes. */
cce_result cce_ssm_load(cce_ssm_model** out, const char* path);

/* Run tokens through the recurrent model. State carries across calls
 * (feed a prompt, then generate token by token). logits_out receives the
 * LAST token's logits (up to logits_cap entries), matching
 * cce_gguf_qwen2_forward semantics. */
cce_result cce_ssm_forward(cce_ssm_model* m, const int* tokens, int n_tokens,
                           float* logits_out, int logits_cap);

/* Zero the recurrent state (conv + ssm) and reset the position. */
void cce_ssm_reset(cce_ssm_model* m);

void cce_ssm_free(cce_ssm_model* m);

#ifdef __cplusplus
}
#endif

#endif /* CCE_SSM_H */
