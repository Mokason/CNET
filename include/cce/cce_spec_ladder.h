/* Progressive specialist conversion ladder (quant-aware, fail-closed).
 *
 * Unlike whole-model post-hoc ternary (cce_gguf_qwen2_quantize_ternary), this
 * converts ONE forest specialist (or name-matched family) at a time:
 *
 *   1. Optional STE QAT on FP shadow (ternary forward, STE into shadow)
 *   2. Optional data-aware OBQ using calibration activations
 *   3. Ternary quantize from the adapted shadow
 *   4. CERTIFY: held-out relerr vs FP teacher outputs ≤ threshold
 *   5. On PASS → pack_trits (1.6-bit); on FAIL → leave wider (restore FP)
 *
 * Real activations: capture bank filled via cce_gguf_set_capture_hook during
 * calibration forwards, then consumed per specialist by name.
 *
 * Family schedule: gate/up (looser) → down (stricter) → attn projections.
 *
 * Env (tools): CNET_LADDER_FAMILY  CNET_LADDER_MODE  CNET_LADDER_CERT
 */
#ifndef CCE_SPEC_LADDER_H
#define CCE_SPEC_LADDER_H

#include "cce_defs.h"
#include "cce_block.h"
#include "cce_forest.h"
#include "cce_gguf.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CCE_LADDER_POSTHOC = 0, /* ternary PTQ only (baseline; often fails cert) */
    CCE_LADDER_STE     = 1, /* STE QAT then ternary (recommended) */
    CCE_LADDER_OBQ     = 2  /* data-aware OBQ then ternary (small in_dim) */
} cce_ladder_mode;

typedef struct {
    cce_ladder_mode mode;
    float cert_relerr; /* max allowed ||Yq-Yt||/||Yt|| on holdout (default 0.20) */
    int   ste_steps;   /* Adam STE steps (default 48) */
    float ste_lr;      /* Adam lr */
    int   pack_trits;  /* 1 = pack on PASS (default 1) */
    int   n_calib;     /* calib rows if generating synthetic X (default 256) */
    int   n_holdout;   /* holdout rows for cert (default 64) */
    int   obq_max_in;  /* skip OBQ if in_dim > this (default 512) */
    unsigned seed;
} cce_ladder_cfg;

typedef struct {
    char  name[160];
    int   in_dim, out_dim;
    int   certified;       /* 1 = PASS */
    int   packed;          /* 1 = trits live */
    int   mode_used;
    int   used_real_acts;  /* 1 = calib from capture bank */
    float relerr_posthoc;  /* naive ternary vs teacher (diagnostic) */
    float relerr_final;    /* after ladder vs teacher */
    float ste_loss0, ste_loss1;
} cce_ladder_report;

/* ---- activation capture bank ---- */
typedef struct {
    char  name[160];
    int   in_dim;
    int   n_rows;
    int   cap_rows;
    float *rows; /* [cap_rows][in_dim] */
} cce_ladder_spec_cap;

typedef struct {
    cce_ladder_spec_cap *specs;
    int n_specs;
    int max_specs;
    int max_rows_per; /* cap per specialist */
} cce_ladder_bank;

/* One schedule stage: name substring + cert bar + mode overrides */
typedef struct {
    const char *family;    /* e.g. "gate_proj", "down_proj", "attn_q" */
    float cert_relerr;     /* stage-specific bar */
    cce_ladder_mode mode;  /* usually STE */
    int ste_steps;
} cce_ladder_stage;

/* Defaults: STE, cert 0.20, pack on pass. */
void cce_ladder_cfg_default(cce_ladder_cfg *cfg);
cce_ladder_mode cce_ladder_mode_parse(const char *s);

/* Optional OpenCL + hipBLAS for STE (tiled T≤8). NULL = CPU.
 * Attach cl/hip to the model for GPU capture forwards. */
struct cce_clgemm;
struct cce_hipgemm;
void cce_ladder_set_clgemm(struct cce_clgemm *cl);
struct cce_clgemm *cce_ladder_get_clgemm(void);
void cce_ladder_set_hipgemm(struct cce_hipgemm *h);
struct cce_hipgemm *cce_ladder_get_hipgemm(void);
/* STE matmul telemetry (ephemeral GPU path vs CPU fallback). */
void cce_ladder_gpu_stats(unsigned long *ok, unsigned long *fail);
/* Host STE worker threads (ternize/Adam/CPU matmul). Env CNET_LADDER_THREADS. */
int cce_ladder_nthreads(void);
/* Concurrent specialists (device-pinned OpenCL lanes). Env CNET_LADDER_PARALLEL
 * (1–4; default 2 when dual GPU). */
int cce_ladder_spec_parallel(void);

/* Convert one linear block. X is [n_rows][in_dim]; NULL → synthetic Gaussian. */
cce_result cce_ladder_convert_block(cce_block *blk, const char *name,
                                    const float *X, int n_rows,
                                    const cce_ladder_cfg *cfg,
                                    cce_ladder_report *rep);

/* Synthetic-X forest walk (family substring). Returns count attempted. */
int cce_ladder_convert_forest(cce_forest *f, const char *family_sub,
                              const cce_ladder_cfg *cfg,
                              cce_ladder_report *reps, int max_reps);

/* ---- capture bank ---- */
cce_result cce_ladder_bank_init(cce_ladder_bank *bank, int max_specs,
                                int max_rows_per);
void cce_ladder_bank_free(cce_ladder_bank *bank);
/* Hook-compatible callback: install with cce_gguf_set_capture_hook(..., bank). */
void cce_ladder_bank_on_capture(const char *spec_name, const float *rows,
                                int n_rows, int in_dim, void *uctx);
/* Lookup rows for a specialist name (substring or exact). */
const cce_ladder_spec_cap *cce_ladder_bank_find(const cce_ladder_bank *bank,
                                                const char *name);

/* Run n_seq calibration forwards on model to fill bank (hook auto install/detach).
 * tokens: [n_seq][seq_len]. Returns CCE_OK. */
cce_result cce_ladder_capture_model(cce_gguf_qwen2 *m, cce_ladder_bank *bank,
                                    const int *tokens, int n_seq, int seq_len);

/* Convert forest family using bank activations when available. */
int cce_ladder_convert_forest_bank(cce_forest *f, const char *family_sub,
                                   const cce_ladder_bank *bank,
                                   const cce_ladder_cfg *cfg,
                                   cce_ladder_report *reps, int max_reps);

/* Default multi-stage schedule (gate/up → down → attn). *n_out stages. */
const cce_ladder_stage *cce_ladder_default_schedule(int *n_out);

/* Run full schedule on model forest (one process — no reload between families).
 * checkpoint_export: if non-NULL, rewrite certified LDTR after each stage
 * (resume-safe). Returns total converts (including already-packed skips). */
int cce_ladder_run_schedule(cce_gguf_qwen2 *m, const cce_ladder_bank *bank,
                            const cce_ladder_stage *stages, int n_stages,
                            const cce_ladder_cfg *base_cfg,
                            cce_ladder_report *reps, int max_reps,
                            const char *checkpoint_export);

/* Persist only certified/packed specialists (magic 'LDTR' v1). FP specialists
 * are skipped (not forced posthoc). Returns count written. */
int cce_ladder_export_certified(const cce_forest *f, const char *path);

/* Load certified trit specialists into an existing forest (name match).
 * Returns count loaded. */
int cce_ladder_import_certified(cce_forest *f, const char *path);

#ifdef __cplusplus
}
#endif

#endif /* CCE_SPEC_LADDER_H */
