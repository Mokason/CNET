#ifndef CNET_SELF_IMPROVE_H
#define CNET_SELF_IMPROVE_H

/* Bounded self-improvement helpers: distillation of proven multi-step plans
 * into certified chunks, and recipe-proposal records that never lower bars.
 *
 * Authority rules:
 *   - Distillation reuses consolidate_route / library_evolve_gated only.
 *   - Proposals are advisory JSONL rows; they do not mutate AcquireConfig.
 *   - Resource governor budgets may refuse further closes; they never skip
 *     certification of a unit already under construction.
 *
 * Gate: make self_improve → SELF_IMPROVE_PASS.
 */

#include <stddef.h>

#include "cnet_export.h"
#include "consolidate.h"
#include "library.h"
#include "nn.h"
#include "resource_governor.h"
#include "router.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    size_t distilled;          /* chunks minted this pass */
    size_t refused;            /* consolidate refusals */
    size_t skipped_budget;     /* refused by resource governor rate limit */
    size_t proposals_written;  /* recipe proposal lines appended */
} SelfImproveReport;

/* Distill one multi-step route plan into a new primitive when the plan has
   length >= 2 and every member clears the library evidence gate. Registers
   the chunk into *reg on success. cfg/gate NULL = defaults. gov may be NULL
   (unlimited). Returns 0 on mint, 1 if skipped (budget/gate), <0 on error. */
CNET_API int self_improve_distill_route(
    PrimitiveRegistry *reg,
    const RoutePlan *plan,
    const ConsolidateConfig *cfg,
    const LibraryGateConfig *gate,
    CnetResourceGovernor *gov,
    BinaryTransformNetwork **out_chunk,  /* owned by caller on success; may be NULL */
    SelfImproveReport *report);

/* Append one evidence-backed recipe proposal to path (JSONL). kind is a
   short atom (e.g. "topk_set_v2", "train_fast_default"). evidence is free
   text / path. Never writes a row that requests lowering a cert bar.
   Returns 0, or <0. */
CNET_API int self_improve_propose_recipe(
    const char *path,
    const char *kind,
    const char *evidence,
    int priority,
    SelfImproveReport *report);

/* Apply deploy free-win environment defaults when the corresponding env
   vars are unset (does not override an explicit operator choice).
   Returns the number of variables set. */
CNET_API int self_improve_apply_deploy_env(void);

#ifdef __cplusplus
}
#endif

#endif /* CNET_SELF_IMPROVE_H */
