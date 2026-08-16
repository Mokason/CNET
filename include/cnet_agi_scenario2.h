#ifndef CNET_AGI_SCENARIO2_H
#define CNET_AGI_SCENARIO2_H

/* AGI scenario layer 2 — multi-step goals, active gather, chains, persist.
 *
 * Builds on layer 1 (cnet_agi_scenario). Still not a mind / not parrot.
 *
 * Layer 2 adds:
 *   - GOALS: multi-step plans over CERT bricks + given domains
 *   - ACTIVE GATHER: ask for missing table cells when value high but incomplete
 *   - CHAINS: execute tag1|tag2|tag3 pipelines in one turn
 *   - PERSIST: save/load workspace (facts + open goals) across restarts
 *   - TRANSFER: reuse parked bricks on new goal shapes
 *
 * Gate: make cnet_agi_scenario2 → CNET_AGI_SCENARIO2_PASS
 */

#include "cnet_agi_scenario.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CNET_AGI2_MAX_GOALS 16
#define CNET_AGI2_MAX_STEPS 8
#define CNET_AGI2_MAX_GAPS 16
#define CNET_AGI2_STEP 64

typedef enum {
    CNET_AGI2_STEP_SERVE = 0,   /* TAG n */
    CNET_AGI2_STEP_COMPOSE = 1, /* compose A B as C */
    CNET_AGI2_STEP_GATHER = 2,  /* need pair for domain */
    CNET_AGI2_STEP_EVOLVE = 3
} CnetAgi2StepKind;

typedef struct {
    CnetAgi2StepKind kind;
    char a[32];
    char b[32];
    char c[32];
    unsigned nibble;
    int done;
    int ok;
} CnetAgi2Step;

typedef struct {
    char id[32];
    char title[80];
    CnetAgi2Step steps[CNET_AGI2_MAX_STEPS];
    int n_steps;
    int cur;
    int complete;
    int failed;
} CnetAgi2Goal;

typedef struct {
    char domain[32];
    unsigned slot; /* missing input 0..15 */
    double value;
    int filled;
} CnetAgi2Gap;

typedef struct {
    CnetAgiScenario base; /* layer 1 */
    CnetAgi2Goal goals[CNET_AGI2_MAX_GOALS];
    int n_goals;
    CnetAgi2Gap gaps[CNET_AGI2_MAX_GAPS];
    int n_gaps;
    char workspace_path[512];
    /* layer2 counters */
    int goals_started;
    int goals_completed;
    int gathers_asked;
    int gathers_filled;
    int chains_run;
    int chains_ok;
    int persists;
    int restores;
    int transfers;
} CnetAgiScenario2;

typedef struct {
    int turns;
    int cert_answers;
    int abstains;
    int goals_completed;
    int gathers_asked;
    int gathers_filled;
    int chains_ok;
    int persists;
    int restores;
    int transfers;
    int parrot_blocks;
    int bricks_end;
    double cert_rate;
    double goal_rate;
    double gather_fill_rate;
    double chain_rate;
    double honesty;
    double ms_total;
    int pass;
} CnetAgiScenario2Bench;

void cnet_agi2_init(CnetAgiScenario2 *S, const char *bricks_dir,
                    const char *miss_path, const char *bonsai,
                    const char *workspace_path);
void cnet_agi2_free(CnetAgiScenario2 *S);

int cnet_agi2_boot(CnetAgiScenario2 *S, int factory_seed);
int cnet_agi2_persist(const CnetAgiScenario2 *S);
int cnet_agi2_restore(CnetAgiScenario2 *S);

/* Goal API */
int cnet_agi2_goal_add(CnetAgiScenario2 *S, const char *id, const char *title);
int cnet_agi2_goal_add_serve(CnetAgiScenario2 *S, int gi, const char *tag,
                             unsigned nibble);
int cnet_agi2_goal_add_compose(CnetAgiScenario2 *S, int gi, const char *a,
                               const char *b, const char *out);
int cnet_agi2_goal_add_gather(CnetAgiScenario2 *S, int gi, const char *domain,
                              unsigned slot);
int cnet_agi2_goal_run(CnetAgiScenario2 *S, int gi);

/* Active gather: record missing slot; fill via "fill DOMAIN slot=N out=V" */
int cnet_agi2_gather_ask(CnetAgiScenario2 *S, const char *domain, unsigned slot,
                         double value);
int cnet_agi2_gather_fill(CnetAgiScenario2 *S, const char *domain, unsigned slot,
                          unsigned out_v);

/* Chain: "chain TAG1:n1 | TAG2:n2 | ..." uses prior output as next nibble if :auto */
int cnet_agi2_chain(CnetAgiScenario2 *S, const char *chain_expr, char *out_spoken,
                    size_t cap);

/* Unified turn (layer1 + layer2 directives). */
int cnet_agi2_turn(CnetAgiScenario2 *S, const char *turn);

/* Hard multi-phase episode + bench. */
int cnet_agi2_run_episode(CnetAgiScenario2 *S, CnetAgiScenario2Bench *B);

#ifdef __cplusplus
}
#endif

#endif /* CNET_AGI_SCENARIO2_H */
