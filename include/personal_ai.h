#ifndef CNET_PERSONAL_AI_H
#define CNET_PERSONAL_AI_H

/* Personal AI serving policy: local certified library first; big-AI teacher
 * only on gaps; optional budgeted teach so help becomes a local skill.
 *
 * This is the product-facing composition of gap_lane + resource_governor +
 * deploy free-wins — not a second planner. Certification remains the only
 * admission door; teachers never outrank sealed units.
 *
 * Gate: make personal_ai → PERSONAL_AI_PASS.
 */

#include <stddef.h>
#include <stdint.h>

#include "cnet_export.h"
#include "gap_lane.h"
#include "resource_governor.h"
#include "router.h"
#include "nn.h"

#ifdef __cplusplus
extern "C" {
#endif

/* How the last serve was answered. */
typedef enum {
    PERSONAL_AI_LOCAL = 0,     /* certified plan served locally */
    PERSONAL_AI_TEACHER = 1,   /* no plan; teacher answered (big AI on call) */
    PERSONAL_AI_ABSTAIN = 2,   /* no plan and no teacher / teacher refused */
    PERSONAL_AI_ERROR = 3
} PersonalAiSource;

typedef struct {
    PersonalAiSource source;
    int taught;                /* 1 if this call also closed a teach */
    int gap_noted;             /* 1 if a NO_PLAN was recorded */
    size_t local_hits;
    size_t teacher_helps;
    size_t abstains;
    size_t teaches;
} PersonalAiReport;

typedef struct {
    /* 1 = after a teacher-answered miss, attempt acquire_now immediately
       (synchronous teach). 0 = only note the gap; gap_lane_tick teaches later
       (async, preferred for interactive latency). Default 0. */
    int teach_inline;
    /* 1 = bind teacher oracles for serve-time help. 0 = local-only mode
       (still notes gaps). Default 1. */
    int allow_teacher;
    /* Max inline teaches per process lifetime (0 = unlimited). */
    size_t max_inline_teaches;
} PersonalAiPolicy;

typedef struct {
    GapLane lane;
    CnetResourceGovernor gov;
    PersonalAiPolicy policy;
    PersonalAiReport totals;
    size_t inline_teaches_done;
    int loaded;
} PersonalAi;

CNET_API void personal_ai_policy_defaults(PersonalAiPolicy *p);

/* Apply personal-ai + deploy free-win env when unset (CNET_PERSONAL_*). */
CNET_API int personal_ai_apply_env(void);

/* Open on an existing or fresh base/ledger. inbox may be NULL.
   Applies deploy free-wins (unset only) and opens a resource governor with
   deploy defaults. Returns 0, or <0. */
CNET_API int personal_ai_open(PersonalAi *ai,
                              const char *base_path,
                              const char *ledger_path,
                              const char *inbox_path,
                              const PersonalAiPolicy *policy /* NULL = defaults */);

/* Register a teacher oracle (big AI on call) for a port signature.
   identity/fn via existing acquire_oracle_register on the lane. */
CNET_API int personal_ai_bind_teacher(PersonalAi *ai,
                                      const char *name,
                                      Port input_port,
                                      Port goal_port,
                                      CnetOracleFn fn,
                                      void *ctx);

/* Serve one request under the local-first policy.
   Fills *rep (may be NULL). Returns 0 if an answer was written (local or
   teacher), -1 if abstained/error (no output guaranteed). */
CNET_API int personal_ai_serve(PersonalAi *ai,
                               Port input_port,
                               Port goal_port,
                               const double *input,
                               size_t in_len,
                               double *output,
                               size_t out_cap,
                               PersonalAiReport *rep);

/* One maintenance tick: gap_lane_tick (scan/drain/checkpoint under budgets). */
CNET_API int personal_ai_tick(PersonalAi *ai, GapLaneTickReport *tick_rep);

CNET_API void personal_ai_close(PersonalAi *ai);

/* Copy cumulative counters into *out. */
CNET_API void personal_ai_totals(const PersonalAi *ai, PersonalAiReport *out);

CNET_API const char *personal_ai_source_name(PersonalAiSource s);

#ifdef __cplusplus
}
#endif

#endif /* CNET_PERSONAL_AI_H */
