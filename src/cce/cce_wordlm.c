#include "../../include/cce/cce_wordlm.h"
#include "../../include/cce/cce_trit_lut.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include <stdint.h>

/* ---- a parameter array bundled with its Adam moments and grad buffer ---- */
typedef struct { float *w, *m, *v, *g; int n; } Param;

/* ---- BitNet b1.58 BitLinear helpers (per-row absmean ternary {-1,0,+1}) ----
   Used only when m->ternary is set. Training keeps FP "shadow" weights in
   Param.w; the forward uses the ternary projection, and the backward uses a
   straight-through estimator: the activation-gradient path uses the ternary
   weight, while the weight-gradient accumulates into the FP shadow as if the
   quantizer were the identity. */
static float wlm_absmean(const float* w, int n) {
    float s = 0.0f; for (int i = 0; i < n; i++) s += fabsf(w[i]);
    return n ? s / (float)n : 0.0f;
}
static float wlm_tern(float w, float gamma) {
    if (gamma <= 0.0f) return 0.0f;
    float r = roundf(w / gamma);
    if (r >  1.0f) r =  1.0f;
    if (r < -1.0f) r = -1.0f;
    return gamma * r;   /* effective weight = gamma * {-1,0,+1} */
}

/* Embedding lookup: copies a token's row into dst, optionally ternarized
   (per-row absmean) so the forward sees E_q[token] = gamma * {-1,0,+1}. STE makes
   the backward accumulate grads into the FP shadow E unchanged (see backward). */
static void wlm_embed_lookup(const float* erow, int d, int ternary, float* dst) {
    if (!ternary) { memcpy(dst, erow, (size_t)d * sizeof(float)); return; }
    float g = wlm_absmean(erow, d);
    for (int i = 0; i < d; i++) dst[i] = wlm_tern(erow[i], g);
}

static float frand_sym(float scale) { return ((float)rand() / (float)RAND_MAX * 2.0f - 1.0f) * scale; }

static int param_init(Param* p, int n, float scale) {
    p->n = n;
    p->w = (float*)calloc((size_t)n, sizeof(float));
    p->m = (float*)calloc((size_t)n, sizeof(float));
    p->v = (float*)calloc((size_t)n, sizeof(float));
    p->g = (float*)calloc((size_t)n, sizeof(float));
    if (!p->w || !p->m || !p->v || !p->g) return -1;
    for (int i = 0; i < n; i++) p->w[i] = (scale > 0.0f) ? frand_sym(scale) : 0.0f;
    return 0;
}
static void param_free(Param* p) { free(p->w); free(p->m); free(p->v); free(p->g); memset(p, 0, sizeof(*p)); }
static void param_zero_grad(Param* p) { memset(p->g, 0, (size_t)p->n * sizeof(float)); }
static void param_adam(Param* p, float lr, float bc1, float bc2) {
    const float b1 = 0.9f, b2 = 0.999f, eps = 1e-8f;
    for (int i = 0; i < p->n; i++) {
        float g = p->g[i];
        p->m[i] = b1 * p->m[i] + (1.0f - b1) * g;
        p->v[i] = b2 * p->v[i] + (1.0f - b2) * g * g;
        float mhat = p->m[i] / bc1;
        float vhat = p->v[i] / bc2;
        p->w[i] -= lr * mhat / (sqrtf(vhat) + eps);
    }
}

struct cce_wordlm {
    int V, d, ctx, hid, C, S;   /* S = ceil(V/C) class size */
    Param E;    /* [V*d]  tied embedding (shared across ctx slots) */
    Param W1;   /* [hid*(ctx*d)] bottleneck */
    Param b1;   /* [hid] */
    Param Wc;   /* [C*hid] class head */
    Param bc;   /* [C] */
    Param Ww;   /* [V*hid] within-class word head (indexed by global word id) */
    Param bw;   /* [V] */
    /* scratch (forward caches reused by backward) */
    float *x;   /* [ctx*d] */
    float *h;   /* [hid] tanh activations */
    float *Lc, *Pc;   /* [C] */
    float *Lw, *Pw;   /* [S] within-class */
    float *dh, *dhp, *gx;
    int t;      /* Adam timestep */
    int ternary;       /* 1 = BitLinear ternary on W1/Wc/Ww (FP shadow + STE) */
    int ternary_embed; /* 1 = ternary lookup on E (FP shadow + STE), independent */
};

static int classes_for(int V) { int c = (int)ceilf(sqrtf((float)(V > 1 ? V : 1))); return c < 1 ? 1 : c; }

long cce_wordlm_param_count(int V, int d, int ctx, int hid) {
    int C = classes_for(V);
    long p = 0;
    p += (long)V * d;            /* E */
    p += (long)hid * ctx * d;    /* W1 */
    p += hid;                    /* b1 */
    p += (long)C * hid;          /* Wc */
    p += C;                      /* bc */
    p += (long)V * hid;          /* Ww */
    p += V;                      /* bw */
    return p;
}

cce_wordlm* cce_wordlm_create(int V, int d, int ctx, int hid, unsigned seed) {
    if (V < 2 || d < 1 || ctx < 1 || hid < 1) return NULL;
    srand(seed);
    cce_wordlm* m = (cce_wordlm*)calloc(1, sizeof(cce_wordlm));
    if (!m) return NULL;
    m->V = V; m->d = d; m->ctx = ctx; m->hid = hid;
    m->C = classes_for(V);
    m->S = (V + m->C - 1) / m->C;

    float s1 = 1.0f / sqrtf((float)(ctx * d));
    float sh = 1.0f / sqrtf((float)hid);
    int ok = 0;
    ok |= param_init(&m->E,  V * d,        0.1f);
    ok |= param_init(&m->W1, hid * ctx * d, s1);
    ok |= param_init(&m->b1, hid,           0.0f);
    ok |= param_init(&m->Wc, m->C * hid,    sh);
    ok |= param_init(&m->bc, m->C,          0.0f);
    ok |= param_init(&m->Ww, V * hid,       sh);
    ok |= param_init(&m->bw, V,             0.0f);
    m->x  = (float*)calloc((size_t)ctx * d, sizeof(float));
    m->h  = (float*)calloc((size_t)hid, sizeof(float));
    m->Lc = (float*)calloc((size_t)m->C, sizeof(float));
    m->Pc = (float*)calloc((size_t)m->C, sizeof(float));
    m->Lw = (float*)calloc((size_t)m->S, sizeof(float));
    m->Pw = (float*)calloc((size_t)m->S, sizeof(float));
    m->dh = (float*)calloc((size_t)hid, sizeof(float));
    m->dhp= (float*)calloc((size_t)hid, sizeof(float));
    m->gx = (float*)calloc((size_t)ctx * d, sizeof(float));
    if (ok || !m->x || !m->h || !m->Lc || !m->Pc || !m->Lw || !m->Pw || !m->dh || !m->dhp || !m->gx) {
        cce_wordlm_free(m);
        return NULL;
    }
    return m;
}

void cce_wordlm_free(cce_wordlm* m) {
    if (!m) return;
    param_free(&m->E); param_free(&m->W1); param_free(&m->b1);
    param_free(&m->Wc); param_free(&m->bc); param_free(&m->Ww); param_free(&m->bw);
    free(m->x); free(m->h); free(m->Lc); free(m->Pc); free(m->Lw); free(m->Pw);
    free(m->dh); free(m->dhp); free(m->gx);
    free(m);
}

int cce_wordlm_vocab(const cce_wordlm* m)   { return m ? m->V : 0; }
int cce_wordlm_classes(const cce_wordlm* m) { return m ? m->C : 0; }

/* Enable/disable BitNet b1.58 BitLinear (ternary W1/Wc/Ww with FP shadow + STE).
   Set BEFORE training for QAT; set on a trained FP model for post-hoc ternary. */
void cce_wordlm_set_ternary(cce_wordlm* m, int on) { if (m) m->ternary = on ? 1 : 0; }
int  cce_wordlm_ternary(const cce_wordlm* m) { return m ? m->ternary : 0; }

/* Independent BitNet ternary on the input embedding E (FP shadow + STE). */
void cce_wordlm_set_ternary_embed(cce_wordlm* m, int on) { if (m) m->ternary_embed = on ? 1 : 0; }
int  cce_wordlm_ternary_embed(const cce_wordlm* m) { return m ? m->ternary_embed : 0; }

/* Forward to the hidden layer: fills m->x and m->h from the context words. */
static void forward_hidden(cce_wordlm* m, const int* ctx_words) {
    int d = m->d, ctxd = m->ctx * m->d, hid = m->hid;
    for (int k = 0; k < m->ctx; k++) {
        int w = ctx_words[k];
        if (w < 0 || w >= m->V) memset(m->x + (size_t)k*d, 0, d*sizeof(float));
        else wlm_embed_lookup(m->E.w + (size_t)w*d, d, m->ternary_embed, m->x + (size_t)k*d);
    }
    for (int j = 0; j < hid; j++) {
        const float* row = m->W1.w + (size_t)j*ctxd;
        float g = m->ternary ? wlm_absmean(row, ctxd) : 0.0f;
        double s = m->b1.w[j];
        for (int i = 0; i < ctxd; i++)
            s += (double)(m->ternary ? wlm_tern(row[i], g) : row[i]) * m->x[i];
        m->h[j] = tanhf((float)s);
    }
}

static void softmax_into(const float* logit, int n, float* out) {
    float mx = -1e30f;
    for (int i = 0; i < n; i++) if (logit[i] > mx) mx = logit[i];
    double sum = 0.0;
    for (int i = 0; i < n; i++) { out[i] = expf(logit[i] - mx); sum += out[i]; }
    float inv = (float)(sum > 0 ? 1.0 / sum : 1.0);
    for (int i = 0; i < n; i++) out[i] *= inv;
}

/* class range [cstart,cend) for class c */
static void class_range(const cce_wordlm* m, int c, int* cstart, int* cend) {
    int s = c * m->S;
    int e = s + m->S;
    if (e > m->V) e = m->V;
    *cstart = s; *cend = e;
}

double cce_wordlm_nll(cce_wordlm* m, const int* ctx_words, int target) {
    if (!m || target < 0 || target >= m->V) return 0.0;
    forward_hidden(m, ctx_words);
    int hid = m->hid;
    for (int c = 0; c < m->C; c++) {
        const float* row = m->Wc.w + (size_t)c*hid;
        float g = m->ternary ? wlm_absmean(row, hid) : 0.0f;
        double s = m->bc.w[c];
        for (int j = 0; j < hid; j++) s += (double)(m->ternary ? wlm_tern(row[j], g) : row[j]) * m->h[j];
        m->Lc[c] = (float)s;
    }
    softmax_into(m->Lc, m->C, m->Pc);
    int tc = target / m->S, cs, ce;
    class_range(m, tc, &cs, &ce);
    int cn = ce - cs;
    for (int i = 0; i < cn; i++) {
        int w = cs + i;
        const float* row = m->Ww.w + (size_t)w*hid;
        float g = m->ternary ? wlm_absmean(row, hid) : 0.0f;
        double s = m->bw.w[w];
        for (int j = 0; j < hid; j++) s += (double)(m->ternary ? wlm_tern(row[j], g) : row[j]) * m->h[j];
        m->Lw[i] = (float)s;
    }
    softmax_into(m->Lw, cn, m->Pw);
    double pc = m->Pc[tc] > 1e-30 ? m->Pc[tc] : 1e-30;
    double pw = m->Pw[target - cs] > 1e-30 ? m->Pw[target - cs] : 1e-30;
    return -log(pc) - log(pw);
}

/* Forward + exact backprop; fills .g buffers (zeroed first). Returns CE loss.
   Does NOT update parameters. */
static double backward(cce_wordlm* m, const int* ctx_words, int target) {
    int hid = m->hid, d = m->d, ctxd = m->ctx * m->d;
    double loss = cce_wordlm_nll(m, ctx_words, target);  /* fills h, Pc, Pw */

    param_zero_grad(&m->E); param_zero_grad(&m->W1); param_zero_grad(&m->b1);
    param_zero_grad(&m->Wc); param_zero_grad(&m->bc); param_zero_grad(&m->Ww); param_zero_grad(&m->bw);

    int tc = target / m->S, cs, ce;
    class_range(m, tc, &cs, &ce);
    int cn = ce - cs;

    for (int j = 0; j < hid; j++) m->dh[j] = 0.0f;

    /* class head: dLc = Pc - onehot(tc) */
    for (int c = 0; c < m->C; c++) {
        float g = m->Pc[c] - (c == tc ? 1.0f : 0.0f);
        m->bc.g[c] += g;
        float* gw = m->Wc.g + (size_t)c*hid;
        const float* w = m->Wc.w + (size_t)c*hid;
        float gamma = m->ternary ? wlm_absmean(w, hid) : 0.0f;
        for (int j = 0; j < hid; j++) {
            gw[j] += g * m->h[j];                                   /* STE: grad -> FP shadow */
            m->dh[j] += g * (m->ternary ? wlm_tern(w[j], gamma) : w[j]); /* activation grad uses ternary */
        }
    }
    /* within-class word head (only the target's class): dLw = Pw - onehot(target) */
    for (int i = 0; i < cn; i++) {
        int wid = cs + i;
        float g = m->Pw[i] - (wid == target ? 1.0f : 0.0f);
        m->bw.g[wid] += g;
        float* gw = m->Ww.g + (size_t)wid*hid;
        const float* w = m->Ww.w + (size_t)wid*hid;
        float gamma = m->ternary ? wlm_absmean(w, hid) : 0.0f;
        for (int j = 0; j < hid; j++) {
            gw[j] += g * m->h[j];
            m->dh[j] += g * (m->ternary ? wlm_tern(w[j], gamma) : w[j]);
        }
    }
    /* through tanh */
    for (int j = 0; j < hid; j++) m->dhp[j] = m->dh[j] * (1.0f - m->h[j] * m->h[j]);
    /* W1, b1, and grad wrt x */
    for (int i = 0; i < ctxd; i++) m->gx[i] = 0.0f;
    for (int j = 0; j < hid; j++) {
        float gj = m->dhp[j];
        m->b1.g[j] += gj;
        float* gw = m->W1.g + (size_t)j*ctxd;
        const float* w = m->W1.w + (size_t)j*ctxd;
        float gamma = m->ternary ? wlm_absmean(w, ctxd) : 0.0f;
        for (int i = 0; i < ctxd; i++) {
            gw[i] += gj * m->x[i];                                       /* STE: grad -> FP shadow */
            m->gx[i] += gj * (m->ternary ? wlm_tern(w[i], gamma) : w[i]); /* activation grad uses ternary */
        }
    }
    /* tied embedding: scatter gx slots back to the words' rows (accumulate) */
    for (int k = 0; k < m->ctx; k++) {
        int w = ctx_words[k];
        if (w < 0 || w >= m->V) continue;
        float* ge = m->E.g + (size_t)w*d;
        const float* gxk = m->gx + (size_t)k*d;
        for (int mi = 0; mi < d; mi++) ge[mi] += gxk[mi];
    }
    return loss;
}

double cce_wordlm_train_step(cce_wordlm* m, const int* ctx_words, int target, float lr) {
    if (!m || target < 0 || target >= m->V) return 0.0;
    double loss = backward(m, ctx_words, target);
    /* shared Adam timestep + bias correction (computed once per step) */
    m->t++;
    float bc1 = 1.0f - powf(0.9f,   (float)m->t);
    float bc2 = 1.0f - powf(0.999f, (float)m->t);
    param_adam(&m->E,  lr, bc1, bc2);
    param_adam(&m->W1, lr, bc1, bc2);
    param_adam(&m->b1, lr, bc1, bc2);
    param_adam(&m->Wc, lr, bc1, bc2);
    param_adam(&m->bc, lr, bc1, bc2);
    param_adam(&m->Ww, lr, bc1, bc2);
    param_adam(&m->bw, lr, bc1, bc2);
    return loss;
}

/* Greedy hierarchical decode: pick best class, then best word within it.
   penalty (NULL ok) is added to within-class word logits. Both steps O(sqrt(V)). */
int cce_wordlm_predict(cce_wordlm* m, const int* ctx_words, const float* penalty) {
    if (!m) return 0;
    forward_hidden(m, ctx_words);
    int hid = m->hid;
    for (int c = 0; c < m->C; c++) {
        const float* row = m->Wc.w + (size_t)c*hid;
        float g = m->ternary ? wlm_absmean(row, hid) : 0.0f;
        double s = m->bc.w[c];
        for (int j = 0; j < hid; j++) s += (double)(m->ternary ? wlm_tern(row[j], g) : row[j]) * m->h[j];
        m->Lc[c] = (float)s;
    }
    int cbest = 0; float lbest = -1e30f;
    for (int c = 0; c < m->C; c++) if (m->Lc[c] > lbest) { lbest = m->Lc[c]; cbest = c; }
    int cs, ce; class_range(m, cbest, &cs, &ce);
    int wbest = cs; float wlbest = -1e30f;
    for (int w = cs; w < ce; w++) {
        const float* row = m->Ww.w + (size_t)w*hid;
        float g = m->ternary ? wlm_absmean(row, hid) : 0.0f;
        double s = m->bw.w[w];
        for (int j = 0; j < hid; j++) s += (double)(m->ternary ? wlm_tern(row[j], g) : row[j]) * m->h[j];
        float v = (float)s + (penalty ? penalty[w] : 0.0f);
        if (v > wlbest) { wlbest = v; wbest = w; }
    }
    return wbest;
}

/* Finite-difference gradient check over a sample of parameters. */
double cce_wordlm_gradcheck(cce_wordlm* m, const int* ctx_words, int target) {
    if (!m || target < 0 || target >= m->V) return -1.0;
    backward(m, ctx_words, target);  /* fills .g with analytic grads */
    const float eps = 1e-2f;
    Param* ps[] = { &m->E, &m->W1, &m->b1, &m->Wc, &m->bc, &m->Ww, &m->bw };
    int np = (int)(sizeof(ps)/sizeof(ps[0]));
    double max_rel = 0.0;
    for (int pi = 0; pi < np; pi++) {
        Param* p = ps[pi];
        /* sample a few indices spread across the array */
        for (int s = 0; s < 5; s++) {
            int idx = (int)(((long)(s + 1) * 2654435761u) % (unsigned)p->n);
            float saved = p->w[idx];
            p->w[idx] = saved + eps; double lp = cce_wordlm_nll(m, ctx_words, target);
            p->w[idx] = saved - eps; double lm = cce_wordlm_nll(m, ctx_words, target);
            p->w[idx] = saved;
            double num = (lp - lm) / (2.0 * eps);
            double ana = p->g[idx];
            double denom = fabs(num) + fabs(ana) + 1e-8;
            double rel = fabs(num - ana) / denom;
            if (rel > max_rel) max_rel = rel;
        }
    }
    /* nll above left scratch in a perturbed-consistent state; restore caches */
    cce_wordlm_nll(m, ctx_words, target);
    return max_rel;
}

/* ===================== Packed 1.6-bit ternary export / inference =====================
   Base-3 layout: ternary code -1/0/+1 -> trit 0/1/2; one byte holds 5 trits
   (3^5 = 243 <= 256) => 8/5 = 1.6 bits per weight. Weights are stored per row,
   each row padded up to ceil(n/5) bytes; each row also carries its absmean scale.
   The inference path never materializes FP shadow weights — it reads the packed
   trits directly (wlm_trit_dot), and is bit-identical to the QAT ternary forward
   because the effective weight gamma*code is formed per element in the same order. */

static int wlm_bytes_per_row(int n) { return (n + 4) / 5; }

static void wlm_pack_row(const float* row, int n, float gamma, uint8_t* out) {
    int i = 0;
    while (i < n) {
        int b = 0, mul = 1;
        for (int k = 0; k < 5; k++) {
            int code = 0;
            if (i < n) {
                if (gamma > 0.0f) { float r = roundf(row[i] / gamma); if (r > 1) r = 1; if (r < -1) r = -1; code = (int)r; }
                i++;
            }
            b += (code + 1) * mul;   /* trit = code+1 in {0,1,2} */
            mul *= 3;
        }
        *out++ = (uint8_t)b;
    }
}

/* sum_init + Sum_i (gamma*code_i)*vec[i] — matches the QAT forward exactly.
   Decode via cce_trit_lut (same values, same accumulation order as the old
   div/mod chain -> bit-identical), which drops the serial byte dependency. */
static double wlm_trit_dot(const uint8_t* packed, float gamma, const float* vec, int n, double init) {
    double s = init; int i = 0; const uint8_t* p = packed;
    while (i < n) {
        const int8_t* c5 = cce_trit_lut[*p++];
        for (int k = 0; k < 5 && i < n; k++, i++) {
            float eff = gamma * (float)c5[k];
            s += (double)eff * vec[i];
        }
    }
    return s;
}

/* Unpack a packed ternary row into dst[i] = scale * code_i (== wlm_tern output). */
static void wlm_trit_unpack_scaled(const uint8_t* packed, float scale, int n, float* dst) {
    int i = 0; const uint8_t* p = packed;
    while (i < n) {
        const int8_t* c5 = cce_trit_lut[*p++];
        for (int k = 0; k < 5 && i < n; k++, i++) dst[i] = scale * (float)c5[k];
    }
}

struct cce_wordlm_packed {
    int V, d, ctx, hid, C, S;
    int embed_packed;                  /* 1 = E stored as trits (Ep/Es), else FP (E) */
    float *E, *b1, *bc, *bw;            /* FP embedding (if !embed_packed) + biases */
    uint8_t *Ep;  float *Es;  int E_bpr;/* packed ternary embedding (if embed_packed) */
    uint8_t *W1p, *Wcp, *Wwp;          /* packed trits, per row  */
    float   *W1s, *Wcs, *Wws;          /* per-row absmean scales */
    int W1_bpr, Wc_bpr, Ww_bpr;        /* bytes per row          */
    float *x, *h, *Lc, *Pc, *Lw, *Pw;  /* scratch                */
};

#define WLM_TRIT_MAGIC 0x544D4C57u  /* 'W','L','M','T' */

static void wlm_write_ternary_mat(FILE* f, const float* w, int rows, int n) {
    int bpr = wlm_bytes_per_row(n);
    uint8_t* buf = (uint8_t*)malloc((size_t)bpr);
    for (int r = 0; r < rows; r++) {
        const float* row = w + (size_t)r * n;
        float g = wlm_absmean(row, n);
        fwrite(&g, sizeof(float), 1, f);
        wlm_pack_row(row, n, g, buf);
        fwrite(buf, 1, (size_t)bpr, f);
    }
    free(buf);
}

int cce_wordlm_export_trits(const cce_wordlm* m, const char* path) {
    if (!m || !path) return -1;
    FILE* f = fopen(path, "wb");
    if (!f) return -1;
    int ctxd = m->ctx * m->d;
    uint32_t magic = WLM_TRIT_MAGIC, ver = 2;
    int hdr[6] = { m->V, m->d, m->ctx, m->hid, m->C, m->S };
    int embed_packed = m->ternary_embed ? 1 : 0;
    fwrite(&magic, 4, 1, f); fwrite(&ver, 4, 1, f);
    fwrite(hdr, sizeof(int), 6, f);
    fwrite(&embed_packed, sizeof(int), 1, f);
    if (embed_packed) wlm_write_ternary_mat(f, m->E.w, m->V, m->d);  /* packed ternary E */
    else fwrite(m->E.w, sizeof(float), (size_t)m->V * m->d, f);      /* FP embedding */
    fwrite(m->b1.w, sizeof(float), (size_t)m->hid, f);
    fwrite(m->bc.w, sizeof(float), (size_t)m->C, f);
    fwrite(m->bw.w, sizeof(float), (size_t)m->V, f);
    wlm_write_ternary_mat(f, m->W1.w, m->hid, ctxd);          /* packed ternary */
    wlm_write_ternary_mat(f, m->Wc.w, m->C,   m->hid);
    wlm_write_ternary_mat(f, m->Ww.w, m->V,   m->hid);
    fclose(f);
    return 0;
}

static int wlm_read_ternary_mat(FILE* f, uint8_t** packed, float** scales, int rows, int n) {
    int bpr = wlm_bytes_per_row(n);
    *scales = (float*)malloc(sizeof(float) * (size_t)rows);
    *packed = (uint8_t*)malloc((size_t)rows * bpr);
    if (!*scales || !*packed) return -1;
    for (int r = 0; r < rows; r++) {
        if (fread(&(*scales)[r], sizeof(float), 1, f) != 1) return -1;
        if (fread(*packed + (size_t)r * bpr, 1, (size_t)bpr, f) != (size_t)bpr) return -1;
    }
    return 0;
}

cce_wordlm_packed* cce_wordlm_packed_load(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    uint32_t magic = 0, ver = 0;
    if (fread(&magic, 4, 1, f) != 1 || magic != WLM_TRIT_MAGIC) { fclose(f); return NULL; }
    if (fread(&ver, 4, 1, f) != 1) { fclose(f); return NULL; }
    int hdr[6];
    if (fread(hdr, sizeof(int), 6, f) != 6) { fclose(f); return NULL; }
    cce_wordlm_packed* p = (cce_wordlm_packed*)calloc(1, sizeof(*p));
    if (!p) { fclose(f); return NULL; }
    p->V = hdr[0]; p->d = hdr[1]; p->ctx = hdr[2]; p->hid = hdr[3]; p->C = hdr[4]; p->S = hdr[5];
    int ctxd = p->ctx * p->d;
    int embed_packed = 0;
    if (ver >= 2) { if (fread(&embed_packed, sizeof(int), 1, f) != 1) { fclose(f); cce_wordlm_packed_free(p); return NULL; } }
    p->embed_packed = embed_packed;
    p->b1 = (float*)malloc(sizeof(float) * (size_t)p->hid);
    p->bc = (float*)malloc(sizeof(float) * (size_t)p->C);
    p->bw = (float*)malloc(sizeof(float) * (size_t)p->V);
    int ok = (p->b1 && p->bc && p->bw);
    if (ok) {                              /* E first in the file (packed or FP) */
        if (embed_packed) {
            p->E_bpr = wlm_bytes_per_row(p->d);
            ok = wlm_read_ternary_mat(f, &p->Ep, &p->Es, p->V, p->d) == 0;
        } else {
            p->E = (float*)malloc(sizeof(float) * (size_t)p->V * p->d);
            ok = p->E && fread(p->E, sizeof(float), (size_t)p->V*p->d, f) == (size_t)p->V*p->d;
        }
    }
    ok = ok
      && fread(p->b1, sizeof(float), (size_t)p->hid, f) == (size_t)p->hid
      && fread(p->bc, sizeof(float), (size_t)p->C, f)   == (size_t)p->C
      && fread(p->bw, sizeof(float), (size_t)p->V, f)   == (size_t)p->V
      && wlm_read_ternary_mat(f, &p->W1p, &p->W1s, p->hid, ctxd)  == 0
      && wlm_read_ternary_mat(f, &p->Wcp, &p->Wcs, p->C,   p->hid) == 0
      && wlm_read_ternary_mat(f, &p->Wwp, &p->Wws, p->V,   p->hid) == 0;
    fclose(f);
    p->W1_bpr = wlm_bytes_per_row(ctxd); p->Wc_bpr = wlm_bytes_per_row(p->hid); p->Ww_bpr = wlm_bytes_per_row(p->hid);
    p->x  = (float*)calloc((size_t)ctxd, sizeof(float));
    p->h  = (float*)calloc((size_t)p->hid, sizeof(float));
    p->Lc = (float*)calloc((size_t)p->C, sizeof(float));
    p->Pc = (float*)calloc((size_t)p->C, sizeof(float));
    p->Lw = (float*)calloc((size_t)p->S, sizeof(float));
    p->Pw = (float*)calloc((size_t)p->S, sizeof(float));
    if (!ok || !p->x || !p->h || !p->Lc || !p->Pc || !p->Lw || !p->Pw) { cce_wordlm_packed_free(p); return NULL; }
    return p;
}

void cce_wordlm_packed_free(cce_wordlm_packed* p) {
    if (!p) return;
    free(p->E); free(p->Ep); free(p->Es);
    free(p->b1); free(p->bc); free(p->bw);
    free(p->W1p); free(p->Wcp); free(p->Wwp);
    free(p->W1s); free(p->Wcs); free(p->Wws);
    free(p->x); free(p->h); free(p->Lc); free(p->Pc); free(p->Lw); free(p->Pw);
    free(p);
}

int cce_wordlm_packed_vocab(const cce_wordlm_packed* p) { return p ? p->V : 0; }

static void packed_forward_hidden(cce_wordlm_packed* p, const int* ctx_words) {
    int d = p->d, ctxd = p->ctx * p->d, hid = p->hid;
    for (int k = 0; k < p->ctx; k++) {
        int w = ctx_words[k];
        if (w < 0 || w >= p->V) memset(p->x + (size_t)k*d, 0, d*sizeof(float));
        else if (p->embed_packed) wlm_trit_unpack_scaled(p->Ep + (size_t)w*p->E_bpr, p->Es[w], d, p->x + (size_t)k*d);
        else memcpy(p->x + (size_t)k*d, p->E + (size_t)w*d, d*sizeof(float));
    }
    /* rows are independent (each h[j] is its own dot) -> thread-parallel with
       NO reassociation: per-row accumulation order is untouched, so the
       result stays bit-identical to the serial loop. Active only in builds
       with -fopenmp (wordlm targets); elsewhere the pragma is inert. */
    #pragma omp parallel for schedule(static) default(none) \
            shared(p, hid, ctxd) if(hid >= 128)
    for (int j = 0; j < hid; j++) {
        double s = wlm_trit_dot(p->W1p + (size_t)j*p->W1_bpr, p->W1s[j], p->x, ctxd, p->b1[j]);
        p->h[j] = tanhf((float)s);
    }
}

double cce_wordlm_packed_nll(cce_wordlm_packed* p, const int* ctx_words, int target) {
    if (!p || target < 0 || target >= p->V) return 0.0;
    packed_forward_hidden(p, ctx_words);
    int hid = p->hid;
    for (int c = 0; c < p->C; c++)
        p->Lc[c] = (float)wlm_trit_dot(p->Wcp + (size_t)c*p->Wc_bpr, p->Wcs[c], p->h, hid, p->bc[c]);
    softmax_into(p->Lc, p->C, p->Pc);
    int tc = target / p->S, cs = tc * p->S, ce = cs + p->S; if (ce > p->V) ce = p->V;
    int cn = ce - cs;
    for (int i = 0; i < cn; i++) {
        int w = cs + i;
        p->Lw[i] = (float)wlm_trit_dot(p->Wwp + (size_t)w*p->Ww_bpr, p->Wws[w], p->h, hid, p->bw[w]);
    }
    softmax_into(p->Lw, cn, p->Pw);
    double pc = p->Pc[tc] > 1e-30 ? p->Pc[tc] : 1e-30;
    double pw = p->Pw[target - cs] > 1e-30 ? p->Pw[target - cs] : 1e-30;
    return -log(pc) - log(pw);
}

int cce_wordlm_packed_predict(cce_wordlm_packed* p, const int* ctx_words, const float* penalty) {
    if (!p) return 0;
    packed_forward_hidden(p, ctx_words);
    int hid = p->hid;
    for (int c = 0; c < p->C; c++)
        p->Lc[c] = (float)wlm_trit_dot(p->Wcp + (size_t)c*p->Wc_bpr, p->Wcs[c], p->h, hid, p->bc[c]);
    int cbest = 0; float lbest = -1e30f;
    for (int c = 0; c < p->C; c++) if (p->Lc[c] > lbest) { lbest = p->Lc[c]; cbest = c; }
    int cs = cbest * p->S, ce = cs + p->S; if (ce > p->V) ce = p->V;
    int wbest = cs; float wlbest = -1e30f;
    for (int w = cs; w < ce; w++) {
        float v = (float)wlm_trit_dot(p->Wwp + (size_t)w*p->Ww_bpr, p->Wws[w], p->h, hid, p->bw[w])
                + (penalty ? penalty[w] : 0.0f);
        if (v > wlbest) { wlbest = v; wbest = w; }
    }
    return wbest;
}

