#ifndef CCE_ROUTER_H
#define CCE_ROUTER_H

#include "cce_defs.h"
#include "cce_forest.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Router: decides which branch(es) to use (SSMax / novelty / goodness) */
typedef struct {
    float temperature;
    float novelty_threshold;
    int   top_k;               /* for sparse top-k SSMax */
} cce_router;

/* Sparse Softmax (SSMax): computes a sparse distribution over scores.
   Zeros out low values below a threshold for efficiency in routing. */
void cce_ssmax(const float* logits, int n, float temperature, float* probs, int top_k);

/* Initialize router with SSMax params */
cce_result cce_router_init(cce_router* r, float temp, int top_k);

/* Route using real scores from forest (centroid similarity + goodness).
   Returns top branch index and its routing score. */
cce_result cce_router_route(cce_router* r, cce_forest* forest,
                            const float* input, int dim,
                            int* top_branch, float* score);

#ifdef __cplusplus
}
#endif

#endif /* CCE_ROUTER_H */
