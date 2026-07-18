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
    int   top_k;               /* sparse top-k SSMax support cap */
    float ssmax_margin;        /* DSA-style relative margin (scaled logits) */
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

/* Sparse Softmax (SSMax) — DSA-inspired sparse selection for routing.
 *
 * Dense softmax spreads nonzero mass under the whole curve (weak candidates
 * still get weight). DeepSeek Sparse Attention (DSA) selects top heavy-hitters
 * then normalizes only over that support. SSMax does the same for branch
 * routing: top-k ∩ relative margin of max → stable softmax on support only;
 * exact zeros elsewhere.
 *
 * cce_ssmax:      default margin (4.0)
 * cce_ssmax_ex:   explicit margin (pass <0 for default)
 * cce_ssmax_support_size: count of nonzero probs (tests/telemetry)
 * cce_ssmax_topk_weights: MoE-style top-k on logits → renorm only on support
 *   (bit-identical to dense-softmax→top-k→renorm before optional sleep)
 * cce_sleep_renorm: physics-style dead-zone — w < eps → exact 0, renorm;
 *   survivors can be skipped next cycle (no matmul / no prefetch)
 */
#ifndef CCE_SLEEP_DEFAULT_EPS
#define CCE_SLEEP_DEFAULT_EPS 1.0e-4f
#endif

void cce_ssmax(const float* logits, int n, float temperature, float* probs, int top_k);
void cce_ssmax_ex(const float* logits, int n, float temperature, float* probs,
                  int top_k, float margin);
int  cce_ssmax_support_size(const float* probs, int n);

/* Select top_k indices by logit (ties → lowest index), then stable softmax
 * only over that support. Writes sel[0..k) and w[0..k) with sum(w)=1.
 * Returns k (= min(top_k, n)), or 0 on invalid args. */
int cce_ssmax_topk_weights(const float* logits, int n, int top_k,
                           int* sel, float* w);

/* Sleep dead-zone on a weight vector (unit-sum mix). Values in (0, eps) become
 * exact 0; survivors renormed to sum 1. If all sleep, one-hot at fallback_idx
 * (clamped to [0,n)). Returns surviving nonzero count. eps < 0 → default.
 * For aggressive floor+sleep keep≥1 see cce_round_down_keep_one in cce_dsa.h
 * (negatives allowed on raw index scores; weights stay non-negative). */
int cce_sleep_renorm(float* w, int n, float eps, int fallback_idx);

/* Enable x86 FTZ+DAZ (flush/denormals-are-zero) for the calling thread.
 * Idempotent; no-op on non-x86. Cuts denormal thrash in exp/softmax tails. */
void cce_fp_enable_ftz_daz(void);

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
