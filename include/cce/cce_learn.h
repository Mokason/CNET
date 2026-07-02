#ifndef CCE_LEARN_H
#define CCE_LEARN_H

#include "cce_defs.h"
#include "cce_cascade.h"
#include "cce_tensor.h"
#include "cce_gpu.h"   /* for optional gpu ctx in train_dynamic */

#ifdef __cplusplus
extern "C" {
#endif

/* Learning engine for CCE: goodness gating + hybrid credit (DFA / local) */
typedef struct {
    float goodness_threshold;
    float dfa_strength;
    struct cce_gpu_ctx* gpu;   /* optional: set for CUDA acceleration on 4070 etc. */
    int classify;              /* 0 = MSE/regression output; 1 = softmax cross-entropy */
    cce_diff_mode_t diff_mode; /* LOCAL (default) | HYBRID | EXACT -- see above */
    float grad_clip;           /* 0 = disabled; >0 clips |local_e| norm for stable training */
} cce_learner;

cce_result cce_learner_init(cce_learner* l, float thresh);

/* Set differentiation mode (default LOCAL on init). Can be changed anytime.
   LOCAL: core compositional local credit (NoProp/DFA/FF)
   EXACT: full backprop through cascade for higher quality where needed (e.g. embeddings)
   Does not affect contracts, freezing, or sub-branch structure. */
void cce_learner_set_diff_mode(cce_learner* l, cce_diff_mode_t mode);

cce_result cce_learner_adapt(cce_learner* l,
                             cce_cascade* cas,
                             const cce_tensor* input,
                             const cce_tensor* target,
                             float lr);

/* Goodness gate: returns true if the cascade/block should adapt based on error/goodness */
bool cce_goodness_gate(cce_cascade* cas, float error, float threshold);

/* High-level CCE training entry (deep training engine). Mirrors legacy dynamic training.
   Trains cascade on table with early stop, validation. Returns final loss or negative on err.
   Uses per-block local targets, momentum (in dfa), freezing.
   diff_mode: 0=LOCAL (default, preserves core), 2=EXACT for general-purpose differentiation quality. */
double cce_train_dynamic(cce_cascade* cas,
                        const float* inputs, const float* targets,
                        size_t sample_count, int in_dim, int out_dim,
                        size_t max_epochs, float target_loss, float lr,
                        int classify,        /* 1 => softmax cross-entropy output */
                        cce_gpu_ctx* gpu,    /* pass CUDA ctx for acceleration on 4070 */
                        int diff_mode);      /* 0=LOCAL (default), 2=EXACT backprop opt-in */

/* Tiny scheduler helper struct (avoids env-var only hacks) */
typedef enum {
    CCE_SCHED_COSINE = 0,
    CCE_SCHED_WARMUP = 1,
    CCE_SCHED_PLATEAU = 2,
    CCE_SCHED_STEP = 3
} cce_sched_type_t;

typedef struct {
    cce_sched_type_t type;
    float initial_lr;
    int warmup_epochs;
    float decay_factor;     /* e.g. 0.5 for step */
    int step_size;
    float plateau_factor;   /* e.g. 0.5 */
    int plateau_patience;
    /* internal state */
    float current_lr;
    float best_loss;
    int patience_counter;
    int total_steps;
} cce_scheduler;

cce_result cce_scheduler_init(cce_scheduler* s, cce_sched_type_t type, float initial_lr);
float cce_scheduler_get_lr(cce_scheduler* s, int epoch, float current_loss);

/* DLL export versions (so they are visible when building cce.dll) */
#ifdef CCE_BUILD_DLL
#ifdef _WIN32
#define CCE_API __declspec(dllexport)
#else
#define CCE_API __attribute__((visibility("default")))
#endif
#else
#define CCE_API
#endif

CCE_API cce_result cce_scheduler_init(cce_scheduler* s, cce_sched_type_t type, float initial_lr);
CCE_API float      cce_scheduler_get_lr(cce_scheduler* s, int epoch, float current_loss);

#ifdef __cplusplus
}
#endif

#endif /* CCE_LEARN_H */
