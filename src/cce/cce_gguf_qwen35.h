#ifndef CCE_GGUF_QWEN35_INTERNAL_H
#define CCE_GGUF_QWEN35_INTERNAL_H

/* Internal seam between cce_gguf.c (the qwen2-family runner every oracle
 * consumer binds to) and cce_gguf_qwen35.c (the Qwen3.5 hybrid runner:
 * gated attention + Gated-DeltaNet on one residual stream).
 *
 * The extension hangs off cce_gguf_qwen2.qwen35; NULL means classic
 * transformer and every legacy path stays byte-identical. The recurrent
 * state protocol (stream_pos / one checkpoint slot) exists because the
 * flagship oracle rewinds m->cur_pos for KV-prefix reuse: a KV cache
 * rewinds positionally, a DeltaNet state does not — it must be restored
 * from a snapshot taken at the prefix boundary, or reset at 0, and any
 * other rewind target is REFUSED loudly. */

#include "../../include/cce/cce_gguf.h"
#include "../../include/cce/cce_qwen35.h"

struct cce_clgemm;

typedef struct cce_gguf_qwen35_ext {
    int n_layer;                    /* trunk layers = block_count - nextn */
    cce_qwen35_layer_kind *kind;    /* [n_layer] */

    /* DeltaNet geometry (uniform across recurrent layers; from tensor
       shapes, cross-checked against qwen35.ssm.* metadata at load) */
    int hk, hv;                     /* key / value head counts (16 / 32) */
    int dk, dv;                     /* key / value head dims (128 / 128) */
    int conv_k;                     /* causal depthwise kernel (4) */
    int key_dim;                    /* hk*dk (2048) */
    int qkv_dim;                    /* 2*key_dim + hv*dv (8192) = conv channels */

    /* per-layer F32 side tensors (empty tensors on attention layers) */
    cce_tensor *ssm_a;              /* [n_layer] each [hv], stored -exp(A_log) */
    cce_tensor *ssm_dt_bias;        /* [n_layer] each [hv] */
    cce_tensor *ssm_conv1d;         /* [n_layer] each [conv_k, qkv_dim] */
    cce_tensor *ssm_norm;           /* [n_layer] each [dv] gated-RMSNorm weight */

    /* recurrent state (live) */
    cce_qwen35_deltanet_state *dstate; /* [n_layer]; initialized on recurrent */
    float **conv_ring;              /* [n_layer] -> [(conv_k-1)*qkv_dim] raw
                                       pre-conv qkv history, index 0 = oldest */
    long stream_pos;                /* position the live state corresponds to */

    /* one checkpoint slot (the flagship prefix boundary) */
    long ckpt_pos;                  /* -1 = none */
    float **ckpt_state;             /* [n_layer] -> [hv*dk*dv] */
    float **ckpt_ring;              /* [n_layer] -> [(conv_k-1)*qkv_dim] */

    /* rope + YaRN precompute (ggml rope_yarn semantics; text-only IMROPE
       with sections reduces exactly to NEOX over rope_dim dims) */
    int   rope_dim;                 /* rotated dims per head (64 of 256) */
    float rope_base;                /* 1e7 */
    float yarn_freq_scale;          /* 1/scaling.factor; 1.0 = no scaling */
    float yarn_ext_factor;          /* 1.0 when yarn; 0 = plain rope */
    float yarn_attn_factor;         /* kernel attn_factor AFTER the reference's
                                       context-level cancellation (== 1.0) */
    float yarn_corr0, yarn_corr1;   /* ggml_rope_yarn_corr_dims, pair units */
} cce_gguf_qwen35_ext;

/* whole-forward implementation cce_gguf_qwen2_forward dispatches to */
cce_result cce_gguf_qwen35_forward_impl(cce_gguf_qwen2 *m, const int *tokens,
                                        int n_tokens, float *logits_out,
                                        int logits_cap);

/* frees the extension (called from cce_gguf_qwen2_free before the rest) */
void cce_gguf_qwen35_ext_free(cce_gguf_qwen2 *m);

/* ---- shims exported from cce_gguf.c (single source of truth for the
        linear seam, RMSNorm, tap/capture hooks, and metadata access) ---- */
cce_result cce_gguf__apply_linear_rows(struct cce_clgemm *gpu, cce_cascade *cas,
                                       const cce_tensor *in, cce_tensor *out);
cce_result cce_gguf__rms_norm(const cce_tensor *in, const cce_tensor *w,
                              float eps, cce_tensor *out);
void cce_gguf__fire_layer_tap(int layer, const float *x, int n_tokens, int dim);
void cce_gguf__fire_capture(const char *spec, const cce_tensor *in);
struct cce_clgemm *cce_gguf__global_clgemm(void);
double cce_gguf__get_scalar(const cce_gguf *g, const char *key_suffix,
                            double fallback);
/* returns 1 + copies the string value when a key with this suffix exists */
int cce_gguf__get_string(const cce_gguf *g, const char *key_suffix,
                         char *buf, size_t cap);

#endif /* CCE_GGUF_QWEN35_INTERNAL_H */
