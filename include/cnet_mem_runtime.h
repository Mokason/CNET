/* Three-lane runtime memory — CNET law, not chat soup.
 *
 *   STM  — hot CERT skills (serve now)
 *   LTM  — cold capsule index (fetch by id/path)
 *   FORM — async experience → verify → promote capsule (never serve raw)
 *   ASI  — catalog resolve gate (capability/exec/conformal) + episodes (evidence)
 *
 * Serve path never trains. Form path never mutates STM without verify.
 * ASI episodes never serve. Floors never lowered.
 */
#ifndef CNET_MEM_RUNTIME_H
#define CNET_MEM_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

#include "cnet_export.h"
#include "base.h"
#include "hybrid_ai.h"
#include "router.h"
#include "cnet_asi_improve.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CNET_MEM_NAME_MAX 64
#define CNET_MEM_PATH_MAX 384
#define CNET_MEM_STM_MAX 64
#define CNET_MEM_LTM_MAX 128
#define CNET_MEM_FORM_MAX 64

typedef enum {
    CNET_MEM_OK = 0,
    CNET_MEM_ABSTAIN = 1,
    CNET_MEM_ERR = -1
} CnetMemStatus;

typedef struct {
    char name[CNET_MEM_NAME_MAX];
    char capsule_dir[CNET_MEM_PATH_MAX]; /* LTM path; empty if STM-only */
    int active;
    uint64_t hits;
    uint64_t last_tick;
    double ema_good; /* 1 = recent good serve proxy */
} CnetMemSlot;

typedef struct {
    char name[CNET_MEM_NAME_MAX];
    char capsule_dir[CNET_MEM_PATH_MAX];
    int active;
    int pending;      /* 1 = waiting form */
    int promoted;     /* 1 = became CERT capsule */
    int rejected;
    double score;     /* higher = better candidate */
    double incumbent; /* score to beat; 0 = new domain */
} CnetMemFormItem;

typedef struct CnetMemRuntime {
    CnetBase base;           /* owns materialised units */
    HybridAi cov;
    PrimitiveRegistry reg;
    int base_ready;
    int reg_ready;

    CnetMemSlot stm[CNET_MEM_STM_MAX];
    size_t stm_n;
    size_t stm_cap; /* max hot pins */

    CnetMemSlot ltm[CNET_MEM_LTM_MAX];
    size_t ltm_n;

    CnetMemFormItem form[CNET_MEM_FORM_MAX];
    size_t form_n;

    /* Product wire: ASI catalog + episodes (always inited; precond 0 = open) */
    CnetAsiLib asi;
    int asi_gate; /* 1 = run ASI resolve before CERT fetch (default 1) */

    uint64_t tick;
    /* stats */
    uint64_t n_serve;
    uint64_t n_stm_hit;
    uint64_t n_ltm_fetch;
    uint64_t n_abstain;
    uint64_t n_form_submit;
    uint64_t n_form_promote;
    uint64_t n_form_reject;
    uint64_t n_async_ticks;
    uint64_t n_asi_block;     /* exec/conf/recall from ASI gate */
    uint64_t n_episode_log;
} CnetMemRuntime;

CNET_API void cnet_mem_init(CnetMemRuntime *m, size_t stm_cap);
CNET_API void cnet_mem_free(CnetMemRuntime *m);

/* LTM: register a capsule directory (does not load until fetch).
 * Also registers ASI skill name with open precond (capability=name). */
CNET_API int cnet_mem_ltm_add(CnetMemRuntime *m, const char *name,
                              const char *capsule_dir);

/* Optional ASI catalog fields for a skill already (or about to be) in LTM. */
CNET_API int cnet_mem_asi_configure(CnetMemRuntime *m, const char *name,
                                    const char *capability, uint32_t precond_mask,
                                    uint32_t privilege, int kind, double conf_q);

/* STM: pin an already-CERT unit name (must exist in reg after fetch/seal). */
CNET_API int cnet_mem_stm_pin(CnetMemRuntime *m, const char *name);

/* Ensure name is runnable: ASI gate → STM hit, else LTM import+bridge, else abstain.
 * Back-compat: world_mask all-bits, residual unmeasured. */
CNET_API int cnet_mem_resolve(CnetMemRuntime *m, const char *name,
                              const char **unit_out);

/* Product wire resolve: host world_mask + residual for conformal. */
CNET_API int cnet_mem_resolve_ex(CnetMemRuntime *m, const char *name,
                                 uint32_t world_mask, double residual,
                                 const char **unit_out);

/* Record serve outcome for STM ranking (good=1 / bad=0). */
CNET_API void cnet_mem_feedback(CnetMemRuntime *m, const char *name, int good);

/* Product wire feedback: also logs ASI episode (evidence only). */
CNET_API void cnet_mem_feedback_ex(CnetMemRuntime *m, const char *name, int good,
                                   uint32_t world_mask, double residual);

/* FORM: submit candidate (path may be empty until worker fills capsule).
 * score must beat incumbent (or incumbent==0 for new). Async promote via tick. */
CNET_API int cnet_mem_form_submit(CnetMemRuntime *m, const char *name,
                                  const char *capsule_dir, double score,
                                  double incumbent);

/* One forming pass: promote pending items that already have a sealed capsule
 * dir and score > incumbent; import into base, index LTM, optional STM pin.
 * Skills with continual ASI regression are rejected (fail-closed).
 * Returns number promoted this tick. */
CNET_API int cnet_mem_form_tick(CnetMemRuntime *m, int pin_on_promote);

/* Stats snapshot helpers */
CNET_API double cnet_mem_stm_hit_rate(const CnetMemRuntime *m);
CNET_API void cnet_mem_dump_stats(const CnetMemRuntime *m, char *buf, size_t cap);
CNET_API int cnet_mem_continual_regressions(const CnetMemRuntime *m);

#ifdef __cplusplus
}
#endif

#endif
