#ifndef CNET_RESOURCE_GOVERNOR_H
#define CNET_RESOURCE_GOVERNOR_H

/* Single control-plane budget policy for the unified self-improve deploy.
 *
 * The governor does NOT replace model_runtime leases or forest tiers. It is
 * the policy layer that every long-lived process (SoulHost, gap_lane_run,
 * harness) can share: RAM/VRAM ceilings, teach rate, teacher idle sleep, and
 * whether free-win knobs are requested. Fail-closed: invalid budgets refuse
 * admission; over-budget acquire returns OVER_BUDGET without mutating state.
 *
 * Gate: make resource_governor → RESOURCE_GOVERNOR_PASS.
 */

#include <stddef.h>
#include <stdint.h>

#include "cnet_export.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CNET_GOV_ABI_VERSION 1u

/* Named resource slots (bitmasks align with model_runtime where useful). */
#define CNET_GOV_RES_CPU   (1u << 0)
#define CNET_GOV_RES_GPU0  (1u << 1)
#define CNET_GOV_RES_GPU1  (1u << 2)

typedef struct {
    uint32_t abi_version;
    uint32_t struct_size;
    /* Soft ceilings (0 = unlimited for that dimension). */
    uint64_t ram_budget_bytes;
    uint64_t vram_budget_bytes;
    /* Teach / drain rate (0 = unlimited closures per drain). */
    size_t   max_closures_per_drain;
    /* Seconds of idle (no open-gap work) before a teacher may sleep (0 = off). */
    long     teacher_idle_sec;
    /* Deploy free-win requests (policy intent; hosts apply env if unset). */
    int      want_oracle_int8;     /* 1 = prefer int8 teacher */
    int      want_train_fast;      /* 1 = prefer CNET_TRAIN_FAST */
    int      want_topk_set;        /* 1 = prefer CNET_TOPK_SET set semantics */
    int      want_health_tick;     /* 1 = prefer periodic health */
    long     health_tick_seconds;  /* 0 = off */
    size_t   acq_stages;           /* 0 = leave default */
    int      want_counterfactual_order; /* 1 = CF as ORDER_ONLY tie-break */
} CnetGovPolicy;

typedef struct {
    uint64_t ram_resident_bytes;
    uint64_t vram_resident_bytes;
    size_t   closures_this_drain;
    int      teacher_resident;     /* 1 if teacher weights held */
    long     teacher_idle_for_sec; /* consecutive idle seconds reported by host */
} CnetGovUsage;

typedef struct {
    CnetGovPolicy policy;
    CnetGovUsage  usage;
    uint32_t      refuses;         /* count of fail-closed refusals */
    uint32_t      admits;          /* count of successful admits */
    int           loaded;
} CnetResourceGovernor;

/* Fail-closed validation: bad abi/size, zero-struct nonsense that would
   silently disable every budget, or negative-like idle/tick values. */
CNET_API int cnet_gov_policy_validate(const CnetGovPolicy *p);

/* Zero-init then fill measured deploy defaults (int8/train_fast/stages=40
   requested; budgets unlimited until the host sets them). */
CNET_API void cnet_gov_policy_deploy_defaults(CnetGovPolicy *p);

/* Overlay non-empty environment knobs onto *p (never lowers a set budget
   to zero unless the env value is an explicit 0). Returns 0. */
CNET_API int cnet_gov_policy_from_env(CnetGovPolicy *p);

CNET_API int cnet_gov_open(CnetResourceGovernor *g, const CnetGovPolicy *p);
CNET_API void cnet_gov_close(CnetResourceGovernor *g);

/* Host reports current residency / idle. Returns 0, or <0 on bad args. */
CNET_API int cnet_gov_report_usage(CnetResourceGovernor *g,
                                   const CnetGovUsage *u);

/* Would this reservation fit? Does not mutate. 1 = ok, 0 = refuse. */
CNET_API int cnet_gov_would_admit(const CnetResourceGovernor *g,
                                  uint64_t ram_delta,
                                  uint64_t vram_delta);

/* Commit a reservation (RAM/VRAM). Returns 0 or CNET_GOV_OVER_BUDGET (-3). */
#define CNET_GOV_OK           0
#define CNET_GOV_OVER_BUDGET -3
#define CNET_GOV_INVALID     -2

CNET_API int cnet_gov_admit(CnetResourceGovernor *g,
                            uint64_t ram_delta,
                            uint64_t vram_delta);
CNET_API int cnet_gov_release(CnetResourceGovernor *g,
                              uint64_t ram_delta,
                              uint64_t vram_delta);

/* Drain-rate accounting: begin_drain resets the closure counter; note_close
   returns 1 if another close is still allowed, 0 if the budget is exhausted. */
CNET_API void cnet_gov_begin_drain(CnetResourceGovernor *g);
CNET_API int  cnet_gov_note_close(CnetResourceGovernor *g);

/* Teacher sleep policy: 1 iff idle long enough and no forced residency. */
CNET_API int cnet_gov_teacher_should_sleep(const CnetResourceGovernor *g);

/* Copy a short human/log line into out (NUL-terminated). Returns length or -1. */
CNET_API int cnet_gov_format(const CnetResourceGovernor *g,
                             char *out, size_t out_cap);

#ifdef __cplusplus
}
#endif

#endif /* CNET_RESOURCE_GOVERNOR_H */
