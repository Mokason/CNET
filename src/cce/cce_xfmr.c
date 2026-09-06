/* CNET-native transformer: capsule-shaped linears + optional amdmath GEMM.
 * Residual logits only. Not CERT. Not cce_transformer_qat.
 */
#include "cce/cce_xfmr.h"

#ifdef CCE_XFMR_CPU_ONLY
static int cce_amdmath_below_floor(size_t a, size_t b, size_t c)
{
    (void)a;
    (void)b;
    (void)c;
    return 1;
}
static int cce_amdmath_linear_f32(cce_amdmath *h, const float *X, const float *W, float *Y,
                                  size_t E, size_t N, size_t in_dim, size_t out_dim)
{
    (void)h;
    (void)X;
    (void)W;
    (void)Y;
    (void)E;
    (void)N;
    (void)in_dim;
    (void)out_dim;
    return -1;
}
#else
#include "cce/cce_amdmath.h"
#endif

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    float *rms1, *wq, *wk, *wv, *wo;
    float *rms2, *w1, *w2;
} Layer;

struct cce_xfmr {
    cce_xfmr_config c;
    cce_amdmath *gpu;
    unsigned rng;
    int hidden;
    float *tok_emb; /* V*D */
    float *pos_emb; /* Tmax*D */
    Layer *L;
    float *rms_f;
    float *w_head; /* V*D */
    /* scratch */
    float *x;      /* T*D working */
    float *xb, *xc, *q, *k, *v, *att, *cat, *h, *logits;
    float *dx, *dxb, *datt;
    float *weff;
    int ternary;
};

static float rnd(cce_xfmr *m)
{
    m->rng = m->rng * 1664525u + 1013904223u;
    return ((int)(m->rng >> 16) % 2001 - 1000) / 50000.0f;
}

static float *falloc(size_t n)
{
    float *p = (float *)calloc(n, sizeof(float));
    return p;
}

static void fill_rand(cce_xfmr *m, float *p, int n)
{
    for (int i = 0; i < n; i++)
        p[i] = rnd(m);
}

static void cpu_linear(const float *X, const float *W, float *Y, int T, int in, int out)
{
    for (int t = 0; t < T; t++) {
        const float *x = X + (size_t)t * in;
        float *y = Y + (size_t)t * out;
        for (int o = 0; o < out; o++) {
            const float *w = W + (size_t)o * in;
            float a = 0.f;
            for (int i = 0; i < in; i++)
                a += x[i] * w[i];
            y[o] = a;
        }
    }
}

int cce_xfmr_pack_linear(const float *W, int out, int in, int8_t *codes, float *gamma)
{
    if (!W || !codes || !gamma || out < 1 || in < 1)
        return CCE_XFMR_ERR;
    for (int o = 0; o < out; o++) {
        const float *row = W + (size_t)o * in;
        double s = 0.0;
        for (int i = 0; i < in; i++)
            s += fabs((double)row[i]);
        float g = in ? (float)(s / (double)in) : 0.f;
        if (g <= 0.f)
            g = 1.f;
        gamma[o] = g;
        for (int i = 0; i < in; i++) {
            long c = lroundf(row[i] / g);
            if (c > 1)
                c = 1;
            if (c < -1)
                c = -1;
            codes[(size_t)o * in + i] = (int8_t)c;
        }
    }
    return CCE_XFMR_OK;
}

static void tern_materialize(const float *W, float *Weff, int out, int in)
{
    for (int o = 0; o < out; o++) {
        const float *row = W + (size_t)o * in;
        float *er = Weff + (size_t)o * in;
        double s = 0.0;
        for (int i = 0; i < in; i++)
            s += fabs((double)row[i]);
        float g = in ? (float)(s / (double)in) : 0.f;
        if (g <= 0.f)
            g = 1.f;
        for (int i = 0; i < in; i++) {
            float r = roundf(row[i] / g);
            if (r > 1.f)
                r = 1.f;
            if (r < -1.f)
                r = -1.f;
            er[i] = g * r;
        }
    }
}

static void linear(cce_xfmr *m, const float *X, const float *W, float *Y, int T, int in,
                   int out)
{
    const float *Ww = W;
    if (m->ternary && m->weff) {
        tern_materialize(W, m->weff, out, in);
        Ww = m->weff;
    }
    if (m->gpu && !cce_amdmath_below_floor((size_t)T, (size_t)out, (size_t)in)) {
        if (cce_amdmath_linear_f32(m->gpu, X, Ww, Y, 1, (size_t)T, (size_t)in,
                                   (size_t)out) == 0)
            return;
    }
    cpu_linear(X, Ww, Y, T, in, out);
}

static void rms(const float *x, const float *w, float *y, int T, int D, float eps)
{
    for (int t = 0; t < T; t++) {
        const float *xt = x + (size_t)t * D;
        float *yt = y + (size_t)t * D;
        float ss = 0.f;
        for (int i = 0; i < D; i++)
            ss += xt[i] * xt[i];
        float inv = 1.f / sqrtf(ss / (float)D + eps);
        for (int i = 0; i < D; i++)
            yt[i] = xt[i] * inv * w[i];
    }
}

static float gelu(float x)
{
    return 0.5f * x * (1.f + tanhf(0.79788456f * (x + 0.044715f * x * x * x)));
}

static void causal_attn(cce_xfmr *m, const float *qkv_src, int T)
{
    int D = m->c.n_embd, H = m->c.n_head, hd = D / H;
    float scale = 1.f / sqrtf((float)hd);
    /* q,k,v already in m->q, m->k, m->v as [T,D] */
    (void)qkv_src;
    memset(m->cat, 0, (size_t)T * D * sizeof(float));
    for (int h = 0; h < H; h++) {
        for (int t = 0; t < T; t++) {
            const float *qt = m->q + (size_t)t * D + h * hd;
            float maxs = -1e30f;
            for (int s = 0; s <= t; s++) {
                const float *ks = m->k + (size_t)s * D + h * hd;
                float dot = 0.f;
                for (int i = 0; i < hd; i++)
                    dot += qt[i] * ks[i];
                dot *= scale;
                m->att[(size_t)t * T + s] = dot;
                if (dot > maxs)
                    maxs = dot;
            }
            float sum = 0.f;
            for (int s = 0; s <= t; s++) {
                float e = expf(m->att[(size_t)t * T + s] - maxs);
                m->att[(size_t)t * T + s] = e;
                sum += e;
            }
            float inv = 1.f / (sum + 1e-9f);
            float *ot = m->cat + (size_t)t * D + h * hd;
            for (int i = 0; i < hd; i++)
                ot[i] = 0.f;
            for (int s = 0; s <= t; s++) {
                float a = m->att[(size_t)t * T + s] * inv;
                m->att[(size_t)t * T + s] = a;
                const float *vs = m->v + (size_t)s * D + h * hd;
                for (int i = 0; i < hd; i++)
                    ot[i] += a * vs[i];
            }
        }
    }
}

cce_xfmr *cce_xfmr_create(const cce_xfmr_config *cfg)
{
    if (!cfg || cfg->n_layer < 1 || cfg->n_embd < 8 || cfg->n_head < 1 ||
        cfg->n_embd % cfg->n_head || cfg->vocab < 2 || cfg->block_size < 2)
        return NULL;
    cce_xfmr *m = (cce_xfmr *)calloc(1, sizeof *m);
    if (!m)
        return NULL;
    m->c = *cfg;
    m->rng = cfg->seed ? cfg->seed : 1u;
    int D = cfg->n_embd, Tm = cfg->block_size, V = cfg->vocab, nL = cfg->n_layer;
    m->hidden = cfg->n_hidden > 0 ? cfg->n_hidden : 4 * D;
    int Hhid = m->hidden;
    m->tok_emb = falloc((size_t)V * D);
    m->pos_emb = falloc((size_t)Tm * D);
    m->rms_f = falloc(D);
    m->w_head = falloc((size_t)V * D);
    m->L = (Layer *)calloc((size_t)nL, sizeof(Layer));
    fill_rand(m, m->tok_emb, V * D);
    fill_rand(m, m->pos_emb, Tm * D);
    fill_rand(m, m->w_head, V * D);
    for (int i = 0; i < D; i++)
        m->rms_f[i] = 1.f;
    for (int l = 0; l < nL; l++) {
        Layer *L = &m->L[l];
        L->rms1 = falloc(D);
        L->rms2 = falloc(D);
        L->wq = falloc((size_t)D * D);
        L->wk = falloc((size_t)D * D);
        L->wv = falloc((size_t)D * D);
        L->wo = falloc((size_t)D * D);
        L->w1 = falloc((size_t)Hhid * D);
        L->w2 = falloc((size_t)D * Hhid);
        if (!L->w2) {
            cce_xfmr_free(m);
            return NULL;
        }
        for (int i = 0; i < D; i++) {
            L->rms1[i] = 1.f;
            L->rms2[i] = 1.f;
        }
        fill_rand(m, L->wq, D * D);
        fill_rand(m, L->wk, D * D);
        fill_rand(m, L->wv, D * D);
        fill_rand(m, L->wo, D * D);
        fill_rand(m, L->w1, Hhid * D);
        fill_rand(m, L->w2, D * Hhid);
    }
    size_t TD = (size_t)Tm * D, TT = (size_t)Tm * Tm;
    m->x = falloc(TD);
    m->xb = falloc(TD);
    m->xc = falloc(TD);
    m->q = falloc(TD);
    m->k = falloc(TD);
    m->v = falloc(TD);
    m->cat = falloc(TD);
    m->h = falloc((size_t)Tm * Hhid);
    m->att = falloc(TT);
    m->logits = falloc((size_t)Tm * V);
    m->dx = falloc(TD);
    m->dxb = falloc(TD);
    m->datt = falloc(TT);
    {
        size_t we = (size_t)Hhid * D;
        if ((size_t)V * D > we)
            we = (size_t)V * D;
        if ((size_t)D * D > we)
            we = (size_t)D * D;
        m->weff = falloc(we);
    }
    if (!m->logits || !m->dx || !m->weff) {
        cce_xfmr_free(m);
        return NULL;
    }
    return m;
}

void cce_xfmr_free(cce_xfmr *m)
{
    if (!m)
        return;
    for (int l = 0; l < m->c.n_layer; l++) {
        Layer *L = &m->L[l];
        free(L->rms1);
        free(L->rms2);
        free(L->wq);
        free(L->wk);
        free(L->wv);
        free(L->wo);
        free(L->w1);
        free(L->w2);
    }
    free(m->L);
    free(m->tok_emb);
    free(m->pos_emb);
    free(m->rms_f);
    free(m->w_head);
    free(m->x);
    free(m->xb);
    free(m->xc);
    free(m->q);
    free(m->k);
    free(m->v);
    free(m->cat);
    free(m->h);
    free(m->att);
    free(m->logits);
    free(m->dx);
    free(m->dxb);
    free(m->datt);
    free(m->weff);
    free(m);
}

void cce_xfmr_attach_gpu(cce_xfmr *m, cce_amdmath *gpu)
{
    if (m)
        m->gpu = gpu;
}

void cce_xfmr_set_ternary(cce_xfmr *m, int on)
{
    if (m)
        m->ternary = on ? 1 : 0;
}

int cce_xfmr_ternary(const cce_xfmr *m)
{
    return m ? m->ternary : 0;
}

const char *cce_xfmr_version(void)
{
    return CCE_XFMR_VERSION;
}

int cce_xfmr_param_count(const cce_xfmr *m)
{
    if (!m)
        return 0;
    int D = m->c.n_embd, V = m->c.vocab, Tm = m->c.block_size, nL = m->c.n_layer;
    int Hhid = m->hidden;
    return V * D + Tm * D + D + V * D +
           nL * (2 * D + 4 * D * D + Hhid * D + D * Hhid);
}

int cce_xfmr_head_shape(const cce_xfmr *m, int *vocab, int *d)
{
    if (!m)
        return CCE_XFMR_ERR;
    if (vocab)
        *vocab = m->c.vocab;
    if (d)
        *d = m->c.n_embd;
    return CCE_XFMR_OK;
}

int cce_xfmr_pack_head(const cce_xfmr *m, int8_t *codes, float *gamma)
{
    if (!m || !m->w_head)
        return CCE_XFMR_ERR;
    return cce_xfmr_pack_linear(m->w_head, m->c.vocab, m->c.n_embd, codes, gamma);
}

static int sanitize_unit(const char *in, char *out, size_t cap)
{
    size_t j = 0;
    if (!in || !out || cap < 2)
        return -1;
    for (size_t i = 0; in[i] && j + 1 < cap; i++) {
        char c = in[i];
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')
            out[j++] = c;
        else if (c >= 'A' && c <= 'Z')
            out[j++] = (char)(c - 'A' + 'a');
        else
            out[j++] = '_';
    }
    out[j] = 0;
    return j ? 0 : -1;
}

int cce_xfmr_propose(const cce_xfmr *m, const char *unit, const char *dir_out,
                     char *path_out, size_t path_cap)
{
    char un[64], path[512], tmp[640];
    if (!m || sanitize_unit(unit, un, sizeof un) != 0)
        return CCE_XFMR_ERR;
    if (!dir_out || !dir_out[0])
        dir_out = "var/capsule_inbox";
    if (mkdir(dir_out, 0755) != 0 && access(dir_out, W_OK) != 0)
        return CCE_XFMR_ERR;
    snprintf(path, sizeof path, "%s/%s-%ld", dir_out, un, (long)time(NULL));
    if (mkdir(path, 0755) != 0)
        return CCE_XFMR_ERR;

    int V = m->c.vocab, D = m->c.n_embd;
    int8_t *codes = (int8_t *)malloc((size_t)V * D);
    float *gamma = (float *)malloc((size_t)V * sizeof(float));
    if (!codes || !gamma) {
        free(codes);
        free(gamma);
        return CCE_XFMR_ERR;
    }
    if (cce_xfmr_pack_head(m, codes, gamma) != 0) {
        free(codes);
        free(gamma);
        return CCE_XFMR_ERR;
    }
    snprintf(tmp, sizeof tmp, "%s/head.codes", path);
    FILE *f = fopen(tmp, "wb");
    if (!f || fwrite(codes, 1, (size_t)V * D, f) != (size_t)V * D) {
        if (f)
            fclose(f);
        free(codes);
        free(gamma);
        return CCE_XFMR_ERR;
    }
    fclose(f);
    snprintf(tmp, sizeof tmp, "%s/head.gamma", path);
    f = fopen(tmp, "wb");
    if (!f || fwrite(gamma, sizeof(float), (size_t)V, f) != (size_t)V) {
        if (f)
            fclose(f);
        free(codes);
        free(gamma);
        return CCE_XFMR_ERR;
    }
    fclose(f);
    free(codes);
    free(gamma);

    snprintf(tmp, sizeof tmp, "%s/PROPOSE.json", path);
    f = fopen(tmp, "w");
    if (!f)
        return CCE_XFMR_ERR;
    fprintf(f,
            "{\n"
            "  \"kind\": \"capsule_propose\",\n"
            "  \"unit\": \"%s\",\n"
            "  \"source\": \"cce_xfmr\",\n"
            "  \"version\": \"%s\",\n"
            "  \"ternary\": %s,\n"
            "  \"head_vocab\": %d,\n"
            "  \"head_d\": %d,\n"
            "  \"params\": %d,\n"
            "  \"auto_cert\": false,\n"
            "  \"status\": \"pending_verify\",\n"
            "  \"export\": \"deferred\",\n"
            "  \"has_unit_cnb\": false,\n"
            "  \"law\": \"propose_only_never_self_cert\"\n"
            "}\n",
            un, CCE_XFMR_VERSION, m->ternary ? "true" : "false", V, D,
            cce_xfmr_param_count(m));
    fclose(f);
    if (path_out && path_cap)
        snprintf(path_out, path_cap, "%s", path);
    return CCE_XFMR_OK;
}

int cce_xfmr_forward(cce_xfmr *m, const int *toks, int T, float *logits)
{
    if (!m || !toks || T < 1 || T > m->c.block_size)
        return CCE_XFMR_ERR;
    int D = m->c.n_embd, V = m->c.vocab;
    for (int t = 0; t < T; t++) {
        int id = toks[t];
        if (id < 0 || id >= V)
            return CCE_XFMR_ERR;
        memcpy(m->x + (size_t)t * D, m->tok_emb + (size_t)id * D, (size_t)D * sizeof(float));
        const float *pe = m->pos_emb + (size_t)t * D;
        float *xt = m->x + (size_t)t * D;
        for (int i = 0; i < D; i++)
            xt[i] += pe[i];
    }
    int hid = m->hidden;
    for (int l = 0; l < m->c.n_layer; l++) {
        Layer *L = &m->L[l];
        rms(m->x, L->rms1, m->xb, T, D, 1e-5f);
        linear(m, m->xb, L->wq, m->q, T, D, D);
        linear(m, m->xb, L->wk, m->k, T, D, D);
        linear(m, m->xb, L->wv, m->v, T, D, D);
        causal_attn(m, m->xb, T);
        linear(m, m->cat, L->wo, m->xc, T, D, D);
        for (int i = 0; i < T * D; i++)
            m->x[i] += m->xc[i];
        rms(m->x, L->rms2, m->xb, T, D, 1e-5f);
        linear(m, m->xb, L->w1, m->h, T, D, hid);
        for (int i = 0; i < T * hid; i++)
            m->h[i] = gelu(m->h[i]);
        linear(m, m->h, L->w2, m->xc, T, hid, D);
        for (int i = 0; i < T * D; i++)
            m->x[i] += m->xc[i];
    }
    rms(m->x, m->rms_f, m->xb, T, D, 1e-5f);
    linear(m, m->xb, m->w_head, m->logits, T, D, V);
    if (logits)
        memcpy(logits, m->logits, (size_t)T * V * sizeof(float));
    return CCE_XFMR_OK;
}

static void sgd_outer(float *W, const float *X, const float *dY, int T, int in, int out,
                      float lr)
{
    /* W[out,in] -= lr * dY[t,out] * X[t,in]  (sum t) */
    for (int t = 0; t < T; t++) {
        const float *x = X + (size_t)t * in;
        const float *dy = dY + (size_t)t * out;
        for (int o = 0; o < out; o++) {
            float *w = W + (size_t)o * in;
            float g = dy[o] * lr;
            for (int i = 0; i < in; i++)
                w[i] -= g * x[i];
        }
    }
}

double cce_xfmr_train_ce(cce_xfmr *m, const int *toks, int T, float lr)
{
    if (!m || T < 2 || cce_xfmr_forward(m, toks, T, NULL) != 0)
        return -1.0;
    int D = m->c.n_embd, V = m->c.vocab;
    int npred = T - 1;
    double loss = 0.0;
    /* dlogits in m->logits after converting to grad in-place copy via dx as V-scratch:
     * use logits buffer: convert row t to softmax, accumulate CE for target toks[t+1] */
    float *dlog = falloc((size_t)T * V);
    if (!dlog)
        return -1.0;
    memset(dlog, 0, (size_t)T * V * sizeof(float));
    for (int t = 0; t < npred; t++) {
        float *lg = m->logits + (size_t)t * V;
        float mx = lg[0];
        for (int i = 1; i < V; i++)
            if (lg[i] > mx)
                mx = lg[i];
        float sum = 0.f;
        for (int i = 0; i < V; i++) {
            lg[i] = expf(lg[i] - mx);
            sum += lg[i];
        }
        float inv = 1.f / (sum + 1e-9f);
        int y = toks[t + 1];
        for (int i = 0; i < V; i++) {
            float p = lg[i] * inv;
            lg[i] = p;
            dlog[(size_t)t * V + i] = (p - (i == y ? 1.f : 0.f)) / (float)npred;
        }
        loss += -log((double)lg[y] + 1e-9);
    }
    loss /= npred;
    /* SGD head: W_head[V,D] from xb[T,D] (post-final RMS) and dlog[T,V] */
    sgd_outer(m->w_head, m->xb, dlog, npred, D, V, lr);
    /* cheap residual path: also SGD last MLP down/up using stored h, xb */
    Layer *L = &m->L[m->c.n_layer - 1];
    int hid = m->hidden;
    /* d_xc ≈ dx through identity skip; use head backprop to xb as proxy:
     * dxb[t,i] = sum_v dlog[t,v] * w_head[v,i] */
    memset(m->dxb, 0, (size_t)T * D * sizeof(float));
    for (int t = 0; t < npred; t++) {
        const float *dl = dlog + (size_t)t * V;
        float *dx = m->dxb + (size_t)t * D;
        for (int v = 0; v < V; v++) {
            const float *wh = m->w_head + (size_t)v * D;
            if (m->ternary && m->weff)
                wh = m->weff + (size_t)v * D; /* STE: activation path uses ternary */
            float g = dl[v];
            for (int i = 0; i < D; i++)
                dx[i] += g * wh[i];
        }
    }
    sgd_outer(L->w2, m->h, m->dxb, npred, hid, D, lr);
    /* d_h ≈ dxb @ w2  (w2 is [D, hid]) */
    memset(m->h, 0, (size_t)T * hid * sizeof(float)); /* reuse as dh — h was gelu act */
    /* restore: we overwrote h. Recompute gelu(h) would need saved preact.
     * Skip deeper MLP/attn SGD this step; head+last down is the residual speaker
     * update. Full-block bwd is the next capsule. */
    (void)L->w1;
    free(dlog);
    return loss;
}
