#include "../include/cnet_vsa_device.h"
#include "../include/cnet_vsa.h"
#include "../include/cnet_vsa_bsc.h"
#include "../include/cnet_vsa_gpu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static CnetVsaDeviceConfig s_config = {
    .backend = CNET_VSA_BACKEND_GPU,
    .gpu_device_id = 1,
    .gpu_available = 0,
    .device_name = "Uninitialized",
    .compute_units = 0,
    .clock_mhz = 0
};

static int s_initialized = 0;

static uint64_t get_mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

int cnet_vsa_device_init(void) {
    s_config.backend = CNET_VSA_BACKEND_GPU; /* Default is GPU */
    s_config.gpu_device_id = 1;

    /* Check CNET_VSA_GPU_ID environment variable */
    const char *gpu_id_env = getenv("CNET_VSA_GPU_ID");
    if (gpu_id_env && gpu_id_env[0]) {
        s_config.gpu_device_id = atoi(gpu_id_env);
    }

    /* Probe and initialize GPU */
    CnetVsaGpuDeviceInfo gpu_info;
    memset(&gpu_info, 0, sizeof(gpu_info));
    int gpu_rc = cnet_vsa_gpu_init(s_config.gpu_device_id, &gpu_info);
    if (gpu_rc != 0 && s_config.gpu_device_id != 0) {
        /* Fallback probe on device 0 */
        s_config.gpu_device_id = 0;
        gpu_rc = cnet_vsa_gpu_init(0, &gpu_info);
    }

    if (gpu_rc == 0) {
        s_config.gpu_available = 1;
        snprintf(s_config.device_name, sizeof(s_config.device_name), "%s", gpu_info.device_name);
        s_config.compute_units = gpu_info.compute_units;
        s_config.clock_mhz = gpu_info.clock_mhz;
    } else {
        s_config.gpu_available = 0;
        snprintf(s_config.device_name, sizeof(s_config.device_name), "Host CPU (SIMD AVX-512 / AVX2)");
        s_config.compute_units = 16;
        s_config.clock_mhz = 4200;
    }

    /* Check CNET_VSA_DEVICE override */
    const char *dev_env = getenv("CNET_VSA_DEVICE");
    if (dev_env && dev_env[0]) {
        if (strcasecmp(dev_env, "cpu") == 0 || strcmp(dev_env, "0") == 0) {
            s_config.backend = CNET_VSA_BACKEND_CPU;
        } else if (strcasecmp(dev_env, "gpu") == 0 || strcmp(dev_env, "1") == 0 || strcasecmp(dev_env, "rocm") == 0) {
            if (s_config.gpu_available) {
                s_config.backend = CNET_VSA_BACKEND_GPU;
            } else {
                fprintf(stderr, "[CNET_VSA_DEVICE] Warning: GPU requested but unavailable. Falling back to CPU.\n");
                s_config.backend = CNET_VSA_BACKEND_CPU;
            }
        }
    } else {
        /* Default: GPU if available, else CPU */
        s_config.backend = s_config.gpu_available ? CNET_VSA_BACKEND_GPU : CNET_VSA_BACKEND_CPU;
    }

    s_initialized = 1;
    return 0;
}

const CnetVsaDeviceConfig *cnet_vsa_device_get_config(void) {
    if (!s_initialized) cnet_vsa_device_init();
    return &s_config;
}

int cnet_vsa_device_set_backend(CnetVsaDeviceBackend backend) {
    if (!s_initialized) cnet_vsa_device_init();
    if (backend == CNET_VSA_BACKEND_GPU && !s_config.gpu_available) {
        return -1;
    }
    s_config.backend = backend;
    return 0;
}

int cnet_vsa_device_set_gpu_id(int gpu_id) {
    if (!s_initialized) cnet_vsa_device_init();
    CnetVsaGpuDeviceInfo gpu_info;
    int rc = cnet_vsa_gpu_init(gpu_id, &gpu_info);
    if (rc == 0) {
        s_config.gpu_device_id = gpu_id;
        s_config.gpu_available = 1;
        snprintf(s_config.device_name, sizeof(s_config.device_name), "%s", gpu_info.device_name);
        s_config.compute_units = gpu_info.compute_units;
        s_config.clock_mhz = gpu_info.clock_mhz;
        return 0;
    }
    return -1;
}

int cnet_vsa_device_is_gpu_active(void) {
    if (!s_initialized) cnet_vsa_device_init();
    return (s_config.backend == CNET_VSA_BACKEND_GPU && s_config.gpu_available);
}

int cnet_vsa_dispatch_batch_bind(const CnetVsaBsc *a,
                                  const CnetVsaBsc *b,
                                  CnetVsaBsc *out_dst,
                                  size_t count,
                                  double *out_time_ms) {
    if (!a || !b || !out_dst || count == 0) return -1;
    if (!s_initialized) cnet_vsa_device_init();

    if (cnet_vsa_device_is_gpu_active()) {
        return cnet_vsa_gpu_bsc_batch_bind(a, b, out_dst, count, out_time_ms);
    }

    /* CPU Fallback / Explicit CPU Mode */
    uint64_t t0 = get_mono_ns();
    for (size_t i = 0; i < count; ++i) {
        cnet_vsa_bsc_bind(&out_dst[i], &a[i], &b[i]);
    }
    uint64_t t1 = get_mono_ns();
    if (out_time_ms) {
        *out_time_ms = (double)(t1 - t0) / 1000000.0;
    }
    return 0;
}

int cnet_vsa_dispatch_codebook_search(const CnetVsaBsc *query,
                                       const CnetVsaBsc *codebook,
                                       size_t count,
                                       size_t *out_best_idx,
                                       int *out_best_dist,
                                       double *out_time_ms) {
    if (!query || !codebook || count == 0) return -1;
    if (!s_initialized) cnet_vsa_device_init();

    if (cnet_vsa_device_is_gpu_active()) {
        return cnet_vsa_gpu_bsc_codebook_search(query, codebook, count, out_best_idx, out_best_dist, out_time_ms);
    }

    /* CPU Fallback / Explicit CPU Mode */
    uint64_t t0 = get_mono_ns();
    int best_dist = 999999;
    size_t best_idx = 0;

    for (size_t i = 0; i < count; ++i) {
        int d = cnet_vsa_bsc_hamming(query, &codebook[i]);
        if (d < best_dist) {
            best_dist = d;
            best_idx = i;
        }
    }
    uint64_t t1 = get_mono_ns();

    if (out_best_idx) *out_best_idx = best_idx;
    if (out_best_dist) *out_best_dist = best_dist;
    if (out_time_ms) {
        *out_time_ms = (double)(t1 - t0) / 1000000.0;
    }
    return 0;
}

int cnet_vsa_dispatch_float_batch_dot(const float *query_vec,
                                       const float *matrix,
                                       size_t count,
                                       float *out_scores,
                                       size_t *out_best_idx,
                                       float *out_best_score,
                                       double *out_time_ms) {
    if (!query_vec || !matrix || count == 0) return -1;
    if (!s_initialized) cnet_vsa_device_init();

    if (cnet_vsa_device_is_gpu_active()) {
        return cnet_vsa_gpu_float_batch_dot(query_vec, matrix, count, out_scores, out_best_idx, out_best_score, out_time_ms);
    }

    /* CPU Fallback / Explicit CPU Mode */
    uint64_t t0 = get_mono_ns();
    float best_score = -2.0f;
    size_t best_idx = 0;

    for (size_t i = 0; i < count; ++i) {
        float s = cnet_vsa_similarity(query_vec, &matrix[i * 512], 512);
        if (out_scores) out_scores[i] = s;
        if (s > best_score) {
            best_score = s;
            best_idx = i;
        }
    }
    uint64_t t1 = get_mono_ns();

    if (out_best_idx) *out_best_idx = best_idx;
    if (out_best_score) *out_best_score = best_score;
    if (out_time_ms) {
        *out_time_ms = (double)(t1 - t0) / 1000000.0;
    }
    return 0;
}
