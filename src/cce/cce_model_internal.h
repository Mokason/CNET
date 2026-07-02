#ifndef CCE_MODEL_INTERNAL_H
#define CCE_MODEL_INTERNAL_H

#include "../../include/cce/cce_forest.h"
#include "../../include/cce/cce_learn.h"
#include "../../include/cce/cce_router.h"
#include "../../include/cce/cce_autograd.h"

struct cce_model {
    char name[64];
    cce_forest* forests[16];
    char        forest_names[16][64];
    int num_forests;
    cce_scheduler* scheduler;
    cce_diff_mode_t diff_mode;
    cce_router    router;

    /* training knobs (default to today's hardcoded behavior) */
    int   classify;             /* 0=MSE, 1=softmax cross-entropy */
    float goodness_threshold;   /* default 0.7 */
    float dfa_strength;         /* 0 => leave learner default */
    float grad_clip;            /* 0 => disabled */

    /* GPU acceleration (optional). When set, dispatch_train_sample passes it to learner. */
    struct cce_gpu_ctx* gpu;

    /* Optional autograd for EXACT mode */
    int use_autograd;
    cce_ag_ctx* ag;  /* reused across samples when use_autograd; created on set */

    /* ownership for loaded models */
    int owns_forests;
    int owns_scheduler;
    int owns_gpu;
};

#endif
