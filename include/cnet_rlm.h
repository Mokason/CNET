#ifndef CNET_RLM_H
#define CNET_RLM_H

/* RLM — Recursive Loop Module (outer host)
 *
 * Wraps CORE middle ground + both planes:
 *
 *   user ──► RLM (bounded recursive host)
 *              │
 *              ▼
 *            CORE  (discern · policy · mouth gate)
 *           /    \
 *       CERT      OPEN_CHAT
 *     logic       creativity
 *
 * RLM is NOT AGI competence and NOT a second mouth.
 * Every bind still goes through CORE law:
 *   - claimed_cert only on CERT plane
 *   - open chat never auto-CERTs
 *   - default never_voice_llm for open chat
 *
 * "Recursive" = bounded re-entry into CORE for multi-part turns
 * (capsule_loop / sequential core_ask), not unbounded agent spin.
 *
 * Gate: make cnet_rlm → CNET_RLM_PASS
 */

#include "cnet_capsule_loop.h"
#include "cnet_hemisphere.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CNET_RLM_MAX_STEPS 4
#define CNET_RLM_TEXT CNET_SKILL_LANE_TEXT

typedef struct {
    CnetHemiPolicy core; /* forwarded to cnet_core_ask */
    int max_steps;       /* 1..CNET_RLM_MAX_STEPS, default 3 */
    int use_capsule_loop; /* 1 = multi-skill turns use capsule_loop */
} CnetRlmPolicy;

typedef struct {
    CnetHemiResult step; /* per-step CORE result (or capsule mapped) */
    char note[48];       /* "core" | "capsule" | "done" */
} CnetRlmStep;

typedef struct {
    int via_rlm; /* always 1 on ask */
    int n_steps;
    CnetRlmStep steps[CNET_RLM_MAX_STEPS];
    CnetHemiResult final; /* last decisive bind or abstain */
    CnetCoreIntent intent;
    char summary[CNET_RLM_TEXT];
} CnetRlmResult;

void cnet_rlm_policy_default(CnetRlmPolicy *p);

/* Outer host ask. Returns 0 if final.bound, 1 abstain, <0 invalid. */
int cnet_rlm_ask(const char *turn, const CnetRlmPolicy *policy,
                 CnetRlmResult *out);

#ifdef __cplusplus
}
#endif

#endif /* CNET_RLM_H */
