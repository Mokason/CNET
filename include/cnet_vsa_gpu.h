#ifndef CNET_VSA_GPU_H
#define CNET_VSA_GPU_H

#include <stddef.h>
#include <stdint.h>
#include "cnet_vsa_bsc.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int device_id;
    char device_name[128];
    int compute_units;
    int clock_mhz;
    size_t total_vram_bytes;
} CnetVsaGpuDeviceInfo;

/* Query and select AMD ROCm GPU device */
int cnet_vsa_gpu_init(int preferred_device, CnetVsaGpuDeviceInfo *out_info);

/* Batch 512-bit BSC XOR Binding:
   out_dst[i] = src_a[i] ^ src_b[i] for i in 0..count-1.
   Measures raw kernel execution time in milliseconds. */
int cnet_vsa_gpu_bsc_batch_bind(const CnetVsaBsc *src_a,
                                const CnetVsaBsc *src_b,
                                CnetVsaBsc *out_dst,
                                size_t count,
                                double *out_kernel_ms);

/* Batch 512-bit BSC Codebook Search (Nearest Neighbor Hamming Distance):
   Finds index of vector in codebook with minimum Hamming distance to query.
   codebook: array of `count` 512-bit BSC hypervectors. */
int cnet_vsa_gpu_bsc_codebook_search(const CnetVsaBsc *query,
                                     const CnetVsaBsc *codebook,
                                     size_t count,
                                     size_t *out_best_idx,
                                     int *out_best_dist,
                                     double *out_kernel_ms);

/* Batch Continuous 512-Dim Float Dot Product:
   Computes dot product of query_vec (1x512) against matrix of count vectors (count x 512).
   Returns array of dot products and identifies top matching index. */
int cnet_vsa_gpu_float_batch_dot(const float *query_vec,
                                 const float *matrix,
                                 size_t count,
                                 float *out_scores,
                                 size_t *out_best_idx,
                                 float *out_best_score,
                                 double *out_kernel_ms);

/* Parallel Resonator Batch Factorization on GPU:
   Factorizes `batch_size` composite triples S[i] = X[i] * Y[i] * Z[i] concurrently.
   Returns percentage of triples accurately recovered and average iterations. */
int cnet_vsa_gpu_batch_resonator(size_t batch_size,
                                 int *out_recovered_count,
                                 float *out_avg_iters,
                                 double *out_kernel_ms);

#ifdef __cplusplus
}
#endif

#endif /* CNET_VSA_GPU_H */
