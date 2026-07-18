#ifndef CNET_LEARN_LOOP_H
#define CNET_LEARN_LOOP_H

/* Hermes-style experience learning loop in pure C.
 *
 * Mirrors Hermes agent learning without Python:
 *   1) declarative memory first (MCP facts + agent KB)
 *   2) on miss: tools (wiki → web_search)
 *   3) on success: memorize fact + write procedural skill (SKILL.md)
 *   4) optional: seed teachable gap-lane demand (NO_PLAN inbox)
 *
 * Does NOT lower CNB certification bars. Skills/memory are host-side
 * procedural/declarative knowledge; gap seeds may later become certified
 * units via gap_lane_run.
 *
 * Gate: make learn_loop → LEARN_LOOP_PASS
 */

#include <stddef.h>

#include "cnet_export.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CNET_LEARN_SRC_NONE = 0,
    CNET_LEARN_SRC_MEMORY = 1,   /* MCP fact cache or agent KB */
    CNET_LEARN_SRC_WIKI = 2,
    CNET_LEARN_SRC_WEB = 3,
    CNET_LEARN_SRC_SKILL = 4     /* reused procedural skill file */
} CnetLearnSource;

typedef struct {
    int use_tools;           /* 1 = wiki/web on miss (default 1) */
    int write_skill;         /* 1 = write SKILL.md on success (default 1) */
    int memorize;            /* 1 = mcp_memorize_fact (default 1) */
    int seed_gap;            /* 1 = append teachable NO_PLAN if inbox set */
    int prefer_wiki;         /* 1 = wiki before web (default 1) */
    int force_tools;         /* 1 = skip memory/skill reuse; always tool (default 0) */
    char skills_dir[512];    /* default: logs/personal_ai_skills or CNET_SKILLS_DIR */
    char inbox_path[512];    /* optional gap inbox */
    char window_path[512];   /* for NO_PLAN width; default english_window_256 */
    int gap_k;               /* top-k for seeded goals (default 3) */
} CnetLearnConfig;

typedef struct {
    CnetLearnSource source;
    int from_cache;          /* tool served from memory cache */
    int memorized;           /* wrote declarative fact */
    int skill_written;       /* created/updated SKILL.md */
    int skill_reused;        /* answered from existing skill */
    int gap_seeded;          /* wrote NO_PLAN */
    char answer[1024];
    char skill_path[512];
    char skill_name[128];
    char detail[256];
} CnetLearnReport;

CNET_API void cnet_learn_config_defaults(CnetLearnConfig *c);
CNET_API void cnet_learn_config_from_env(CnetLearnConfig *c);

/* One Hermes-like learn cycle for a natural-language query/task. */
CNET_API int cnet_learn_cycle(const char *query,
                              const CnetLearnConfig *cfg,
                              CnetLearnReport *rep);

/* List / load skills (best-effort). Returns count written into names[] (max max_n). */
CNET_API int cnet_learn_list_skills(const char *skills_dir,
                                    char names[][128], int max_n);

CNET_API const char *cnet_learn_source_name(CnetLearnSource s);

#ifdef __cplusplus
}
#endif

#endif /* CNET_LEARN_LOOP_H */
