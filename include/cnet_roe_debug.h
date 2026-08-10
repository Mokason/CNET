/* ROE-ASI Debug L1–L3: recipes, checklists, project memory.
 * L3 = project-scoped (project_id + error signature) → certified fix skill.
 * Verify required before project promote. Never self-CERT from one guess.
 */
#ifndef CNET_ROE_DEBUG_H
#define CNET_ROE_DEBUG_H

#include "cnet_roe_asi.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ROE_DBG_PROJ_MAX 32
#define ROE_DBG_MEM_MAX 256
#define ROE_DBG_SIG_MAX 160
#define ROE_DBG_PROJ_NAME 64

typedef enum {
    ROE_DBG_L1_RECIPE = 1,
    ROE_DBG_L2_CHECKLIST = 2,
    ROE_DBG_L3_PROJECT = 3,
    ROE_DBG_MISS = 4,
    ROE_DBG_ABSTAIN = 5
} RoeDbgLevel;

typedef struct {
    char project_id[ROE_DBG_PROJ_NAME];
    char sig[ROE_DBG_SIG_MAX];
    char skill_id[ROE_NAME_MAX];
    char fix[ROE_ANSWER_MAX];
    char note[ROE_TEXT_MAX];
    int certified;
    int active;
    uint64_t hits;
    uint64_t verifies;
    uint64_t tick_learned;
} RoeDbgMemory;

typedef struct {
    char id[ROE_DBG_PROJ_NAME];
    int active;
    uint64_t n_debugs;
    uint64_t n_l3_hits;
} RoeDbgProject;

typedef struct {
    RoeAsi roe; /* shared shell + global recipes as skills */
    RoeDbgProject projects[ROE_DBG_PROJ_MAX];
    size_t n_projects;
    RoeDbgMemory mem[ROE_DBG_MEM_MAX];
    size_t n_mem;
    char catalog_dir[ROE_PATH_MAX];

    uint64_t n_turns;
    uint64_t n_l1;
    uint64_t n_l2;
    uint64_t n_l3;
    uint64_t n_miss;
    uint64_t n_abstain;
    uint64_t n_verify_ok;
    uint64_t n_verify_fail;
    uint64_t n_promote_l3;
    uint64_t tokens_used;
    uint64_t tokens_baseline;
} RoeDebug;

typedef struct {
    int level; /* RoeDbgLevel */
    char sig[ROE_DBG_SIG_MAX];
    char answer[ROE_ANSWER_MAX];
    char skill_id[ROE_NAME_MAX];
    int verified_local; /* 1 if L3/L1 certified local */
    uint64_t tokens_est;
    int source; /* RoeSource passthrough when using roe_turn */
} RoeDbgReply;

void roe_dbg_init(RoeDebug *D);
void roe_dbg_set_catalog(RoeDebug *D, const char *dir);

/* Normalize traceback/error text → stable signature */
void roe_dbg_signature(const char *traceback_or_msg, char *sig, size_t cap);

int roe_dbg_add_project(RoeDebug *D, const char *project_id);

/* L1 global error recipes + L2 checklist into underlying ROE */
int roe_dbg_seed_curriculum(RoeDebug *D);

/* Core: debug within a project. Prefer L3 project memory, else L1/L2 ROE, else teach/miss. */
int roe_dbg_turn(RoeDebug *D, const char *project_id, const char *query_or_tb,
                 RoeDbgReply *out);

/* Shell verify: tests_passed or user_accept → promote L3 project memory.
 * fix_override NULL → use last answer text. Returns 1 if L3 promoted. */
int roe_dbg_verify(RoeDebug *D, const char *project_id, const char *query_or_tb,
                   const char *fix_override, int tests_passed, int user_accept);

/* Explicit teach for a project bug (simulates verified session). */
int roe_dbg_learn(RoeDebug *D, const char *project_id, const char *query_or_tb,
                  const char *fix, const char *note);

int roe_dbg_save(const RoeDebug *D);
int roe_dbg_load(RoeDebug *D);

void roe_dbg_dump_stats(const RoeDebug *D, char *buf, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
