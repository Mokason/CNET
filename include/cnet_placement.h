#ifndef CNET_PLACEMENT_H
#define CNET_PLACEMENT_H

/* Placement plan/doctor (Colibrì-inspired): MemAvailable + residual/teacher/CNB
 * layout without loading model tensors. Fail-closed dual-load advice.
 *
 * Gate: make colibri_integrate → COLIBRI_INTEGRATE_PASS
 * CLI:  tools/cnet_plan.c → bin/cnet_plan plan|doctor
 */

#include <stddef.h>
#include <stdint.h>

#include "cnet_export.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t mem_available_bytes;
    uint64_t mem_total_bytes;
    uint64_t cnb_bytes;
    uint64_t residual_gguf_bytes; /* 0 if unset/missing */
    uint64_t teacher_gguf_bytes;
    uint64_t window_ids;
    int residual_path_ok;
    int teacher_path_ok;
    int cnb_path_ok;
    int window_path_ok;
    /* Projected peaks (honest envelopes, not measured RSS). */
    uint64_t peak_residual_alone;
    uint64_t peak_teacher_alone;
    uint64_t peak_dual; /* residual + teacher concurrent */
    int dual_safe;      /* 1 if dual fits MemAvailable * safety margin */
    int residual_alone_safe;
    int teacher_alone_safe;
    int prefer_warm_only; /* 1 if dual unsafe → serve residual only on demand */
    char residual_path[512];
    char teacher_path[512];
    char cnb_path[512];
    char window_path[512];
    char advice[256];
} CnetPlacementPlan;

/* Read /proc/meminfo (Linux). Returns 0 and fills avail and total; -1 on fail. */
CNET_API int cnet_mem_available(uint64_t *avail_out, uint64_t *total_out);

/* File size or 0 if missing. */
CNET_API uint64_t cnet_file_size(const char *path);

/* Build plan from env (CNET_RESIDUAL_GGUF, CNET_PERSONAL_TEACHER,
 * CNET_BASE_PATH, CNET_RESIDUAL_WINDOW) with optional path overrides (NULL ok).
 * safety_pct: use this fraction of MemAvailable (default 70 if <=0 or >100). */
CNET_API int cnet_placement_plan(CnetPlacementPlan *out,
                                 const char *cnb_path,
                                 const char *residual_path,
                                 const char *teacher_path,
                                 const char *window_path,
                                 int safety_pct);

/* Doctor: 0 = runnable (warnings ok), 1 = unsafe dual or missing base,
 * 2 = invalid args. Writes human lines into report (NUL-term). */
CNET_API int cnet_placement_doctor(const CnetPlacementPlan *plan,
                                   char *report, size_t report_cap);

/* JSON one-liner into out (truncated). Returns 0. */
CNET_API int cnet_placement_json(const CnetPlacementPlan *plan,
                                 char *out, size_t out_cap);

#ifdef __cplusplus
}
#endif

#endif /* CNET_PLACEMENT_H */
