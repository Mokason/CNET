#ifndef CNET_ROUTE_LOG_H
#define CNET_ROUTE_LOG_H

/* Append-only route decision log for the real AICIMO routing surface.
 *
 * Records one JSONL line per decision with:
 *   mechanism, selected expert, entropy, outcome, latency, cost
 *
 * Env: CNET_ROUTE_LOG=/path/to/file.jsonl  (empty/unset = logging off)
 * Gate: make route_log → ROUTE_LOG_PASS
 *
 * Cost units (documented, not currency):
 *   decision_cost = 1.0 always (one route decision)
 *   token_cost     = prompt_tokens + generated_tokens
 *   cost           = decision_cost + token_cost
 */

#include <stddef.h>
#include <stdint.h>

#include "cnet_export.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Mechanism atoms — stable strings for telemetry. */
#define CNET_ROUTE_MECH_AGENT_ROLE   "aicimo_agent_role"
#define CNET_ROUTE_MECH_ROLE_HASH    "aicimo_role_hash"
/* Core CNET (SoulHost planner / residual / miss). */
#define CNET_ROUTE_MECH_PLANNER      "planner"
#define CNET_ROUTE_MECH_RESIDUAL     "residual"
#define CNET_ROUTE_MECH_NO_PLAN      "no_plan"
#define CNET_ROUTE_MECH_PROBE        "probe"

typedef struct {
    /* Required for a useful line; any may be NULL / zero when unknown. */
    const char *mechanism;         /* CNET_ROUTE_MECH_*                    */
    const char *role;              /* caller role string                   */
    const char *role_canonical;    /* after agent-role resolve, or role    */
    uint32_t selected_expert;      /* AICIMO adapter index, or plan length */
    const char *selected_unit;     /* core CNET unit name (optional)       */
    uint32_t plan_length;          /* planner steps (0 if N/A)             */
    const char *expert_profile;    /* sampling profile or serve source     */
    float entropy;                 /* route uncertainty in [0, 1]          */
    const char *outcome;           /* "ok" or error / no_plan atom         */
    int outcome_code;              /* harness status or soul return code   */
    double route_latency_ms;       /* plan / AICIMO decision only          */
    double total_latency_ms;       /* decision + execute (if any)          */
    double cost;                   /* decision + tokens or plan adapter_cost */
    uint32_t prompt_tokens;
    uint32_t generated_tokens;
    int aicimo_override;           /* 1 if sampling override in effect     */
    const char *model_id;          /* optional session / base id           */
    const char *event;             /* generate | probe_route | soul_route  */
} CnetRouteLogEvent;

/* Format one JSON object (no trailing newline) into buf. Returns 0, or -1. */
CNET_API int cnet_route_log_format_json(const CnetRouteLogEvent *ev,
                                        char *buf, size_t cap);

/* Append one JSONL line to path (create/append). Returns 0, or -1. */
CNET_API int cnet_route_log_append(const char *path, const CnetRouteLogEvent *ev);

/* Read CNET_ROUTE_LOG from the environment. NULL if unset/empty. */
CNET_API const char *cnet_route_log_path_from_env(void);

/* Compute cost = 1.0 + prompt_tokens + generated_tokens. */
CNET_API double cnet_route_log_cost(uint32_t prompt_tokens,
                                    uint32_t generated_tokens);

/* Stable lowercase profile atom for harness sampling modes (1..4), or "auto"/"unknown". */
CNET_API const char *cnet_route_log_profile_name(int sampling_mode);

/* Stable outcome atom from a harness status code. */
CNET_API const char *cnet_route_log_outcome_name(int status);

#ifdef __cplusplus
}
#endif

#endif /* CNET_ROUTE_LOG_H */
