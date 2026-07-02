#ifndef CCE_AUTOGRAD_H
#define CCE_AUTOGRAD_H

/* Optional reverse-mode autograd tape for CCE.
 * Used ONLY for exact differentiable tails/heads (CCE_DIFF_EXACT).
 * Local and hybrid learning paths remain unchanged and do not use this.
 *
 * Design: small, allocation-light tape per context. No global graph.
 * Not a replacement for CCE's local credit assignment.
 */

#include "cce_defs.h"
#include "cce_tensor.h"

/* Forward for GPU context (used in optional set) */
typedef struct cce_gpu_ctx cce_gpu_ctx;

#ifdef CCE_BUILD_DLL
#ifdef _WIN32
#define CCE_API __declspec(dllexport)
#else
#define CCE_API __attribute__((visibility("default")))
#endif
#else
#define CCE_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque types */
typedef struct cce_ag_ctx cce_ag_ctx;
typedef struct cce_ag_tensor cce_ag_tensor;

typedef enum {
    CCE_AG_OP_NONE = 0,
    CCE_AG_OP_ADD,
    CCE_AG_OP_ADD_BIAS,
    CCE_AG_OP_SUB,
    CCE_AG_OP_MUL,
    CCE_AG_OP_DIV,
    CCE_AG_OP_MATMUL,
    CCE_AG_OP_RELU,
    CCE_AG_OP_SIGMOID,
    CCE_AG_OP_TANH,
    CCE_AG_OP_MSE_LOSS,
    CCE_AG_OP_SOFTMAX_CE,
    CCE_AG_OP_RESHAPE,
    CCE_AG_OP_SUM,
    CCE_AG_OP_MEAN,
    CCE_AG_OP_CUSTOM
} cce_ag_op;

/* Context creation / destruction */
CCE_API cce_result cce_ag_create(cce_ag_ctx** ctx, size_t max_nodes);
CCE_API void cce_ag_destroy(cce_ag_ctx* ctx);

/* Reset graph for next forward (keeps buffers) */
CCE_API void cce_ag_zero_grad(cce_ag_ctx* ctx);

/* Full reset for reuse (frees current, resets counters). For scaling when reusing ctx across independent samples. */
void cce_ag_reset(cce_ag_ctx* ctx);

/* Backward from a scalar loss */
CCE_API cce_result cce_ag_backward(cce_ag_ctx* ctx, cce_ag_tensor* loss);

/* Create a tensor from host data. If requires_grad, grad buffer is allocated. */
CCE_API cce_result cce_ag_tensor_from_array(
    cce_ag_ctx* ctx,
    const float* data,
    int rows,
    int cols,
    int requires_grad,
    cce_ag_tensor** out);

/* Accessors */
CCE_API const float* cce_ag_data(const cce_ag_tensor* t);
CCE_API const float* cce_ag_grad(const cce_ag_tensor* t);
CCE_API int cce_ag_tensor_rows(const cce_ag_tensor* t);
CCE_API int cce_ag_tensor_cols(const cce_ag_tensor* t);

/* Ops */
CCE_API cce_result cce_ag_add(cce_ag_ctx* ctx, cce_ag_tensor* a, cce_ag_tensor* b, cce_ag_tensor** out);
CCE_API cce_result cce_ag_add_bias(cce_ag_ctx* ctx, cce_ag_tensor* x, cce_ag_tensor* bias, cce_ag_tensor** out);
CCE_API cce_result cce_ag_sub(cce_ag_ctx* ctx, cce_ag_tensor* a, cce_ag_tensor* b, cce_ag_tensor** out);
CCE_API cce_result cce_ag_mul(cce_ag_ctx* ctx, cce_ag_tensor* a, cce_ag_tensor* b, cce_ag_tensor** out);
CCE_API cce_result cce_ag_matmul(cce_ag_ctx* ctx, cce_ag_tensor* a, cce_ag_tensor* b, cce_ag_tensor** out);
CCE_API cce_result cce_ag_relu(cce_ag_ctx* ctx, cce_ag_tensor* x, cce_ag_tensor** out);
CCE_API cce_result cce_ag_tanh(cce_ag_ctx* ctx, cce_ag_tensor* x, cce_ag_tensor** out);
CCE_API cce_result cce_ag_sigmoid(cce_ag_ctx* ctx, cce_ag_tensor* x, cce_ag_tensor** out);
CCE_API cce_result cce_ag_exp(cce_ag_ctx* ctx, cce_ag_tensor* x, cce_ag_tensor** out);
CCE_API cce_result cce_ag_log(cce_ag_ctx* ctx, cce_ag_tensor* x, cce_ag_tensor** out);
CCE_API cce_result cce_ag_mse_loss(cce_ag_ctx* ctx, cce_ag_tensor* pred, cce_ag_tensor* target, cce_ag_tensor** loss);
CCE_API cce_result cce_ag_softmax_cross_entropy(cce_ag_ctx* ctx, cce_ag_tensor* logits, cce_ag_tensor* target, cce_ag_tensor** loss);

/* Additional ops for completeness */
CCE_API cce_result cce_ag_mul(cce_ag_ctx* ctx, cce_ag_tensor* a, cce_ag_tensor* b, cce_ag_tensor** out);
CCE_API cce_result cce_ag_sub(cce_ag_ctx* ctx, cce_ag_tensor* a, cce_ag_tensor* b, cce_ag_tensor** out);
CCE_API cce_result cce_ag_div(cce_ag_ctx* ctx, cce_ag_tensor* a, cce_ag_tensor* b, cce_ag_tensor** out);
CCE_API cce_result cce_ag_reshape(cce_ag_ctx* ctx, cce_ag_tensor* t, int new_rows, int new_cols, cce_ag_tensor** out);
CCE_API cce_result cce_ag_sum(cce_ag_ctx* ctx, cce_ag_tensor* t, int dim, cce_ag_tensor** out);  /* dim=-1 for all -> scalar */
CCE_API cce_result cce_ag_mean(cce_ag_ctx* ctx, cce_ag_tensor* t, int dim, cce_ag_tensor** out);

/* Tiny optimizer (SGD). Parameters must be requires_grad tensors. */
CCE_API cce_result cce_ag_sgd_step(cce_ag_tensor** parameters, size_t count, float lr);

/* Adam step for autograd tensors (breadth). */
CCE_API cce_result cce_ag_adam_step(cce_ag_tensor** parameters, size_t count, float lr,
                                    float beta1, float beta2, float eps, int t);

/* Custom op ABI: allows CCE internal structures (cascades, blocks) to participate in autograd.
 * The backward_fn receives the output tensor and must accumulate grads into the input tensors.
 * user_ctx is passed through.
 */
typedef void (*cce_ag_custom_backward_fn)(cce_ag_ctx* ctx,
                                          cce_ag_tensor* out_grad_owner,
                                          cce_ag_tensor** inputs,
                                          int num_inputs,
                                          void* user_ctx);

CCE_API cce_result cce_ag_custom_op(cce_ag_ctx* ctx,
                                    cce_ag_tensor** inputs,
                                    int num_inputs,
                                    int out_rows,
                                    int out_cols,
                                    cce_ag_custom_backward_fn backward_fn,
                                    void* user_ctx,
                                    cce_ag_tensor** out);

/* Attach GPU context to autograd (optional). When set, supported ops (matmul etc.)
 * will attempt GPU acceleration using the existing cce_gpu backend.
 */
CCE_API cce_result cce_ag_set_gpu(cce_ag_ctx* ctx, struct cce_gpu_ctx* gpu_ctx);

#ifdef __cplusplus
}
#endif

#endif /* CCE_AUTOGRAD_H */
