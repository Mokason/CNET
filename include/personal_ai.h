#ifndef CNET_PERSONAL_AI_H
#define CNET_PERSONAL_AI_H

/* Personal AI: hybrid A/B/C serve policy.
 *
 *   A  certified library plan     (highest trust)
 *   B  soft / medium specialists  (provisional, margin-gated)
 *   C  residual generator         (uncertified; local dense stand-in or teacher)
 *
 * Optional: structure-mine residual successes into A; distill multi-step plans.
 *
 * Gate: make personal_ai → PERSONAL_AI_PASS
 *       make hybrid_ai   → HYBRID_AI_PASS
 */

#include <stddef.h>
#include <stdint.h>

#include "cnet_export.h"
#include "gap_lane.h"
#include "hybrid_ai.h"
#include "resource_governor.h"
#include "router.h"
#include "nn.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PERSONAL_AI_LOCAL = 0,     /* Tier A certified */
    PERSONAL_AI_TEACHER = 1,   /* legacy: bound teacher oracle (also Tier C) */
    PERSONAL_AI_ABSTAIN = 2,
    PERSONAL_AI_ERROR = 3,
    PERSONAL_AI_SOFT = 4,      /* Tier B */
    PERSONAL_AI_RESIDUAL = 5   /* Tier C residual */
} PersonalAiSource;

typedef struct {
    PersonalAiSource source;
    HybridTrust trust;
    HybridTier tier;
    int taught;
    int gap_noted;
    int structure_mined;
    int distilled;
    size_t local_hits;
    size_t teacher_helps;
    size_t soft_hits;
    size_t residual_hits;
    size_t abstains;
    size_t teaches;
    size_t residual_captures; /* Tier-C pairs offered to the fault bus */
} PersonalAiReport;

typedef struct {
    int teach_inline;
    int allow_teacher;
    size_t max_inline_teaches;
    /* Hybrid extensions */
    int allow_soft;              /* default 1 */
    int allow_residual;          /* default 1 */
    int allow_medium;            /* default 1 */
    int structure_mine_on_serve; /* default 0 — mine in tick */
    size_t structure_min_hits;   /* default 3 */
} PersonalAiPolicy;

struct ResidualGguf; /* opaque; see residual_gguf.h */
struct ResidualHttp; /* opaque; see residual_http.h */

typedef struct {
    GapLane lane;
    CnetResourceGovernor gov;
    HybridAi hybrid;
    PersonalAiPolicy policy;
    PersonalAiReport totals;
    size_t inline_teaches_done;
    int loaded;
    /* Owned when CNET_RESIDUAL_GGUF auto-bound at open; freed in close. */
    struct ResidualGguf *owned_residual;
    /* Owned when CNET_RESIDUAL_HTTP auto-bound at open; freed in close. */
    struct ResidualHttp *owned_residual_http;
} PersonalAi;

CNET_API void personal_ai_policy_defaults(PersonalAiPolicy *p);
CNET_API int personal_ai_apply_env(void);

CNET_API int personal_ai_open(PersonalAi *ai,
                              const char *base_path,
                              const char *ledger_path,
                              const char *inbox_path,
                              const PersonalAiPolicy *policy);

CNET_API int personal_ai_bind_teacher(PersonalAi *ai,
                                      const char *name,
                                      Port input_port,
                                      Port goal_port,
                                      CnetOracleFn fn,
                                      void *ctx);

/* Bind Tier C residual (open-ended). Separate from signature teachers. */
CNET_API int personal_ai_bind_residual(PersonalAi *ai, const char *name,
                                       CnetOracleFn fn, void *ctx);

/* Real GGUF residual (Tier C) — see residual_gguf.h:
 * personal_ai_bind_residual_gguf / personal_ai_auto_residual_gguf */

CNET_API int personal_ai_bind_soft(PersonalAi *ai, const char *name,
                                   Port in, Port out, CnetOracleFn fn,
                                   void *ctx, double min_margin);

CNET_API int personal_ai_bind_medium(PersonalAi *ai, const char *name,
                                     Port in, Port out, CnetOracleFn fn,
                                     void *ctx, uint64_t resident_bytes);

CNET_API int personal_ai_enable_adapter(PersonalAi *ai, const double *bias,
                                        size_t dim, double scale);

CNET_API int personal_ai_serve(PersonalAi *ai,
                               Port input_port,
                               Port goal_port,
                               const double *input,
                               size_t in_len,
                               double *output,
                               size_t out_cap,
                               PersonalAiReport *rep);

/* Maintenance: gap_lane_tick + optional structure mining from residual traces. */
CNET_API int personal_ai_tick(PersonalAi *ai, GapLaneTickReport *tick_rep);

/* Explicit structure mine / distill hooks. */
CNET_API int personal_ai_structure_mine(PersonalAi *ai,
                                        BinaryTransformNetwork **student_out);
CNET_API int personal_ai_distill_plan(PersonalAi *ai, const RoutePlan *plan,
                                      BinaryTransformNetwork **chunk_out);

CNET_API void personal_ai_close(PersonalAi *ai);
CNET_API void personal_ai_totals(const PersonalAi *ai, PersonalAiReport *out);

/* Own-learning KPI as one JSON line. The scoreboard for "is CNET actually
 * displacing the residual": substitution_rate up, residual_rate down, read
 * jointly with abstain_rate so a drop in residual_rate cannot be bought by
 * abstaining more. Returns bytes written, or negative on error/truncation. */
CNET_API int personal_ai_kpi_json(const PersonalAi *ai, char *out,
                                  size_t out_capacity);

/* Write personal_ai_kpi_json to path (truncating). Returns 0 or negative. */
CNET_API int personal_ai_kpi_write(const PersonalAi *ai, const char *path);
CNET_API const char *personal_ai_source_name(PersonalAiSource s);

/* Access hybrid counters. */
CNET_API const HybridAi *personal_ai_hybrid(const PersonalAi *ai);

#ifdef __cplusplus
}
#endif

#endif /* CNET_PERSONAL_AI_H */
