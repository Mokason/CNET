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

#define CCE_ROUTER_MAX_COUNTERFACTUALS 3

typedef struct CounterfactualRoute {
    int   branch_index;
    float route_score;
    float logit;
    float consistency_score;
    float contrast_margin;
    char  branch_name[64];
} CounterfactualRoute;

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

/* Lightweight task classifier for router hints. Nonzero means the prompt is
   creative/narrative enough to prefer branches marked NARRATIVE_SPECIALIST. */
int cce_router_is_creative_task(const char* task_text);

/* Route with an optional task hint. Existing cce_router_route behavior is
   unchanged; this entry point applies a small preference to narrative
   specialists when task_text is creative/story/testimony oriented. */
cce_result cce_router_route_for_task(cce_router* r, cce_forest* forest,
                                     const float* input, int dim,
                                     const char* task_text,
                                     int* top_branch, float* score);

/* Sample ranked alternative branches for counterfactual verification.
   The primary branch is excluded; up to CCE_ROUTER_MAX_COUNTERFACTUALS
   alternatives are returned, even if max_routes is larger. */
cce_result cce_router_sample_counterfactuals(cce_router* r, cce_forest* forest,
                                             const float* input, int dim,
                                             int primary_branch,
                                             CounterfactualRoute* out_routes,
                                             int max_routes,
                                             int* out_count);

/* Conservative consistency score: minimum primary-vs-counterfactual margin
   mapped to [0, 1]. 1.0 means no meaningful alternative challenged the route. */
float cce_router_consistency_score(const CounterfactualRoute* route,
                                   const CounterfactualRoute* counterfactual_routes,
                                   int counterfactual_count);

/* Claim verification interface for counterfactual routing. This layer is
   intentionally route-grounded: it scores route stability around the claim
   rather than trying to prove the claim from raw text alone. */
cce_result cce_router_verify_claim(const CounterfactualRoute* route,
                                   const char* claim,
                                   const CounterfactualRoute* counterfactual_routes,
                                   int counterfactual_count,
                                   float* consistency_score,
                                   float* uncertainty);

#ifdef __cplusplus
}
#endif

#endif /* CCE_ROUTER_H */
