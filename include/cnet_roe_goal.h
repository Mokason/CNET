/* ROE Goal Mapper — microsplit goals by knowledge taxonomy.
 *
 *   goal → microsplits[] (ordered)
 *        → each split maps to category/subcategory capsule slot
 *        → HAVE knowledge → serve local
 *        → MISS → learn (verify) → tidy slot under cat/sub
 *
 * Capsules stay tidy: catalog/cat/sub/skill_id/{SKILL.roe,manifest.roe}
 * Shell law: no self-CERT; learn only via teach gold or user_accept.
 */
#ifndef CNET_ROE_GOAL_H
#define CNET_ROE_GOAL_H

#include "cnet_roe_asi.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ROE_GOAL_MAX_SPLITS 16
#define ROE_GOAL_MAX_CAT 32
#define ROE_GOAL_MAX_SUB 64
#define ROE_GOAL_MAX_MAP 256
#define ROE_GOAL_CAT 48
#define ROE_GOAL_SUB 48
#define ROE_GOAL_TEXT 256

typedef enum {
    ROE_GOAL_HAVE = 1,
    ROE_GOAL_LEARNED = 2,
    ROE_GOAL_MISS = 3,
    ROE_GOAL_BLOCKED = 4
} RoeGoalSplitStatus;

typedef struct {
    char text[ROE_GOAL_TEXT];
    char cat[ROE_GOAL_CAT];
    char sub[ROE_GOAL_SUB];
    char skill_id[ROE_NAME_MAX];
    char answer[ROE_ANSWER_MAX];
    int status; /* RoeGoalSplitStatus */
    int known;  /* 1 if catalog had it before turn */
    uint64_t tokens;
} RoeGoalSplit;

typedef struct {
    char cat[ROE_GOAL_CAT];
    char sub[ROE_GOAL_SUB];
    char pattern[ROE_TEXT_MAX];
    char skill_id[ROE_NAME_MAX];
    int active;
} RoeGoalMapEntry;

typedef struct {
    char name[ROE_GOAL_CAT];
    int active;
    int n_subs;
    char subs[16][ROE_GOAL_SUB];
} RoeGoalCategory;

typedef struct {
    RoeAsi roe;
    RoeGoalCategory cats[ROE_GOAL_MAX_CAT];
    size_t n_cats;
    RoeGoalMapEntry maps[ROE_GOAL_MAX_MAP];
    size_t n_maps;
    char catalog_dir[ROE_PATH_MAX];

    uint64_t n_goals;
    uint64_t n_splits;
    uint64_t n_have;
    uint64_t n_learned;
    uint64_t n_miss;
    uint64_t tokens_used;
    uint64_t tokens_baseline;
} RoeGoalEngine;

typedef struct {
    char goal[ROE_GOAL_TEXT];
    RoeGoalSplit splits[ROE_GOAL_MAX_SPLITS];
    int n_splits;
    int all_resolved; /* every split HAVE or LEARNED */
    double knowledge_rate; /* have/(have+miss+learned) at start */
    double token_save;
    char summary[320];
} RoeGoalPlan;

void roe_goal_init(RoeGoalEngine *G);
void roe_goal_set_catalog(RoeGoalEngine *G, const char *dir);

/* Tidy taxonomy */
int roe_goal_add_category(RoeGoalEngine *G, const char *cat);
int roe_goal_add_sub(RoeGoalEngine *G, const char *cat, const char *sub);
int roe_goal_map_pattern(RoeGoalEngine *G, const char *cat, const char *sub,
                         const char *pattern, const char *skill_id);

/* Seed coding+debug tidy tree + starter knowledge */
int roe_goal_seed_default(RoeGoalEngine *G);

/* Microsplit goal into ordered subgoals with cat/sub assignment */
int roe_goal_microsplit(RoeGoalEngine *G, const char *goal, RoeGoalPlan *plan);

/* Execute plan: serve known, learn unknown (teach table / accept), tidy place */
int roe_goal_run(RoeGoalEngine *G, const char *goal, int auto_learn,
                 RoeGoalPlan *plan);

/* Persist taxonomy + skills in cat/sub folders */
int roe_goal_save(const RoeGoalEngine *G);
int roe_goal_load(RoeGoalEngine *G);

void roe_goal_dump_stats(const RoeGoalEngine *G, char *buf, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
