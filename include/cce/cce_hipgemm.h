#ifndef CCE_HIPGEMM_H
#define CCE_HIPGEMM_H

/*
 * Pure-C hipBLAS GEMM for CNET (AMD ROCm) — peer to cce_clgemm.
 * =================================================================
 * Dynamic load of libamdhip64 + libhipblas (no hard ROCm link, no Python,
 * no HIP C++ kernels). NULL open when ROCm is absent → CPU path.
 *
 * Discrete GPUs only (hipDeviceAttributeIntegrated filtered). Multi-device
 * column-split of large weight matrices so dual R9700 can beat single.
 *
 * Env: CNET_GPU_DEVICES, CNET_GPU_COUNT, CNET_GPU_SPLIT_MB (default 8 for HIP),
 *      CNET_GPU_MIN_FLOPS (skip GPU when T*K*N below this; default 1<<20).
 *
 * Matmul semantics match cce_clgemm (row-major C = A[T,K] * W[K,N] + bias).
 * hipBLAS uses FMA → not bit-identical to CPU; decision-identity only.
 * int8: expand-to-float on first use (LRU VRAM budget CNET_HIP_Q8_MB, default
 * 8192) then hipblasSgemm — decision-identity only.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cce_hipgemm cce_hipgemm;

cce_hipgemm *cce_hipgemm_open(char *device_name_out, size_t device_name_cap);
cce_hipgemm *cce_hipgemm_open_device(int device_index,
                                     char *device_name_out,
                                     size_t device_name_cap);
void         cce_hipgemm_close(cce_hipgemm *h);

int cce_hipgemm_matmul(cce_hipgemm *h, const float *A, size_t T, size_t K,
                       const float *W, const float *bias, size_t N, float *C);

/* Re-upload W every call (no resident cache) — for STE/training. */
int cce_hipgemm_matmul_ephemeral(cce_hipgemm *h, const float *A, size_t T,
                                 size_t K, const float *W, const float *bias,
                                 size_t N, float *C);

/* int8 weight-only → float expand (cached) + sgemm. */
int cce_hipgemm_matmul_q8(cce_hipgemm *h, const float *A, size_t T, size_t K,
                          const int8_t *Wq, const float *scales,
                          const float *bias, size_t N, float *C);

size_t cce_hipgemm_resident_bytes(const cce_hipgemm *h);
size_t cce_hipgemm_device_count(const cce_hipgemm *h);

/* --- Resource/telemetry contract (audit 2026-07-18) -----------------------
 * Surfaced so DeepSeek-scale use cannot silently fall back to CPU:
 *   - resident_count:        live entries in the fixed resident weight table.
 *   - resident_evictions:    cumulative LRU evictions since open (a non-zero
 *                            delta after a workload is a surfaced contract
 *                            breach, NOT a silent -1 → CPU fallback).
 *   - max_resident:          compile-time cap of the resident table.
 *   - q8_cache_slots_used:   live file-static q8 cache slots (process-wide).
 *                            Reclaimed on close so repeated open/close cannot
 *                            exhaust the 8-slot global table with dead handles.
 */
size_t cce_hipgemm_resident_count(const cce_hipgemm *h);
size_t cce_hipgemm_resident_evictions(const cce_hipgemm *h);
int     cce_hipgemm_max_resident(void);
size_t cce_hipgemm_q8_cache_slots_used(void);

#ifdef __cplusplus
}
#endif

#endif /* CCE_HIPGEMM_H */
