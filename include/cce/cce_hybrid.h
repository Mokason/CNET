#ifndef CCE_HYBRID_H
#define CCE_HYBRID_H

/* Native hybrid attention+SSM runner for CCE (jamba/zamba structural class).
 *
 * A hybrid model interleaves two mixer kinds over ONE shared residual stream:
 *   - attention layers  (llama/qwen2-style: RMSNorm -> GQA + NEOX RoPE ->
 *                        o_proj -> residual -> RMSNorm -> SwiGLU MLP -> residual)
 *   - state-space layers (mamba-1 selective SSM: the exact cce_ssm mixer —
 *                        RMSNorm -> in_proj -> depthwise conv -> x/dt proj ->
 *                        selective scan -> out_proj -> residual)
 * Which kind a layer is comes from STRUCTURE (blk.N.attn_q vs blk.N.ssm_in),
 * never from the arch label alone, matching the rest of CCE's detection.
 *
 * This is a genuine MIXED forward: a single pass threads one residual stream
 * through the layers in model order, dispatching each layer to its mixer. It
 * is NOT two full-model runners with concatenated logits — attention state
 * (a per-attention-layer KV cache) and SSM state (a per-SSM-layer conv ring +
 * scan state) coexist and advance together as tokens stream through.
 *
 * Every large linear projection (q/k/v/o, gate/up/down, in/x/dt/out, head)
 * becomes an independent named specialist cascade in one cce_forest, reusing
 * the same GGUF specialist-branch primitive the pure-transformer and pure-SSM
 * loaders use. Small per-channel parameters (norms, conv1d, A_log, D, dt bias)
 * stay as plain tensors.
 *
 * SCOPE / LIMITS: attention is llama/qwen2-style (separate q/k/v/o, GQA,
 * NEOX rotary, SwiGLU); the SSM mixer is mamba-1 selective-scan. This is the
 * jamba/zamba hybrid convention. It is deliberately NOT Qwen3.5's
 * Gated-DeltaNet linear attention — that convention is not mapped here.
 *
 * GGUF naming (llama.cpp jamba-style, one blk.N index space):
 *   shared:     token_embd.weight, output_norm.weight, output.weight (tied if absent)
 *   every blk:  blk.N.attn_norm.weight            (mixer-input RMSNorm)
 *   attn blk:   blk.N.attn_{q,k,v,output}.weight, blk.N.ffn_norm.weight,
 *               blk.N.ffn_{gate,up,down}.weight
 *   ssm  blk:   blk.N.ssm_in.weight, blk.N.ssm_conv1d.{weight,bias},
 *               blk.N.ssm_x.weight, blk.N.ssm_dt.{weight,bias},
 *               blk.N.ssm_a, blk.N.ssm_d, blk.N.ssm_out.weight
 */

#include "cce_defs.h"
#include "cce_tensor.h"
#include "cce_forest.h"
#include "cce_cascade.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CCE_HYBRID_LAYER_ATTN = 0,
    CCE_HYBRID_LAYER_SSM  = 1,
} cce_hybrid_layer_kind;

typedef struct cce_hybrid_model {
    cce_forest* forest;          /* all linear specialists (attn + ssm + head) */

    int  n_layer;
    int* layer_kind;             /* [L] cce_hybrid_layer_kind per layer, in model order */
    int  n_attn_layer;
    int  n_ssm_layer;

    int  d_model;                /* D: shared residual width */
    int  vocab_size;
    float norm_eps;

    /* attention geometry (uniform across attention layers) */
    int  n_head;
    int  n_kv_head;
    int  head_dim;
    int  q_dim, k_dim, v_dim;     /* projection widths */
    int  ffn_hidden;
    float rope_base;

    /* ssm geometry (uniform across ssm layers) */
    int  d_inner;                 /* E */
    int  d_state;                 /* N */
    int  d_conv;                  /* K */
    int  dt_rank;                 /* R */

    /* per-layer linear specialist cascades (owned by forest; NULL when the
       layer is the other kind) */
    cce_cascade** q_cas;          /* [L] attn */
    cce_cascade** k_cas;
    cce_cascade** v_cas;
    cce_cascade** o_cas;
    cce_cascade** gate_cas;
    cce_cascade** up_cas;
    cce_cascade** down_cas;
    cce_cascade** in_cas;         /* [L] ssm */
    cce_cascade** x_cas;
    cce_cascade** dt_cas;
    cce_cascade** out_cas;
    cce_cascade*  head_cas;       /* D -> V */

    /* per-layer small parameters */
    cce_tensor* mixer_norm;       /* [L] pre-mixer RMSNorm weight [D] (both kinds) */
    cce_tensor* ffn_norm;         /* [L] pre-MLP RMSNorm weight [D] (attn only) */
    cce_tensor* conv_w;           /* [L] depthwise conv weight [E][K] (ssm only) */
    cce_tensor* conv_b;           /* [L] conv bias [E] (ssm, optional) */
    cce_tensor* A_log;            /* [L] [E][N] (ssm only) */
    cce_tensor* Dvec;             /* [L] skip weight [E] (ssm only) */

    cce_tensor tok_emb;           /* [V][D] */
    cce_tensor output_norm;       /* final RMSNorm weight [D] */

    int bos_token_id;
    int eos_token_id;

    /* runtime state (advances across forward calls; reset with cce_hybrid_reset) */
    int max_ctx;
    float** k_cache;              /* [L] attn: [max_ctx * k_dim] (NULL for ssm) */
    float** v_cache;              /* [L] attn: [max_ctx * v_dim] */
    float*  conv_state;           /* [L][E][K] ssm ring buffer, newest at K-1 */
    float*  ssm_state;            /* [L][E][N] ssm scan state */
    int cur_pos;

    char forest_scratch[160];     /* this instance's temp forest backing file */
} cce_hybrid_model;

/* Load a hybrid attention+SSM model from a .gguf file (llama.cpp jamba-style
 * naming). Per-layer kind, all dims, and the vocab are derived from the tensor
 * table + metadata; no config file needed. Refuses (does not silently drop a
 * sublayer) when a layer resolves to neither a complete attention block nor a
 * complete SSM block, or when a required shared tensor is missing. */
cce_result cce_hybrid_load(cce_hybrid_model** out, const char* path);

/* Run tokens through the mixed model. State carries across calls (feed a
 * prompt, then generate token by token). logits_out receives the LAST token's
 * logits (up to logits_cap entries), matching cce_gguf_qwen2_forward and
 * cce_ssm_forward semantics. */
cce_result cce_hybrid_forward(cce_hybrid_model* m, const int* tokens, int n_tokens,
                              float* logits_out, int logits_cap);

/* Zero all recurrent/cache state (KV caches + conv/scan state) and reset pos. */
void cce_hybrid_reset(cce_hybrid_model* m);

void cce_hybrid_free(cce_hybrid_model* m);

#ifdef __cplusplus
}
#endif

#endif /* CCE_HYBRID_H */
