/* Supra QAT trainer — the transformer backward. See include/cce/cce_supra_train.h.
 *
 * Every dot product accumulates in double (house pattern from cce_wordlm);
 * the backward mirrors the verified forward op-for-op:
 *
 *   x = tok_emb[t] + pos_emb[p]
 *   per layer: a  = LN1(x); qkv = a·Wqkv + bqkv
 *              per head: S = mask(q·kᵀ/√hd); P = softmax(S); o = P·v
 *              x = x + concat(o)·Wproj + bproj
 *              b  = LN2(x); m = gelu(b·Wup + bup)
 *              x = x + m·Wdown + bdown
 *   h = LNf(x[last]); logits = h·Whead + bhead
 *
 * Ternary QAT groups run the BitNet b1.58 recipe: effective weight
 * γ_o·clamp(round(W[:,o]/γ_o)) with γ_o = per-OUTPUT-column absmean
 * (cce_block_quantize_ternary orientation), STE backward (shadow grad =
 * ternary grad), γ treated as constant (cce_wordlm precedent).
 */

#include "../../include/cce/cce_supra_train.h"
#include "../../include/cce/cce_safetensors.h"   /* cce_supra_decomposed, head_fp, forest */

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

/* ---- a parameter matrix bundled with grad + Adam moments ---- */
typedef struct {
    float *w, *g, *m, *v;   /* shadow, grad, adam m, adam v */
    int in, out;            /* [in][out] row-major; bias has in == 1 */
} P;

static int p_alloc(P* p, int in, int out) {
    size_t n = (size_t)in * out;
    p->w = (float*)calloc(n, sizeof(float));
    p->g = (float*)calloc(n, sizeof(float));
    p->m = (float*)calloc(n, sizeof(float));
    p->v = (float*)calloc(n, sizeof(float));
    p->in = in; p->out = out;
    return p->w && p->g && p->m && p->v;
}
static void p_free(P* p) { free(p->w); free(p->g); free(p->m); free(p->v); }

struct cce_supra_train {
    cce_supra_train_config cfg;
    int hd;                   /* head_dim = D / n_head */
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
};

/* ---- rng ---- */
static float tr_rnd(cce_supra_train* t) {
    t->rng = t->rng * 6364136223846793005ULL + 1442695040888963407ULL;
    return (float)((double)(t->rng >> 33) / 2147483648.0 - 1.0);
}

/* ---- ternary projection (per-OUTPUT column absmean, cce_block orientation) ---- */
static void tern_effective(const P* p, float* eff) {
    int in = p->in, out = p->out;
    for (int o = 0; o < out; ++o) {
        double s = 0.0;
        for (int i = 0; i < in; ++i) s += fabs((double)p->w[(size_t)i*out + o]);
        float g = in ? (float)(s / in) : 0.0f;
        for (int i = 0; i < in; ++i) {
            float wv = p->w[(size_t)i*out + o];
            int code = 0;
            if (g > 0.0f) { float r = roundf(wv / g); if (r > 1) r = 1; if (r < -1) r = -1; code = (int)r; }
            eff[(size_t)i*out + o] = g * (float)code;
        }
    }
}

/* effective weight pointer for a matrix: ternary copy if qat, else shadow */
static const float* mat_eff(cce_supra_train* t, const P* p, int qat) {
    if (!qat) return p->w;
    tern_effective(p, t->eff);
    return t->eff;
}

/* ---- linear: y[T][out] = x[T][in]·W[in][out] + b[out] (double acc) ---- */
static void lin_fwd(const float* x, const float* W, const float* b,
                    float* y, int T, int in, int out) {
    for (int r = 0; r < T; ++r) {
        const float* xr = x + (size_t)r*in;
        float* yr = y + (size_t)r*out;
        for (int o = 0; o < out; ++o) {
            double s = b ? (double)b[o] : 0.0;
            for (int i = 0; i < in; ++i) s += (double)xr[i] * W[(size_t)i*out + o];
            yr[o] = (float)s;
        }
    }
}

/* backward of lin_fwd: given dy, accumulate dW += xᵀ·dy, db += Σdy, dx = dy·Wᵀ.
   W passed here is the EFFECTIVE weight used in the forward (STE: the shadow
   grad target is p->g regardless). dx may be NULL. */
static void lin_bwd(const float* x, const float* W, const float* dy,
                    float* dW, float* db, float* dx, int T, int in, int out) {
    if (dW) {
        for (int i = 0; i < in; ++i) {
            for (int o = 0; o < out; ++o) {
                double s = 0.0;
                for (int r = 0; r < T; ++r)
                    s += (double)x[(size_t)r*in + i] * dy[(size_t)r*out + o];
                dW[(size_t)i*out + o] += (float)s;
            }
        }
    }
    if (db) {
        for (int o = 0; o < out; ++o) {
            double s = 0.0;
            for (int r = 0; r < T; ++r) s += (double)dy[(size_t)r*out + o];
            db[o] += (float)s;
        }
    }
    if (dx) {
        for (int r = 0; r < T; ++r) {
            const float* dyr = dy + (size_t)r*out;
            float* dxr = dx + (size_t)r*in;
            for (int i = 0; i < in; ++i) {
                double s = 0.0;
                for (int o = 0; o < out; ++o) s += (double)dyr[o] * W[(size_t)i*out + o];
                dxr[i] = (float)s;
            }
        }
    }
}

/* ---- LayerNorm (matches cce_tensor_layer_norm: biased var, eps in sqrt) ---- */
static void ln_fwd(const float* x, const float* w, const float* b, int T, int D,
                   float* y, float* mean_out, float* rstd_out) {
    for (int r = 0; r < T; ++r) {
        const float* xr = x + (size_t)r*D;
        float* yr = y + (size_t)r*D;
        double mu = 0.0;
        for (int i = 0; i < D; ++i) mu += xr[i];
        mu /= D;
        double var = 0.0;
        for (int i = 0; i < D; ++i) { double d = xr[i] - mu; var += d*d; }
        var /= D;
        float rstd = (float)(1.0 / sqrt(var + 1e-5));
        for (int i = 0; i < D; ++i)
            yr[i] = ((xr[i] - (float)mu) * rstd) * w[i] + b[i];
        mean_out[r] = (float)mu; rstd_out[r] = rstd;
    }
}

/* dx = rstd·(dyw − mean(dyw) − x̂·mean(dyw·x̂)), dyw = dy·w; accumulates dw/db. */
static void ln_bwd(const float* x, const float* w, const float* dy,
                   const float* mean, const float* rstd, int T, int D,
                   float* dx, float* dw, float* db) {
    for (int r = 0; r < T; ++r) {
        const float* xr = x + (size_t)r*D;
        const float* dyr = dy + (size_t)r*D;
        float* dxr = dx + (size_t)r*D;
        float mu = mean[r], rs = rstd[r];
        double s1 = 0.0, s2 = 0.0;
        for (int i = 0; i < D; ++i) {
            float xhat = (xr[i] - mu) * rs;
            float dyw = dyr[i] * w[i];
            s1 += dyw; s2 += (double)dyw * xhat;
            if (dw) dw[i] += dyr[i] * xhat;
            if (db) db[i] += dyr[i];
        }
        s1 /= D; s2 /= D;
        for (int i = 0; i < D; ++i) {
            float xhat = (xr[i] - mu) * rs;
            float dyw = dyr[i] * w[i];
            dxr[i] = rs * (dyw - (float)s1 - xhat * (float)s2);
        }
    }
}

/* ---- tanh-GELU (matches cce_tensor_gelu constants) ---- */
static float gelu_f(float x) {
    float x3 = x*x*x;
    float inner = 0.79788456f * (x + 0.044715f * x3);
    return 0.5f * x * (1.0f + tanhf(inner));
}
static float gelu_df(float x) {
    float x2 = x*x;
    float inner = 0.79788456f * (x + 0.044715f * x2 * x);
    float th = tanhf(inner);
    float dinner = 0.79788456f * (1.0f + 0.134145f * x2);
    return 0.5f * (1.0f + th) + 0.5f * x * (1.0f - th*th) * dinner;
}

/* ================= create / free ================= */

cce_supra_train* cce_supra_train_create(const cce_supra_train_config* cfg) {
    if (!cfg || cfg->n_head < 1 || cfg->n_embd % cfg->n_head) return NULL;
    /* fixed-size locals in forward/backward cap these (srow/dprow: block_size,
       dh/dxl: n_embd). Real Supra is 384/256; refuse beyond the caps. */
    if (cfg->block_size < 1 || cfg->block_size > 1024) return NULL;
    if (cfg->n_embd < 1 || cfg->n_embd > 4096) return NULL;
    if (cfg->n_layer < 1 || cfg->mlp_hidden < 1 || cfg->vocab < 2) return NULL;
    cce_supra_train* t = (cce_supra_train*)calloc(1, sizeof(*t));
    if (!t) return NULL;
    t->cfg = *cfg;
    t->hd = cfg->n_embd / cfg->n_head;
    t->rng = cfg->seed ? cfg->seed : 0x9E3779B97F4A7C15ULL;

    int L = cfg->n_layer, D = cfg->n_embd, M = cfg->mlp_hidden,
        V = cfg->vocab, B = cfg->block_size;
    int ok = 1;
    ok &= p_alloc(&t->tok_emb, V, D);
    ok &= p_alloc(&t->pos_emb, B, D);
    t->qkv_w  = (P*)calloc(L, sizeof(P)); t->qkv_b  = (P*)calloc(L, sizeof(P));
    t->proj_w = (P*)calloc(L, sizeof(P)); t->proj_b = (P*)calloc(L, sizeof(P));
    t->up_w   = (P*)calloc(L, sizeof(P)); t->up_b   = (P*)calloc(L, sizeof(P));
    t->down_w = (P*)calloc(L, sizeof(P)); t->down_b = (P*)calloc(L, sizeof(P));
    t->ln1_w  = (P*)calloc(L, sizeof(P)); t->ln1_b  = (P*)calloc(L, sizeof(P));
    t->ln2_w  = (P*)calloc(L, sizeof(P)); t->ln2_b  = (P*)calloc(L, sizeof(P));
    if (!t->qkv_w || !t->qkv_b || !t->proj_w || !t->proj_b || !t->up_w || !t->up_b ||
        !t->down_w || !t->down_b || !t->ln1_w || !t->ln1_b || !t->ln2_w || !t->ln2_b) ok = 0;
    for (int l = 0; ok && l < L; ++l) {
        ok &= p_alloc(&t->qkv_w[l], D, 3*D);  ok &= p_alloc(&t->qkv_b[l], 1, 3*D);
        ok &= p_alloc(&t->proj_w[l], D, D);   ok &= p_alloc(&t->proj_b[l], 1, D);
        ok &= p_alloc(&t->up_w[l], D, M);     ok &= p_alloc(&t->up_b[l], 1, M);
        ok &= p_alloc(&t->down_w[l], M, D);   ok &= p_alloc(&t->down_b[l], 1, D);
        ok &= p_alloc(&t->ln1_w[l], 1, D);    ok &= p_alloc(&t->ln1_b[l], 1, D);
        ok &= p_alloc(&t->ln2_w[l], 1, D);    ok &= p_alloc(&t->ln2_b[l], 1, D);
    }
    ok &= p_alloc(&t->lnf_w, 1, D); ok &= p_alloc(&t->lnf_b, 1, D);
    ok &= p_alloc(&t->head_w, D, V); ok &= p_alloc(&t->head_b, 1, V);

    /* caches */
    size_t TD = (size_t)B * D, TM = (size_t)B * M;
    t->x0    = (float*)calloc(TD, sizeof(float));
    t->xin   = (float*)calloc((size_t)L*TD, sizeof(float));
    t->ln1o  = (float*)calloc((size_t)L*TD, sizeof(float));
    t->ln2o  = (float*)calloc((size_t)L*TD, sizeof(float));
    t->ln1_mean = (float*)calloc((size_t)L*B, sizeof(float));
    t->ln1_rstd = (float*)calloc((size_t)L*B, sizeof(float));
    t->ln2_mean = (float*)calloc((size_t)L*B, sizeof(float));
    t->ln2_rstd = (float*)calloc((size_t)L*B, sizeof(float));
    t->qkv   = (float*)calloc((size_t)L*B*3*D, sizeof(float));
    t->probs = (float*)calloc((size_t)L*cfg->n_head*B*B, sizeof(float));
    t->cat   = (float*)calloc((size_t)L*TD, sizeof(float));
    t->xattn = (float*)calloc((size_t)L*TD, sizeof(float));
    t->mpre  = (float*)calloc((size_t)L*TM, sizeof(float));
    t->mpost = (float*)calloc((size_t)L*TM, sizeof(float));
    t->hfin  = (float*)calloc(D, sizeof(float));
    t->logits= (float*)calloc(V, sizeof(float));
    size_t effmax = (size_t)D * V;                    /* head is the biggest */
    if ((size_t)D*3*D > effmax) effmax = (size_t)D*3*D;
    if ((size_t)D*M   > effmax) effmax = (size_t)D*M;
    if ((size_t)V*D   > effmax) effmax = (size_t)V*D; /* tok_emb */
    t->eff  = (float*)calloc(effmax, sizeof(float));
    t->dx   = (float*)calloc(TD, sizeof(float));
    t->dtmp = (float*)calloc(TD > (size_t)B*3*D ? TD : (size_t)B*3*D, sizeof(float));
    t->dmid = (float*)calloc(TM, sizeof(float));
    t->dqkv = (float*)calloc((size_t)B*3*D, sizeof(float));
    t->dcat = (float*)calloc(TD, sizeof(float));
    ok &= (t->x0 && t->xin && t->ln1o && t->ln2o && t->ln1_mean && t->ln1_rstd &&
           t->ln2_mean && t->ln2_rstd && t->qkv && t->probs && t->cat && t->xattn &&
           t->mpre && t->mpost && t->hfin && t->logits && t->eff &&
           t->dx && t->dtmp && t->dmid && t->dqkv && t->dcat);
    if (!ok) { cce_supra_train_free(t); return NULL; }

    /* init: small uniform for matrices, ones/zeros for norms */
    float sc = 0.08f;
    for (size_t i = 0; i < (size_t)V*D; ++i) t->tok_emb.w[i] = sc * tr_rnd(t);
    for (size_t i = 0; i < (size_t)B*D; ++i) t->pos_emb.w[i] = sc * tr_rnd(t);
    for (int l = 0; l < L; ++l) {
        for (size_t i = 0; i < (size_t)D*3*D; ++i) t->qkv_w[l].w[i] = sc * tr_rnd(t);
        for (size_t i = 0; i < (size_t)D*D; ++i)   t->proj_w[l].w[i] = sc * tr_rnd(t);
        for (size_t i = 0; i < (size_t)D*M; ++i)   t->up_w[l].w[i] = sc * tr_rnd(t);
        for (size_t i = 0; i < (size_t)M*D; ++i)   t->down_w[l].w[i] = sc * tr_rnd(t);
        for (int i = 0; i < D; ++i) { t->ln1_w[l].w[i] = 1.0f; t->ln2_w[l].w[i] = 1.0f; }
    }
    for (int i = 0; i < D; ++i) t->lnf_w.w[i] = 1.0f;
    for (size_t i = 0; i < (size_t)D*V; ++i) t->head_w.w[i] = sc * tr_rnd(t);
    return t;
}

void cce_supra_train_free(cce_supra_train* t) {
    if (!t) return;
    int L = t->cfg.n_layer;
    p_free(&t->tok_emb); p_free(&t->pos_emb);
    for (int l = 0; l < L; ++l) {
        if (t->qkv_w)  p_free(&t->qkv_w[l]);
        if (t->qkv_b)  p_free(&t->qkv_b[l]);
        if (t->proj_w) p_free(&t->proj_w[l]);
        if (t->proj_b) p_free(&t->proj_b[l]);
        if (t->up_w)   p_free(&t->up_w[l]);
        if (t->up_b)   p_free(&t->up_b[l]);
        if (t->down_w) p_free(&t->down_w[l]);
        if (t->down_b) p_free(&t->down_b[l]);
        if (t->ln1_w)  p_free(&t->ln1_w[l]);
        if (t->ln1_b)  p_free(&t->ln1_b[l]);
        if (t->ln2_w)  p_free(&t->ln2_w[l]);
        if (t->ln2_b)  p_free(&t->ln2_b[l]);
    }
    free(t->qkv_w); free(t->qkv_b); free(t->proj_w); free(t->proj_b);
    free(t->up_w); free(t->up_b); free(t->down_w); free(t->down_b);
    free(t->ln1_w); free(t->ln1_b); free(t->ln2_w); free(t->ln2_b);
    p_free(&t->lnf_w); p_free(&t->lnf_b); p_free(&t->head_w); p_free(&t->head_b);
    free(t->x0); free(t->xin); free(t->ln1o); free(t->ln2o);
    free(t->ln1_mean); free(t->ln1_rstd); free(t->ln2_mean); free(t->ln2_rstd);
    free(t->qkv); free(t->probs); free(t->cat); free(t->xattn);
    free(t->mpre); free(t->mpost); free(t->hfin); free(t->logits);
    free(t->eff); free(t->dx); free(t->dtmp); free(t->dmid); free(t->dqkv); free(t->dcat);
    free(t);
}

/* ================= real-weight loader ================= */

/* Copy `in*out` FP values from src into the shadow of a trainer P, asserting the
   element count matches (STE moments/grad left at their fresh-zero state). */
static cce_result copy_into_P(P* p, const float* src, int in, int out) {
    if (!p || !src) return CCE_ERR_INVALID_ARG;
    if (p->in != in || p->out != out) return CCE_ERR_INVALID_ARG;
    memcpy(p->w, src, (size_t)in * out * sizeof(float));
    return CCE_OK;
}

/* The pure-linear projection for a named branch lives in the LAST block of its
   cascade (single-block LINEAR_HEAD cascades in the decomposed model), weights
   stored [in][out] row-major — same orientation as the trainer's P. */
static const cce_block* supra_last_block(cce_forest* f, const char* name) {
    cce_cascade* c = cce_forest_get_resident(f, name);
    if (!c || c->num_blocks < 1) return NULL;
    return &c->blocks[c->num_blocks - 1];
}

cce_result cce_supra_train_load_decomposed(cce_supra_train* t, void* decomposed_model) {
    if (!t || !decomposed_model) return CCE_ERR_INVALID_ARG;
    cce_supra_decomposed* m = (cce_supra_decomposed*)decomposed_model;
    const cce_supra_train_config* c = &t->cfg;
    int L = c->n_layer, D = c->n_embd, M = c->mlp_hidden, V = c->vocab, B = c->block_size;

    /* dims must line up exactly (direct memcpy, no reshape) */
    if (m->n_layer != L || m->n_embd != D || m->n_head != c->n_head ||
        m->vocab_size != V || m->block_size != B) return CCE_ERR_INVALID_ARG;
    if (!m->forest) return CCE_ERR_INVALID_ARG;

    /* mlp_hidden must match the real up-projection out width */
    {
        const cce_block* up0 = supra_last_block(m->forest, "gpt.block0.mlp_up");
        if (!up0 || up0->weights.ndim != 2 || up0->weights.shape[1] != M)
            return CCE_ERR_INVALID_ARG;
    }

    /* --- embeddings (tok_emb QAT-able, pos_emb frozen FP) --- */
    if (!m->tok_emb.data || m->tok_emb.numel != (size_t)V * D) return CCE_ERR_INVALID_ARG;
    if (!m->pos_emb.data || m->pos_emb.numel != (size_t)B * D) return CCE_ERR_INVALID_ARG;
    if (copy_into_P(&t->tok_emb, m->tok_emb.data, V, D) != CCE_OK) return CCE_ERR_INVALID_ARG;
    if (copy_into_P(&t->pos_emb, m->pos_emb.data, B, D) != CCE_OK) return CCE_ERR_INVALID_ARG;

    /* --- per-layer LayerNorm params + linear projections --- */
    for (int l = 0; l < L; ++l) {
        char name[128];
        /* LN1 / LN2 (weights + biases), each [D] -> P[1][D] */
        if (m->ln1_w[l].numel != (size_t)D || m->ln1_b[l].numel != (size_t)D ||
            m->ln2_w[l].numel != (size_t)D || m->ln2_b[l].numel != (size_t)D)
            return CCE_ERR_INVALID_ARG;
        if (copy_into_P(&t->ln1_w[l], m->ln1_w[l].data, 1, D) != CCE_OK) return CCE_ERR_INVALID_ARG;
        if (copy_into_P(&t->ln1_b[l], m->ln1_b[l].data, 1, D) != CCE_OK) return CCE_ERR_INVALID_ARG;
        if (copy_into_P(&t->ln2_w[l], m->ln2_w[l].data, 1, D) != CCE_OK) return CCE_ERR_INVALID_ARG;
        if (copy_into_P(&t->ln2_b[l], m->ln2_b[l].data, 1, D) != CCE_OK) return CCE_ERR_INVALID_ARG;

        /* fused QKV [D][3D] + bias [3D] */
        snprintf(name, sizeof(name), "gpt.block%d.qkv", l);
        const cce_block* b = supra_last_block(m->forest, name);
        if (!b || b->weights.ndim != 2 || !b->bias.data) return CCE_ERR_INVALID_ARG;
        if (copy_into_P(&t->qkv_w[l], b->weights.data, b->weights.shape[0], b->weights.shape[1]) != CCE_OK ||
            copy_into_P(&t->qkv_b[l], b->bias.data, 1, b->weights.shape[1]) != CCE_OK) return CCE_ERR_INVALID_ARG;

        /* attn output projection [D][D] + bias [D] */
        snprintf(name, sizeof(name), "gpt.block%d.attn_proj", l);
        b = supra_last_block(m->forest, name);
        if (!b || b->weights.ndim != 2 || !b->bias.data) return CCE_ERR_INVALID_ARG;
        if (copy_into_P(&t->proj_w[l], b->weights.data, b->weights.shape[0], b->weights.shape[1]) != CCE_OK ||
            copy_into_P(&t->proj_b[l], b->bias.data, 1, b->weights.shape[1]) != CCE_OK) return CCE_ERR_INVALID_ARG;

        /* MLP up [D][M] + bias [M] */
        snprintf(name, sizeof(name), "gpt.block%d.mlp_up", l);
        b = supra_last_block(m->forest, name);
        if (!b || b->weights.ndim != 2 || !b->bias.data) return CCE_ERR_INVALID_ARG;
        if (copy_into_P(&t->up_w[l], b->weights.data, b->weights.shape[0], b->weights.shape[1]) != CCE_OK ||
            copy_into_P(&t->up_b[l], b->bias.data, 1, b->weights.shape[1]) != CCE_OK) return CCE_ERR_INVALID_ARG;

        /* MLP down [M][D] + bias [D] */
        snprintf(name, sizeof(name), "gpt.block%d.mlp_down", l);
        b = supra_last_block(m->forest, name);
        if (!b || b->weights.ndim != 2 || !b->bias.data) return CCE_ERR_INVALID_ARG;
        if (copy_into_P(&t->down_w[l], b->weights.data, b->weights.shape[0], b->weights.shape[1]) != CCE_OK ||
            copy_into_P(&t->down_b[l], b->bias.data, 1, b->weights.shape[1]) != CCE_OK) return CCE_ERR_INVALID_ARG;
    }

    /* --- final LayerNorm --- */
    if (m->ln_f_w.numel != (size_t)D || m->ln_f_b.numel != (size_t)D) return CCE_ERR_INVALID_ARG;
    if (copy_into_P(&t->lnf_w, m->ln_f_w.data, 1, D) != CCE_OK) return CCE_ERR_INVALID_ARG;
    if (copy_into_P(&t->lnf_b, m->ln_f_b.data, 1, D) != CCE_OK) return CCE_ERR_INVALID_ARG;

    /* --- logits head [D][V] + bias [V] (borrowed via the public FP accessor) --- */
    {
        const float* hw = NULL; const float* hb = NULL; int hin = 0, hout = 0;
        if (cce_supra_head_fp(m, &hw, &hb, &hin, &hout) != CCE_OK) return CCE_ERR_INVALID_ARG;
        if (hin != D || hout != V || !hw) return CCE_ERR_INVALID_ARG;
        if (copy_into_P(&t->head_w, hw, D, V) != CCE_OK) return CCE_ERR_INVALID_ARG;
        if (hb) { if (copy_into_P(&t->head_b, hb, 1, V) != CCE_OK) return CCE_ERR_INVALID_ARG; }
        else    { memset(t->head_b.w, 0, (size_t)V * sizeof(float)); }
    }

    return CCE_OK;
}

void cce_supra_train_set_qat(cce_supra_train* t, int qkv, int proj, int mlp,
                             int head, int emb) {
    if (!t) return;
    t->cfg.qat_qkv = qkv; t->cfg.qat_proj = proj; t->cfg.qat_mlp = mlp;
    t->cfg.qat_head = head; t->cfg.qat_emb = emb;
}

/* ================= forward (with cache) ================= */

static cce_result tr_forward(cce_supra_train* t, const int* tokens, int T) {
    const cce_supra_train_config* c = &t->cfg;
    int L = c->n_layer, D = c->n_embd, H = c->n_head, hd = t->hd,
        M = c->mlp_hidden, V = c->vocab;
    if (T < 1 || T > c->block_size) return CCE_ERR_INVALID_ARG;
    t->T = T;

    /* embedding (QAT-able tok_emb rows + frozen FP pos rows) */
    for (int r = 0; r < T; ++r) {
        int tok = tokens[r];
        if (tok < 0 || tok >= V) return CCE_ERR_INVALID_ARG;
        float* xr = t->x0 + (size_t)r*D;
        if (c->qat_emb) {
            /* per-ROW absmean ternary lookup (BitNet embedding recipe) */
            const float* row = t->tok_emb.w + (size_t)tok*D;
            double s = 0.0;
            for (int i = 0; i < D; ++i) s += fabs((double)row[i]);
            float g = D ? (float)(s / D) : 0.0f;
            for (int i = 0; i < D; ++i) {
                int code = 0;
                if (g > 0.0f) { float rr = roundf(row[i]/g); if (rr>1)rr=1; if (rr<-1)rr=-1; code=(int)rr; }
                xr[i] = g * (float)code + t->pos_emb.w[(size_t)r*D + i];
            }
        } else {
            const float* row = t->tok_emb.w + (size_t)tok*D;
            for (int i = 0; i < D; ++i) xr[i] = row[i] + t->pos_emb.w[(size_t)r*D + i];
        }
    }

    float* x = t->dtmp;  /* reuse as the running activation [T][D] */
    memcpy(x, t->x0, (size_t)T*D*sizeof(float));

    for (int l = 0; l < L; ++l) {
        float* xin  = t->xin  + (size_t)l*c->block_size*D;
        float* ln1o = t->ln1o + (size_t)l*c->block_size*D;
        float* qkv  = t->qkv  + (size_t)l*c->block_size*3*D;
        float* cat  = t->cat  + (size_t)l*c->block_size*D;
        float* xatt = t->xattn+ (size_t)l*c->block_size*D;
        float* ln2o = t->ln2o + (size_t)l*c->block_size*D;
        float* mpre = t->mpre + (size_t)l*c->block_size*M;
        float* mpost= t->mpost+ (size_t)l*c->block_size*M;
        memcpy(xin, x, (size_t)T*D*sizeof(float));

        ln_fwd(xin, t->ln1_w[l].w, t->ln1_b[l].w, T, D, ln1o,
               t->ln1_mean + (size_t)l*c->block_size, t->ln1_rstd + (size_t)l*c->block_size);
        lin_fwd(ln1o, mat_eff(t, &t->qkv_w[l], c->qat_qkv), t->qkv_b[l].w, qkv, T, D, 3*D);

        /* attention per head */
        float scale = 1.0f / sqrtf((float)hd);
        for (int h = 0; h < H; ++h) {
            float* Pm = t->probs + (((size_t)l*H + h)*c->block_size)*c->block_size;
            for (int i = 0; i < T; ++i) {
                const float* qi = qkv + (size_t)i*3*D + h*hd;
                /* scores row (causal) with stable softmax */
                double maxv = -1e30;
                float srow[1024];  /* block_size cap for the gate models */
                for (int j = 0; j <= i; ++j) {
                    const float* kj = qkv + (size_t)j*3*D + D + h*hd;
                    double s = 0.0;
                    for (int d = 0; d < hd; ++d) s += (double)qi[d] * kj[d];
                    s *= scale;
                    srow[j] = (float)s;
                    if (s > maxv) maxv = s;
                }
                double sum = 0.0;
                for (int j = 0; j <= i; ++j) { srow[j] = expf(srow[j] - (float)maxv); sum += srow[j]; }
                float inv = (float)(1.0 / sum);
                float* prow = Pm + (size_t)i*c->block_size;
                for (int j = 0; j <= i; ++j) prow[j] = srow[j] * inv;
                for (int j = i+1; j < T; ++j) prow[j] = 0.0f;
                /* out row for this head into cat */
                float* orow = cat + (size_t)i*D + h*hd;
                for (int d = 0; d < hd; ++d) {
                    double s = 0.0;
                    for (int j = 0; j <= i; ++j)
                        s += (double)prow[j] * qkv[(size_t)j*3*D + 2*D + h*hd + d];
                    orow[d] = (float)s;
                }
            }
        }

        /* proj + residual */
        lin_fwd(cat, mat_eff(t, &t->proj_w[l], c->qat_proj), t->proj_b[l].w, xatt, T, D, D);
        for (size_t i = 0; i < (size_t)T*D; ++i) xatt[i] += xin[i];

        /* LN2 + MLP + residual */
        ln_fwd(xatt, t->ln2_w[l].w, t->ln2_b[l].w, T, D, ln2o,
               t->ln2_mean + (size_t)l*c->block_size, t->ln2_rstd + (size_t)l*c->block_size);
        lin_fwd(ln2o, mat_eff(t, &t->up_w[l], c->qat_mlp), t->up_b[l].w, mpre, T, D, M);
        for (size_t i = 0; i < (size_t)T*M; ++i) mpost[i] = gelu_f(mpre[i]);
        lin_fwd(mpost, mat_eff(t, &t->down_w[l], c->qat_mlp), t->down_b[l].w, x, T, M, D);
        for (size_t i = 0; i < (size_t)T*D; ++i) x[i] += xatt[i];
    }

    /* final LN on the LAST row + head */
    {
        const float* xl = x + (size_t)(T-1)*D;
        float mu_f, rs_f;
        ln_fwd(xl, t->lnf_w.w, t->lnf_b.w, 1, D, t->hfin, &mu_f, &rs_f);
        t->lnf_mean = mu_f; t->lnf_rstd = rs_f;
        lin_fwd(t->hfin, mat_eff(t, &t->head_w, c->qat_head), t->head_b.w,
                t->logits, 1, D, V);
        /* keep the final x for the backward: stash it in x0? no — store in dcat
           temporarily is fragile; keep in xin cache of a virtual layer instead.
           Simplest: copy to t->dx (it is scratch until backward runs). */
        memcpy(t->dx, x, (size_t)T*D*sizeof(float));
    }
    return CCE_OK;
}

cce_result cce_supra_train_logits(cce_supra_train* t, const int* tokens, int T,
                                  float* logits) {
    if (!t || !tokens || !logits) return CCE_ERR_INVALID_ARG;
    cce_result rc = tr_forward(t, tokens, T);
    if (rc != CCE_OK) return rc;
    memcpy(logits, t->logits, (size_t)t->cfg.vocab * sizeof(float));
    return rc;
}

/* ================= backward ================= */

/* zero all grads */
static void tr_zero_grads(cce_supra_train* t) {
    int L = t->cfg.n_layer, D = t->cfg.n_embd, M = t->cfg.mlp_hidden,
        V = t->cfg.vocab;
    memset(t->tok_emb.g, 0, (size_t)V*D*sizeof(float));
    for (int l = 0; l < L; ++l) {
        memset(t->qkv_w[l].g, 0, (size_t)D*3*D*sizeof(float));
        memset(t->qkv_b[l].g, 0, (size_t)3*D*sizeof(float));
        memset(t->proj_w[l].g, 0, (size_t)D*D*sizeof(float));
        memset(t->proj_b[l].g, 0, (size_t)D*sizeof(float));
        memset(t->up_w[l].g, 0, (size_t)D*M*sizeof(float));
        memset(t->up_b[l].g, 0, (size_t)M*sizeof(float));
        memset(t->down_w[l].g, 0, (size_t)M*D*sizeof(float));
        memset(t->down_b[l].g, 0, (size_t)D*sizeof(float));
        memset(t->ln1_w[l].g, 0, (size_t)D*sizeof(float));
        memset(t->ln1_b[l].g, 0, (size_t)D*sizeof(float));
        memset(t->ln2_w[l].g, 0, (size_t)D*sizeof(float));
        memset(t->ln2_b[l].g, 0, (size_t)D*sizeof(float));
    }
    memset(t->lnf_w.g, 0, (size_t)D*sizeof(float));
    memset(t->lnf_b.g, 0, (size_t)D*sizeof(float));
    memset(t->head_w.g, 0, (size_t)D*V*sizeof(float));
    memset(t->head_b.g, 0, (size_t)V*sizeof(float));
}

/* full backward from dlogits[V] at the last position. Consumes the caches of
   the immediately preceding tr_forward. tokens needed for the embedding scatter. */
static void tr_backward(cce_supra_train* t, const int* tokens, const float* dlogits) {
    const cce_supra_train_config* c = &t->cfg;
    int L = c->n_layer, D = c->n_embd, H = c->n_head, hd = t->hd,
        M = c->mlp_hidden, V = c->vocab, T = t->T;

    /* dx currently holds the FINAL x [T][D] (stashed by forward). Move it out. */
    float* xfinal = t->dcat;               /* borrow: dcat is per-layer scratch */
    memcpy(xfinal, t->dx, (size_t)T*D*sizeof(float));
    memset(t->dx, 0, (size_t)T*D*sizeof(float));

    /* head backward (last row only) */
    {
        float dh[4096];                    /* D cap for gate models */
        lin_bwd(t->hfin, mat_eff(t, &t->head_w, c->qat_head), dlogits,
                t->head_w.g, t->head_b.g, dh, 1, D, V);
        /* final LN backward on the last row */
        float dxl[4096];
        ln_bwd(xfinal + (size_t)(T-1)*D, t->lnf_w.w, dh,
               &t->lnf_mean, &t->lnf_rstd, 1, D, dxl, t->lnf_w.g, t->lnf_b.g);
        for (int i = 0; i < D; ++i) t->dx[(size_t)(T-1)*D + i] = dxl[i];
    }

    /* blocks in reverse */
    for (int l = L - 1; l >= 0; --l) {
        float* xin  = t->xin  + (size_t)l*c->block_size*D;
        float* ln1o = t->ln1o + (size_t)l*c->block_size*D;
        float* qkv  = t->qkv  + (size_t)l*c->block_size*3*D;
        float* cat  = t->cat  + (size_t)l*c->block_size*D;
        float* xatt = t->xattn+ (size_t)l*c->block_size*D;
        float* ln2o = t->ln2o + (size_t)l*c->block_size*D;
        float* mpre = t->mpre + (size_t)l*c->block_size*M;
        float* mpost= t->mpost+ (size_t)l*c->block_size*M;

        /* ---- MLP residual: dx flows to xatt AND through down/gelu/up/LN2 ---- */
        /* d(mpost) = dx·down_wᵀ ; d(down) += mpostᵀ·dx */
        lin_bwd(mpost, mat_eff(t, &t->down_w[l], c->qat_mlp), t->dx,
                t->down_w[l].g, t->down_b[l].g, t->dmid, T, M, D);
        for (size_t i = 0; i < (size_t)T*M; ++i) t->dmid[i] *= gelu_df(mpre[i]);
        /* d(ln2o) via up ; d(up) += ln2oᵀ·dmid */
        lin_bwd(ln2o, mat_eff(t, &t->up_w[l], c->qat_mlp), t->dmid,
                t->up_w[l].g, t->up_b[l].g, t->dtmp, T, D, M);
        /* dxatt = dx (residual) + LN2-bwd(dtmp) */
        ln_bwd(xatt, t->ln2_w[l].w, t->dtmp,
               t->ln2_mean + (size_t)l*c->block_size, t->ln2_rstd + (size_t)l*c->block_size,
               T, D, t->dcat /* reuse as dxatt-partial */, t->ln2_w[l].g, t->ln2_b[l].g);
        for (size_t i = 0; i < (size_t)T*D; ++i) t->dx[i] += t->dcat[i];

        /* ---- attention residual: dx flows to xin AND through proj/attn/LN1 ---- */
        /* d(cat) via proj ; d(proj) += catᵀ·dx */
        lin_bwd(cat, mat_eff(t, &t->proj_w[l], c->qat_proj), t->dx,
                t->proj_w[l].g, t->proj_b[l].g, t->dcat, T, D, D);

        /* attention backward per head: dcat -> dqkv */
        memset(t->dqkv, 0, (size_t)T*3*D*sizeof(float));
        float scale = 1.0f / sqrtf((float)hd);
        for (int h = 0; h < H; ++h) {
            const float* Pm = t->probs + (((size_t)l*H + h)*c->block_size)*c->block_size;
            for (int i = 0; i < T; ++i) {
                const float* prow = Pm + (size_t)i*c->block_size;
                const float* doi  = t->dcat + (size_t)i*D + h*hd;   /* d(out row) */
                /* dP[j] = doi · v_j ; dV_j += P[j]·doi */
                float dprow[1024];
                for (int j = 0; j <= i; ++j) {
                    const float* vj = qkv + (size_t)j*3*D + 2*D + h*hd;
                    float* dvj = t->dqkv + (size_t)j*3*D + 2*D + h*hd;
                    double s = 0.0;
                    for (int d = 0; d < hd; ++d) {
                        s += (double)doi[d] * vj[d];
                        dvj[d] += prow[j] * doi[d];
                    }
                    dprow[j] = (float)s;
                }
                /* softmax bwd: dS = P∘(dP − Σ dP∘P) */
                double dot = 0.0;
                for (int j = 0; j <= i; ++j) dot += (double)dprow[j] * prow[j];
                /* dq_i += Σ_j dS_ij·scale·k_j ; dk_j += dS_ij·scale·q_i */
                const float* qi = qkv + (size_t)i*3*D + h*hd;
                float* dqi = t->dqkv + (size_t)i*3*D + h*hd;
                for (int j = 0; j <= i; ++j) {
                    float ds = prow[j] * (dprow[j] - (float)dot) * scale;
                    const float* kj = qkv + (size_t)j*3*D + D + h*hd;
                    float* dkj = t->dqkv + (size_t)j*3*D + D + h*hd;
                    for (int d = 0; d < hd; ++d) {
                        dqi[d] += ds * kj[d];
                        dkj[d] += ds * qi[d];
                    }
                }
            }
        }

        /* d(ln1o) via qkv ; d(qkv_w) += ln1oᵀ·dqkv */
        lin_bwd(ln1o, mat_eff(t, &t->qkv_w[l], c->qat_qkv), t->dqkv,
                t->qkv_w[l].g, t->qkv_b[l].g, t->dtmp, T, D, 3*D);
        /* dxin = dx (residual) + LN1-bwd(dtmp) */
        ln_bwd(xin, t->ln1_w[l].w, t->dtmp,
               t->ln1_mean + (size_t)l*c->block_size, t->ln1_rstd + (size_t)l*c->block_size,
               T, D, t->dcat, t->ln1_w[l].g, t->ln1_b[l].g);
        for (size_t i = 0; i < (size_t)T*D; ++i) t->dx[i] += t->dcat[i];
    }

    /* embedding scatter (STE for the ternary lookup: identity into the shadow
       row; pos_emb frozen by design) */
    for (int r = 0; r < T; ++r) {
        int tok = tokens[r];
        float* g = t->tok_emb.g + (size_t)tok*D;
        const float* dxr = t->dx + (size_t)r*D;
        for (int i = 0; i < D; ++i) g[i] += dxr[i];
    }
}

/* ---- Adam ---- */
static void adam_p(P* p, float lr, int step) {
    const float b1 = 0.9f, b2 = 0.999f, eps = 1e-8f;
    float c1 = 1.0f - powf(b1, (float)step), c2 = 1.0f - powf(b2, (float)step);
    size_t n = (size_t)p->in * p->out;
    for (size_t i = 0; i < n; ++i) {
        float g = p->g[i];
        p->m[i] = b1 * p->m[i] + (1 - b1) * g;
        p->v[i] = b2 * p->v[i] + (1 - b2) * g * g;
        float mh = p->m[i] / c1, vh = p->v[i] / c2;
        p->w[i] -= lr * mh / (sqrtf(vh) + eps);
    }
}

double cce_supra_train_step(cce_supra_train* t, const int* tokens, int T,
                            const float* teacher_probs, int target, float lr) {
    if (!t || !tokens) return -1.0;
    if (tr_forward(t, tokens, T) != CCE_OK) return -1.0;
    int V = t->cfg.vocab;

    /* softmax + loss (double) */
    double maxv = t->logits[0];
    for (int i = 1; i < V; ++i) if (t->logits[i] > maxv) maxv = t->logits[i];
    double sum = 0.0;
    static float pbuf_static[65536];
    float* pr = (V <= 65536) ? pbuf_static : (float*)malloc((size_t)V*sizeof(float));
    for (int i = 0; i < V; ++i) { pr[i] = (float)exp((double)t->logits[i] - maxv); sum += pr[i]; }
    double loss = 0.0;
    for (int i = 0; i < V; ++i) pr[i] = (float)(pr[i] / sum);
    if (teacher_probs) {
        for (int i = 0; i < V; ++i) {
            double q = teacher_probs[i];
            if (q > 0.0) loss -= q * log((double)pr[i] > 1e-30 ? (double)pr[i] : 1e-30);
        }
    } else {
        if (target < 0 || target >= V) { if (pr != pbuf_static) free(pr); return -1.0; }
        loss = -log((double)pr[target] > 1e-30 ? (double)pr[target] : 1e-30);
    }

    /* dlogits = p − q */
    float* dl = pr;   /* overwrite in place */
    if (teacher_probs) { for (int i = 0; i < V; ++i) dl[i] = pr[i] - teacher_probs[i]; }
    else               { dl[target] -= 1.0f; }

    tr_zero_grads(t);
    tr_backward(t, tokens, dl);
    if (pr != pbuf_static) free(pr);

    /* Adam on every trainable group (pos_emb frozen by design) */
    int step = ++t->adam_t;
    adam_p(&t->tok_emb, lr, step);
    for (int l = 0; l < t->cfg.n_layer; ++l) {
        adam_p(&t->qkv_w[l], lr, step);  adam_p(&t->qkv_b[l], lr, step);
        adam_p(&t->proj_w[l], lr, step); adam_p(&t->proj_b[l], lr, step);
        adam_p(&t->up_w[l], lr, step);   adam_p(&t->up_b[l], lr, step);
        adam_p(&t->down_w[l], lr, step); adam_p(&t->down_b[l], lr, step);
        adam_p(&t->ln1_w[l], lr, step);  adam_p(&t->ln1_b[l], lr, step);
        adam_p(&t->ln2_w[l], lr, step);  adam_p(&t->ln2_b[l], lr, step);
    }
    adam_p(&t->lnf_w, lr, step); adam_p(&t->lnf_b, lr, step);
    adam_p(&t->head_w, lr, step); adam_p(&t->head_b, lr, step);
    return loss;
}

/* ================= gradcheck ================= */

static double gc_loss(cce_supra_train* t, const int* tokens, int T, int target) {
    if (tr_forward(t, tokens, T) != CCE_OK) return 0.0;
    int V = t->cfg.vocab;
    double maxv = t->logits[0];
    for (int i = 1; i < V; ++i) if (t->logits[i] > maxv) maxv = t->logits[i];
    double sum = 0.0;
    for (int i = 0; i < V; ++i) sum += exp((double)t->logits[i] - maxv);
    return -((double)t->logits[target] - maxv - log(sum));
}

double cce_supra_train_gradcheck(cce_supra_train* t, const int* tokens, int T,
                                 int target, int n_samples) {
    if (!t) return 1e9;
    /* analytic grads */
    if (tr_forward(t, tokens, T) != CCE_OK) return 1e9;
    int V = t->cfg.vocab;
    float* dl = (float*)malloc((size_t)V*sizeof(float));
    double maxv = t->logits[0];
    for (int i = 1; i < V; ++i) if (t->logits[i] > maxv) maxv = t->logits[i];
    double sum = 0.0;
    for (int i = 0; i < V; ++i) { dl[i] = (float)exp((double)t->logits[i] - maxv); sum += dl[i]; }
    for (int i = 0; i < V; ++i) dl[i] = (float)(dl[i] / sum);
    dl[target] -= 1.0f;
    tr_zero_grads(t);
    tr_backward(t, tokens, dl);
    free(dl);

    /* numeric vs analytic over samples from EVERY group */
    P* groups[128];
    int ng = 0;
    groups[ng++] = &t->tok_emb;
    for (int l = 0; l < t->cfg.n_layer; ++l) {
        if (ng + 12 > 124) break;  /* keep room for the 4 tail groups */
        groups[ng++] = &t->qkv_w[l];  groups[ng++] = &t->qkv_b[l];
        groups[ng++] = &t->proj_w[l]; groups[ng++] = &t->proj_b[l];
        groups[ng++] = &t->up_w[l];   groups[ng++] = &t->up_b[l];
        groups[ng++] = &t->down_w[l]; groups[ng++] = &t->down_b[l];
        groups[ng++] = &t->ln1_w[l];  groups[ng++] = &t->ln1_b[l];
        groups[ng++] = &t->ln2_w[l];  groups[ng++] = &t->ln2_b[l];
    }
    groups[ng++] = &t->lnf_w; groups[ng++] = &t->lnf_b;
    groups[ng++] = &t->head_w; groups[ng++] = &t->head_b;

    (void)n_samples;
    double max_rel = 0.0;

    /* (a) DIRECTIONAL derivative over the WHOLE parameter vector: perturb
       every param by ±h·u (Rademacher u), compare (L+ − L−)/2h against g·u.
       The signal is the full gradient norm, so float32 forward noise cannot
       dominate the way it does for individual near-zero params. */
    {
        const double h = 1e-3;
        unsigned long long r = 0xD1CEBA5EULL;
        double gdotu = 0.0;
        for (int g = 0; g < ng; ++g) {
            P* p = groups[g];
            size_t n = (size_t)p->in * p->out;
            for (size_t i = 0; i < n; ++i) {
                r = r * 6364136223846793005ULL + 1442695040888963407ULL;
                float u = (r >> 33) & 1 ? 1.0f : -1.0f;
                gdotu += (double)p->g[i] * u;
                p->w[i] += (float)(h * u);
            }
        }
        double lp = gc_loss(t, tokens, T, target);
        r = 0xD1CEBA5EULL;
        for (int g = 0; g < ng; ++g) {
            P* p = groups[g];
            size_t n = (size_t)p->in * p->out;
            for (size_t i = 0; i < n; ++i) {
                r = r * 6364136223846793005ULL + 1442695040888963407ULL;
                float u = (r >> 33) & 1 ? 1.0f : -1.0f;
                p->w[i] -= (float)(2.0 * h * u);
            }
        }
        double lm = gc_loss(t, tokens, T, target);
        r = 0xD1CEBA5EULL;
        for (int g = 0; g < ng; ++g) {   /* restore */
            P* p = groups[g];
            size_t n = (size_t)p->in * p->out;
            for (size_t i = 0; i < n; ++i) {
                r = r * 6364136223846793005ULL + 1442695040888963407ULL;
                float u = (r >> 33) & 1 ? 1.0f : -1.0f;
                p->w[i] += (float)(h * u);
            }
        }
        double num = (lp - lm) / (2.0 * h);
        double rel = fabs(num - gdotu) / (fabs(num) + fabs(gdotu) + 1e-9);
        printf("    [gradcheck] directional: numeric %.6e vs analytic %.6e (rel %.3e)\n",
               num, gdotu, rel);
        if (rel > max_rel) max_rel = rel;
    }

    /* (b) per-group spot check at the LARGEST-|grad| param (max signal over
       the float32 noise floor), h=1e-2. */
    {
        const double h = 1e-2;
        for (int g = 0; g < ng; ++g) {
            P* p = groups[g];
            size_t n = (size_t)p->in * p->out, idx = 0;
            for (size_t i = 1; i < n; ++i)
                if (fabsf(p->g[i]) > fabsf(p->g[idx])) idx = i;
            if (fabsf(p->g[idx]) < 1e-6f) continue;  /* untouched group (e.g. unused rows) */
            float keep = p->w[idx];
            p->w[idx] = keep + (float)h;
            double lp = gc_loss(t, tokens, T, target);
            p->w[idx] = keep - (float)h;
            double lm = gc_loss(t, tokens, T, target);
            p->w[idx] = keep;
            double num = (lp - lm) / (2.0 * h);
            double ana = (double)p->g[idx];
            double rel = fabs(num - ana) / (fabs(num) + fabs(ana) + 1e-9);
            if (rel > 5e-3)
                printf("    [gradcheck] group %d ([%d][%d]) rel %.3e (num %.3e ana %.3e)\n",
                       g, p->in, p->out, rel, num, ana);
            if (rel > max_rel) max_rel = rel;
        }
    }

    /* restore caches for the caller (forward once more) */
    tr_forward(t, tokens, T);
    return max_rel;
}
