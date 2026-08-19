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
 *       CERT      OPEN_CHAT / Ember residual draft
 *     logic       creativity (draft only; never CERT)
 *
 * RLM is NOT AGI competence and NOT a second mouth for CERT.
 * Every CERT bind still goes through CORE law:
 *   - claimed_cert only on CERT plane
 *   - open chat / ember drafts never auto-CERT
 *   - default never_voice_llm for residual
 *
 * "Recursive" = bounded re-entry into CORE for multi-part turns
 * (capsule_loop / sequential core_ask / brick hops), not unbounded agent spin.
 *
 * Multi-turn (optional CnetRlmSession): remembers last CERT hop tags/values
 * so chained follow-ups can resolve without residual. Session never evolves.
 *
 * Ember residual draft (policy.allow_ember_draft): after CERT abstain on
 * creative/mixed intent, optional draft via cnet_ember — claimed_cert=0.
 * Default OFF so live waist stays leftover-mouth-killed unless ops enable.
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
#define CNET_RLM_SESSION_HOPS 8

typedef struct {
    CnetHemiPolicy core; /* forwarded to cnet_core_ask */
    int max_steps;       /* 1..CNET_RLM_MAX_STEPS, default 3 */
    int use_capsule_loop; /* 1 = multi-skill turns use capsule_loop */
    /* 1 = after CERT miss, allow ember residual draft (claimed_cert=0).
       Default 0 — product waist kills leftover mouths unless enabled. */
    int allow_ember_draft;
} CnetRlmPolicy;

typedef struct {
    CnetHemiResult step; /* per-step CORE result (or capsule mapped) */
    char note[48];       /* "core" | "capsule" | "brick" | "ember" | "budget" */
} CnetRlmStep;

typedef struct {
    int via_rlm; /* always 1 on ask */
    int n_steps;
    CnetRlmStep steps[CNET_RLM_MAX_STEPS];
    CnetHemiResult final; /* last decisive bind or abstain */
    CnetCoreIntent intent;
    char summary[CNET_RLM_TEXT];
    int ember_draft;     /* 1 if final came from ember residual */
    int session_used;    /* 1 if multi-turn session memory applied */
} CnetRlmResult;

/* Multi-turn CERT memory (process-local). Never residual CERT. */
typedef struct {
    int n;
    char skill[CNET_RLM_SESSION_HOPS][64];
    char value[CNET_RLM_SESSION_HOPS][CNET_RLM_TEXT];
    char last_turn[CNET_RLM_TEXT];
    int open;
} CnetRlmSession;

void cnet_rlm_policy_default(CnetRlmPolicy *p);

void cnet_rlm_session_init(CnetRlmSession *s);
void cnet_rlm_session_clear(CnetRlmSession *s);
/* Remember a CERT hop (skill + value). Drops oldest when full. */
void cnet_rlm_session_remember(CnetRlmSession *s, const char *skill,
                               const char *value);

/* 1 if the turn names a multi-hop chain (" then " / " | "). Live cnetd
   must not prefix-CERT the first brick and drop the rest. */
int cnet_rlm_is_chain_turn(const char *turn);

/* Outer host ask. Returns 0 if final.bound (CERT), 1 abstain/draft, <0 invalid.
 * session may be NULL. On CERT success, session is updated when non-NULL. */
int cnet_rlm_ask(const char *turn, const CnetRlmPolicy *policy,
                 CnetRlmResult *out);
int cnet_rlm_ask_session(const char *turn, const CnetRlmPolicy *policy,
                         CnetRlmSession *session, CnetRlmResult *out);

#ifdef __cplusplus
}
#endif

#endif /* CNET_RLM_H */
