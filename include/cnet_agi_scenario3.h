#ifndef CNET_AGI_SCENARIO3_H
#define CNET_AGI_SCENARIO3_H

/* AGI scenario layer 3
 *
 *  A) Open-ended goal language → CERT-only plan synthesis
 *     "goal: prove user_pref at 3 then chain add and xor"
 *     → only SERVE / COMPOSE / GATHER / EVOLVE steps (no chat steps)
 *
 *  B) Multi-agent brick specialists
 *     Each parked domain tag is a specialist agent; router picks by tag/prefix
 *     and can consult a small committee (primary + verify secondary).
 *
 * Still not a mind. Still prove-or-abstain. Still no residual auto-CERT.
 *
 * Gate: make cnet_agi_scenario3 → CNET_AGI_SCENARIO3_PASS
 */

#include "cnet_agi_scenario2.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CNET_AGI3_MAX_SPECIALISTS 16
#define CNET_AGI3_MAX_PLANS 16
#define CNET_AGI3_PLAN_TEXT 256

typedef struct {
    char tag[32];
    char name[64];
    int live;
    int consults;
    int proves;
    int fails;
    double reliability; /* proves/(proves+fails) */
} CnetAgi3Specialist;

typedef struct {
    char raw[CNET_AGI3_PLAN_TEXT];
    int goal_index; /* into layer2 goals[] after synthesis */
    int synthesized;
    int ran;
    int ok;
    int rejected_non_cert; /* refused chatty plan fragments */
} CnetAgi3Plan;

typedef struct {
    CnetAgiScenario2 L2;
    CnetAgi3Specialist specialists[CNET_AGI3_MAX_SPECIALISTS];
    int n_specialists;
    CnetAgi3Plan plans[CNET_AGI3_MAX_PLANS];
    int n_plans;
    /* counters */
    int synth_ok;
    int synth_reject;
    int committee_ok;
    int committee_fail;
    int specialist_routes;
} CnetAgiScenario3;

typedef struct {
    int turns;
    int cert_answers;
    int abstains;
    int synth_ok;
    int synth_reject;
    int goals_completed;
    int specialist_routes;
    int committee_ok;
    int chains_ok;
    int parrot_blocks;
    int bricks_end;
    double cert_rate;
    /* executed-ok / synthesized. Refusals are NOT in the denominator: refusing
       a chat/roleplay plan is the parrot block succeeding, and counting it as
       imprecision made the score fall as refusal got better. Refusals are
       counted separately by synth_reject, still required by the pass rule. */
    double synth_precision;
    double committee_rate;
    double honesty;
    double ms_total;
    int pass;
} CnetAgiScenario3Bench;

void cnet_agi3_init(CnetAgiScenario3 *S, const char *bricks_dir,
                    const char *miss_path, const char *bonsai,
                    const char *workspace_path);
void cnet_agi3_free(CnetAgiScenario3 *S);

int cnet_agi3_boot(CnetAgiScenario3 *S, int factory_seed);

/* Refresh specialist roster from serve bank / bus bricks. */
int cnet_agi3_specialists_sync(CnetAgiScenario3 *S);

/* Route a serve turn to a specialist by tag; optional committee verify. */
int cnet_agi3_specialist_serve(CnetAgiScenario3 *S, const char *tag,
                               unsigned nibble, int committee,
                               CnetServeResult *out);

/* Synthesize CERT-only plan from open language. Returns goal index or -1.
 * Rejects plans that would require chat/roleplay/open mouth. */
int cnet_agi3_synthesize(CnetAgiScenario3 *S, const char *goal_text);

/* Run synthesized plan by id/index. */
int cnet_agi3_plan_run(CnetAgiScenario3 *S, int plan_index);

/* Unified turn (+ layer2 + layer1). */
int cnet_agi3_turn(CnetAgiScenario3 *S, const char *turn);

/* Full episode bench. */
int cnet_agi3_run_episode(CnetAgiScenario3 *S, CnetAgiScenario3Bench *B);

#ifdef __cplusplus
}
#endif

#endif /* CNET_AGI_SCENARIO3_H */
