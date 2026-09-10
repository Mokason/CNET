#ifndef CNET_VSA_DEVICE_H
#define CNET_VSA_DEVICE_H

#include <stddef.h>
#include <stdint.h>
#include "cnet_vsa_bsc.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CNET_VSA_BACKEND_CPU = 0,
    CNET_VSA_BACKEND_GPU = 1
} CnetVsaDeviceBackend;

typedef struct {
    CnetVsaDeviceBackend backend; /* Default: CNET_VSA_BACKEND_GPU if available */
    int gpu_device_id;           /* Selected ROCm device index */
    int gpu_available;           /* 1 if ROCm HIP GPU detected, 0 otherwise */
    char device_name[128];       /* Hardware device name */
    int compute_units;           /* GPU CUs or CPU cores */
    int clock_mhz;               /* Clock frequency in MHz */
} CnetVsaDeviceConfig;

/* Initialize device subsystem.
   Checks CNET_VSA_DEVICE ("gpu", "cpu", "auto") and CNET_VSA_GPU_ID.
   Defaults to GPU if available. */
int  cnet_vsa_device_init(void);

/* Get current active device configuration */
const CnetVsaDeviceConfig *cnet_vsa_device_get_config(void);

/* Runtime optional switch between GPU and CPU */
int  cnet_vsa_device_set_backend(CnetVsaDeviceBackend backend);

/* Runtime optional selection of specific GPU device ID */
int  cnet_vsa_device_set_gpu_id(int gpu_id);

/* Check if GPU is currently active for computation */
int  cnet_vsa_device_is_gpu_active(void);

/* Unified High-Level Dispatch (executes on GPU by default, falls back to CPU if configured) */

/* Batch 512-bit BSC XOR Binding */
int  cnet_vsa_dispatch_batch_bind(const CnetVsaBsc *a,
                                  const CnetVsaBsc *b,
                                  CnetVsaBsc *out_dst,
                                  size_t count,
                                  double *out_time_ms);

/* Batch 512-bit BSC Nearest Neighbor Codebook Clean-up */
int  cnet_vsa_dispatch_codebook_search(const CnetVsaBsc *query,
                                       const CnetVsaBsc *codebook,
                                       size_t count,
                                       size_t *out_best_idx,
                                       int *out_best_dist,
                                       double *out_time_ms);

/* Batch Continuous 512-Dim Float Dot Product (GEMV) */
int  cnet_vsa_dispatch_float_batch_dot(const float *query_vec,
                                       const float *matrix,
                                       size_t count,
                                       float *out_scores,
                                       size_t *out_best_idx,
                                       float *out_best_score,
                                       double *out_time_ms);

#ifdef __cplusplus
}
#endif

#endif /* CNET_VSA_DEVICE_H */
