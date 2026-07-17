#ifndef CNET_EG_H
#define CNET_EG_H

/* Local Efficiency Gain (CNET_EG) — hill-climb metric for personal AI.
 *
 * Inspired by MAI "hill-climbing machine" EG: convert work into a single
 * comparable cost for useful sealed skill.
 *
 * Definitions (local personal-AI loop):
 *   teacher_work  = drain examined (oracle/teach attempts) over the window
 *   seals         = drain closed (gaps sealed into CNB) over the window
 *   cost_per_seal = teacher_work / max(seals, 1)     // lower is better
 *   seal_rate     = seals / max(hours, epsilon)       // higher is better
 *   local_eg      = baseline_cost_per_seal / cost_per_seal
 *                   ( >1 means more efficient than baseline )
 *
 * Baseline defaults to first snapshot or CNET_EG_BASELINE_COST (default 32
 * examined per seal — roughly one min_evidence window pass).
 *
 * Gate: make eg → EG_PASS
 * Report: scripts/personal_ai_hill_climb_report.sh
 */

#include <stddef.h>
#include <stdint.h>

#include "cnet_export.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t teacher_work;   /* examined */
    uint64_t seals;          /* closed */
    uint64_t deferred;
    uint64_t no_oracle;
    uint64_t curiosity_proposed;
    uint64_t units;          /* registry unit count at end of window */
    double hours;            /* wall window hours */
} CnetEgSample;

typedef struct {
    double cost_per_seal;    /* teacher_work / max(seals,1) */
    double seal_rate;        /* seals / max(hours, eps) */
    double local_eg;         /* baseline / cost_per_seal (1 = baseline) */
    double baseline_cost;    /* comparator */
    int ok;                  /* 1 if sample usable */
} CnetEgResult;

/* Pure formula; no I/O. baseline_cost <= 0 → use 32.0 */
CNET_API void cnet_eg_compute(const CnetEgSample *s, double baseline_cost,
                              CnetEgResult *out);

/* Append one JSONL event line to path (creates file). Returns 0. */
CNET_API int cnet_eg_log_tick(const char *path, uint64_t tick_no,
                              uint64_t examined, uint64_t closed,
                              uint64_t deferred, uint64_t no_oracle,
                              uint64_t units, uint64_t curiosity);

/* Aggregate events from JSONL between unix times [t0,t1] inclusive.
 * t1=0 → now. Returns 0, fills *out. */
CNET_API int cnet_eg_aggregate_file(const char *path, int64_t t0, int64_t t1,
                                    CnetEgSample *out);

/* Write compact JSON result into buf. */
CNET_API int cnet_eg_result_json(const CnetEgResult *r, const CnetEgSample *s,
                                 char *buf, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* CNET_EG_H */
