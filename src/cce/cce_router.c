#include "../../include/cce/cce_router.h"

#include <math.h>
#include <string.h>
#include <stdlib.h>

void cce_ssmax(const float* logits, int n, float temperature, float* probs, int top_k) {
    if (!logits || !probs || n <= 0) return;
    if (temperature <= 0) temperature = 1.0f;

    /* Scale logits */
    float* scaled = (float*)malloc(n * sizeof(float));
    for (int i = 0; i < n; ++i) {
        scaled[i] = logits[i] / temperature;
    }

    /* Find top-k (simple partial sort for small n) */
    int* idx = (int*)malloc(n * sizeof(int));
    for (int i = 0; i < n; ++i) idx[i] = i;

    /* Bubble sort top-k (demo, fine for <1000 branches) */
    for (int i = 0; i < top_k && i < n; ++i) {
        for (int j = i + 1; j < n; ++j) {
            if (scaled[idx[j]] > scaled[idx[i]]) {
                int tmp = idx[i]; idx[i] = idx[j]; idx[j] = tmp;
            }
        }
    }

    /* Zero out everything beyond top-k */
    float sum = 0.0f;
    for (int i = 0; i < n; ++i) {
        if (i < top_k) {
            probs[idx[i]] = expf(scaled[idx[i]]);
            sum += probs[idx[i]];
        } else {
            probs[idx[i]] = 0.0f;
        }
    }

    /* Normalize */
    if (sum > 0) {
        for (int i = 0; i < top_k && i < n; ++i) {
            probs[idx[i]] /= sum;
        }
    } else {
        /* fallback uniform on top-k */
        float uni = 1.0f / top_k;
        for (int i = 0; i < top_k && i < n; ++i) {
            probs[idx[i]] = uni;
        }
    }

    free(scaled);
    free(idx);
}

cce_result cce_router_init(cce_router* r, float temp, int top_k) {
    if (!r) return CCE_ERR_INVALID_ARG;
    r->temperature = (temp > 0.0f) ? temp : 1.0f;
    r->novelty_threshold = 0.3f;
    r->top_k = (top_k > 0) ? top_k : 1;
    return CCE_OK;
}

cce_result cce_router_route(cce_router* r, cce_forest* forest,
                            const float* input, int dim,
                            int* top_branch, float* score) {
    if (!r || !forest || !input || !top_branch || forest->num_branches == 0)
        return CCE_ERR_INVALID_ARG;

    float* logits = (float*)malloc(forest->num_branches * sizeof(float));
    if (!logits) return CCE_ERR_OOM;

    /* Compute real routing scores: similarity to centroid + goodness */
    for (int i = 0; i < forest->num_branches; ++i) {
        cce_branch* br = &forest->branches[i];
        float sim = 0.0f;
        int cdim = (br->centroid_dim < dim) ? br->centroid_dim : dim;
        for (int d = 0; d < cdim; ++d) {
            float diff = input[d] - br->centroid[d];
            sim -= diff * diff;  /* negative distance */
        }
        float goodness = (br->cascade && br->cascade->goodness > 0) ? br->cascade->goodness : 0.5f;
        logits[i] = sim * 0.7f + goodness * 0.3f;
    }

    /* Apply SSMax */
    float* probs = (float*)calloc(forest->num_branches, sizeof(float));
    cce_ssmax(logits, forest->num_branches, r->temperature, probs, r->top_k);

    /* Pick top branch (highest prob) */
    int best = 0;
    float best_p = 0.0f;
    for (int i = 0; i < forest->num_branches; ++i) {
        if (probs[i] > best_p) {
            best_p = probs[i];
            best = i;
        }
    }

    *top_branch = best;
    if (score) *score = best_p;

    free(logits);
    free(probs);
    return CCE_OK;
}
