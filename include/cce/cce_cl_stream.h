#ifndef CCE_CL_STREAM_H
#define CCE_CL_STREAM_H

/*
 * CNET GPU residual stream (pure C, OpenCL primary device)
 * ========================================================
 * Accelerates the *same* CCE residual path used on CPU — not a second model
 * stack. Weights still come from forest specialists (linears via host W/Wq
 * already resident in cce_clgemm). This stream keeps:
 *   - residual x[D] on device across layers
 *   - K/V cache on device (same layout as host: pos * slot + layer_off)
 *   - ln / tmp activations for chained linears without H2D each matmul
 *
 * Soft-fail: every entry returns -1 → caller keeps the existing CPU path.
 * Dual discrete GPUs: residual/KV live on device 0; large weight matmuls
 * still use cce_clgemm multi-device when weights are split.
 *
 * Env: CNET_GPU_STREAM=0 disables bind (default on when clgemm present).
 */

#include "cce_clgemm.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bind capacity. k_slot/v_slot match cce_gguf_qwen2 k_slot_floats/v_slot_floats.
 * Idempotent if same shape; realloc if grown. */
int cce_clgemm_stream_bind(cce_clgemm *h, int d_model, int max_ctx,
                           size_t k_slot, size_t v_slot);

void cce_clgemm_stream_reset(cce_clgemm *h); /* clear live flag (new sequence) */
int  cce_clgemm_stream_live(const cce_clgemm *h);

/* Residual x[D] ↔ host */
int cce_clgemm_stream_set_x(cce_clgemm *h, const float *x, int D);
int cce_clgemm_stream_get_x(cce_clgemm *h, float *x, int D);

/* ln = rms_norm(x) * w.  add_one: gemma-style (1+w) if w present.
 * Leaves ln on device as the next matmul A. */
int cce_clgemm_stream_rms_x(cce_clgemm *h, const float *w, int D, float eps,
                            int add_one);

/* x += y (y host). */
int cce_clgemm_stream_add_x_host(cce_clgemm *h, const float *y, int D);

/* Linear from device ln → host C[N]. Uses resident W (fp or q8). T=1. */
int cce_clgemm_stream_linear_fp(cce_clgemm *h, const float *W,
                                const float *bias, int K, int N, float *C_host);
int cce_clgemm_stream_linear_q8(cce_clgemm *h, const int8_t *Wq,
                                const float *scales, const float *bias, int K,
                                int N, float *C_host);

/* Linear from device ln → named device slot (no host round-trip).
 * slot: 0=Q, 1=K, 2=V, 3=AO (attn out), 4=TMP. */
#define CCE_CL_SLOT_Q   0
#define CCE_CL_SLOT_K   1
#define CCE_CL_SLOT_V   2
#define CCE_CL_SLOT_AO  3
#define CCE_CL_SLOT_TMP 4
int cce_clgemm_stream_linear_fp_slot(cce_clgemm *h, const float *W,
                                     const float *bias, int K, int N, int slot);
int cce_clgemm_stream_linear_q8_slot(cce_clgemm *h, const int8_t *Wq,
                                     const float *scales, const float *bias,
                                     int K, int N, int slot);

/* NEOX RoPE on device Q or K slot (pairs i with i+rope_dim/2). */
int cce_clgemm_stream_rope_slot(cce_clgemm *h, int slot, int n_heads,
                                int head_dim, int rope_dim, int pos, float base,
                                const float *freq_factors /*nullable half*/);

/* Write K/V row at position pos into device cache (after host RoPE). */
int cce_clgemm_stream_kv_write(cce_clgemm *h, int pos, const float *k,
                               const float *v, int k_dim, int v_dim,
                               size_t k_off, size_t v_off);

/* Write K/V from device slots into cache at pos. */
int cce_clgemm_stream_kv_write_slots(cce_clgemm *h, int pos, int k_dim,
                                     int v_dim, size_t k_off, size_t v_off);

/* Multi-head decode attention reading device KV (no host pack).
 * q is host [n_q * head_dim]; out host [n_q * v_head_dim]. */
int cce_clgemm_stream_attn(cce_clgemm *h, const float *q, size_t k_off,
                           size_t v_off, int n_q, int n_k, int n_v,
                           int head_dim, int v_head_dim, int jmin, int abs_t,
                           float scale, float *attn_out);

/* Attn using device Q slot → device AO slot (optional host download). */
int cce_clgemm_stream_attn_dev(cce_clgemm *h, size_t k_off, size_t v_off,
                               int n_q, int n_k, int n_v, int head_dim,
                               int v_head_dim, int jmin, int abs_t, float scale,
                               float *attn_out_host /*nullable*/);

/* Use AO slot as next matmul A (for o_proj). */
int cce_clgemm_stream_use_slot_as_a(cce_clgemm *h, int slot, int n);

/* Download a device slot to host. */
int cce_clgemm_stream_get_slot(cce_clgemm *h, int slot, float *host, int n);

/* silu(gate)*up → host y (or could leave on device later). */
int cce_clgemm_stream_silu_mul_host(cce_clgemm *h, const float *gate,
                                    const float *up, float *y, int n);

#ifdef __cplusplus
}
#endif

#endif /* CCE_CL_STREAM_H */
