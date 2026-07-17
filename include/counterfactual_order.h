#ifndef CNET_COUNTERFACTUAL_ORDER_H
#define CNET_COUNTERFACTUAL_ORDER_H

/* ORDER_ONLY ranking among already-certified peers.
 *
 * Primary key: learned reliability (desc).
 * Secondary (only when enabled): counterfactual consistency score (desc).
 * Tertiary: stable name order (asc) for determinism.
 *
 * Certification is never consulted here — callers must pass only certified
 * units. Enabling CF order must not invent validity or demote a certified
 * unit. Gate: make counterfactual_order → CF_ORDER_PASS.
 */

#include <stddef.h>

#include "cnet_export.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *name;          /* borrowed; used only for stable tie-break */
    double      reliability;   /* Laplace reliability in [0,1] */
    double      cf_score;      /* counterfactual consistency; ignored if !use_cf */
    int         certified;     /* must be nonzero to enter the ranking */
} CnetCfOrderCandidate;

/* Rank up to n candidates into order[0..*out_n): certified-only, reliability
   primary, optional CF secondary. order[] must hold at least n indices.
   Returns 0, or -1 on bad args. *out_n is the count of certified candidates. */
CNET_API int cnet_cf_order_rank(const CnetCfOrderCandidate *cands, size_t n,
                                int use_cf,
                                size_t *order, size_t *out_n);

/* 1 iff env CNET_CF_ORDER is set to a non-empty value other than "0". */
CNET_API int cnet_cf_order_env_enabled(void);

#ifdef __cplusplus
}
#endif

#endif /* CNET_COUNTERFACTUAL_ORDER_H */
