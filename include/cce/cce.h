#ifndef CCE_H
#define CCE_H

/* CCE Public C ABI for hosting (Unity, C#, etc.)
 * Stable interface. Link against libcce or the built .dll/.so
 */

#include "cce_defs.h"
#include "cce_forest.h"
#include "cce_router.h"
#include "cce_learn.h"
#include "cce_dataset.h"
#include "cce_model.h"
#include "cce_autograd.h"
#include "cce_safetensors.h"

#ifdef __cplusplus
extern "C" {
#endif

/* High-level convenient API */
typedef struct cce_handle cce_handle;

cce_result cce_open(cce_handle** h, const char* archive_path);
void       cce_close(cce_handle* h);

cce_result cce_infer(cce_handle* h, const float* input, int dim, int* out_label, float* conf);
cce_result cce_adapt(cce_handle* h, const float* input, int dim, int label, float lr);
cce_result cce_tick(cce_handle* h);  /* run tiering / ACE transitions */

void cce_set_diff_mode(cce_handle* h, cce_diff_mode_t mode);  /* set LOCAL/HYBRID/EXACT for this forest */

#ifdef CCE_BUILD_DLL
#ifdef _WIN32
#define CCE_API __declspec(dllexport)
#else
#define CCE_API __attribute__((visibility("default")))
#endif
#else
#define CCE_API
#endif

/* Explicit exports for P/Invoke / DllImport */
CCE_API cce_result cce_open(cce_handle** h, const char* archive_path);
CCE_API void       cce_close(cce_handle* h);
CCE_API cce_result cce_infer(cce_handle* h, const float* input, int dim, int* out_label, float* conf);
CCE_API cce_result cce_adapt(cce_handle* h, const float* input, int dim, int label, float lr);
CCE_API void       cce_set_diff_mode(cce_handle* h, cce_diff_mode_t mode);

/* Scheduler functions (needed for .NET managed scheduler wrappers) */
CCE_API cce_result cce_scheduler_init(cce_scheduler* s, cce_sched_type_t type, float initial_lr);
CCE_API float      cce_scheduler_get_lr(cce_scheduler* s, int epoch, float current_loss);

/* New high-level training API (for .NET P/Invoke etc.) */
CCE_API cce_result cce_model_create(cce_model** model, const char* name);
CCE_API void cce_model_destroy(cce_model* model);
CCE_API cce_result cce_model_add_forest(cce_model* model, cce_forest* forest, const char* name);
CCE_API cce_result cce_model_set_scheduler(cce_model* model, cce_scheduler* scheduler);
CCE_API cce_result cce_model_set_diff_mode(cce_model* model, cce_diff_mode_t mode);
CCE_API double cce_model_train(cce_model* model, cce_dataset* dataset, size_t max_epochs, float target_loss);
CCE_API cce_result cce_model_infer_batch(cce_model* model, const cce_batch* batch, int* out_labels, float* confs);
CCE_API cce_result cce_model_forward(cce_model* model, const float* input, int in_dim,
                                     float* output, int output_cap, int* out_dim);
CCE_API cce_result cce_model_forward_batch(cce_model* model, const cce_batch* batch,
                                           float* outputs, int output_cap_per_sample, int* out_dim);
CCE_API double cce_model_train_batch(cce_model* model, cce_batch* batch);

/* Composition-first forest construction API */
CCE_API cce_result cce_cascade_create(cce_cascade** cas, int max_blocks);
CCE_API void cce_cascade_destroy(cce_cascade* cas);
CCE_API cce_result cce_cascade_add_linear(cce_cascade* cas, int input_dim, int output_dim, float init_scale);
CCE_API cce_result cce_cascade_add_linear_head(cce_cascade* cas, int input_dim, int output_dim, float init_scale);
CCE_API cce_result cce_cascade_add_patch(cce_cascade* cas, int patch_size, int stride, int channels);
CCE_API cce_result cce_forest_add_cascade_branch(cce_forest* forest, cce_cascade* cascade,
                                                 const char* name, int* out_branch_idx);
CCE_API cce_result cce_forest_add_linear_branch(cce_forest* forest, const char* name,
                                                int input_dim, int hidden_dim, int output_dim,
                                                float init_scale, int* out_branch_idx);
CCE_API cce_result cce_forest_add_patch_branch(cce_forest* forest, const char* name,
                                               int patch_size, int stride, int channels,
                                               int hidden_dim, int output_dim,
                                               float init_scale, int* out_branch_idx);
CCE_API cce_result cce_forest_get_branch_blocks(cce_forest* forest, int branch_idx,
                                                int types[], int input_dims[], int output_dims[],
                                                int* num, int max_blocks);

/* Dataset API exports */
CCE_API cce_result cce_dataset_from_arrays(cce_dataset** ds, const float* inputs, const float* targets,
                                           size_t n_samples, size_t in_dim, size_t out_dim, size_t batch_size);
CCE_API cce_result cce_dataset_wrap_arrays(cce_dataset** ds, const float* inputs, const float* targets,
                                           size_t n_samples, size_t in_dim, size_t out_dim, size_t batch_size);
CCE_API cce_result cce_dataset_next_batch(cce_dataset* ds, cce_batch* batch);
CCE_API cce_result cce_dataset_reset(cce_dataset* ds);
CCE_API void cce_dataset_destroy(cce_dataset* ds);
CCE_API cce_result cce_dataset_shuffle(cce_dataset* ds);

/* Scheduler on handle for adapt */
CCE_API void cce_set_scheduler(cce_handle* h, cce_scheduler* sched);

/* Autograd (optional, for CCE_DIFF_EXACT tails only) */
CCE_API cce_result cce_ag_create(cce_ag_ctx** ctx, size_t max_nodes);
CCE_API void cce_ag_destroy(cce_ag_ctx* ctx);
CCE_API void cce_ag_zero_grad(cce_ag_ctx* ctx);
CCE_API cce_result cce_ag_backward(cce_ag_ctx* ctx, cce_ag_tensor* loss);

CCE_API cce_result cce_ag_tensor_from_array(
    cce_ag_ctx* ctx,
    const float* data,
    int rows,
    int cols,
    int requires_grad,
    cce_ag_tensor** out);

CCE_API const float* cce_ag_data(const cce_ag_tensor* t);
CCE_API const float* cce_ag_grad(const cce_ag_tensor* t);

CCE_API cce_result cce_ag_matmul(cce_ag_ctx* ctx, cce_ag_tensor* a, cce_ag_tensor* b, cce_ag_tensor** out);
CCE_API cce_result cce_ag_add_bias(cce_ag_ctx* ctx, cce_ag_tensor* x, cce_ag_tensor* bias, cce_ag_tensor** out);
CCE_API cce_result cce_ag_relu(cce_ag_ctx* ctx, cce_ag_tensor* x, cce_ag_tensor** out);
CCE_API cce_result cce_ag_mse_loss(cce_ag_ctx* ctx, cce_ag_tensor* pred, cce_ag_tensor* target, cce_ag_tensor** loss);
CCE_API cce_result cce_ag_softmax_cross_entropy(cce_ag_ctx* ctx, cce_ag_tensor* logits, cce_ag_tensor* target, cce_ag_tensor** loss);
CCE_API cce_result cce_ag_sgd_step(cce_ag_tensor** parameters, size_t count, float lr);

#ifdef __cplusplus
}
#endif

#endif /* CCE_H */
