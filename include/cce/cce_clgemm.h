#ifndef CCE_CLGEMM_H
#define CCE_CLGEMM_H

/* Self-contained OpenCL GEMM for the transformer forward path — now
 * multi-device.
 *
 * No OpenCL SDK, no import library, no nvcc: OpenCL.dll (shipped by every
 * GPU driver) is loaded dynamically and the kernel is compiled at runtime.
 * On any failure — no DLL, no GPU device, kernel build error — open returns
 * NULL and callers keep the CPU path; a GPU-less machine runs unchanged.
 *
 * Workload shape: a few activation rows (T <= 8) against LARGE resident
 * weight matrices. Weights/biases are uploaded ONCE and cached device-side
 * keyed by their host pointer (model weights are stable for the process
 * lifetime).
 *
 * Multi-device: open() selects every DISCRETE GPU of the first platform
 * that has one (integrated GPUs are excluded via the host-unified-memory
 * property, never used unless explicitly forced). Weight matrices at or
 * above a split threshold are column-split across the selected devices —
 * each device holds and reads only its share, both queues run concurrently;
 * smaller matrices go whole to one device round-robin. Every output element
 * keeps the same k-ascending accumulation, so multi-device results are
 * BIT-IDENTICAL to single-device (and gpu_equiv gates GPU-vs-CPU as before).
 *
 * Env knobs: CNET_GPU_DEVICES=i,j (explicit enumeration indices,
 * platform-major order), CNET_GPU_COUNT=n (cap; 1 = single-GPU baseline),
 * CNET_GPU_SPLIT_MB=m (split threshold, default 32).
 *
 * Semantics MATCH cce_block_forward's plain-float LINEAR_HEAD exactly:
 *   C[t][n] = (bias ? bias[n] : 0) + sum_k A[t][k] * W[k*N + n]
 * (same k-ascending accumulation order; fp differences vs CPU are ulp-level
 * and the gpu_equiv gate measures them).
 * Specs: docs/superpowers/specs/2026-07-03-gpu-forward-design.md
 *        docs/superpowers/specs/2026-07-04-dual-gpu-clgemm-design.md */

#include <stddef.h>
#include <stdint.h>

typedef struct cce_clgemm cce_clgemm;

/* Open the GPU(s). dll_name NULL = platform default ("OpenCL.dll" on
 * Windows, "libOpenCL.so.1" elsewhere); a bogus name is the unit-testable
 * failure path. device_name_out (optional) receives the device string,
 * e.g. "AMD Radeon AI PRO R9700 x2". NULL on any failure. */
cce_clgemm *cce_clgemm_open(const char *dll_name,
                            char *device_name_out, size_t device_name_cap);
void cce_clgemm_close(cce_clgemm *h);

/* Open ONLY the device_index-th device of the set cce_clgemm_open would
 * OPEN (0-based over the devices that initialize successfully — the same
 * index space cce_clgemm_device_count reports; env knobs respected). This
 * is how an oracle-pool lane pins to its own GPU so concurrent lanes never
 * share a queue. NULL when the index is out of range.
 * Note: an explicit CNET_GPU_DEVICES spec that matches no device makes
 * every open fail (refusal beats silently grabbing unrequested GPUs). */
cce_clgemm *cce_clgemm_open_device(const char *dll_name, int device_index,
                                   char *device_name_out,
                                   size_t device_name_cap);

/* C[T x N] = A[T x K] . W[K x N] (+ bias[N] when bias != NULL).
 * W and bias become device-resident on first use (keyed by host pointer).
 * Returns 0, or -1 on any failure (caller falls back to the CPU path). */
int cce_clgemm_matmul(cce_clgemm *h, const float *A, size_t T, size_t K,
                      const float *W, const float *bias, size_t N, float *C);

/* int8 weight-only GEMM: C = (bias?) + scale[n] * (A . (float)Wq).
 * Same accumulation order as cce_block's int8 matvec — BIT-identical to
 * the CPU path (FP_CONTRACT OFF), so per-matrix CPU fallback cannot move a
 * decision. Wq/scales/bias become device-resident on first use. */
int cce_clgemm_matmul_q8(cce_clgemm *h, const float *A, size_t T, size_t K,
                         const signed char *Wq, const float *scales,
                         const float *bias, size_t N, float *C);

/* Telemetry: resident weight bytes across all devices (split slices sum to
 * the original host bytes) and the number of devices actually opened. */
size_t cce_clgemm_resident_bytes(const cce_clgemm *h);
size_t cce_clgemm_device_count(const cce_clgemm *h);

/* ---- Residency + attention (primary device; pure C OpenCL) ----
 * RMS / residual / silu stay on-GPU so linears can reuse device activations.
 * Attn decode: one query token vs prefix K/V (classic causal path).
 * Returns 0 ok, -1 → caller keeps CPU path. */

/* y = rms_norm(x) * w  (w may be NULL → pure rms scale only). */
int cce_clgemm_rms_norm(cce_clgemm *h, const float *x, const float *w,
                        float *y, int D, float eps);

/* y[i] = a[i] + b[i] */
int cce_clgemm_add(cce_clgemm *h, const float *a, const float *b, float *y,
                   int n);

/* y[i] = silu(gate[i]) * up[i] */
int cce_clgemm_silu_mul(cce_clgemm *h, const float *gate, const float *up,
                        float *y, int n);

/* Causal multi-head decode attention (n_tokens=1). Packs K/V from cache
 * layout: row j at base + j*slot + off, head h at + h*head_dim. */
int cce_clgemm_attn_decode(cce_clgemm *h,
                           const float *q, /* [n_q * head_dim] */
                           const float *k_cache, const float *v_cache,
                           size_t k_slot, size_t v_slot, size_t k_off,
                           size_t v_off, int n_q, int n_k, int n_v,
                           int head_dim, int v_head_dim, int jmin, int abs_t,
                           float scale, float *attn_out /* [n_q * v_hd] */);

#endif /* CCE_CLGEMM_H */
