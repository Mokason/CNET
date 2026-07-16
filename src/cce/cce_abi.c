#include "../../include/cce/cce.h"

#include <stdlib.h>
#include <string.h>

struct cce_handle {
    cce_forest* forest;
    cce_router  router;
    cce_learner learner;
    cce_scheduler* scheduler;
    int adapt_step;
};

cce_result cce_open(cce_handle** h, const char* archive_path) {
    if (!h || !archive_path) return CCE_ERR_INVALID_ARG;
    cce_handle* handle = (cce_handle*)calloc(1, sizeof(cce_handle));
    if (!handle) return CCE_ERR_OOM;

    if (cce_forest_open(&handle->forest, archive_path, 1024) != CCE_OK) {
        free(handle);
        return CCE_ERR_IO;
    }
    cce_router_init(&handle->router, 1.0f, 8);
    cce_learner_init(&handle->learner, 0.7f);
    handle->learner.diff_mode = handle->forest->diff_mode;  /* sync with forest */
    handle->scheduler = NULL;
    handle->adapt_step = 0;

    *h = handle;
    return CCE_OK;
}

void cce_close(cce_handle* h) {
    if (!h) return;
    if (h->forest) cce_forest_close(h->forest);
    free(h);
}

cce_result cce_infer(cce_handle* h, const float* input, int dim, int* out_label, float* conf) {
    if (!h || !h->forest) return CCE_ERR_INVALID_ARG;
    return cce_forest_infer(h->forest, input, dim, out_label, conf);
}

cce_result cce_adapt(cce_handle* h, const float* input, int dim, int label, float lr) {
    if (!h || !h->forest || !input || dim <= 0) return CCE_ERR_INVALID_ARG;
    if (h->forest->num_branches == 0) return CCE_ERR_NOT_FOUND;

    /* Real router-learn loop:
       1. Route using current forest centroids + goodness (SSMax)
       2. Promote the branch to HOT if needed (loads from archive if persisted)
       3. Build target tensor (treat label as regression target or one-hot proxy for demo)
       4. Adapt the selected cascade with stored-activation local learning (NoProp/FF aware)
          -> automatically uses per-branch diff_mode (branch override > forest > learner)
       5. Refresh branch centroid (EMA of seen input) and cascade goodness
       6. Router sees updated goodness automatically on next route
    */
    int branch_idx = 0;
    float score = 0.0f;
    if (cce_router_route(&h->router, h->forest, input, dim, &branch_idx, &score) != CCE_OK) {
        branch_idx = 0;
    }

    cce_branch* br = &h->forest->branches[branch_idx];
    if (br->tier != CCE_TIER_HOT || !br->cascade) {
        cce_forest_promote_to_hot(h->forest, branch_idx);
    }
    if (!br->cascade || br->cascade->num_blocks == 0) return CCE_ERR_INVALID_ARG;

    /* Wire per-branch diff_mode (and cascade inherit) so cce_learner_adapt + dfa_update respect it */
    cce_diff_mode_t use_dm = (br->diff_mode >= 0) ? br->diff_mode : h->forest->diff_mode;
    h->learner.diff_mode = use_dm;
    if (br->cascade->diff_mode < 0) br->cascade->diff_mode = use_dm;

    /* Build input tensor and a target (simple: scalar label expanded or basic regression on label) */
    cce_tensor xin, yt;
    int xsh[1] = {dim};
    int ydim = 1;

    if (br->cascade->num_blocks > 0) {
        cce_block* head = &br->cascade->blocks[br->cascade->num_blocks - 1];
        if (head->weights.shape[1] <= 0) return CCE_ERR_INVALID_ARG;
        ydim = head->weights.shape[1];
    }
    int ysh[1] = {ydim};  /* for demo treat as scalar target; multi-class makes one-hot */
    if (cce_tensor_alloc(&xin, xsh, 1) != CCE_OK) return CCE_ERR_OOM;
    memcpy(xin.data, input, dim * sizeof(float));

    if (cce_tensor_alloc(&yt, ysh, 1) != CCE_OK) {
        cce_tensor_free(&xin);
        return CCE_ERR_OOM;
    }
    if (ydim == 1) {
        yt.data[0] = (float)label * 0.1f;  /* demo mapping; for classification use onehot in future task */
    } else {
        int i;
        for (i = 0; i < ydim; ++i) {
            yt.data[i] = 0.0f;
        }
        if (label < 0 || label >= ydim) {
            cce_tensor_free(&xin);
            cce_tensor_free(&yt);
            return CCE_ERR_INVALID_ARG;
        }
        yt.data[label] = 1.0f;
    }

    int prev_classify = h->learner.classify;
    h->learner.classify = (ydim > 1) ? 1 : 0;

    /* Respect attached scheduler (model's) for LR when present */
    float eff_lr = lr;
    if (h->scheduler) {
        float dummy_loss = 0.2f;
        int fake_epoch = h->adapt_step / 64;  /* rough step->epoch */
        eff_lr = cce_scheduler_get_lr(h->scheduler, fake_epoch, dummy_loss);
        h->adapt_step++;
    }

    cce_result rc = cce_learner_adapt(&h->learner, br->cascade, &xin, &yt, eff_lr);
    h->learner.classify = prev_classify;

    /* Update router-visible state */
    br->cascade->goodness = (br->cascade->goodness * 0.7f) + 0.3f * (1.0f - (rc == CCE_OK ? 0.1f : 0.5f));

    /* EMA centroid update from this input (router-learn feedback) */
    int cdim = br->centroid_dim;
    for (int d = 0; d < cdim && d < dim; ++d) {
        br->centroid[d] = br->centroid[d] * 0.9f + input[d] * 0.1f;
        h->forest->centroids[branch_idx * cdim + d] = br->centroid[d];
    }

    /* Online forest growth + micro-split during router-learn loop */
    if ((rand() % 20) == 0 && br->cascade->num_blocks > 0) {
        /* Micro-split a block for exploration (ACE style) on this specialist */
        int split_idx = rand() % br->cascade->num_blocks;
        cce_cascade_micro_split(br->cascade, split_idx);
        /* If too many branches, could grow new specialist, but here deepen current */
    }

    cce_tensor_free(&xin);
    cce_tensor_free(&yt);
    return rc;
}

cce_result cce_tick(cce_handle* h) {
    (void)h;
    /* Run tiering, contract frozen branches, etc. */
    return CCE_OK;
}

void cce_set_diff_mode(cce_handle* h, cce_diff_mode_t mode) {
    if (!h) return;
    if (h->forest) {
        cce_forest_set_diff_mode(h->forest, mode);
    }
    h->learner.diff_mode = mode;
}

void cce_set_scheduler(cce_handle* h, cce_scheduler* sched) {
    if (!h) return;
    h->scheduler = sched;
}
