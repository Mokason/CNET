#include "../include/cnet_vsa_gpu.h"
#include <hip/hip_runtime.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>

#define HIP_CHECK(call) do { \
    hipError_t err = (call); \
    if (err != hipSuccess) { \
        fprintf(stderr, "HIP Error at %s:%d - %s\n", __FILE__, __LINE__, hipGetErrorString(err)); \
        return -1; \
    } \
} while (0)

#define HIP_IGNORE(call) do { \
    (void)(call); \
} while (0)

static uint64_t xorshift64(uint64_t *state) {
    uint64_t x = *state;
    if (x == 0) x = 88172645463325252ULL;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *state = x;
    return x;
}

int cnet_vsa_gpu_init(int preferred_device, CnetVsaGpuDeviceInfo *out_info) {
    int dev_count = 0;
    HIP_CHECK(hipGetDeviceCount(&dev_count));
    if (dev_count <= 0) {
        return -1;
    }

    int dev = 0;
    if (preferred_device >= 0 && preferred_device < dev_count) {
        dev = preferred_device;
    } else if (dev_count > 1) {
        dev = 1;
    }

    HIP_CHECK(hipSetDevice(dev));

    if (out_info) {
        hipDeviceProp_t prop;
        HIP_CHECK(hipGetDeviceProperties(&prop, dev));
        out_info->device_id = dev;
        snprintf(out_info->device_name, sizeof(out_info->device_name), "%s", prop.name);
        out_info->compute_units = prop.multiProcessorCount;
        out_info->clock_mhz = prop.clockRate / 1000;
        out_info->total_vram_bytes = prop.totalGlobalMem;
    }

    return 0;
}

/* -------------------------------------------------------------
 * Kernel: Batch BSC 512-bit XOR Binding
 * ------------------------------------------------------------- */
__global__ void k_bsc_batch_bind(const uint64_t* __restrict__ a,
                                 const uint64_t* __restrict__ b,
                                 uint64_t* __restrict__ out,
                                 size_t count) {
    size_t idx = (size_t)blockIdx.x * blockDim.x + threadIdx.x;
    if (idx < count) {
        size_t base = idx * 8;
        #pragma unroll
        for (int k = 0; k < 8; ++k) {
            out[base + k] = a[base + k] ^ b[base + k];
        }
    }
}

int cnet_vsa_gpu_bsc_batch_bind(const CnetVsaBsc *src_a,
                                const CnetVsaBsc *src_b,
                                CnetVsaBsc *out_dst,
                                size_t count,
                                double *out_kernel_ms) {
    if (!src_a || !src_b || !out_dst || count == 0) return -1;

    size_t bytes = count * sizeof(CnetVsaBsc);
    uint64_t *d_a = NULL, *d_b = NULL, *d_out = NULL;

    HIP_CHECK(hipMalloc(&d_a, bytes));
    HIP_CHECK(hipMalloc(&d_b, bytes));
    HIP_CHECK(hipMalloc(&d_out, bytes));

    HIP_CHECK(hipMemcpy(d_a, src_a, bytes, hipMemcpyHostToDevice));
    HIP_CHECK(hipMemcpy(d_b, src_b, bytes, hipMemcpyHostToDevice));

    int block = 256;
    int grid = (int)((count + block - 1) / block);

    hipEvent_t start, stop;
    HIP_CHECK(hipEventCreate(&start));
    HIP_CHECK(hipEventCreate(&stop));

    /* Warm-up pass */
    hipLaunchKernelGGL(k_bsc_batch_bind, dim3(grid), dim3(block), 0, 0, d_a, d_b, d_out, count);
    HIP_CHECK(hipDeviceSynchronize());

    /* Timed pass */
    HIP_CHECK(hipEventRecord(start, 0));
    hipLaunchKernelGGL(k_bsc_batch_bind, dim3(grid), dim3(block), 0, 0, d_a, d_b, d_out, count);
    HIP_CHECK(hipEventRecord(stop, 0));
    HIP_CHECK(hipEventSynchronize(stop));

    float ms = 0.0f;
    HIP_CHECK(hipEventElapsedTime(&ms, start, stop));
    if (out_kernel_ms) *out_kernel_ms = (double)ms;

    HIP_CHECK(hipMemcpy(out_dst, d_out, bytes, hipMemcpyDeviceToHost));

    HIP_IGNORE(hipEventDestroy(start));
    HIP_IGNORE(hipEventDestroy(stop));
    HIP_IGNORE(hipFree(d_a));
    HIP_IGNORE(hipFree(d_b));
    HIP_IGNORE(hipFree(d_out));

    return 0;
}

/* -------------------------------------------------------------
 * Kernel: Batch BSC 512-bit Codebook Hamming Distance Search
 * ------------------------------------------------------------- */
__global__ void k_bsc_search_block_min(const uint64_t* __restrict__ codebook,
                                       uint64_t q0, uint64_t q1, uint64_t q2, uint64_t q3,
                                       uint64_t q4, uint64_t q5, uint64_t q6, uint64_t q7,
                                       size_t count,
                                       int* __restrict__ block_min_dists,
                                       size_t* __restrict__ block_min_indices) {
    __shared__ int s_dist[256];
    __shared__ size_t s_idx[256];

    int tid = threadIdx.x;
    size_t gidx = (size_t)blockIdx.x * blockDim.x + tid;

    int my_dist = 999999;
    size_t my_idx = 0;

    if (gidx < count) {
        size_t base = gidx * 8;
        int d = 0;
        d += __popcll(codebook[base + 0] ^ q0);
        d += __popcll(codebook[base + 1] ^ q1);
        d += __popcll(codebook[base + 2] ^ q2);
        d += __popcll(codebook[base + 3] ^ q3);
        d += __popcll(codebook[base + 4] ^ q4);
        d += __popcll(codebook[base + 5] ^ q5);
        d += __popcll(codebook[base + 6] ^ q6);
        d += __popcll(codebook[base + 7] ^ q7);
        my_dist = d;
        my_idx = gidx;
    }

    s_dist[tid] = my_dist;
    s_idx[tid] = my_idx;
    __syncthreads();

    /* Tree reduction within block */
    for (int stride = blockDim.x / 2; stride > 0; stride >>= 1) {
        if (tid < stride) {
            if (s_dist[tid + stride] < s_dist[tid]) {
                s_dist[tid] = s_dist[tid + stride];
                s_idx[tid] = s_idx[tid + stride];
            }
        }
        __syncthreads();
    }

    if (tid == 0) {
        block_min_dists[blockIdx.x] = s_dist[0];
        block_min_indices[blockIdx.x] = s_idx[0];
    }
}

int cnet_vsa_gpu_bsc_codebook_search(const CnetVsaBsc *query,
                                     const CnetVsaBsc *codebook,
                                     size_t count,
                                     size_t *out_best_idx,
                                     int *out_best_dist,
                                     double *out_kernel_ms) {
    if (!query || !codebook || count == 0) return -1;

    size_t cb_bytes = count * sizeof(CnetVsaBsc);
    uint64_t *d_cb = NULL;
    HIP_CHECK(hipMalloc(&d_cb, cb_bytes));
    HIP_CHECK(hipMemcpy(d_cb, codebook, cb_bytes, hipMemcpyHostToDevice));

    int block = 256;
    int grid = (int)((count + block - 1) / block);

    int *d_block_dists = NULL;
    size_t *d_block_indices = NULL;
    HIP_CHECK(hipMalloc(&d_block_dists, (size_t)grid * sizeof(int)));
    HIP_CHECK(hipMalloc(&d_block_indices, (size_t)grid * sizeof(size_t)));

    uint64_t q[8];
    memcpy(q, query->w, sizeof(q));

    hipEvent_t start, stop;
    HIP_CHECK(hipEventCreate(&start));
    HIP_CHECK(hipEventCreate(&stop));

    /* Warm-up pass */
    hipLaunchKernelGGL(k_bsc_search_block_min, dim3(grid), dim3(block), 0, 0,
                       d_cb, q[0], q[1], q[2], q[3], q[4], q[5], q[6], q[7],
                       count, d_block_dists, d_block_indices);
    HIP_CHECK(hipDeviceSynchronize());

    /* Timed pass */
    HIP_CHECK(hipEventRecord(start, 0));
    hipLaunchKernelGGL(k_bsc_search_block_min, dim3(grid), dim3(block), 0, 0,
                       d_cb, q[0], q[1], q[2], q[3], q[4], q[5], q[6], q[7],
                       count, d_block_dists, d_block_indices);
    HIP_CHECK(hipEventRecord(stop, 0));
    HIP_CHECK(hipEventSynchronize(stop));

    float ms = 0.0f;
    HIP_CHECK(hipEventElapsedTime(&ms, start, stop));
    if (out_kernel_ms) *out_kernel_ms = (double)ms;

    int *h_block_dists = (int*)malloc((size_t)grid * sizeof(int));
    size_t *h_block_indices = (size_t*)malloc((size_t)grid * sizeof(size_t));

    HIP_CHECK(hipMemcpy(h_block_dists, d_block_dists, (size_t)grid * sizeof(int), hipMemcpyDeviceToHost));
    HIP_CHECK(hipMemcpy(h_block_indices, d_block_indices, (size_t)grid * sizeof(size_t), hipMemcpyDeviceToHost));

    int best_dist = 999999;
    size_t best_idx = 0;
    for (int g = 0; g < grid; ++g) {
        if (h_block_dists[g] < best_dist) {
            best_dist = h_block_dists[g];
            best_idx = h_block_indices[g];
        }
    }

    if (out_best_idx) *out_best_idx = best_idx;
    if (out_best_dist) *out_best_dist = best_dist;

    free(h_block_dists);
    free(h_block_indices);
    HIP_IGNORE(hipEventDestroy(start));
    HIP_IGNORE(hipEventDestroy(stop));
    HIP_IGNORE(hipFree(d_cb));
    HIP_IGNORE(hipFree(d_block_dists));
    HIP_IGNORE(hipFree(d_block_indices));

    return 0;
}

/* -------------------------------------------------------------
 * Kernel: Batch Continuous Float 512-Dim Dot Product (GEMV)
 * ------------------------------------------------------------- */
__global__ void k_float_batch_dot(const float4* __restrict__ matrix4,
                                  const float4* __restrict__ query4,
                                  size_t count,
                                  float* __restrict__ out_scores) {
    size_t idx = (size_t)blockIdx.x * blockDim.x + threadIdx.x;
    if (idx < count) {
        size_t base = idx * 128; // 128 float4 = 512 floats
        float sum = 0.0f;
        #pragma unroll 8
        for (int k = 0; k < 128; ++k) {
            float4 m = matrix4[base + k];
            float4 q = query4[k];
            sum += m.x * q.x + m.y * q.y + m.z * q.z + m.w * q.w;
        }
        out_scores[idx] = sum;
    }
}

int cnet_vsa_gpu_float_batch_dot(const float *query_vec,
                                 const float *matrix,
                                 size_t count,
                                 float *out_scores,
                                 size_t *out_best_idx,
                                 float *out_best_score,
                                 double *out_kernel_ms) {
    if (!query_vec || !matrix || count == 0) return -1;

    size_t matrix_bytes = count * 512 * sizeof(float);
    size_t query_bytes = 512 * sizeof(float);
    size_t score_bytes = count * sizeof(float);

    float4 *d_matrix = NULL, *d_query = NULL;
    float *d_scores = NULL;

    HIP_CHECK(hipMalloc(&d_matrix, matrix_bytes));
    HIP_CHECK(hipMalloc(&d_query, query_bytes));
    HIP_CHECK(hipMalloc(&d_scores, score_bytes));

    HIP_CHECK(hipMemcpy(d_matrix, matrix, matrix_bytes, hipMemcpyHostToDevice));
    HIP_CHECK(hipMemcpy(d_query, query_vec, query_bytes, hipMemcpyHostToDevice));

    int block = 256;
    int grid = (int)((count + block - 1) / block);

    hipEvent_t start, stop;
    HIP_CHECK(hipEventCreate(&start));
    HIP_CHECK(hipEventCreate(&stop));

    /* Warm-up */
    hipLaunchKernelGGL(k_float_batch_dot, dim3(grid), dim3(block), 0, 0,
                       d_matrix, d_query, count, d_scores);
    HIP_CHECK(hipDeviceSynchronize());

    /* Timed pass */
    HIP_CHECK(hipEventRecord(start, 0));
    hipLaunchKernelGGL(k_float_batch_dot, dim3(grid), dim3(block), 0, 0,
                       d_matrix, d_query, count, d_scores);
    HIP_CHECK(hipEventRecord(stop, 0));
    HIP_CHECK(hipEventSynchronize(stop));

    float ms = 0.0f;
    HIP_CHECK(hipEventElapsedTime(&ms, start, stop));
    if (out_kernel_ms) *out_kernel_ms = (double)ms;

    float *h_scores = out_scores;
    int allocated = 0;
    if (!h_scores) {
        h_scores = (float*)malloc(score_bytes);
        allocated = 1;
    }

    HIP_CHECK(hipMemcpy(h_scores, d_scores, score_bytes, hipMemcpyDeviceToHost));

    float best_score = -2.0f;
    size_t best_idx = 0;
    for (size_t i = 0; i < count; ++i) {
        if (h_scores[i] > best_score) {
            best_score = h_scores[i];
            best_idx = i;
        }
    }

    if (out_best_idx) *out_best_idx = best_idx;
    if (out_best_score) *out_best_score = best_score;

    if (allocated) free(h_scores);
    HIP_IGNORE(hipEventDestroy(start));
    HIP_IGNORE(hipEventDestroy(stop));
    HIP_IGNORE(hipFree(d_matrix));
    HIP_IGNORE(hipFree(d_query));
    HIP_IGNORE(hipFree(d_scores));

    return 0;
}

/* -------------------------------------------------------------
 * Kernel: Parallel Batch Resonator Factorization on GPU
 * ------------------------------------------------------------- */
#define GPU_RESONATOR_DIM 512
#define GPU_RESONATOR_ITEMS 5

__device__ void dev_res_normalize(float *v) {
    float sum = 0.0f;
    #pragma unroll 8
    for (int i = 0; i < GPU_RESONATOR_DIM; ++i) sum += v[i] * v[i];
    float inv = rsqrtf(sum + 1e-12f);
    #pragma unroll 8
    for (int i = 0; i < GPU_RESONATOR_DIM; ++i) v[i] *= inv;
}

__global__ void k_batch_resonator_continuous(const float* __restrict__ S_batch,
                                            const float* __restrict__ cb_x,
                                            const float* __restrict__ cb_y,
                                            const float* __restrict__ cb_z,
                                            const float* __restrict__ init_y,
                                            const float* __restrict__ init_z,
                                            const int* __restrict__ true_x,
                                            const int* __restrict__ true_y,
                                            const int* __restrict__ true_z,
                                            size_t batch_size,
                                            int* __restrict__ out_correct,
                                            int* __restrict__ out_iters) {
    size_t bid = (size_t)blockIdx.x * blockDim.x + threadIdx.x;
    if (bid >= batch_size) return;

    const float* S = &S_batch[bid * GPU_RESONATOR_DIM];
    float est_y[GPU_RESONATOR_DIM];
    float est_z[GPU_RESONATOR_DIM];
    #pragma unroll 4
    for (int j = 0; j < GPU_RESONATOR_DIM; ++j) {
        est_y[j] = init_y[j];
        est_z[j] = init_z[j];
    }

    int best_x = -1, best_y = -1, best_z = -1;
    int iters = 0;

    for (int iter = 0; iter < 10; ++iter) {
        iters++;

        /* 1. Estimate X: probe_yz = est_y * est_z; probe_x = S * probe_yz */
        float probe_yz[GPU_RESONATOR_DIM];
        for (int j = 0; j < GPU_RESONATOR_DIM; ++j) probe_yz[j] = est_y[j] * est_z[j];
        dev_res_normalize(probe_yz);

        float probe_x[GPU_RESONATOR_DIM];
        for (int j = 0; j < GPU_RESONATOR_DIM; ++j) probe_x[j] = S[j] * probe_yz[j];
        dev_res_normalize(probe_x);

        /* Cleanup X */
        float max_sx = -999.0f;
        int new_x = 0;
        for (int i = 0; i < GPU_RESONATOR_ITEMS; ++i) {
            float dot = 0.0f;
            const float* vi = &cb_x[i * GPU_RESONATOR_DIM];
            for (int j = 0; j < GPU_RESONATOR_DIM; ++j) dot += probe_x[j] * vi[j];
            if (dot > max_sx) { max_sx = dot; new_x = i; }
        }

        /* 2. Estimate Y: probe_xz = cb_x[new_x] * est_z; probe_y = S * probe_xz */
        const float* cur_x = &cb_x[new_x * GPU_RESONATOR_DIM];
        float probe_xz[GPU_RESONATOR_DIM];
        for (int j = 0; j < GPU_RESONATOR_DIM; ++j) probe_xz[j] = cur_x[j] * est_z[j];
        dev_res_normalize(probe_xz);

        float probe_y[GPU_RESONATOR_DIM];
        for (int j = 0; j < GPU_RESONATOR_DIM; ++j) probe_y[j] = S[j] * probe_xz[j];
        dev_res_normalize(probe_y);

        /* Cleanup Y */
        float max_sy = -999.0f;
        int new_y = 0;
        for (int i = 0; i < GPU_RESONATOR_ITEMS; ++i) {
            float dot = 0.0f;
            const float* vi = &cb_y[i * GPU_RESONATOR_DIM];
            for (int j = 0; j < GPU_RESONATOR_DIM; ++j) dot += probe_y[j] * vi[j];
            if (dot > max_sy) { max_sy = dot; new_y = i; }
        }

        /* 3. Estimate Z: probe_xy = cur_x * cur_y; probe_z = S * probe_xy */
        const float* cur_y = &cb_y[new_y * GPU_RESONATOR_DIM];
        float probe_xy[GPU_RESONATOR_DIM];
        for (int j = 0; j < GPU_RESONATOR_DIM; ++j) probe_xy[j] = cur_x[j] * cur_y[j];
        dev_res_normalize(probe_xy);

        float probe_z[GPU_RESONATOR_DIM];
        for (int j = 0; j < GPU_RESONATOR_DIM; ++j) probe_z[j] = S[j] * probe_xy[j];
        dev_res_normalize(probe_z);

        /* Cleanup Z */
        float max_sz = -999.0f;
        int new_z = 0;
        for (int i = 0; i < GPU_RESONATOR_ITEMS; ++i) {
            float dot = 0.0f;
            const float* vi = &cb_z[i * GPU_RESONATOR_DIM];
            for (int j = 0; j < GPU_RESONATOR_DIM; ++j) dot += probe_z[j] * vi[j];
            if (dot > max_sz) { max_sz = dot; new_z = i; }
        }

        if (new_x == best_x && new_y == best_y && new_z == best_z) {
            break;
        }
        best_x = new_x;
        best_y = new_y;
        best_z = new_z;
        for (int j = 0; j < GPU_RESONATOR_DIM; ++j) {
            est_y[j] = cur_y[j];
            est_z[j] = cb_z[new_z * GPU_RESONATOR_DIM + j];
        }
    }

    out_correct[bid] = (best_x == true_x[bid] && best_y == true_y[bid] && best_z == true_z[bid]);
    out_iters[bid] = iters;
}

int cnet_vsa_gpu_batch_resonator(size_t batch_size,
                                 int *out_recovered_count,
                                 float *out_avg_iters,
                                 double *out_kernel_ms) {
    if (batch_size == 0) return -1;

    const int D = GPU_RESONATOR_DIM;
    const int NUM_ITEMS = GPU_RESONATOR_ITEMS;
    float cb_x[NUM_ITEMS * D], cb_y[NUM_ITEMS * D], cb_z[NUM_ITEMS * D];
    float init_y[D] = {0}, init_z[D] = {0};
    uint64_t rng = 123456789ULL;

    for (int i = 0; i < NUM_ITEMS; ++i) {
        for (int j = 0; j < D; ++j) {
            cb_x[i * D + j] = (xorshift64(&rng) & 1) ? 1.0f / sqrtf(D) : -1.0f / sqrtf(D);
            cb_y[i * D + j] = (xorshift64(&rng) & 1) ? 1.0f / sqrtf(D) : -1.0f / sqrtf(D);
            cb_z[i * D + j] = (xorshift64(&rng) & 1) ? 1.0f / sqrtf(D) : -1.0f / sqrtf(D);
            init_y[j] += cb_y[i * D + j];
            init_z[j] += cb_z[i * D + j];
        }
    }
    float sum_y = 0, sum_z = 0;
    for (int j = 0; j < D; ++j) { sum_y += init_y[j] * init_y[j]; sum_z += init_z[j] * init_z[j]; }
    for (int j = 0; j < D; ++j) { init_y[j] /= sqrtf(sum_y); init_z[j] /= sqrtf(sum_z); }

    float* h_S = (float*)malloc(batch_size * D * sizeof(float));
    int* h_tx = (int*)malloc(batch_size * sizeof(int));
    int* h_ty = (int*)malloc(batch_size * sizeof(int));
    int* h_tz = (int*)malloc(batch_size * sizeof(int));

    for (size_t b = 0; b < batch_size; ++b) {
        int x = (int)(xorshift64(&rng) % NUM_ITEMS);
        int y = (int)(xorshift64(&rng) % NUM_ITEMS);
        int z = (int)(xorshift64(&rng) % NUM_ITEMS);
        h_tx[b] = x; h_ty[b] = y; h_tz[b] = z;

        float bound_xy[D];
        for (int j = 0; j < D; ++j) bound_xy[j] = cb_x[x * D + j] * cb_y[y * D + j];
        float sum_xy = 0;
        for (int j = 0; j < D; ++j) sum_xy += bound_xy[j] * bound_xy[j];
        for (int j = 0; j < D; ++j) bound_xy[j] /= sqrtf(sum_xy);

        for (int j = 0; j < D; ++j) h_S[b * D + j] = bound_xy[j] * cb_z[z * D + j];
        float sum_s = 0;
        for (int j = 0; j < D; ++j) sum_s += h_S[b * D + j] * h_S[b * D + j];
        for (int j = 0; j < D; ++j) h_S[b * D + j] /= sqrtf(sum_s);
    }

    float *d_S, *d_cbx, *d_cby, *d_cbz, *d_iy, *d_iz;
    int *d_tx, *d_ty, *d_tz, *d_corr, *d_iters;
    HIP_CHECK(hipMalloc(&d_S, batch_size * D * sizeof(float)));
    HIP_CHECK(hipMalloc(&d_cbx, NUM_ITEMS * D * sizeof(float)));
    HIP_CHECK(hipMalloc(&d_cby, NUM_ITEMS * D * sizeof(float)));
    HIP_CHECK(hipMalloc(&d_cbz, NUM_ITEMS * D * sizeof(float)));
    HIP_CHECK(hipMalloc(&d_iy, D * sizeof(float)));
    HIP_CHECK(hipMalloc(&d_iz, D * sizeof(float)));
    HIP_CHECK(hipMalloc(&d_tx, batch_size * sizeof(int)));
    HIP_CHECK(hipMalloc(&d_ty, batch_size * sizeof(int)));
    HIP_CHECK(hipMalloc(&d_tz, batch_size * sizeof(int)));
    HIP_CHECK(hipMalloc(&d_corr, batch_size * sizeof(int)));
    HIP_CHECK(hipMalloc(&d_iters, batch_size * sizeof(int)));

    HIP_CHECK(hipMemcpy(d_S, h_S, batch_size * D * sizeof(float), hipMemcpyHostToDevice));
    HIP_CHECK(hipMemcpy(d_cbx, cb_x, NUM_ITEMS * D * sizeof(float), hipMemcpyHostToDevice));
    HIP_CHECK(hipMemcpy(d_cby, cb_y, NUM_ITEMS * D * sizeof(float), hipMemcpyHostToDevice));
    HIP_CHECK(hipMemcpy(d_cbz, cb_z, NUM_ITEMS * D * sizeof(float), hipMemcpyHostToDevice));
    HIP_CHECK(hipMemcpy(d_iy, init_y, D * sizeof(float), hipMemcpyHostToDevice));
    HIP_CHECK(hipMemcpy(d_iz, init_z, D * sizeof(float), hipMemcpyHostToDevice));
    HIP_CHECK(hipMemcpy(d_tx, h_tx, batch_size * sizeof(int), hipMemcpyHostToDevice));
    HIP_CHECK(hipMemcpy(d_ty, h_ty, batch_size * sizeof(int), hipMemcpyHostToDevice));
    HIP_CHECK(hipMemcpy(d_tz, h_tz, batch_size * sizeof(int), hipMemcpyHostToDevice));

    int block = 64;
    int grid = (int)((batch_size + block - 1) / block);

    hipEvent_t start, stop;
    HIP_CHECK(hipEventCreate(&start));
    HIP_CHECK(hipEventCreate(&stop));

    /* Warm-up */
    hipLaunchKernelGGL(k_batch_resonator_continuous, dim3(grid), dim3(block), 0, 0,
                       d_S, d_cbx, d_cby, d_cbz, d_iy, d_iz,
                       d_tx, d_ty, d_tz, batch_size, d_corr, d_iters);
    HIP_CHECK(hipDeviceSynchronize());

    /* Timed pass */
    HIP_CHECK(hipEventRecord(start, 0));
    hipLaunchKernelGGL(k_batch_resonator_continuous, dim3(grid), dim3(block), 0, 0,
                       d_S, d_cbx, d_cby, d_cbz, d_iy, d_iz,
                       d_tx, d_ty, d_tz, batch_size, d_corr, d_iters);
    HIP_CHECK(hipEventRecord(stop, 0));
    HIP_CHECK(hipEventSynchronize(stop));

    float ms = 0.0f;
    HIP_CHECK(hipEventElapsedTime(&ms, start, stop));
    if (out_kernel_ms) *out_kernel_ms = (double)ms;

    int *h_corr = (int*)malloc(batch_size * sizeof(int));
    int *h_iters_out = (int*)malloc(batch_size * sizeof(int));
    HIP_CHECK(hipMemcpy(h_corr, d_corr, batch_size * sizeof(int), hipMemcpyDeviceToHost));
    HIP_CHECK(hipMemcpy(h_iters_out, d_iters, batch_size * sizeof(int), hipMemcpyDeviceToHost));

    int total_corr = 0;
    int total_iters = 0;
    for (size_t b = 0; b < batch_size; ++b) {
        if (h_corr[b]) total_corr++;
        total_iters += h_iters_out[b];
    }

    if (out_recovered_count) *out_recovered_count = total_corr;
    if (out_avg_iters) *out_avg_iters = (float)total_iters / (float)batch_size;

    free(h_S);
    free(h_tx);
    free(h_ty);
    free(h_tz);
    free(h_corr);
    free(h_iters_out);

    HIP_IGNORE(hipEventDestroy(start));
    HIP_IGNORE(hipEventDestroy(stop));
    HIP_IGNORE(hipFree(d_S));
    HIP_IGNORE(hipFree(d_cbx));
    HIP_IGNORE(hipFree(d_cby));
    HIP_IGNORE(hipFree(d_cbz));
    HIP_IGNORE(hipFree(d_iy));
    HIP_IGNORE(hipFree(d_iz));
    HIP_IGNORE(hipFree(d_tx));
    HIP_IGNORE(hipFree(d_ty));
    HIP_IGNORE(hipFree(d_tz));
    HIP_IGNORE(hipFree(d_corr));
    HIP_IGNORE(hipFree(d_iters));

    return 0;
}
