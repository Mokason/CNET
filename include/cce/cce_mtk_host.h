#ifndef CCE_MTK_HOST_H
#define CCE_MTK_HOST_H

/*
 * Product host: GGUF model + MTK skills + resource governor + optional GPU.
 * Boot once, route skills per prompt, generate with duty-cycle.
 *
 * Env (optional):
 *   CNET_GOV_PROFILE=eco|balanced|turbo
 *   CNET_MTK_ROUTES=path/to/routes.txt
 *   CNET_MTK_HOOK=1          install block linear hook
 *   CNET_FOREST_NO_PERSIST=1 for scratch forests
 *   CNET_GPU=0|1|auto        (default auto: OpenCL dual if present)
 *   CNET_GPU_BACKEND=opencl|hip|auto
 *   CNET_ORACLE_INT8=1       defaulted on when GPU attach is attempted
 *   CNET_INFER_FP=1          keep FP weights (slower GPU path)
 *
 * Gate / eval: make mtk_eval
 */

#include "cce_defs.h"
#include "cce_gguf.h"
#include "cce_mtk.h"
#include "../resource_governor.h"

struct cce_clgemm;
struct cce_hipgemm;

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cce_mtk_host {
    cce_gguf_qwen2       *model; /* owned */
    cce_mtk              *mtk;   /* owned */
    struct cce_clgemm    *cl;    /* owned when GPU attached */
    struct cce_hipgemm   *hip;   /* owned when GPU attached */
    CnetResourceGovernor  gov;
    int                   loaded;
    int                   gpu_active; /* 1 if cl and/or hip bound to model */
    int                   gen_tokens;
    double                last_prefill_ms;
    double                last_decode_ms;
    char                  routes_path[320];
    char                  last_skill[160];
    char                  gpu_name[160];
} cce_mtk_host;

/* Open GGUF (or qwen35), bind MTK, optional routes, apply gov profile. */
cce_result cce_mtk_host_open(cce_mtk_host **out, const char *gguf_path,
                             const char *routes_path /*nullable*/);

void cce_mtk_host_close(cce_mtk_host *h);

/* Keyword-route skill for this prompt (no-op NOT_FOUND if no match). */
cce_result cce_mtk_host_route_prompt(cce_mtk_host *h, const char *prompt);

/* Explicit skill file. */
cce_result cce_mtk_host_apply_skill(cce_mtk_host *h, const char *skill_path,
                                    float scale);
cce_result cce_mtk_host_revert(cce_mtk_host *h);

/* Teacher-forced / greedy generate: prefill prompt, then n_new tokens.
 * out_tokens must hold n_prompt + n_new. Writes total into *out_n. */
cce_result cce_mtk_host_generate(cce_mtk_host *h, const int *prompt,
                                 int n_prompt, int n_new, int *out_tokens,
                                 int *out_n);

/* Accessors */
cce_gguf_qwen2 *cce_mtk_host_model(cce_mtk_host *h);
cce_mtk        *cce_mtk_host_mtk(cce_mtk_host *h);
int             cce_mtk_host_gpu_active(const cce_mtk_host *h);
const char     *cce_mtk_host_gpu_name(const cce_mtk_host *h);
double          cce_mtk_host_last_prefill_ms(const cce_mtk_host *h);
double          cce_mtk_host_last_decode_ms(const cce_mtk_host *h);

#ifdef __cplusplus
}
#endif

#endif /* CCE_MTK_HOST_H */
