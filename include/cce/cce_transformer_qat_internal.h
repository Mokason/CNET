#ifndef CCE_TRANSFORMER_QAT_INTERNAL_H
#define CCE_TRANSFORMER_QAT_INTERNAL_H

/* INTERNAL to the QAT trainer — NOT a public API.
 *
 * Exists so the loader translation unit (cce_transformer_qat_load.c) can see
 * the parameter layout without the trainer core having to depend on $(CCE).
 * That split is what lets the hermetic gradcheck gate link without
 * safetensors / gguf / kv_page, and therefore build on every platform.
 * Spec: docs/superpowers/specs/2026-08-16-qat-modern-block-design.md section 3
 */

#include "cce_transformer_qat.h"

/* ---- a parameter matrix bundled with grad + Adam moments ---- */
typedef struct {
    float *w, *g, *m, *v;   /* shadow, grad, adam m, adam v */
    int in, out;            /* [in][out] row-major; bias has in == 1 */
} P;


struct cce_transformer_qat {
    cce_transformer_qat_config cfg;
    int hd;                   /* head_dim = D / n_head */
    int   kvh;                /* effective KV heads: cfg.n_kv_head ? : n_head */
    float eps;                /* effective norm epsilon: cfg.norm_eps ? : 1e-5f */
    /* params */
    P tok_emb;                /* [vocab][D]  (QAT-able) */
    P pos_emb;                /* [block][D]  FROZEN FP  */
    P *qkv_w, *qkv_b;         /* per layer [D][3D], [1][3D] */
    P *proj_w, *proj_b;       /* per layer [D][D],  [1][D]  */
    P *up_w, *up_b;           /* per layer [D][M],  [1][M]  */
    P *down_w, *down_b;       /* per layer [M][D],  [1][D]  */
    P *ln1_w, *ln1_b, *ln2_w, *ln2_b;  /* per layer [1][D] */
    P lnf_w, lnf_b;           /* [1][D] */
    P head_w, head_b;         /* [D][vocab], [1][vocab] (QAT-able) */
    /* adam step counter */
    int adam_t;
    /* per-step activation cache (sized for block_size) */
    int T;                    /* live sequence length of the cache */
    float *x0;                /* [T][D] embedding output */
    float *xin;               /* [L][T][D] block input */
    float *ln1o, *ln2o;       /* [L][T][D] */
    float *ln1_mean, *ln1_rstd, *ln2_mean, *ln2_rstd;   /* [L][T] */
    float *qkv;               /* [L][T][3D] */
    float *probs;             /* [L][H][T][T] softmax rows */
    float *cat;               /* [L][T][D] concat of head outputs */
    float *xattn;             /* [L][T][D] x after attention residual */
    float *mpre;              /* [L][T][M] pre-GELU */
    float *mpost;             /* [L][T][M] post-GELU */
    float *hfin;              /* [D] final LN output (last position) */
    float lnf_mean, lnf_rstd;
    float *logits;            /* [vocab] */
    /* ternary scratch: effective weights for one matrix at a time */
    float *eff;               /* max(in*out) over QAT-able matrices */
    /* backward scratch */
    float *dx, *dtmp, *dmid, *dqkv, *dcat;
    unsigned long long rng;
    /* Gradcheck group registry. Registration is a SIDE EFFECT of allocation
       (the PA macro in create allocates and registers in one call), so a new
       parameter group cannot be added without becoming gradcheckable. The
       previous hand-built P* groups[128] array silently dropped unregistered
       groups, and its 'ng + 12 > 124' guard would quietly truncate the moment
       a 13th per-layer matrix was added. */
    P**  groups;
    const char** group_names;
    int* group_trainable;   /* 0 = frozen by design (pos_emb): registered so
                               the audit stays total, but skipped by gradcheck
                               since backward deliberately writes no gradient
                               for it and Adam deliberately skips it. */
    int  n_groups, cap_groups;
};

/* Shared by the core and the loader TU (was static before the split). */
int  p_alloc(P* p, int in, int out);
void p_free(P* p);


/* Gradcheck group registry (see the struct comment). */
void qat_group_reset(cce_transformer_qat* t);
int  qat_group_add(cce_transformer_qat* t, P* p, const char* name, int trainable);

#endif /* CCE_TRANSFORMER_QAT_INTERNAL_H */
