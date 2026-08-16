/* Supra QAT trainer — the transformer backward. See include/cce/cce_transformer_qat.h.
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

#include "../../include/cce/cce_transformer_qat.h"
#include "../../include/cce/cce_safetensors.h"   /* cce_supra_decomposed, head_fp, forest */

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

/* Parameter layout + trainer struct live in the internal header so the
   loader TU can see them without the core depending on $(CCE). */
#include "../../include/cce/cce_transformer_qat_internal.h"

int p_alloc(P* p, int in, int out) {
    size_t n = (size_t)in * out;
    p->w = (float*)calloc(n, sizeof(float));
    p->g = (float*)calloc(n, sizeof(float));
    p->m = (float*)calloc(n, sizeof(float));
    p->v = (float*)calloc(n, sizeof(float));
    p->in = in; p->out = out;
    return p->w && p->g && p->m && p->v;
}
void p_free(P* p) { free(p->w); free(p->g); free(p->m); free(p->v); }

/* ---- gradcheck group registry ----------------------------------------
   Registration is a side effect of allocation (the PA macro in create), so a
   parameter group cannot be added without becoming gradcheckable. */
void qat_group_reset(cce_transformer_qat* t) {
    if (!t) return;
    free(t->groups); free((void*)t->group_names); free(t->group_trainable);
    t->groups = NULL; t->group_names = NULL; t->group_trainable = NULL;
    t->n_groups = 0; t->cap_groups = 0;
}

int qat_group_add(cce_transformer_qat* t, P* p, const char* name, int trainable) {
    if (!t || !p) return 0;
    if (t->n_groups >= t->cap_groups) {
        int nc = t->cap_groups ? t->cap_groups * 2 : 32;
        P** ng = (P**)realloc(t->groups, (size_t)nc * sizeof *ng);
        const char** nn; int* nt;
        if (!ng) return 0;
        t->groups = ng;
        nn = (const char**)realloc((void*)t->group_names, (size_t)nc * sizeof *nn);
        if (!nn) return 0;
        t->group_names = nn;
        nt = (int*)realloc(t->group_trainable, (size_t)nc * sizeof *nt);
        if (!nt) return 0;
        t->group_trainable = nt;
        t->cap_groups = nc;
    }
    t->groups[t->n_groups] = p;
    t->group_names[t->n_groups] = name;
    t->group_trainable[t->n_groups] = trainable;
    t->n_groups++;
    return 1;
}

int cce_transformer_qat_trainable_count(const cce_transformer_qat* t) {
    int n = 0, i;
    if (!t) return 0;
    for (i = 0; i < t->n_groups; ++i) if (t->group_trainable[i]) ++n;
    return n;
}

int cce_transformer_qat_group_count(const cce_transformer_qat* t) {
    return t ? t->n_groups : 0;
}

int cce_transformer_qat_param_count(const cce_transformer_qat* t) {
    int n = 0, i;
    if (!t) return 0;
    for (i = 0; i < t->n_groups; ++i)
        n += t->groups[i]->in * t->groups[i]->out;
    return n;
}

/* ---- rng ---- */
static float tr_rnd(cce_transformer_qat* t) {
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
static const float* mat_eff(cce_transformer_qat* t, const P* p, int qat) {
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

/* ---- RoPE: rotary position embedding, NO parameters --------------------
   For pair index j at position p: freq = theta^(-2j/hd), ang = p*freq.
   half-split pairs (j, j+hd/2) -- the HF Llama/Qwen convention;
   interleaved pairs (2j, 2j+1). They are NOT interchangeable: a wrong
   pairing trains perfectly well and matches no reference implementation,
   which gradcheck cannot detect (any consistent rotation differentiates
   correctly). tests/test_qat_block.c section 7 pins it algebraically. */
static void rope_pair(int hd, int pairing, int j, int* a, int* b) {
    if (pairing == QAT_ROPE_INTERLEAVED) { *a = 2*j; *b = 2*j + 1; }
    else { *a = j; *b = j + hd/2; }
}

static void rope_angles(int hd, int pos, float theta, int j,
                        float* cs, float* sn) {
    double freq = pow((double)theta, -2.0 * (double)j / (double)hd);
    double ang = (double)pos * freq;
    *cs = (float)cos(ang);
    *sn = (float)sin(ang);
}

static void rope_apply(float* v, int hd, int pos, float theta, int pairing) {
    for (int j = 0; j < hd/2; ++j) {
        int a, b; float cs, sn, va, vb;
        rope_pair(hd, pairing, j, &a, &b);
        rope_angles(hd, pos, theta, j, &cs, &sn);
        va = v[a]; vb = v[b];
        v[a] = va*cs - vb*sn;
        v[b] = va*sn + vb*cs;
    }
}

/* The exact adjoint of rope_apply: rotation by -ang. */
static void rope_bwd(float* dv, int hd, int pos, float theta, int pairing) {
    for (int j = 0; j < hd/2; ++j) {
        int a, b; float cs, sn, da, db;
        rope_pair(hd, pairing, j, &a, &b);
        rope_angles(hd, pos, theta, j, &cs, &sn);
        da = dv[a]; db = dv[b];
        dv[a] =  da*cs + db*sn;
        dv[b] = -da*sn + db*cs;
    }
}

void cce_transformer_qat_rope_test(float* v, int hd, int pos, float theta,
                                   int pairing) {
    if (v && hd > 1) rope_apply(v, hd, pos, theta, pairing);
}

/* ---- RMSNorm: no mean subtraction, no bias -----------------------------
   y_i = x_i · r · w_i with r = 1/sqrt(mean(x²) + eps).
   Since ∂r/∂x_i = −r³·x_i/D, with g_i = dy_i·w_i:
       dx_i = r·(g_i − (r²·x_i/D)·Σ_j g_j·x_j)
   mean_out is written as 0: RMSNorm has no mean, but the cache slot exists
   and leaving it uninitialised would be read by nothing yet still be a trap. */
static void rms_fwd(const float* x, const float* w, int T, int D, float eps,
                    float* y, float* mean_out, float* rstd_out) {
    for (int r = 0; r < T; ++r) {
        const float* xr = x + (size_t)r*D;
        float* yr = y + (size_t)r*D;
        double ms = 0.0;
        for (int i = 0; i < D; ++i) ms += (double)xr[i] * xr[i];
        ms /= D;
        {
            float rs = (float)(1.0 / sqrt(ms + (double)eps));
            for (int i = 0; i < D; ++i) yr[i] = xr[i] * rs * w[i];
            if (mean_out) mean_out[r] = 0.0f;
            rstd_out[r] = rs;
        }
    }
}

static void rms_bwd(const float* x, const float* w, const float* dy,
                    const float* rstd, int T, int D,
                    float* dx, float* dw) {
    for (int r = 0; r < T; ++r) {
        const float* xr = x + (size_t)r*D;
        const float* dyr = dy + (size_t)r*D;
        float* dxr = dx + (size_t)r*D;
        float rs = rstd[r];
        double gx = 0.0;
        for (int i = 0; i < D; ++i) {
            double g = (double)dyr[i] * w[i];
            gx += g * xr[i];
            if (dw) dw[i] += dyr[i] * xr[i] * rs;
        }
        for (int i = 0; i < D; ++i) {
            double g = (double)dyr[i] * w[i];
            dxr[i] = (float)(rs * (g - ((double)rs * rs * xr[i] / (double)D) * gx));
        }
    }
}

/* Norm dispatch. LayerNorm keeps its hardcoded 1e-5 so the legacy
   bit-identity anchor cannot move; t->eps applies to RMSNorm only. */
static void norm_fwd(const cce_transformer_qat* t, const float* x, const float* w,
                     const float* b, int T, int D,
                     float* y, float* mean_out, float* rstd_out) {
    if (t->cfg.norm_kind == QAT_NORM_RMS)
        rms_fwd(x, w, T, D, t->eps, y, mean_out, rstd_out);
    else
        ln_fwd(x, w, b, T, D, y, mean_out, rstd_out);
}

static void norm_bwd(const cce_transformer_qat* t, const float* x, const float* w,
                     const float* dy, const float* mean, const float* rstd,
                     int T, int D, float* dx, float* dw, float* db) {
    if (t->cfg.norm_kind == QAT_NORM_RMS)
        rms_bwd(x, w, dy, rstd, T, D, dx, dw);
    else
        ln_bwd(x, w, dy, mean, rstd, T, D, dx, dw, db);
}

/* ---- SiLU / swish, for SwiGLU ---- */
static float silu_f(float x) {
    double sg = 1.0 / (1.0 + exp(-(double)x));
    return (float)((double)x * sg);
}
static float silu_df(float x) {
    double sg = 1.0 / (1.0 + exp(-(double)x));
    return (float)(sg * (1.0 + (double)x * (1.0 - sg)));
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

cce_transformer_qat* cce_transformer_qat_create(const cce_transformer_qat_config* cfg) {
    if (!cfg || cfg->n_head < 1 || cfg->n_embd % cfg->n_head) return NULL;
    /* fixed-size locals in forward/backward cap these (srow/dprow: block_size,
       dh/dxl: n_embd). Real Supra is 384/256; refuse beyond the caps. */
    if (cfg->block_size < 1 || cfg->block_size > 1024) return NULL;
    if (cfg->n_embd < 1 || cfg->n_embd > 4096) return NULL;
    if (cfg->n_layer < 1 || cfg->mlp_hidden < 1 || cfg->vocab < 2) return NULL;
    /* Modern-block selectors: REFUSE, never clamp. A silently-clamped config
       trains a model that is not the one that was asked for, and the weights
       would then be labelled as something they are not. */
    if (cfg->norm_kind < QAT_NORM_LN || cfg->norm_kind > QAT_NORM_RMS) return NULL;
    if (cfg->pos_kind  < QAT_POS_LEARNED || cfg->pos_kind > QAT_POS_ROPE) return NULL;
    if (cfg->mlp_kind  < QAT_MLP_GELU || cfg->mlp_kind > QAT_MLP_SWIGLU) return NULL;
    if (cfg->rope_pairing < QAT_ROPE_HALF ||
        cfg->rope_pairing > QAT_ROPE_INTERLEAVED) return NULL;
    if (cfg->n_kv_head < 0 || cfg->n_kv_head > cfg->n_head) return NULL;
    if (cfg->n_kv_head != 0 && cfg->n_head % cfg->n_kv_head != 0) return NULL;
    if (cfg->pos_kind == QAT_POS_ROPE && !(cfg->rope_theta > 0.0f)) return NULL;
    if (cfg->norm_eps < 0.0f) return NULL;
    cce_transformer_qat* t = (cce_transformer_qat*)calloc(1, sizeof(*t));
    if (!t) return NULL;
    t->cfg = *cfg;
    t->hd = cfg->n_embd / cfg->n_head;
    /* Derived, NOT written back into t->cfg: the config stays a faithful
       record of what was asked for, including the "0 means default" zeros. */
    t->kvh = cfg->n_kv_head ? cfg->n_kv_head : cfg->n_head;
    t->eps = cfg->norm_eps > 0.0f ? cfg->norm_eps : 1e-5f;
    t->rng = cfg->seed ? cfg->seed : 0x9E3779B97F4A7C15ULL;

    int L = cfg->n_layer, D = cfg->n_embd, M = cfg->mlp_hidden,
        V = cfg->vocab, B = cfg->block_size;
    int ok = 1;
    /* Allocate AND register in one call: the registry is what the gradcheck
       walks, so an unregistered group would be silently ungradchecked. */
#define PA(fld, in_, out_, nm) (ok &= (p_alloc(&(fld), (in_), (out_)) &&                                        qat_group_add(t, &(fld), (nm), 1)))
#define PA_FROZEN(fld, in_, out_, nm) (ok &= (p_alloc(&(fld), (in_), (out_)) &&                                               qat_group_add(t, &(fld), (nm), 0)))
    PA(t->tok_emb, V, D, "tok_emb");
    if (cfg->pos_kind == QAT_POS_LEARNED)     /* RoPE needs no learned table */
        PA_FROZEN(t->pos_emb, B, D, "pos_emb");   /* frozen FP by design */
    t->qkv_w  = (P*)calloc(L, sizeof(P)); t->qkv_b  = (P*)calloc(L, sizeof(P));
    t->proj_w = (P*)calloc(L, sizeof(P)); t->proj_b = (P*)calloc(L, sizeof(P));
    t->up_w   = (P*)calloc(L, sizeof(P)); t->up_b   = (P*)calloc(L, sizeof(P));
    t->down_w = (P*)calloc(L, sizeof(P)); t->down_b = (P*)calloc(L, sizeof(P));
    t->ln1_w  = (P*)calloc(L, sizeof(P)); t->ln1_b  = (P*)calloc(L, sizeof(P));
    t->ln2_w  = (P*)calloc(L, sizeof(P)); t->ln2_b  = (P*)calloc(L, sizeof(P));
    t->gate_w = (P*)calloc(L, sizeof(P)); t->gate_b = (P*)calloc(L, sizeof(P));
    if (!t->qkv_w || !t->qkv_b || !t->proj_w || !t->proj_b || !t->up_w || !t->up_b ||
        !t->down_w || !t->down_b || !t->ln1_w || !t->ln1_b || !t->ln2_w || !t->ln2_b) ok = 0;
    for (int l = 0; ok && l < L; ++l) {
        PA(t->qkv_w[l], D, 3*D, "qkv_w");  PA(t->qkv_b[l], 1, 3*D, "qkv_b");
        PA(t->proj_w[l], D, D, "proj_w");  PA(t->proj_b[l], 1, D, "proj_b");
        PA(t->up_w[l], D, M, "up_w");      PA(t->up_b[l], 1, M, "up_b");
        if (cfg->mlp_kind == QAT_MLP_SWIGLU) {
            PA(t->gate_w[l], D, M, "gate_w"); PA(t->gate_b[l], 1, M, "gate_b");
        }
        PA(t->down_w[l], M, D, "down_w");  PA(t->down_b[l], 1, D, "down_b");
        PA(t->ln1_w[l], 1, D, "ln1_w");
        PA(t->ln2_w[l], 1, D, "ln2_w");
        if (cfg->norm_kind == QAT_NORM_LN) {   /* RMSNorm has no bias */
            PA(t->ln1_b[l], 1, D, "ln1_b");
            PA(t->ln2_b[l], 1, D, "ln2_b");
        }
    }
    PA(t->lnf_w, 1, D, "lnf_w");
    if (cfg->norm_kind == QAT_NORM_LN) PA(t->lnf_b, 1, D, "lnf_b");
    PA(t->head_w, D, V, "head_w"); PA(t->head_b, 1, V, "head_b");
#undef PA
#undef PA_FROZEN

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
    if (cfg->mlp_kind == QAT_MLP_SWIGLU) {
        t->mgate = (float*)calloc((size_t)L*TM, sizeof(float));
        t->dgate = (float*)calloc((size_t)TM, sizeof(float));
        t->dxg   = (float*)calloc((size_t)cfg->block_size*D, sizeof(float));
        ok &= (t->mgate && t->dgate && t->dxg) ? 1 : 0;
    }
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
    if (!ok) { cce_transformer_qat_free(t); return NULL; }

    /* init: small uniform for matrices, ones/zeros for norms */
    float sc = 0.08f;
    for (size_t i = 0; i < (size_t)V*D; ++i) t->tok_emb.w[i] = sc * tr_rnd(t);
    if (t->pos_emb.w)
        for (size_t i = 0; i < (size_t)B*D; ++i) t->pos_emb.w[i] = sc * tr_rnd(t);
    for (int l = 0; l < L; ++l) {
        for (size_t i = 0; i < (size_t)D*3*D; ++i) t->qkv_w[l].w[i] = sc * tr_rnd(t);
        for (size_t i = 0; i < (size_t)D*D; ++i)   t->proj_w[l].w[i] = sc * tr_rnd(t);
        for (size_t i = 0; i < (size_t)D*M; ++i)   t->up_w[l].w[i] = sc * tr_rnd(t);
        if (t->gate_w[l].w)
            for (size_t i = 0; i < (size_t)D*M; ++i) t->gate_w[l].w[i] = sc * tr_rnd(t);
        for (size_t i = 0; i < (size_t)M*D; ++i)   t->down_w[l].w[i] = sc * tr_rnd(t);
        for (int i = 0; i < D; ++i) { t->ln1_w[l].w[i] = 1.0f; t->ln2_w[l].w[i] = 1.0f; }
    }
    for (int i = 0; i < D; ++i) t->lnf_w.w[i] = 1.0f;
    for (size_t i = 0; i < (size_t)D*V; ++i) t->head_w.w[i] = sc * tr_rnd(t);
    return t;
}

void cce_transformer_qat_free(cce_transformer_qat* t) {
    if (!t) return;
    int L = t->cfg.n_layer;
    qat_group_reset(t);
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
        if (t->gate_w) p_free(&t->gate_w[l]);
        if (t->gate_b) p_free(&t->gate_b[l]);
    }
    free(t->qkv_w); free(t->qkv_b); free(t->proj_w); free(t->proj_b);
    free(t->up_w); free(t->up_b); free(t->down_w); free(t->down_b);
    free(t->ln1_w); free(t->ln1_b); free(t->ln2_w); free(t->ln2_b);
    free(t->gate_w); free(t->gate_b);
    p_free(&t->lnf_w); p_free(&t->lnf_b); p_free(&t->head_w); p_free(&t->head_b);
    free(t->x0); free(t->xin); free(t->ln1o); free(t->ln2o);
    free(t->ln1_mean); free(t->ln1_rstd); free(t->ln2_mean); free(t->ln2_rstd);
    free(t->qkv); free(t->probs); free(t->cat); free(t->xattn);
    free(t->mgate); free(t->dgate); free(t->dxg);
    free(t->mpre); free(t->mpost); free(t->hfin); free(t->logits);
    free(t->eff); free(t->dx); free(t->dtmp); free(t->dmid); free(t->dqkv); free(t->dcat);
    free(t);
}

/* ================= real-weight loader ================= */

void cce_transformer_qat_set_qat(cce_transformer_qat* t, int qkv, int proj, int mlp,
                             int head, int emb) {
    if (!t) return;
    t->cfg.qat_qkv = qkv; t->cfg.qat_proj = proj; t->cfg.qat_mlp = mlp;
    t->cfg.qat_head = head; t->cfg.qat_emb = emb;
}

/* ================= forward (with cache) ================= */

static cce_result tr_forward(cce_transformer_qat* t, const int* tokens, int T) {
    const cce_transformer_qat_config* c = &t->cfg;
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
                xr[i] = g * (float)code +
                        (t->pos_emb.w ? t->pos_emb.w[(size_t)r*D + i] : 0.0f);
            }
        } else {
            const float* row = t->tok_emb.w + (size_t)tok*D;
            for (int i = 0; i < D; ++i)
                xr[i] = row[i] +
                        (t->pos_emb.w ? t->pos_emb.w[(size_t)r*D + i] : 0.0f);
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

        norm_fwd(t, xin, t->ln1_w[l].w, t->ln1_b[l].w, T, D, ln1o,
               t->ln1_mean + (size_t)l*c->block_size, t->ln1_rstd + (size_t)l*c->block_size);
        lin_fwd(ln1o, mat_eff(t, &t->qkv_w[l], c->qat_qkv), t->qkv_b[l].w, qkv, T, D, 3*D);

        /* RoPE rotates Q and K in place (never V), so the attention loop
           below is untouched by the position scheme. */
        if (c->pos_kind == QAT_POS_ROPE) {
            for (int r = 0; r < T; ++r)
                for (int h = 0; h < H; ++h) {
                    rope_apply(qkv + (size_t)r*3*D + h*hd, hd, r,
                               c->rope_theta, c->rope_pairing);
                    rope_apply(qkv + (size_t)r*3*D + D + h*hd, hd, r,
                               c->rope_theta, c->rope_pairing);
                }
        }

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
        norm_fwd(t, xatt, t->ln2_w[l].w, t->ln2_b[l].w, T, D, ln2o,
               t->ln2_mean + (size_t)l*c->block_size, t->ln2_rstd + (size_t)l*c->block_size);
        lin_fwd(ln2o, mat_eff(t, &t->up_w[l], c->qat_mlp), t->up_b[l].w, mpre, T, D, M);
        if (c->mlp_kind == QAT_MLP_SWIGLU) {
            /* h = SiLU(gate) * up */
            float* mgate = t->mgate + (size_t)l*c->block_size*M;
            lin_fwd(ln2o, mat_eff(t, &t->gate_w[l], c->qat_mlp), t->gate_b[l].w,
                    mgate, T, D, M);
            for (size_t i = 0; i < (size_t)T*M; ++i)
                mpost[i] = silu_f(mgate[i]) * mpre[i];
        } else {
            for (size_t i = 0; i < (size_t)T*M; ++i) mpost[i] = gelu_f(mpre[i]);
        }
        lin_fwd(mpost, mat_eff(t, &t->down_w[l], c->qat_mlp), t->down_b[l].w, x, T, M, D);
        for (size_t i = 0; i < (size_t)T*D; ++i) x[i] += xatt[i];
    }

    /* final LN on the LAST row + head */
    {
        const float* xl = x + (size_t)(T-1)*D;
        float mu_f, rs_f;
        norm_fwd(t, xl, t->lnf_w.w, t->lnf_b.w, 1, D, t->hfin, &mu_f, &rs_f);
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

cce_result cce_transformer_qat_logits(cce_transformer_qat* t, const int* tokens, int T,
                                  float* logits) {
    if (!t || !tokens || !logits) return CCE_ERR_INVALID_ARG;
    cce_result rc = tr_forward(t, tokens, T);
    if (rc != CCE_OK) return rc;
    memcpy(logits, t->logits, (size_t)t->cfg.vocab * sizeof(float));
    return rc;
}

/* ================= backward ================= */

/* zero all grads */
/* Registry-driven, like the gradcheck: a group that is not allocated under
   the active config (e.g. norm biases under RMSNorm) is simply not in the
   registry, so this cannot dereference it. The previous hardcoded list
   memset ln1_b[l].g unconditionally and segfaulted the moment a config
   stopped allocating it. */
static void tr_zero_grads(cce_transformer_qat* t) {
    for (int i = 0; i < t->n_groups; ++i) {
        P* p = t->groups[i];
        if (p->g) memset(p->g, 0, (size_t)p->in * p->out * sizeof(float));
    }
}

/* full backward from dlogits[V] at the last position. Consumes the caches of
   the immediately preceding tr_forward. tokens needed for the embedding scatter. */
static void tr_backward(cce_transformer_qat* t, const int* tokens, const float* dlogits) {
    const cce_transformer_qat_config* c = &t->cfg;
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
        norm_bwd(t, xfinal + (size_t)(T-1)*D, t->lnf_w.w, dh,
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
        if (c->mlp_kind == QAT_MLP_SWIGLU) {
            /* product rule across the two branches:
                 d(up)   = dmid * SiLU(gate)
                 d(gate) = dmid * up * SiLU'(gate)               */
            const float* mgate = t->mgate + (size_t)l*c->block_size*M;
            for (size_t i = 0; i < (size_t)T*M; ++i) {
                float dh = t->dmid[i];
                t->dgate[i] = dh * mpre[i] * silu_df(mgate[i]);
                t->dmid[i]  = dh * silu_f(mgate[i]);
            }
            lin_bwd(ln2o, mat_eff(t, &t->up_w[l], c->qat_mlp), t->dmid,
                    t->up_w[l].g, t->up_b[l].g, t->dtmp, T, D, M);
            lin_bwd(ln2o, mat_eff(t, &t->gate_w[l], c->qat_mlp), t->dgate,
                    t->gate_w[l].g, t->gate_b[l].g, t->dxg, T, D, M);
            /* lin_bwd ASSIGNS into its dx output, so the branches are summed
               here rather than accumulating in place. */
            for (size_t i = 0; i < (size_t)T*D; ++i) t->dtmp[i] += t->dxg[i];
        } else {
        for (size_t i = 0; i < (size_t)T*M; ++i) t->dmid[i] *= gelu_df(mpre[i]);
        /* d(ln2o) via up ; d(up) += ln2oᵀ·dmid */
        lin_bwd(ln2o, mat_eff(t, &t->up_w[l], c->qat_mlp), t->dmid,
                t->up_w[l].g, t->up_b[l].g, t->dtmp, T, D, M);
        }
        /* dxatt = dx (residual) + LN2-bwd(dtmp) */
        norm_bwd(t, xatt, t->ln2_w[l].w, t->dtmp,
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

        /* Adjoint of the forward rotation, applied at the mirror point: after
           attention backward has filled dQ/dK, before the qkv linear
           backward. dV is never rotated, matching forward. */
        if (c->pos_kind == QAT_POS_ROPE) {
            for (int r = 0; r < T; ++r)
                for (int h = 0; h < H; ++h) {
                    rope_bwd(t->dqkv + (size_t)r*3*D + h*hd, hd, r,
                             c->rope_theta, c->rope_pairing);
                    rope_bwd(t->dqkv + (size_t)r*3*D + D + h*hd, hd, r,
                             c->rope_theta, c->rope_pairing);
                }
        }

        /* d(ln1o) via qkv ; d(qkv_w) += ln1oᵀ·dqkv */
        lin_bwd(ln1o, mat_eff(t, &t->qkv_w[l], c->qat_qkv), t->dqkv,
                t->qkv_w[l].g, t->qkv_b[l].g, t->dtmp, T, D, 3*D);
        /* dxin = dx (residual) + LN1-bwd(dtmp) */
        norm_bwd(t, xin, t->ln1_w[l].w, t->dtmp,
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

double cce_transformer_qat_step(cce_transformer_qat* t, const int* tokens, int T,
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
    /* Registry-driven: trainable groups only, so pos_emb stays frozen and
       groups absent under the active config are never touched. */
    for (int gi = 0; gi < t->n_groups; ++gi)
        if (t->group_trainable[gi]) adam_p(t->groups[gi], lr, step);
    return loss;
}

/* ================= gradcheck ================= */

static double gc_loss(cce_transformer_qat* t, const int* tokens, int T, int target) {
    if (tr_forward(t, tokens, T) != CCE_OK) return 0.0;
    int V = t->cfg.vocab;
    double maxv = t->logits[0];
    for (int i = 1; i < V; ++i) if (t->logits[i] > maxv) maxv = t->logits[i];
    double sum = 0.0;
    for (int i = 0; i < V; ++i) sum += exp((double)t->logits[i] - maxv);
    return -((double)t->logits[target] - maxv - log(sum));
}

double cce_transformer_qat_gradcheck(cce_transformer_qat* t, const int* tokens, int T,
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
    /* Walk the REGISTRY, not a hand-built list. The old array was
       P* groups[128] with 'if (ng + 12 > 124) break;': any group not appended
       here was silently ungradchecked (pos_emb never was), and a 13th
       per-layer matrix would have truncated the tail without a word.
       Frozen-by-design groups are skipped — backward deliberately writes no
       gradient for them, so perturbing them would fail against a zero
       analytic contribution. */
    P* gbuf[512];
    int ng = 0;
    for (int gi = 0; gi < t->n_groups && ng < 512; ++gi)
        if (t->group_trainable[gi]) gbuf[ng++] = t->groups[gi];
    P** groups = gbuf;

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
