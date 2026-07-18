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
 * int8 weight-only: not on this backend — use cce_clgemm_matmul_q8.
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

size_t cce_hipgemm_resident_bytes(const cce_hipgemm *h);
size_t cce_hipgemm_device_count(const cce_hipgemm *h);

#ifdef __cplusplus
}
#endif

#endif /* CCE_HIPGEMM_H */
