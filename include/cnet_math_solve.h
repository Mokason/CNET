#ifndef CNET_MATH_SOLVE_H
#define CNET_MATH_SOLVE_H

/* Creative math solve (Phases 1–4), CNET approach:
 *   Tier A  certified templates (deterministic procedures)
 *   Tier B  multi-plan creativity (generate alternate plans)
 *   Tier C  executor math_eval (only source of numbers)
 *   Verify  plug-back / identity checks
 *   Learn   procedural SKILL.md + fact memory on success
 *
 * Gate: make math_solve → MATH_SOLVE_PASS
 */

#include <stddef.h>

#include "cnet_export.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CNET_MATH_TIER_NONE = 0,
    CNET_MATH_TIER_A_TEMPLATE = 1, /* certified local procedure */
    CNET_MATH_TIER_B_CREATIVE = 2, /* multi-plan invent */
    CNET_MATH_TIER_C_DIRECT = 3    /* plain expression eval */
} CnetMathTier;

typedef struct {
    int write_skill;       /* default 1 */
    int memorize;          /* default 1 */
    int allow_creative;    /* Phase 4 multi-plan, default 1 */
    int max_plans;         /* default 4 */
    char skills_dir[512];
} CnetMathSolveConfig;

typedef struct {
    CnetMathTier tier;
    int verified;
    int skill_written;
    int plans_tried;
    char method[64];
    char answer[128];
    char steps[512];       /* human-readable trace */
    char detail[256];
    char skill_path[512];
} CnetMathSolveReport;

CNET_API void cnet_math_solve_config_defaults(CnetMathSolveConfig *c);
CNET_API void cnet_math_solve_config_from_env(CnetMathSolveConfig *c);

/* Returns 0 if a verified answer was produced, 1 if abstain, -1 bad args. */
CNET_API int cnet_math_solve(const char *question,
                             const CnetMathSolveConfig *cfg,
                             CnetMathSolveReport *rep);

CNET_API const char *cnet_math_tier_name(CnetMathTier t);

#ifdef __cplusplus
}
#endif

#endif /* CNET_MATH_SOLVE_H */
