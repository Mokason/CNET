#ifndef CCE_MODEL_H
#define CCE_MODEL_H

/* High-level CCE Model wrapper for training API.
 * This is the C-side library for use from .NET (via P/Invoke) or other hosts.
 * Goal: provide PyTorch-like training loop + data pipeline on top of CCE core,
 * while keeping local learning, contracts, sub-branches, etc.
 *
 * Usage from .NET:
 *   - Build as DLL
 *   - P/Invoke the functions
 *   - Wrap in C# classes for nice Training API
 */

#include "cce_defs.h"
#include "cce_forest.h"
#include "cce_learn.h"
#include "cce_dataset.h"

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

typedef struct cce_model cce_model;

/* Create a new model (container for forests/branches + training state). */
CCE_API cce_result cce_model_create(cce_model** model, const char* name);

/* Destroy the model. */
CCE_API void cce_model_destroy(cce_model* model);

/* Add an existing forest as a "module" / branch group.
 * name is used for routing / identification.
 */
CCE_API cce_result cce_model_add_forest(cce_model* model, cce_forest* forest, const char* name);

/* Set a scheduler for the training loop (reused across epochs). */
CCE_API cce_result cce_model_set_scheduler(cce_model* model, cce_scheduler* scheduler);

/* Set global diff mode (can be overridden per-forest/branch). */
CCE_API cce_result cce_model_set_diff_mode(cce_model* model, cce_diff_mode_t mode);

/* Output loss for training: 0 = MSE/regression, 1 = softmax cross-entropy. */
CCE_API cce_result cce_model_set_loss(cce_model* model, int classify);

/* Learner knobs. Pass <=0 to leave a knob at its default.
   goodness_threshold default 0.7; dfa_strength 0 = learner default; grad_clip 0 = disabled. */
CCE_API cce_result cce_model_set_learn_params(cce_model* model,
                                              float goodness_threshold,
                                              float dfa_strength,
                                              float grad_clip);

/* Optional: use the autograd tape for EXACT diff mode (small tails/heads). Default 0. */
CCE_API cce_result cce_model_set_use_autograd(cce_model* model, int use);

/* Attach an optional GPU context (from cce_gpu_init_cuda or cce_gpu_init).
   The model does not take ownership unless you use the _owned variant or set owns_gpu.
   When attached, training dispatch (Train/TrainBatch) will use GPU-accelerated paths
   where the learner and block support it (primarily CUDA Adam + matmul for larger blocks). */
CCE_API cce_result cce_model_set_gpu(cce_model* model, struct cce_gpu_ctx* gpu_ctx);

/* Like set_gpu but the model will destroy the ctx on model destroy. */
CCE_API cce_result cce_model_set_gpu_owned(cce_model* model, struct cce_gpu_ctx* gpu_ctx);

/* Basic training loop.
 * Runs for max_epochs or until target_loss.
 * Uses the attached scheduler for LR.
 * dataset provides batches.
 * Returns final loss or negative on error.
 */
CCE_API double cce_model_train(cce_model* model,
                               cce_dataset* dataset,
                               size_t max_epochs,
                               float target_loss);

/* Inference through the model (routes via forests). */
CCE_API cce_result cce_model_infer(cce_model* model, const float* input, int dim,
                                   int* out_label, float* conf);

/* Batch inference: routes each sample in the cce_batch using names/router to
   the right forest+branch. Writes labels/confs (caller allocates size >= batch_size). */
CCE_API cce_result cce_model_infer_batch(cce_model* model, const cce_batch* batch,
                                         int* out_labels, float* confs);

/* Raw forward through the routed specialist. Copies up to output_cap floats into
   output and writes the actual output dimension to out_dim. */
CCE_API cce_result cce_model_forward(cce_model* model,
                                     const float* input,
                                     int in_dim,
                                     float* output,
                                     int output_cap,
                                     int* out_dim);

/* Batched raw forward. Outputs are row-major with output_cap_per_sample stride. */
CCE_API cce_result cce_model_forward_batch(cce_model* model,
                                           const cce_batch* batch,
                                           float* outputs,
                                           int output_cap_per_sample,
                                           int* out_dim);

/* Train one batch (useful for custom loops or from a cce_batch directly).
   Returns the loss for that batch (or negative). Dispatches + respects modes/scheduler. */
CCE_API double cce_model_train_batch(cce_model* model, cce_batch* batch);

/* Optional: save/load the entire model state (forests + scheduler state). */
CCE_API cce_result cce_model_save(cce_model* model, const char* path);
CCE_API cce_result cce_model_load(cce_model* model, const char* path);

#ifdef __cplusplus
}
#endif

#endif /* CCE_MODEL_H */
