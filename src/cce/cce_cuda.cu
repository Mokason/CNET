#include <cuda_runtime.h>
#include <cublas_v2.h>
#include "../../include/cce/cce_gpu.h"

/* Simple tiled GEMM kernel for cases where we don't use cuBLAS */
#define TILE 32

extern "C" __global__ void cuda_tiled_matmul(const float* A, const float* B, float* C,
                                  int M, int K, int N) {
    __shared__ float As[TILE][TILE];
    __shared__ float Bs[TILE][TILE];

    int row = blockIdx.y * TILE + threadIdx.y;
    int col = blockIdx.x * TILE + threadIdx.x;

    float sum = 0.0f;

    for (int t = 0; t < (K + TILE - 1) / TILE; ++t) {
        int aCol = t * TILE + threadIdx.x;
        int bRow = t * TILE + threadIdx.y;

        As[threadIdx.y][threadIdx.x] = (row < M && aCol < K) ? A[row * K + aCol] : 0.0f;
        Bs[threadIdx.y][threadIdx.x] = (bRow < K && col < N) ? B[bRow * N + col] : 0.0f;

        __syncthreads();

        for (int k = 0; k < TILE; ++k) {
            sum += As[threadIdx.y][k] * Bs[k][threadIdx.x];
        }
        __syncthreads();
    }

    if (row < M && col < N) {
        C[row * N + col] = sum;
    }
}

/* Adam kernel - fused for performance on large layers */
__global__ void cuda_adam_update(float* param,
                                 const float* grad,
                                 float* m,
                                 float* v,
                                 int n,
                                 float lr,
                                 float beta1,
                                 float beta2,
                                 float eps,
                                 float inv_bias1,
                                 float inv_bias2) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;

    float g = grad[i];
    float m_i = beta1 * m[i] + (1.0f - beta1) * g;
    float v_i = beta2 * v[i] + (1.0f - beta2) * g * g;

    m[i] = m_i;
    v[i] = v_i;

    float m_hat = m_i * inv_bias1;
    float v_hat = v_i * inv_bias2;

    param[i] -= lr * m_hat / (sqrtf(v_hat) + eps);
}

extern "C" {

cce_result cce_gpu_adam_update(cce_gpu_ctx* ctx,
                               cce_tensor* weights,
                               cce_tensor* bias,
                               cce_tensor* m_w,
                               cce_tensor* m_b,
                               cce_tensor* v_w,
                               cce_tensor* v_b,
                               const cce_tensor* grad_w,
                               const cce_tensor* grad_b,
                               float lr,
                               float beta1,
                               float beta2,
                               float eps,
                               int timestep) {
    if (!ctx || ctx->backend != CCE_GPU_CUDA || !weights) return CCE_ERR_INVALID_ARG;

    float inv_bias1 = 1.0f / (1.0f - powf(beta1, timestep));
    float inv_bias2 = 1.0f / (1.0f - powf(beta2, timestep));

    int threads = 256;

    if (weights && grad_w && m_w && v_w) {
        int n = weights->numel;
        int blocks = (n + threads - 1) / threads;
        cuda_adam_update<<<blocks, threads>>>(weights->data, grad_w->data,
                                              m_w->data, v_w->data, n,
                                              lr, beta1, beta2, eps,
                                              inv_bias1, inv_bias2);
    }

    if (bias && grad_b && m_b && v_b) {
        int n = bias->numel;
        int blocks = (n + threads - 1) / threads;
        cuda_adam_update<<<blocks, threads>>>(bias->data, grad_b->data,
                                              m_b->data, v_b->data, n,
                                              lr, beta1, beta2, eps,
                                              inv_bias1, inv_bias2);
    }

    cudaDeviceSynchronize();
    return CCE_OK;
}

/* Kernel to compute DFA grad on device */
__global__ void cuda_compute_dfa_grad(const float* input, const float* error,
                                      float* grad_w, float* grad_b,
                                      int in_d, int out_d, float dfa_strength, float inv_out,
                                      unsigned int seed_base) {
    int o = blockIdx.x * blockDim.x + threadIdx.x;
    int i = blockIdx.y * blockDim.y + threadIdx.y;

    if (o >= out_d || i >= in_d) return;

    // Simple rand for feedback (not great, but works for demo)
    unsigned int seed = seed_base + o * in_d + i;
    float feedback = ((float)((seed * 1103515245 + 12345) & 0x7fffffff) / 2147483647.0f - 0.5f) * 1.5f;

    float e = error[o] * dfa_strength * inv_out;
    float g = e * input[i] * feedback;

    grad_w[i * out_d + o] = g;
    if (i == 0) grad_b[o] = e * 0.05f;
}

extern "C"
cce_result cuda_adam_update_wrapper(cce_tensor* weights, cce_tensor* bias,
                                    cce_tensor* m_w, cce_tensor* m_b,
                                    cce_tensor* v_w, cce_tensor* v_b,
                                    const cce_tensor* grad_w, const cce_tensor* grad_b,
                                    float lr, float beta1, float beta2, float eps, int timestep) {
    float inv1 = 1.0f / (1.0f - powf(beta1, timestep));
    float inv2 = 1.0f / (1.0f - powf(beta2, timestep));

    int threads = 256;
    if (weights && grad_w) {
        int n = weights->numel;
        cuda_adam_update<<<(n+threads-1)/threads, threads>>>(
            weights->data, grad_w->data, m_w->data, v_w->data, n,
            lr, beta1, beta2, eps, inv1, inv2);
    }
    if (bias && grad_b) {
        int n = bias->numel;
        cuda_adam_update<<<(n+threads-1)/threads, threads>>>(
            bias->data, grad_b->data, m_b->data, v_b->data, n,
            lr, beta1, beta2, eps, inv1, inv2);
    }
    cudaDeviceSynchronize();
    return CCE_OK;
}

} // extern "C"