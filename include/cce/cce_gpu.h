#ifndef CCE_GPU_H
#define CCE_GPU_H

#include "cce_defs.h"
#include "cce_tensor.h"
#include "cce_block.h"   /* for cce_block in batch helpers */

#ifdef __cplusplus
extern "C" {
#endif

/* GPU backend types */
typedef enum {
    CCE_GPU_NONE = 0,
    CCE_GPU_OPENCL,
    CCE_GPU_CUDA
} cce_gpu_backend_t;

/* Basic GPU context - now supports CUDA or OpenCL */
typedef struct cce_gpu_ctx cce_gpu_ctx;

/* DLL-visible GPU init (guarded so they are exported when building cce.dll) */
#ifdef CCE_BUILD_DLL
#ifdef _WIN32
#define CCE_GPU_API __declspec(dllexport)
#else
#define CCE_GPU_API __attribute__((visibility("default")))
#endif
#else
#define CCE_GPU_API
#endif

CCE_GPU_API cce_result cce_gpu_init(cce_gpu_ctx** ctx);           /* best-effort (OpenCL or none) */
CCE_GPU_API cce_result cce_gpu_init_cuda(cce_gpu_ctx** ctx);     /* explicit CUDA only - returns error if unavailable */
CCE_GPU_API void cce_gpu_destroy(cce_gpu_ctx* ctx);

/* Get active backend */
cce_gpu_backend_t cce_gpu_get_backend(cce_gpu_ctx* ctx);

/* Accessor to avoid exposing struct in all files */
int cce_gpu_is_cuda(cce_gpu_ctx* ctx);

/* Run MatMul on GPU (row major). Falls back to CPU if no GPU. */
cce_result cce_gpu_matmul(cce_gpu_ctx* ctx,
                          const cce_tensor* a,
                          const cce_tensor* b,
                          cce_tensor* c);

/* Adam update on GPU for a block's weights/bias + moments.
   grad_w, grad_b are the gradients (same shape).
   This replaces the CPU dfa/Adam loop for speed on large layers. */
cce_result cce_gpu_adam_update(cce_gpu_ctx* ctx,
                               cce_tensor* weights,
                               cce_tensor* bias,
                               cce_tensor* m_w,
                               cce_tensor* m_b,
                               cce_tensor* v_w,
                               cce_tensor* v_b,
                               const cce_tensor* grad_w,   /* can be computed or passed */
                               const cce_tensor* grad_b,
                               float lr,
                               float beta1,
                               float beta2,
                               float eps,
                               int timestep);

/* --- GPU-resident batch support (optional, for performance) --- */

/* Allocate device memory for a host tensor and copy data to device.
   The returned cce_tensor has .data pointing to device memory. */
cce_result cce_gpu_upload(const cce_tensor* host, cce_tensor* device_out);

/* Copy data back from device to host tensor. */
cce_result cce_gpu_download(const cce_tensor* device, cce_tensor* host_out);

/* Free device memory associated with a tensor created by cce_gpu_upload. */
void cce_gpu_free_device(cce_tensor* dev);

/* Run a full forward pass for a batch on device (assumes weights/bias are on host or device as needed).
   For now this is a high-level helper that can use cuBLAS internally. */
cce_result cce_gpu_batch_forward(cce_gpu_ctx* ctx,
                                 const cce_block* blk,
                                 const cce_tensor* batch_input_dev,   /* device */
                                 cce_tensor* batch_output_dev);       /* device, must be pre-allocated */

/* Emit kernel source (OpenCL or CUDA PTX/C style) */
char* cce_gpu_emit_kernel(const char* block_name, int in_dim, int out_dim);

/* Sync a block's weights/moments to device (allocates device tensors if needed) */
cce_result cce_gpu_sync_block_to_device(cce_gpu_ctx* ctx, cce_block* blk);

/* Run Adam update on device for a block (assumes data is on device) */
cce_result cce_gpu_block_adam_update(cce_gpu_ctx* ctx, cce_block* blk,
                                     const float* error, const cce_tensor* input_host,
                                     float lr, float dfa_strength);

#ifdef __cplusplus
}
#endif

#endif /* CCE_GPU_H */