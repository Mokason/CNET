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

/* Telemetry: resident weight bytes across all devices (split slices sum to
 * the original host bytes) and the number of devices actually opened. */
size_t cce_clgemm_resident_bytes(const cce_clgemm *h);
size_t cce_clgemm_device_count(const cce_clgemm *h);

#endif /* CCE_CLGEMM_H */
