#ifndef CNET_AGI_SCENARIO_H
#define CNET_AGI_SCENARIO_H

/* AGI-type CORE scenario (not a mind, not a parrot).
 *
 * Uses everything we have:
 *   live waist · brick factory · miss→admit · split/compose · light serve · evolve
 *
 * Loop per turn (given info only):
 *   1) ingest given facts into workspace
 *   2) try CERT serve / compose chain
 *   3) else value the miss + log
 *   4) if value high + table complete → admit brick (teacher leaves)
 *   5) never OPEN_CHAT answer
 *
 * Gate: make cnet_agi_scenario → CNET_AGI_SCENARIO_PASS
 */

#include "cnet_core_bus.h"
#include "cnet_core_paths.h"
#include "cnet_core_serve.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CNET_AGI_MAX_FACTS 32
#define CNET_AGI_MAX_TURNS 64
#define CNET_AGI_FACT 128
#define CNET_AGI_TURN 256

typedef struct {
    char key[32];
    float lut[16];
    int has_lut; /* full 16-domain table known from given info */
    int hits;
} CnetAgiFact;

typedef struct {
    char text[CNET_AGI_TURN];
    int proved;
    int abstained;
    int evolved;
    int composed;
    int parrot_blocked;
    double value; /* miss value 0..1 if abstained */
    char detail[80];
} CnetAgiTurnLog;

typedef struct {
    CnetCoreBus bus;
    CnetServeBank serve;
    CnetAgiFact facts[CNET_AGI_MAX_FACTS];
    int n_facts;
    CnetAgiTurnLog turns[CNET_AGI_MAX_TURNS];
    int n_turns;
    char bricks_dir[512];
    char miss_path[512];
    char bonsai[512];
    /* totals */
    int cert_answers;
    int abstains;
    int evolves;
    int composes;
    int parrot_blocks;
    int given_ingests;
    double value_sum;
    double ms_total;
} CnetAgiScenario;

typedef struct {
    int turns;
    int cert_answers;
    int abstains;
    int evolves;
    int composes;
    int parrot_blocks;
    int given_ingests;
    int bricks_end;
    double cert_rate; /* cert / (cert+abstain) on answerable attempts */
    double honesty;   /* 1 - parrot_blocks/turns */
    double ms_total;
    int pass;
} CnetAgiScenarioBench;

void cnet_agi_scenario_init(CnetAgiScenario *S, const char *bricks_dir,
                            const char *miss_path, const char *bonsai);
void cnet_agi_scenario_free(CnetAgiScenario *S);

/* Boot: load .luts; optional factory seed if empty. */
int cnet_agi_scenario_boot(CnetAgiScenario *S, int factory_seed);

/* Ingest given info. Formats:
 *   "given domain TAG lut=a,b,c,...(16)"
 *   "given domain TAG pairs=0:1,1:2,..." (builds lut)
 */
int cnet_agi_scenario_ingest(CnetAgiScenario *S, const char *line);

/* Value a miss for evolve priority [0,1]. Higher = more worth bricking. */
double cnet_agi_scenario_value_miss(const CnetAgiScenario *S, const char *turn);

/* One scenario turn. Returns 0 proved, 1 abstained, <0 error. */
int cnet_agi_scenario_turn(CnetAgiScenario *S, const char *turn);

/* Run built-in multi-step AGI-type episode + fill bench. */
int cnet_agi_scenario_run_episode(CnetAgiScenario *S, CnetAgiScenarioBench *B);

/* Production tick: value miss_log → admit if ready → reload serve. */
int cnet_agi_scenario_evolve_tick(CnetAgiScenario *S);

#ifdef __cplusplus
}
#endif

#endif /* CNET_AGI_SCENARIO_H */
