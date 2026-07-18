#ifndef CCE_INFER_BACKEND_H
#define CCE_INFER_BACKEND_H

/*
 * Dual inference backends (DS residual host + GGUF token path), device-ready.
 * ==========================================================================
 * Kind:
 *   DS   — isolated MLA+MoE+DSA residual host (cce_ds_host), synthetic/.cnetpack
 *   GGUF — full transformer token path (cce_gguf_qwen2_forward), synthetic fixture
 *          or real .gguf import
 *
 * Device:
 *   CPU  — default, hermetic gates and microbenches
 *   GPU  — GGUF via cce_clgemm (OpenCL linear seam); DS GPU not yet wired
 *          (returns CCE_ERR_UNSUPPORTED until a DS device path lands)
 *
 * Both kinds keep separate open/close so CPU then GPU integration and tests
 * can land independently without coupling the runtimes.
 */

#include "cce_defs.h"
#include "cce_ds_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CCE_INFER_KIND_DS   = 0,
    CCE_INFER_KIND_GGUF = 1
} cce_infer_kind;

typedef enum {
    CCE_INFER_DEVICE_CPU = 0,
    CCE_INFER_DEVICE_GPU = 1
} cce_infer_device;

typedef struct cce_infer_opts {
    cce_infer_kind   kind;
    cce_infer_device device;
    /* DS path */
    const cce_ds_hparams* ds_hp;   /* NULL → default_small */
    cce_ds_host_opts      ds_opts; /* archive/pack/dsa; ignored for GGUF */
    /* GGUF path */
    const char* gguf_path;         /* existing file, or NULL if synthetic */
    int         synthetic;         /* 1: write hermetic tiny qwen2 GGUF */
    const char* synthetic_path;    /* where to write fixture; default tmp */
    int         max_ctx;           /* hint; GGUF also honors TL_CTX / env */
    float       sparse_kv;         /* 0 = off; (0,1] enables sparse KV */
    int         dsa_enable;        /* GGUF: map to DSA knobs on model */
    float       dsa_fraction;      /* unused when dsa_enable=0 */
} cce_infer_opts;

typedef struct cce_infer_session cce_infer_session;

void cce_infer_opts_default(cce_infer_opts* o, cce_infer_kind kind,
                            cce_infer_device device);

/* Open one backend on the requested device. GPU+DS → CCE_ERR_UNSUPPORTED.
 * GPU+GGUF with no OpenCL → CCE_ERR_NOT_FOUND (caller may fall back to CPU). */
cce_result cce_infer_open(cce_infer_session** out, const cce_infer_opts* opts);
void       cce_infer_close(cce_infer_session* s);

cce_infer_kind   cce_infer_get_kind(const cce_infer_session* s);
cce_infer_device cce_infer_get_device(const cce_infer_session* s);
const char*      cce_infer_kind_name(cce_infer_kind k);
const char*      cce_infer_device_name(cce_infer_device d);

/* GGUF: vocab size; DS: host vocab (may be 0 if head unbound). */
int cce_infer_vocab(const cce_infer_session* s);
int cce_infer_d_model(const cce_infer_session* s);
int cce_infer_n_layer(const cce_infer_session* s);

/* Token forward:
 *   GGUF — cce_gguf_qwen2_forward (tokens → logits)
 *   DS   — set residual from synthetic embed of tokens[0], then n residual steps;
 *          logits filled if head bound, else left zeroed
 */
cce_result cce_infer_forward_tokens(cce_infer_session* s,
                                    const int* tokens, int n_tokens,
                                    float* logits_out, int logits_cap);

/* Reset KV / pos (GGUF cur_pos=0; DS host_reset). */
void cce_infer_reset(cce_infer_session* s);

/* Microbench: n_tokens residual/token steps, report tok/s. */
cce_result cce_infer_bench(cce_infer_session* s, int n_tokens, double* out_tok_s);

/* Optional raw handles for kind-specific tests (do not free). */
cce_ds_host*     cce_infer_as_ds(cce_infer_session* s);
struct cce_gguf_qwen2* cce_infer_as_gguf(cce_infer_session* s);

#ifdef __cplusplus
}
#endif

#endif /* CCE_INFER_BACKEND_H */
