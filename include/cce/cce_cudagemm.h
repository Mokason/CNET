#ifndef CCE_CUDAGEMM_H
#define CCE_CUDAGEMM_H

/*
 * Pure-C cuBLAS GEMM for CNET (NVIDIA CUDA) — peer to cce_hipgemm / cce_clgemm.
 * ===========================================================================
 * Dynamic load of libcudart + libcublas (no hard CUDA link, no nvcc, no CUDA
 * headers at compile time, no CUDA C++ kernels). NULL open when the CUDA
 * runtime/driver is absent  ->  caller falls back to the CPU path, byte-for-byte
 * unchanged. Nothing here executes unless an NVIDIA GPU is present at runtime,
 * so adding this file does not affect existing builds or existing users.
 *
 * Discrete GPUs only (cudaDevAttrIntegrated filtered). Multi-device column
 * split of large weight matrices so multiple GPUs can beat a single card.
 *
 * Env (shared semantics with cce_hipgemm/cce_clgemm):
 *   CNET_GPU_DEVICES   comma list of discrete-GPU indices to use
 *   CNET_GPU_COUNT     cap on number of devices
 *   CNET_GPU_SPLIT_MB  column-split threshold in MB (default 8)
 *   CNET_GPU_MIN_FLOPS skip GPU when T*K*N below this (default 1<<20)
 *   CNET_CUDA_Q8_MB    int8 expand-cache VRAM budget in MB (default 8192)
 *
 * Matmul semantics match cce_clgemm/cce_hipgemm (row-major
 * C = A[T,K] * W[K,N] + bias). cuBLAS uses FMA  ->  NOT bit-identical to the CPU
 * path; decision-identity only (same as the AMD path). int8 weights expand to
 * float on first use (LRU VRAM budget) then cublasSgemm.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cce_cudagemm cce_cudagemm;

/* Open across all discrete NVIDIA GPUs (honoring CNET_GPU_* env). Returns NULL
 * when CUDA is unavailable — the caller then stays on CPU. Writes a short human
 * device name into device_name_out when provided. */
cce_cudagemm *cce_cudagemm_open(char *device_name_out, size_t device_name_cap);

/* Open a single specific discrete device by index (into the discrete
 * enumeration, same ordering as the AMD/OpenCL paths after the iGPU filter). */
cce_cudagemm *cce_cudagemm_open_device(int device_index,
                                       char *device_name_out,
                                       size_t device_name_cap);

void cce_cudagemm_close(cce_cudagemm *h);

/* Row-major C[T,N] = A[T,K] * W[K,N] (+ bias[N] if non-NULL).
 * Returns 0 on success, -1 to signal "not handled — use the CPU path"
 * (tiny matrices below CNET_GPU_MIN_FLOPS, T out of range, or any device
 * error). T is expected to be small (decode/prefill rows); T > 8 -> CPU. */
int cce_cudagemm_matmul(cce_cudagemm *h, const float *A, size_t T, size_t K,
                        const float *W, const float *bias, size_t N, float *C);

/* int8 weight-only (per-output-channel scale) -> float expand (cached) + sgemm.
 * Wq layout is row-major [K, N] (Wq[k*N + n]); scales are per-column [N]. */
int cce_cudagemm_matmul_q8(cce_cudagemm *h, const float *A, size_t T, size_t K,
                           const int8_t *Wq, const float *scales,
                           const float *bias, size_t N, float *C);

size_t cce_cudagemm_resident_bytes(const cce_cudagemm *h);
size_t cce_cudagemm_device_count(const cce_cudagemm *h);

#ifdef __cplusplus
}
#endif

#endif /* CCE_CUDAGEMM_H */
