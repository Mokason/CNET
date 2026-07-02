#ifndef CCE_PERCEPTUAL_LEAF_H
#define CCE_PERCEPTUAL_LEAF_H

/* Reusable CCE-based perceptual leaf with sub-branches / forest.
 * Hybrid of:
 *   A: Forest of specialist branches for a raw perceptual domain.
 *   C: Cascades (deeper blocks) inside each branch.
 *
 * Designed initially for the 7-segment case to improve margin/abstention
 * by using router score + per-branch goodness + output margin.
 *
 * Supports both hard onehot snap and PORT_EVIDENCE style top-k distrib output
 * (5B late binding) when confidence is marginal.
 */

#include "cce_forest.h"
#include "cce_router.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Generalized perceptual leaf forest with sub-branches.
 * Supports glyph (35), 7seg (7), grid (15), block (16) etc.
 * - general branch + (for 7seg) specialists for aliasing groups
 * - each branch is a small cascade of linear (+ optional head) blocks.
 * Returns 0 on success. Caller owns the forest (close later).
 * diff_mode: CCE_DIFF_LOCAL (default), HYBRID, or EXACT for specific specialists.
 */
int cce_perceptual_create(cce_forest** forest, cce_router* router,
                          int input_dim, int num_classes,
                          const char* domain_prefix, int max_branches,
                          cce_diff_mode_t diff_mode);

/* Train the sub-forest for the given domain (uses embedded renderers).
 * General branch on full distribution; specialists where configured (e.g. 7seg).
 * Updates centroids.
 */
int cce_perceptual_train(cce_forest* forest, int input_dim);

/* Forward through the sub-forest.
 * input: input_dim floats (raw noisy features, 0..1 range)
 * out_onehot: num_classes floats (softmax-like)
 * router_score, branch_goodness: for composite confidence
 * best_class: argmax
 *
 * evidence_out: if non-NULL and topk>0, fills top-k normalized weights
 *               (for PORT_EVIDENCE / late binding).
 *               Caller must provide space for at least topk entries.
 *
 * Returns 0 on success.
 */
int cce_perceptual_forward(cce_forest* forest, cce_router* router,
                           const float* input, int dim,
                           float* out_onehot, int out_dim,
                           float* router_score, float* branch_goodness,
                           int* best_class,
                           float* evidence_out, int topk);

/* Compute an effective margin from base (onehot gap) + routing signals.
 * Useful for the abstention gate.
 */
float cce_perceptual_effective_margin(float base_margin,
                                      float router_score,
                                      float branch_goodness);

/* Back-compat wrappers for 7seg (input_dim=7) */
int cce_perceptual_7seg_create(cce_forest** forest, cce_router* router, int max_branches);
int cce_perceptual_7seg_train(cce_forest* forest);
int cce_perceptual_7seg_forward(cce_forest* forest, cce_router* router,
                                const float* input, int dim,
                                float* out_onehot, int out_dim,
                                float* router_score, float* branch_goodness,
                                int* best_class,
                                float* evidence_out, int topk);
float cce_perceptual_7seg_effective_margin(float base_margin,
                                           float router_score,
                                           float branch_goodness);

#ifdef __cplusplus
}
#endif

#endif /* CCE_PERCEPTUAL_LEAF_H */
