#ifndef CNET_RESOURCE_GOVERNOR_H
#define CNET_RESOURCE_GOVERNOR_H

/* Single control-plane for budgets + compute orchestration.
 *
 * Budget side (self-improve / deploy):
 *   RAM/VRAM ceilings, teach rate, teacher idle sleep, free-win knobs.
 *
 * Compute orchestrator (generation):
 *   Profiles eco | balanced | turbo resolve sparse/page/MTP knobs and a
 *   duty-cycle policy so "in action" is NOT permanent 90–100% GPU/CPU.
 *   Call apply_compute BEFORE model load (env is read at open).
 *   Between tokens: token_gap pacing; after generate: idle → cool/sleep.
 *
 * Fail-closed: invalid budgets refuse admission; over-budget acquire
 * returns OVER_BUDGET without mutating state.
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

/* ---- Compute profile (duty cycle + sparse step cost) -------------------- */

typedef enum {
    CNET_GOV_PROFILE_ECO = 0,      /* min work: sparse, small HOT, pace */
    CNET_GOV_PROFILE_BALANCED = 1, /* default: quality sleep, moderate HOT */
    CNET_GOV_PROFILE_TURBO = 2,    /* max throughput; still not busy-idle */
    CNET_GOV_PROFILE_CUSTOM = 3
} CnetGovProfileKind;

typedef enum {
    CNET_GOV_PHASE_IDLE = 0,       /* no generate; may sleep teacher / cool */
    CNET_GOV_PHASE_WARM = 1,       /* admit / load / bind */
    CNET_GOV_PHASE_GENERATE = 2,   /* prefill or decode in flight */
    CNET_GOV_PHASE_DRAIN = 3       /* flush queues, archive, release */
} CnetGovPhase;

/* Resolved knobs applied to process env (and readable by hosts). */
typedef struct {
    float dsa_fraction;       /* 0 = dense attention; (0,1] sparse */
    int   dsa_enable;         /* 1 = CNET_DSA path */
    int   dsa_speed;          /* 1 = floor grid (more skips) */
    int   mla_kv;             /* int8 latent KV */
    int   kv_page;            /* async paged HOT/WARM/COLD */
    int   kv_hot_pages;       /* HOT ring pages */
    int   kv_page_len;        /* tokens per page */
    int   kv_rehydrate;       /* 1 = load COLD on demand */
    int   kv_quant;           /* 1 = int8 COLD */
    int   mtp_k;              /* 0 = off */
    int   mtp_parallel;       /* Medusa-lite draft */
    int   ep_places;          /* 1 or 2 expert place buckets */
    long  min_token_gap_ms;   /* pace between decode tokens (0 = none) */
    long  idle_cool_ms;       /* after end_generate, suggest cool */
    long  teacher_idle_sec;   /* mirror policy for sleep */
    int   max_gpu_places;     /* 1 = single GPU preferred */
    int   applied;            /* 1 after apply_compute_env */
} CnetGovComputePlan;

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

    /* Compute orchestrator (generation duty cycle). */
    CnetGovProfileKind compute_profile;
    float    dsa_fraction;         /* 0 = leave profile default; else override */
    int      force_env;            /* 1 = overwrite existing CNET_* env */
    long     min_token_gap_ms;     /* 0 = profile default */
    long     idle_cool_ms;         /* 0 = profile default */
    int      want_kv_page;         /* -1 = profile; 0/1 override */
    int      want_mtp_k;           /* -1 = profile; else k */
} CnetGovPolicy;

typedef struct {
    uint64_t ram_resident_bytes;
    uint64_t vram_resident_bytes;
    size_t   closures_this_drain;
    int      teacher_resident;     /* 1 if teacher weights held */
    long     teacher_idle_for_sec; /* consecutive idle seconds reported by host */
    /* Orchestrator telemetry (host may update; governor also tracks). */
    CnetGovPhase phase;
    uint64_t tokens_generated;     /* this process / session */
    uint64_t generate_bursts;      /* begin_generate count */
    uint64_t pace_yields;          /* between_token sleeps */
    uint64_t cool_signals;         /* end_generate → cool recommended */
} CnetGovUsage;

typedef struct {
    CnetGovPolicy      policy;
    CnetGovUsage       usage;
    CnetGovComputePlan plan;
    uint32_t           refuses;    /* count of fail-closed refusals */
    uint32_t           admits;     /* count of successful admits */
    int                loaded;
    /* Internal pacing clock (ms since epoch-ish; 0 = unset). */
    uint64_t           last_token_ms;
} CnetResourceGovernor;

/* Fail-closed validation: bad abi/size, zero-struct nonsense that would
   silently disable every budget, or negative-like idle/tick values. */
CNET_API int cnet_gov_policy_validate(const CnetGovPolicy *p);

/* Zero-init then fill measured deploy defaults (int8/train_fast/stages=40
   requested; budgets unlimited until the host sets them; balanced compute). */
CNET_API void cnet_gov_policy_deploy_defaults(CnetGovPolicy *p);

/* Overlay non-empty environment knobs onto *p (never lowers a set budget
   to zero unless the env value is an explicit 0). Returns 0. */
CNET_API int cnet_gov_policy_from_env(CnetGovPolicy *p);

/* Named profile → policy compute fields (eco|balanced|turbo). */
CNET_API int cnet_gov_policy_set_profile(CnetGovPolicy *p, CnetGovProfileKind k);
CNET_API int cnet_gov_policy_set_profile_name(CnetGovPolicy *p, const char *name);

CNET_API int cnet_gov_open(CnetResourceGovernor *g, const CnetGovPolicy *p);
CNET_API void cnet_gov_close(CnetResourceGovernor *g);

/* Resolve compute plan from policy (does not setenv). */
CNET_API int cnet_gov_resolve_compute(CnetResourceGovernor *g);

/* Apply plan to process environment for CCE loaders.
 * Default: set only if unset. force_env / CNET_GOV_FORCE=1 overwrites.
 * MUST run before cce_gguf_load / ds_host_open. */
CNET_API int cnet_gov_apply_compute_env(CnetResourceGovernor *g);

/* One-shot: defaults + env + profile name + resolve + apply.
 * profile NULL → CNET_GOV_PROFILE / "balanced". */
CNET_API int cnet_gov_orchestrate_boot(CnetResourceGovernor *g,
                                       const char *profile_name);

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

/* ---- Generation duty cycle --------------------------------------------- */

/* Mark generate start (phase=GENERATE). */
CNET_API int cnet_gov_begin_generate(CnetResourceGovernor *g);

/* After each decoded token: optional pace sleep (min_token_gap_ms). */
CNET_API int cnet_gov_between_token(CnetResourceGovernor *g);

/* End generate: phase→DRAIN then IDLE; returns 1 if cool/sleep recommended. */
CNET_API int cnet_gov_end_generate(CnetResourceGovernor *g);

/* Host idle tick (seconds). Updates teacher idle; may recommend sleep. */
CNET_API int cnet_gov_tick_idle(CnetResourceGovernor *g, long idle_sec);

/* 1 if phase is IDLE and cool was signaled (GPU should not stay pinned). */
CNET_API int cnet_gov_should_cool(const CnetResourceGovernor *g);

/* Copy a short human/log line into out (NUL-terminated). Returns length or -1. */
CNET_API int cnet_gov_format(const CnetResourceGovernor *g,
                             char *out, size_t out_cap);

#ifdef __cplusplus
}
#endif

#endif /* CNET_RESOURCE_GOVERNOR_H */
