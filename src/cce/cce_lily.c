/* cce_lily — interconnected multi-layer low-rank adapter (prototype). See header.
   Explicit exact backprop through the frozen L-layer chain; the shared down-
   projection A accumulates gradient from every layer (the interconnection). */

#include "../../include/cce/cce_lily.h"
#include "../../include/cce/cce_ds_runtime.h"   /* g_cce_layer_adapt_hook */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t lcg(uint32_t *s) { *s = *s * 1664525u + 1013904223u; return *s; }
static float randn(uint32_t *s) {
    float u1 = (lcg(s) >> 8) * (1.0f / 16777216.0f);
    float u2 = (lcg(s) >> 8) * (1.0f / 16777216.0f);
    if (u1 < 1e-7f) u1 = 1e-7f;
    return sqrtf(-2.0f * logf(u1)) * cosf(6.2831853f * u2);
}

static inline float scale(const cce_lily *ly) { return ly->rank > 0 ? ly->alpha / (float)ly->rank : 0.0f; }
static inline const float *Aptr(const cce_lily *ly, int l) {
    return ly->A + (size_t)(ly->shared ? 0 : l) * ly->width * ly->rank;
}

cce_result cce_lily_init(cce_lily *ly, int width, int layers, int rank,
                         float alpha, uint32_t seed, int shared) {
    if (!ly || width <= 0 || layers <= 0 || rank <= 0) return CCE_ERR_INVALID_ARG;
    memset(ly, 0, sizeof(*ly));
    ly->width = width; ly->layers = layers; ly->rank = rank; ly->alpha = alpha; ly->shared = shared ? 1 : 0;
    size_t na = (size_t)(ly->shared ? 1 : layers) * width * rank;
    size_t nb = (size_t)layers * rank * width;
    ly->A = malloc(na * sizeof(float));
    ly->B = calloc(nb, sizeof(float));      /* B = 0 => delta starts at 0 */
    if (!ly->A || !ly->B) { free(ly->A); free(ly->B); return CCE_ERR_OOM; }
    uint32_t s = seed ? seed : 0x9E3779B9u;
    float sigma = 1.0f / sqrtf((float)width);
    for (size_t i = 0; i < na; i++) ly->A[i] = randn(&s) * sigma;
    return CCE_OK;
}

void cce_lily_free(cce_lily *ly) {
    if (!ly) return;
    free(ly->A); free(ly->B);
    memset(ly, 0, sizeof(*ly));
}

size_t cce_lily_param_count(const cce_lily *ly) {
    if (!ly) return 0;
    size_t na = (size_t)(ly->shared ? 1 : ly->layers) * ly->width * ly->rank;
    return na + (size_t)ly->layers * ly->rank * ly->width;
}
size_t cce_lily_indep_param_count(const cce_lily *ly) {
    return ly ? 2u * (size_t)ly->layers * ly->rank * ly->width : 0;
}
size_t cce_lily_dense_param_count(const cce_lily *ly) {
    return ly ? (size_t)ly->layers * ly->width * ly->width : 0;
}

cce_result cce_lily_apply(const cce_lily *ly, const float *baseW, const float *x, float *y) {
    if (!ly || !baseW || !x || !y) return CCE_ERR_INVALID_ARG;
    const int d = ly->width, r = ly->rank, L = ly->layers;
    const float s = scale(ly);
    float *h = malloc((size_t)d * sizeof(float));
    float *hn = malloc((size_t)d * sizeof(float));
    float *tmp = malloc((size_t)r * sizeof(float));
    if (!h || !hn || !tmp) { free(h); free(hn); free(tmp); return CCE_ERR_OOM; }
    memcpy(h, x, (size_t)d * sizeof(float));
    for (int l = 0; l < L; l++) {
        const float *W = baseW + (size_t)l * d * d;
        const float *A = Aptr(ly, l);
        const float *B = ly->B + (size_t)l * r * d;
        for (int k = 0; k < r; k++) { float a = 0; for (int i = 0; i < d; i++) a += h[i] * A[i * r + k]; tmp[k] = a; }
        for (int o = 0; o < d; o++) { float b = 0; for (int i = 0; i < d; i++) b += h[i] * W[i * d + o]; hn[o] = b; }
        for (int k = 0; k < r; k++) { float t = s * tmp[k]; const float *Bk = B + (size_t)k * d;
            for (int o = 0; o < d; o++) hn[o] += t * Bk[o]; }
        memcpy(h, hn, (size_t)d * sizeof(float));
    }
    memcpy(y, h, (size_t)d * sizeof(float));
    free(h); free(hn); free(tmp);
    return CCE_OK;
}

cce_result cce_lily_merge(const cce_lily *ly, float *baseW) {
    if (!ly || !baseW) return CCE_ERR_INVALID_ARG;
    const int d = ly->width, r = ly->rank, L = ly->layers;
    const float s = scale(ly);
    for (int l = 0; l < L; l++) {
        float *W = baseW + (size_t)l * d * d;
        const float *A = Aptr(ly, l);
        const float *B = ly->B + (size_t)l * r * d;
        for (int i = 0; i < d; i++) {
            const float *Ai = &A[(size_t)i * r];
            float *Wi = &W[(size_t)i * d];
            for (int k = 0; k < r; k++) {
                float a = s * Ai[k]; if (a == 0.0f) continue;
                const float *Bk = B + (size_t)k * d;
                for (int o = 0; o < d; o++) Wi[o] += a * Bk[o];
            }
        }
    }
    return CCE_OK;
}

cce_lily_train_opts cce_lily_train_defaults(void) {
    cce_lily_train_opts o; o.epochs = 400; o.lr = 0.01f; o.use_adam = 1; o.target_loss = 0.0f; o.log_every = 0;
    return o;
}

double cce_lily_eval_mse(const cce_lily *ly, const float *baseW,
                         const float *inputs, const float *targets, size_t n) {
    if (!ly || !baseW || !inputs || !targets || n == 0) return -1.0;
    const int d = ly->width;
    float *y = malloc((size_t)d * sizeof(float));
    if (!y) return -1.0;
    double se = 0.0;
    for (size_t sIdx = 0; sIdx < n; sIdx++) {
        cce_lily_apply(ly, baseW, inputs + sIdx * d, y);
        const float *t = targets + sIdx * d;
        for (int o = 0; o < d; o++) { double e = y[o] - t[o]; se += e * e; }
    }
    free(y);
    return se / (double)(n * (size_t)d);
}

/* ---- training-data collection through the deep forward --------------------- */
typedef struct { float *out; int L, d; int cur; } lily_cap_t;
static void lily_capture_hook(int layer, float *residual, int width, void *ctx) {
    lily_cap_t *c = (lily_cap_t *)ctx;
    if (!c || layer < 0 || layer >= c->L || width != c->d) return;
    memcpy(c->out + ((size_t)c->cur * c->L + layer) * c->d, residual, (size_t)c->d * sizeof(float));
}

cce_result cce_lily_collect(struct cce_ds_host *h, const float *inputs, size_t n, float *out) {
    if (!h || !inputs || !out || n == 0) return CCE_ERR_INVALID_ARG;
    const int d = h->d_model, L = h->n_layer;
    lily_cap_t c = { out, L, d, 0 };
    CceLayerAdaptHook oldh = g_cce_layer_adapt_hook; void *oldc = g_cce_layer_adapt_ctx;
    g_cce_layer_adapt_hook = lily_capture_hook; g_cce_layer_adapt_ctx = &c;
    for (size_t s = 0; s < n; s++) {
        c.cur = (int)s;
        cce_ds_host_reset(h);
        memcpy(h->residual, inputs + s * (size_t)d, (size_t)d * sizeof(float));
        cce_ds_host_forward_token(h);
    }
    g_cce_layer_adapt_hook = oldh; g_cce_layer_adapt_ctx = oldc;
    return CCE_OK;
}

/* Per-layer residual distillation (no base backprop). */
double cce_lily_train_residual(cce_lily *ly, const float *base_res,
                               const float *target_res, size_t n, const cce_lily_train_opts *opt) {
    if (!ly || !base_res || !target_res || n == 0) return -1.0;
    cce_lily_train_opts o = opt ? *opt : cce_lily_train_defaults();
    const int d = ly->width, r = ly->rank, L = ly->layers;
    const float s = scale(ly);
    const size_t na = (size_t)(ly->shared ? 1 : L) * d * r, nb = (size_t)L * r * d;
    float *gA = calloc(na, sizeof(float)), *gB = calloc(nb, sizeof(float));
    float *mA = calloc(na, sizeof(float)), *vA = calloc(na, sizeof(float));
    float *mB = calloc(nb, sizeof(float)), *vB = calloc(nb, sizeof(float));
    float *tmp = malloc((size_t)r * sizeof(float)), *e = malloc((size_t)d * sizeof(float));
    if (!gA || !gB || !mA || !vA || !mB || !vB || !tmp || !e) {
        free(gA); free(gB); free(mA); free(vA); free(mB); free(vB); free(tmp); free(e); return -1.0;
    }
    const float b1 = 0.9f, b2 = 0.999f, eps = 1e-8f;
    double last = -1.0;
    for (int ep = 1; ep <= o.epochs; ep++) {
        memset(gA, 0, na * sizeof(float)); memset(gB, 0, nb * sizeof(float));
        double se = 0.0;
        for (size_t sIdx = 0; sIdx < n; sIdx++) {
            for (int l = 0; l < L; l++) {
                const float *ri = base_res + ((size_t)sIdx * L + l) * d;
                const float *to = target_res + ((size_t)sIdx * L + l) * d;
                const float *A = Aptr(ly, l);
                const float *B = ly->B + (size_t)l * r * d;
                float *gAl = gA + (size_t)(ly->shared ? 0 : l) * d * r;
                float *gBl = gB + (size_t)l * r * d;
                for (int k = 0; k < r; k++) { float a = 0; for (int i = 0; i < d; i++) a += ri[i] * A[i * r + k]; tmp[k] = a; }
                for (int oo = 0; oo < d; oo++) {
                    float pred = 0; for (int k = 0; k < r; k++) pred += s * tmp[k] * B[k * d + oo];
                    e[oo] = pred - (to[oo] - ri[oo]); se += (double)e[oo] * e[oo];
                }
                for (int k = 0; k < r; k++) {
                    float accb = 0; float *gBk = gBl + (size_t)k * d; const float *Bk = B + (size_t)k * d; float st = s * tmp[k];
                    for (int oo = 0; oo < d; oo++) { gBk[oo] += st * e[oo]; accb += e[oo] * Bk[oo]; }
                    float sb = s * accb;
                    for (int i = 0; i < d; i++) gAl[(size_t)i * r + k] += ri[i] * sb;
                }
            }
        }
        const float invn = 1.0f / (float)(n * (size_t)L);
        for (size_t i = 0; i < na; i++) { float g = gA[i] * invn;
            if (o.use_adam) { mA[i]=b1*mA[i]+(1-b1)*g; vA[i]=b2*vA[i]+(1-b2)*g*g;
                float mh=mA[i]/(1-powf(b1,(float)ep)), vh=vA[i]/(1-powf(b2,(float)ep)); ly->A[i]-=o.lr*mh/(sqrtf(vh)+eps); }
            else ly->A[i]-=o.lr*g; }
        for (size_t i = 0; i < nb; i++) { float g = gB[i] * invn;
            if (o.use_adam) { mB[i]=b1*mB[i]+(1-b1)*g; vB[i]=b2*vB[i]+(1-b2)*g*g;
                float mh=mB[i]/(1-powf(b1,(float)ep)), vh=vB[i]/(1-powf(b2,(float)ep)); ly->B[i]-=o.lr*mh/(sqrtf(vh)+eps); }
            else ly->B[i]-=o.lr*g; }
        last = se / (double)(n * (size_t)L * d);
        if (o.log_every && (ep % o.log_every == 0 || ep == 1)) printf("  [lily-distill] epoch %d mse=%.6g\n", ep, last);
        if (o.target_loss > 0.0f && last <= o.target_loss) break;
    }
    free(gA); free(gB); free(mA); free(vA); free(mB); free(vB); free(tmp); free(e);
    return last;
}

/* ---- interior-layer serving hook -----------------------------------------
   Called from the DS forward after each layer: residual += (alpha/r) B_L (A_L r). */
static void lily_serve_impl(int layer, float *residual, int width, void *ctx) {
    const cce_lily *ly = (const cce_lily *)ctx;
    if (!ly || !residual || layer < 0 || layer >= ly->layers || width != ly->width) return;
    const int d = ly->width, r = ly->rank;
    const float s = scale(ly);
    const float *A = Aptr(ly, layer);
    const float *B = ly->B + (size_t)layer * r * d;
    float stackbuf[64];
    float *tmp = (r <= 64) ? stackbuf : (float *)malloc((size_t)r * sizeof(float));
    if (!tmp) return;
    for (int k = 0; k < r; k++) { float a = 0; for (int i = 0; i < d; i++) a += residual[i] * A[i * r + k]; tmp[k] = a; }
    for (int k = 0; k < r; k++) { float t = s * tmp[k]; if (t == 0.0f) continue;
        const float *Bk = B + (size_t)k * d; for (int o = 0; o < d; o++) residual[o] += t * Bk[o]; }
    if (tmp != stackbuf) free(tmp);
}

void cce_lily_install_serving(const cce_lily *ly) {
    g_cce_layer_adapt_ctx = (void *)ly;
    g_cce_layer_adapt_hook = ly ? lily_serve_impl : NULL;
}
void cce_lily_uninstall_serving(void) {
    g_cce_layer_adapt_hook = NULL;
    g_cce_layer_adapt_ctx = NULL;
}

double cce_lily_train(cce_lily *ly, const float *baseW,
                      const float *inputs, const float *targets, size_t n,
                      const cce_lily_train_opts *opt) {
    if (!ly || !baseW || !inputs || !targets || n == 0) return -1.0;
    cce_lily_train_opts o = opt ? *opt : cce_lily_train_defaults();
    const int d = ly->width, r = ly->rank, L = ly->layers;
    const float s = scale(ly);
    const size_t na = (size_t)(ly->shared ? 1 : L) * d * r, nb = (size_t)L * r * d;

    float *gA = calloc(na, sizeof(float)), *gB = calloc(nb, sizeof(float));
    float *mA = calloc(na, sizeof(float)), *vA = calloc(na, sizeof(float));
    float *mB = calloc(nb, sizeof(float)), *vB = calloc(nb, sizeof(float));
    float *H = malloc((size_t)(L + 1) * d * sizeof(float));   /* activations h[0..L] */
    float *TL = malloc((size_t)L * r * sizeof(float));        /* tmp = A h_l per layer */
    float *dh = malloc((size_t)d * sizeof(float)), *dhp = malloc((size_t)d * sizeof(float));
    float *br = malloc((size_t)r * sizeof(float));
    if (!gA || !gB || !mA || !vA || !mB || !vB || !H || !TL || !dh || !dhp || !br) {
        free(gA); free(gB); free(mA); free(vA); free(mB); free(vB); free(H); free(TL); free(dh); free(dhp); free(br);
        return -1.0;
    }
    const float b1 = 0.9f, b2 = 0.999f, eps = 1e-8f;
    double last = -1.0;

    for (int ep = 1; ep <= o.epochs; ep++) {
        memset(gA, 0, na * sizeof(float));
        memset(gB, 0, nb * sizeof(float));
        double se = 0.0;
        for (size_t sIdx = 0; sIdx < n; sIdx++) {
            /* forward, storing H and per-layer tmp */
            memcpy(H, inputs + sIdx * d, (size_t)d * sizeof(float));
            for (int l = 0; l < L; l++) {
                const float *W = baseW + (size_t)l * d * d;
                const float *A = Aptr(ly, l);
                const float *B = ly->B + (size_t)l * r * d;
                const float *hc = H + (size_t)l * d;
                float *hnx = H + (size_t)(l + 1) * d;
                float *tmp = TL + (size_t)l * r;
                for (int k = 0; k < r; k++) { float a = 0; for (int i = 0; i < d; i++) a += hc[i] * A[i * r + k]; tmp[k] = a; }
                for (int oo = 0; oo < d; oo++) { float b = 0; for (int i = 0; i < d; i++) b += hc[i] * W[i * d + oo]; hnx[oo] = b; }
                for (int k = 0; k < r; k++) { float t = s * tmp[k]; const float *Bk = B + (size_t)k * d;
                    for (int oo = 0; oo < d; oo++) hnx[oo] += t * Bk[oo]; }
            }
            const float *hL = H + (size_t)L * d;
            const float *tgt = targets + sIdx * d;
            for (int oo = 0; oo < d; oo++) { dh[oo] = hL[oo] - tgt[oo]; se += (double)dh[oo] * dh[oo]; }
            /* backward */
            for (int l = L - 1; l >= 0; l--) {
                const float *W = baseW + (size_t)l * d * d;
                const float *A = Aptr(ly, l);
                const float *B = ly->B + (size_t)l * r * d;
                float *gAl = gA + (size_t)(ly->shared ? 0 : l) * d * r;
                float *gBl = gB + (size_t)l * r * d;
                const float *hc = H + (size_t)l * d;
                const float *tmp = TL + (size_t)l * r;
                for (int k = 0; k < r; k++) {
                    float accb = 0.0f; float *gBk = gBl + (size_t)k * d; const float *Bk = B + (size_t)k * d;
                    float st = s * tmp[k];
                    for (int oo = 0; oo < d; oo++) { gBk[oo] += st * dh[oo]; accb += dh[oo] * Bk[oo]; }
                    br[k] = s * accb;                       /* dL/dtmp[k] */
                }
                for (int i = 0; i < d; i++) {
                    const float hi = hc[i]; float *gAi = gAl + (size_t)i * r;
                    for (int k = 0; k < r; k++) gAi[k] += hi * br[k];
                }
                for (int i = 0; i < d; i++) {
                    float acc = 0.0f; for (int oo = 0; oo < d; oo++) acc += W[i * d + oo] * dh[oo];
                    float ad = 0.0f; const float *Ai = A + (size_t)i * r; for (int k = 0; k < r; k++) ad += Ai[k] * br[k];
                    dhp[i] = acc + ad;
                }
                memcpy(dh, dhp, (size_t)d * sizeof(float));
            }
        }
        const float invn = 1.0f / (float)n;
        for (size_t i = 0; i < na; i++) {
            float g = gA[i] * invn;
            if (o.use_adam) { mA[i] = b1 * mA[i] + (1 - b1) * g; vA[i] = b2 * vA[i] + (1 - b2) * g * g;
                float mh = mA[i] / (1 - powf(b1, (float)ep)), vh = vA[i] / (1 - powf(b2, (float)ep));
                ly->A[i] -= o.lr * mh / (sqrtf(vh) + eps); }
            else ly->A[i] -= o.lr * g;
        }
        for (size_t i = 0; i < nb; i++) {
            float g = gB[i] * invn;
            if (o.use_adam) { mB[i] = b1 * mB[i] + (1 - b1) * g; vB[i] = b2 * vB[i] + (1 - b2) * g * g;
                float mh = mB[i] / (1 - powf(b1, (float)ep)), vh = vB[i] / (1 - powf(b2, (float)ep));
                ly->B[i] -= o.lr * mh / (sqrtf(vh) + eps); }
            else ly->B[i] -= o.lr * g;
        }
        last = se / (double)(n * (size_t)d);
        if (o.log_every && (ep % o.log_every == 0 || ep == 1)) printf("  [lily] epoch %d mse=%.6g\n", ep, last);
        if (o.target_loss > 0.0f && last <= o.target_loss) break;
    }
    free(gA); free(gB); free(mA); free(vA); free(mB); free(vB); free(H); free(TL); free(dh); free(dhp); free(br);
    return last;
}
