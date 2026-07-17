#ifndef CNET_CURIOSITY_H
#define CNET_CURIOSITY_H

/* Budgeted idle curiosity for personal AI / gap lane.
 *
 * When demand is quiet, propose a few *lane-teachable* NO_PLAN gaps
 * (w_cur → tk{id}q{id} top-k) for under-covered window tokens so the
 * teacher can seal new certified units without waiting for a user miss.
 *
 * Does NOT lower certification bars. Yields to open demand.
 *
 * Env (see config/personal-ai.env):
 *   CNET_CURIOSITY=1
 *   CNET_CURIOSITY_MAX_PER_HOUR=24
 *   CNET_CURIOSITY_MAX_PER_TICK=2
 *   CNET_CURIOSITY_K=3
 *   CNET_CURIOSITY_YIELD_OPEN=4   # skip if open gaps >= this
 *   CNET_WINDOW_FILE / CNET_RESIDUAL_WINDOW
 *
 * Gate: make curiosity → CURIOSITY_PASS
 */

#include <stddef.h>
#include <stdint.h>

#include "cnet_export.h"
#include "router.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int enabled;              /* 0 = no-op */
    size_t max_per_hour;      /* hard hourly budget */
    size_t max_per_tick;      /* per call */
    int k;                    /* goal field_count (top-k), default 3 */
    size_t yield_if_open;     /* skip if open_gaps >= this (0 = never yield) */
    char window_path[512];
    char state_path[512];     /* hourly counter file */
    char inbox_path[512];
} CnetCuriosityConfig;

typedef struct {
    size_t proposed;          /* NO_PLAN lines written */
    size_t skipped_budget;    /* hit hourly cap */
    size_t skipped_busy;      /* yield to open demand */
    size_t skipped_covered;   /* candidates already in registry */
    size_t candidates;        /* novel tokens considered */
    int enabled;
} CnetCuriosityReport;

CNET_API void cnet_curiosity_config_defaults(CnetCuriosityConfig *c);
CNET_API void cnet_curiosity_config_from_env(CnetCuriosityConfig *c);

/* One curiosity impulse. reg may be NULL (treat all tokens as novel).
 * open_gaps: current OPEN ledger count (for yield).
 * Returns 0 always (fail-soft); fills *rep when non-NULL. */
CNET_API int cnet_curiosity_tick(const CnetCuriosityConfig *cfg,
                                 const PrimitiveRegistry *reg,
                                 size_t open_gaps,
                                 CnetCuriosityReport *rep);

/* 1 if registry already has a unit covering this window token tag. */
CNET_API int cnet_curiosity_token_covered(const PrimitiveRegistry *reg,
                                          int token_id);

#ifdef __cplusplus
}
#endif

#endif /* CNET_CURIOSITY_H */
