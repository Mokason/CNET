#include "../../include/cce/cce_gpu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__APPLE__)
#include <OpenCL/opencl.h>
#elif defined(_WIN32) || defined(__linux__)
#ifdef CCE_HAVE_OPENCL
#include <CL/cl.h>
#endif
#endif

struct cce_gpu_ctx {
    cce_gpu_backend_t backend;
#ifdef CCE_HAVE_CUDA
    cublasHandle_t   cublas_handle;
#endif
#if defined(CCE_HAVE_OPENCL)
    cl_platform_id   platform;
    cl_device_id     device;
    cl_context       context;
    cl_command_queue queue;
#endif
    int              initialized;
};

static cce_gpu_ctx* g_gpu_ctx = NULL;

cce_result cce_gpu_init(cce_gpu_ctx** ctx_out) {
    if (!ctx_out) return CCE_ERR_INVALID_ARG;

    cce_gpu_ctx* ctx = (cce_gpu_ctx*)calloc(1, sizeof(cce_gpu_ctx));
    if (!ctx) return CCE_ERR_OOM;

    /* Default path: try OpenCL first (keeps CUDA completely optional) */
#if defined(CCE_HAVE_OPENCL)
    ctx->backend = CCE_GPU_OPENCL;
    cl_int err;
    err = clGetPlatformIDs(1, &ctx->platform, NULL);
    if (err == CL_SUCCESS) {
        err = clGetDeviceIDs(ctx->platform, CL_DEVICE_TYPE_GPU, 1, &ctx->device, NULL);
        if (err == CL_SUCCESS) {
            ctx->context = clCreateContext(NULL, 1, &ctx->device, NULL, NULL, &err);
            if (err == CL_SUCCESS) {
                ctx->queue = clCreateCommandQueue(ctx->context, ctx->device, 0, &err);
                if (err == CL_SUCCESS) {
                    ctx->initialized = 1;
                    g_gpu_ctx = ctx;
                    *ctx_out = ctx;
                    return CCE_OK;
                }
            }
        }
    }
#endif

    ctx->backend = CCE_GPU_NONE;
    ctx->initialized = 0;
    g_gpu_ctx = ctx;
    *ctx_out = ctx;
    return CCE_OK;
}

cce_result cce_gpu_init_cuda(cce_gpu_ctx** ctx_out) {
    if (!ctx_out) return CCE_ERR_INVALID_ARG;

#ifndef CCE_HAVE_CUDA
    return CCE_ERR_UNSUPPORTED;   /* compiled without CUDA support */
#endif

    cce_gpu_ctx* ctx = (cce_gpu_ctx*)calloc(1, sizeof(cce_gpu_ctx));
    if (!ctx) return CCE_ERR_OOM;

#ifdef CCE_HAVE_CUDA
    int device_count = 0;
    cudaError_t cerr = cudaGetDeviceCount(&device_count);
    if (cerr != cudaSuccess || device_count == 0) {
        free(ctx);
        return CCE_ERR_UNSUPPORTED;
    }

    ctx->backend = CCE_GPU_CUDA;
    ctx->cuda_device = 0;
    cudaSetDevice(0);

    cublasStatus_t stat = cublasCreate(&ctx->cublas_handle);
    if (stat != CUBLAS_STATUS_SUCCESS) {
        free(ctx);
        return CCE_ERR_UNSUPPORTED;
    }

    ctx->initialized = 1;
    g_gpu_ctx = ctx;
    *ctx_out = ctx;
    return CCE_OK;
#endif
}

void cce_gpu_destroy(cce_gpu_ctx* ctx) {
    if (!ctx) return;
#ifdef CCE_HAVE_CUDA
    if (ctx->cublas_handle) cublasDestroy(ctx->cublas_handle);
#endif
#if defined(CCE_HAVE_OPENCL)
    if (ctx->queue) clReleaseCommandQueue(ctx->queue);
    if (ctx->context) clReleaseContext(ctx->context);
#endif
    free(ctx);
}

cce_gpu_backend_t cce_gpu_get_backend(cce_gpu_ctx* ctx) {
    if (!ctx) return CCE_GPU_NONE;
    return ctx->backend;
}

int cce_gpu_is_cuda(cce_gpu_ctx* ctx) {
    return (ctx && ctx->backend == CCE_GPU_CUDA) ? 1 : 0;
}

cce_result cce_gpu_matmul(cce_gpu_ctx* ctx,
                          const cce_tensor* a,
                          const cce_tensor* b,
                          cce_tensor* c) {
    if (!a || !b || !c) return CCE_ERR_INVALID_ARG;

    if (!ctx || !ctx->initialized) {
        return cce_tensor_matmul(a, b, c);
    }

#ifdef CCE_HAVE_CUDA
    if (ctx->backend == CCE_GPU_CUDA) {
        int m = a->shape[0];
        int k = a->shape[1];
        int n = b->shape[1];

        // Temp device for this call (not resident yet)
        cce_tensor da, db, dc;
        cce_gpu_upload(a, &da);
        cce_gpu_upload(b, &db);
        cce_gpu_upload(c, &dc); // to alloc

        dim3 block(TILE, TILE);
        dim3 grid((n + TILE - 1) / TILE, (m + TILE - 1) / TILE);

        extern void cuda_tiled_matmul(const float*, const float*, float*, int, int, int);
        cuda_tiled_matmul<<<grid, block>>>(da.data, db.data, dc.data, m, k, n);
        cudaDeviceSynchronize();

        cce_gpu_download(&dc, c);
        cce_gpu_free_device(&da);
        cce_gpu_free_device(&db);
        cce_gpu_free_device(&dc);

        return CCE_OK;
    }
#endif

    /* Fallback / OpenCL path (stub) */
    printf("[cce_gpu] GPU MatMul fallback (backend=%d)\n", ctx->backend);
    return cce_tensor_matmul(a, b, c);
}

cce_result cce_gpu_try_matmul(const cce_tensor* a, const cce_tensor* b, cce_tensor* c) {
    if (g_gpu_ctx && g_gpu_ctx->initialized) {
        return cce_gpu_matmul(g_gpu_ctx, a, b, c);
    }
    return CCE_ERR_UNSUPPORTED;
}

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
    if (!ctx || !ctx->initialized || !weights) return CCE_ERR_INVALID_ARG;

#ifdef CCE_HAVE_CUDA
    if (ctx->backend == CCE_GPU_CUDA) {
        extern cce_result cuda_adam_update_wrapper(cce_tensor*, cce_tensor*, cce_tensor*, cce_tensor*, cce_tensor*, cce_tensor*, const cce_tensor*, const cce_tensor*, float, float, float, float, int);
        return cuda_adam_update_wrapper(weights, bias, m_w, m_b, v_w, v_b, grad_w, grad_b, lr, beta1, beta2, eps, timestep);
    }
#endif

    /* Fallback to CPU Adam (the one in learn.c) */
    return CCE_ERR_UNSUPPORTED;  /* caller should use CPU version */
}

/* Simple host <-> device helpers (work for CUDA backend) */
cce_result cce_gpu_upload(const cce_tensor* host, cce_tensor* device_out) {
    if (!host || !device_out) return CCE_ERR_INVALID_ARG;
#ifdef CCE_HAVE_CUDA
    if (g_gpu_ctx && g_gpu_ctx->backend == CCE_GPU_CUDA) {
        size_t bytes = host->numel * sizeof(float);
        float* dptr = NULL;
        if (cudaMalloc(&dptr, bytes) != cudaSuccess) return CCE_ERR_OOM;
        cudaMemcpy(dptr, host->data, bytes, cudaMemcpyHostToDevice);

        /* copy metadata */
        *device_out = *host;
        device_out->data = dptr;
        device_out->owns_memory = 1;   /* we own the device pointer */
        return CCE_OK;
    }
#endif
    /* fallback: just alias (no real device) */
    *device_out = *host;
    return CCE_OK;
}

cce_result cce_gpu_download(const cce_tensor* device, cce_tensor* host_out) {
    if (!device || !host_out) return CCE_ERR_INVALID_ARG;
#ifdef CCE_HAVE_CUDA
    if (g_gpu_ctx && g_gpu_ctx->backend == CCE_GPU_CUDA && device->owns_memory) {
        size_t bytes = device->numel * sizeof(float);
        cudaMemcpy(host_out->data, device->data, bytes, cudaMemcpyDeviceToHost);
        return CCE_OK;
    }
#endif
    *host_out = *device;
    return CCE_OK;
}

void cce_gpu_free_device(cce_tensor* dev) {
    if (!dev) return;
#ifdef CCE_HAVE_CUDA
    if (g_gpu_ctx && g_gpu_ctx->backend == CCE_GPU_CUDA && dev->owns_memory && dev->data) {
        cudaFree(dev->data);
    }
#endif
    dev->data = NULL;
    dev->owns_memory = 0;
}

cce_result cce_gpu_sync_block_to_device(cce_gpu_ctx* ctx, cce_block* blk) {
    if (!ctx || !blk || ctx->backend != CCE_GPU_CUDA) return CCE_ERR_UNSUPPORTED;

    /* Helper to upload if not on device or size mismatch */
#define SYNC_TENSOR(host, dev) do { \
    if (!dev.data || dev.numel != host.numel) { \
        if (dev.data) cce_gpu_free_device(&dev); \
        cce_tensor host_copy = host; \
        if (cce_gpu_upload(&host_copy, &dev) != CCE_OK) return CCE_ERR_OOM; \
    } else { \
        /* data already on device, just ensure fresh? for now assume synced on update */ \
    } \
} while(0)

    SYNC_TENSOR(blk->weights, blk->d_weights);
    SYNC_TENSOR(blk->bias, blk->d_bias);
    SYNC_TENSOR(blk->momentum_weights, blk->d_momentum_weights);
    SYNC_TENSOR(blk->momentum_bias, blk->d_momentum_bias);
    SYNC_TENSOR(blk->second_moment_w, blk->d_second_moment_w);
    SYNC_TENSOR(blk->second_moment_b, blk->d_second_moment_b);

    return CCE_OK;
}

char* cce_gpu_emit_kernel(const char* block_name, int in_dim, int out_dim) {
    (void)in_dim; (void)out_dim;
    char* src = (char*)malloc(1024);
    if (!src) return NULL;

    snprintf(src, 1024,
        "__kernel void %s_matmul(\n"
        "    __global const float* A,\n"
        "    __global const float* B,\n"
        "    __global float* C,\n"
        "    const int K, const int N) {\n"
        "  int row = get_global_id(0);\n"
        "  int col = get_global_id(1);\n"
        "  float sum = 0.0f;\n"
        "  for (int k=0; k<K; k++) {\n"
        "    sum += A[row*K + k] * B[k*N + col];\n"
        "  }\n"
        "  C[row*N + col] = sum;\n"
        "}\n",
        block_name);
    return src;
}

cce_result cce_gpu_block_adam_update(cce_gpu_ctx* ctx, cce_block* blk,
                                     const float* error, const cce_tensor* input_host,
                                     float lr, float dfa_strength) {
    if (!ctx || ctx->backend != CCE_GPU_CUDA || !blk) return CCE_ERR_UNSUPPORTED;

#ifdef CCE_HAVE_CUDA
    int in_d = blk->weights.shape[0];
    int out_d = blk->weights.shape[1];

    if (cce_gpu_sync_block_to_device(ctx, blk) != CCE_OK) return CCE_ERR_OOM;

    cce_tensor d_input;
    if (cce_gpu_upload(input_host, &d_input) != CCE_OK) return CCE_ERR_OOM;

    cce_tensor h_err = { (float*)error, {out_d}, 1, {1}, (size_t)out_d, 0, 0 };
    cce_tensor d_error;
    if (cce_gpu_upload(&h_err, &d_error) != CCE_OK) {
        cce_gpu_free_device(&d_input);
        return CCE_ERR_OOM;
    }

    // Temp grads on device
    cce_tensor d_grad_w, d_grad_b;
    cce_tensor dummy_w = {0, {in_d, out_d}, 2, {out_d,1}, (size_t)in_d*out_d, 0,0};
    cce_gpu_upload(&dummy_w, &d_grad_w);
    cce_tensor dummy_b = {0, {out_d}, 1, {1}, (size_t)out_d, 0,0};
    cce_gpu_upload(&dummy_b, &d_grad_b);

    float inv_out = (blk->type == CCE_BLOCK_LINEAR_HEAD) ? 1.0f : (1.0f / (out_d > 0 ? out_d : 1));

#ifdef CCE_HAVE_CUDA
    // Launch grad kernel
    dim3 block(16,16);
    dim3 grid( (out_d+15)/16 , (in_d+15)/16 );
    unsigned int seed = (unsigned) (blk->timestep * 12345);
    cuda_compute_dfa_grad<<<grid, block>>>(d_input.data, d_error.data,
                                           d_grad_w.data, d_grad_b.data,
                                           in_d, out_d, dfa_strength, inv_out, seed);
#endif

    // Update on device
    cce_gpu_adam_update(ctx,
                        &blk->d_weights, &blk->d_bias,
                        &blk->d_momentum_weights, &blk->d_momentum_bias,
                        &blk->d_second_moment_w, &blk->d_second_moment_b,
                        &d_grad_w, &d_grad_b,
                        lr, 0.9f, 0.999f, 1e-8f, blk->timestep);

    // Download back for host consistency (can be optimized away later)
    cce_gpu_download(&blk->d_weights, &blk->weights);
    cce_gpu_download(&blk->d_bias, &blk->bias);
    // moments can stay on device

    cce_gpu_free_device(&d_input);
    cce_gpu_free_device(&d_error);
    cce_gpu_free_device(&d_grad_w);
    cce_gpu_free_device(&d_grad_b);

    return CCE_OK;
#endif
    return CCE_ERR_UNSUPPORTED;
}
