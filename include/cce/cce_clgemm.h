#ifndef CCE_CLGEMM_H
#define CCE_CLGEMM_H

/* Self-contained OpenCL GEMM for the transformer forward path.
 *
 * No OpenCL SDK, no import library, no nvcc: OpenCL.dll (shipped by every
 * GPU driver) is loaded dynamically and the kernel is compiled at runtime.
 * On any failure — no DLL, no GPU device, kernel build error — open returns
 * NULL and callers keep the CPU path; a GPU-less machine runs unchanged.
 *
 * Workload shape: a few activation rows (T <= 8) against LARGE resident
 * weight matrices. Weights/biases are uploaded ONCE and cached device-side
 * keyed by their host pointer (model weights are stable for the process
 * lifetime; the gemma fragment totals ~1.5GB device memory).
 *
 * Semantics MATCH cce_block_forward's plain-float LINEAR_HEAD exactly:
 *   C[t][n] = (bias ? bias[n] : 0) + sum_k A[t][k] * W[k*N + n]
 * (same k-ascending accumulation order; fp differences are ulp-level and
 * the gpu_equiv gate measures them).
 * Spec: docs/superpowers/specs/2026-07-03-gpu-forward-design.md */

#include <stddef.h>

typedef struct cce_clgemm cce_clgemm;

/* Open the GPU. dll_name NULL = platform default ("OpenCL.dll" on Windows,
 * "libOpenCL.so.1" elsewhere); a bogus name is the unit-testable failure
 * path. device_name_out (optional) receives the device string. NULL on any
 * failure. */
cce_clgemm *cce_clgemm_open(const char *dll_name,
                            char *device_name_out, size_t device_name_cap);
void cce_clgemm_close(cce_clgemm *h);

/* C[T x N] = A[T x K] . W[K x N] (+ bias[N] when bias != NULL).
 * W and bias become device-resident on first use (keyed by host pointer).
 * Returns 0, or -1 on any failure (caller falls back to the CPU path). */
int cce_clgemm_matmul(cce_clgemm *h, const float *A, size_t T, size_t K,
                      const float *W, const float *bias, size_t N, float *C);

/* Telemetry: resident weight bytes currently on the device. */
size_t cce_clgemm_resident_bytes(const cce_clgemm *h);

#endif /* CCE_CLGEMM_H */
