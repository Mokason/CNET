/* Recurrent language model trainer: explicit forward/backward, three matrix-state mixers. See cnet_vsa_rlm.h.
 * Layout: per-token forward caches; the backward pass walks tokens in reverse, computing the gradients that
 * flow through the recurrence and caching every linear map's output gradient; the matrix gradients are then
 * accumulated once per chunk (rank-n updates, row-resident), which keeps the pass compute-bound. */
#include "cnet_vsa_rlm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#ifdef _OPENMP
#include <omp.h>
#define IN_PAR() omp_in_parallel()
#define OMP(x) _Pragma(#x)
#else
#define IN_PAR() 0
#define OMP(x)
#endif
#if defined(_OPENMP) || defined(CNET_VSA_RLM_SIMD)
#define PRAGMA_STR(x) _Pragma(#x)
#define SIMD PRAGMA_STR(omp simd)
#define SIMD_RED(v) PRAGMA_STR(omp simd reduction(+ : v))
#else
#define SIMD
#define SIMD_RED(v)
#endif

typedef rlm_real R;
#define EPS ((R)1e-6)
#define MAXD 4096
#define MAXF 8192
#define MAXDH 256

typedef struct { size_t off, n; } Slot;

struct cnet_vsa_rlm {
    cnet_vsa_rlm_cfg cfg;
    int d, H, dh, L, F, V, T, K, shared;
    R *p, *g, *am, *av; size_t np;
    Slot emb, normf;
    Slot *n1, *n2, *wq, *wk, *wv, *wg, *wo, *w1, *w2;
    Slot *wb, *wa;                          /* deltanet: [H][d+1] (bias last) */
    Slot *mu, *ww, *wkap, *wa7;             /* rwkv7: mu[d]; ww,wa7 [d][d+1]; wkap [d][d] */
    Slot *wu, *cw, *cb, *wd, *alog, *dskip; /* ssd */
    R *S, *nprev, *conv;                    /* recurrent state: [L][H][dh][dh], [L][d], [L][d][K-1] */
    R *nprev0, *conv0;                      /* state at the start of the current chunk */
    /* per-token forward caches [L][T][...] */
    R *c_xin, *c_n, *c_rms1, *c_xt, *c_u, *c_c, *c_q, *c_k, *c_kn, *c_v, *c_bet, *c_alp, *c_w, *c_a, *c_kap, *c_dl;
    R *c_Sb, *c_o, *c_gp, *c_m, *c_xmid, *c_n2, *c_rms2, *c_hp, *c_h;
    R *c_xpre, *c_xf, *c_dxf, *c_rmsf, *c_prob;
    /* per-token output-gradient caches for the linear maps */
    R *c_dxout, *c_dhp, *c_dxmid, *c_dgp, *c_dq, *c_dk, *c_dv, *c_du, *c_dzb, *c_dza, *c_dzw, *c_dza7, *c_dkr, *c_dzd, *c_dnprev;
    R *dS;
    uint64_t rng;
    int last_n;
};

static uint64_t xs(uint64_t *s) { uint64_t x = *s; x ^= x << 13; x ^= x >> 7; x ^= x << 17; *s = x; return x; }
static R randn(uint64_t *s) { double u1 = ((xs(s) >> 11) + 1.0) / 9007199254740993.0, u2 = ((xs(s) >> 11) + 1.0) / 9007199254740993.0; return (R)(sqrt(-2.0 * log(u1)) * cos(6.283185307179586 * u2)); }
static R sigm(R x) { return (R)(1.0 / (1.0 + exp(-(double)x))); }
static R silu(R x) { return x * sigm(x); }
static R dsilu(R x) { R s = sigm(x); return s * (1 + x * (1 - s)); }
static R softplus(R x) { return x > 20 ? x : (R)log1p(exp((double)x)); }

static Slot *slots(int n) { return (Slot *)calloc((size_t)n, sizeof(Slot)); }
static void addslot(cnet_vsa_rlm *m, Slot *s, size_t n) { s->off = m->np; s->n = n; m->np += n; }
#define P(slot) (m->p + (slot).off)
#define G(slot) (m->g + (slot).off)
#define C(arr, l, t, w) ((arr) + (((size_t)(l) * m->T + (t)) * (w)))

/* ---- kernels ---- */
static inline R dot(const R *a, const R *b, int n) { R acc = 0;
    SIMD_RED(acc)
    for (int i = 0; i < n; ++i) acc += a[i] * b[i]; return acc; }
static inline void axpy(R *y, const R *x, R a, int n) {
    SIMD
    for (int i = 0; i < n; ++i) y[i] += a * x[i]; }
/* y[out] = W[out][cols] x[in] (+ bias column when cols == in + 1) */
static void matvec(const R *W, const R *x, R *y, int out, int in, int cols) {
    OMP(omp parallel for if (out >= 512 && !IN_PAR()))
    for (int o = 0; o < out; ++o) { const R *w = W + (size_t)o * cols; y[o] = dot(w, x, in) + (cols > in ? w[in] : 0); }
}
/* dx += W^T dy, row-major */
static void matvec_dx(const R *W, const R *dy, R *dx, int out, int in, int cols) { for (int o = 0; o < out; ++o) axpy(dx, W + (size_t)o * cols, dy[o], in); }
/* dW[out][cols] += sum_t DY[t][o] X[t][:]  (bias column gets sum_t DY[t][o]) */
static void gemm_acc(R *dW, const R *DY, int ldy, const R *X, int ldx, int out, int in, int cols, int n) {
    OMP(omp parallel for if (out >= 512 && !IN_PAR()))
    for (int o = 0; o < out; ++o) { R *dw = dW + (size_t)o * cols; R bsum = 0;
        for (int t = 0; t < n; ++t) { R d = DY[(size_t)t * ldy + o]; if (d != 0) { axpy(dw, X + (size_t)t * ldx, d, in); bsum += d; } }
        if (cols > in) dw[in] += bsum; }
}
/* LG[t][v] = W[v] . XF[t] for t < n: one pass over W per chunk */
static void gemm_out(const R *W, const R *XF, R *LG, int V, int d, int n) {
    OMP(omp parallel for if (V >= 512 && !IN_PAR()))
    for (int v = 0; v < V; ++v) { const R *w = W + (size_t)v * d; for (int t = 0; t < n; ++t) LG[(size_t)t * V + v] = dot(w, XF + (size_t)t * d, d); }
}
/* DXF[t] += sum_v DL[t][v] W[v] */
static void gemm_dx(const R *W, const R *DL, R *DXF, int V, int d, int n) {
    if (!IN_PAR()) {
        OMP(omp parallel for)
        for (int t = 0; t < n; ++t) { R *dx = DXF + (size_t)t * d; const R *dl = DL + (size_t)t * V; for (int v = 0; v < V; ++v) if (dl[v] != 0) axpy(dx, W + (size_t)v * d, dl[v], d); }
    } else
        for (int v = 0; v < V; ++v) { const R *w = W + (size_t)v * d; for (int t = 0; t < n; ++t) { R g = DL[(size_t)t * V + v]; if (g != 0) axpy(DXF + (size_t)t * d, w, g, d); } }
}
static R rmsnorm(const R *x, const R *gain, R *out, int d) {
    double ss = 0; for (int i = 0; i < d; ++i) ss += (double)x[i] * x[i];
    R r = (R)sqrt(ss / d + (double)EPS);
    for (int i = 0; i < d; ++i) out[i] = x[i] / r * gain[i];
    return r;
}
static void rmsnorm_bwd(const R *x, const R *gain, R *dgain, R r, const R *dn, R *dx, int d) {
    double s = 0; for (int i = 0; i < d; ++i) { dgain[i] += dn[i] * x[i] / r; s += (double)gain[i] * dn[i] * x[i]; }
    for (int i = 0; i < d; ++i) dx[i] += gain[i] * dn[i] / r - x[i] * (R)(s / ((double)d * r * r * r));
}

static int alloc_work(cnet_vsa_rlm *m) {
    const int d = m->d, L = m->L, H = m->H, F = m->F, V = m->V, K = m->K; const size_t T = (size_t)m->T, st = (size_t)L * d * m->dh, LTd = (size_t)L * T * d, LTH = (size_t)L * T * H;
    m->S = (R *)calloc(st, sizeof(R)); m->dS = (R *)calloc(st, sizeof(R)); m->nprev = (R *)calloc((size_t)L * d, sizeof(R)); m->conv = (R *)calloc((size_t)L * d * (K - 1), sizeof(R));
    m->nprev0 = (R *)calloc((size_t)L * d, sizeof(R)); m->conv0 = (R *)calloc((size_t)L * d * (K - 1), sizeof(R));
    R **big[] = { &m->c_xin, &m->c_n, &m->c_xt, &m->c_u, &m->c_c, &m->c_q, &m->c_k, &m->c_kn, &m->c_v, &m->c_w, &m->c_a, &m->c_kap, &m->c_o, &m->c_gp, &m->c_m, &m->c_xmid, &m->c_n2,
                  &m->c_dxout, &m->c_dxmid, &m->c_dgp, &m->c_dq, &m->c_dk, &m->c_dv, &m->c_du, &m->c_dzw, &m->c_dza7, &m->c_dkr };
    for (size_t i = 0; i < sizeof(big) / sizeof(big[0]); ++i) { *big[i] = (R *)calloc(LTd, sizeof(R)); if (!*big[i]) return -1; }
    R **small[] = { &m->c_bet, &m->c_alp, &m->c_dl, &m->c_dzb, &m->c_dza, &m->c_dzd };
    for (size_t i = 0; i < sizeof(small) / sizeof(small[0]); ++i) { *small[i] = (R *)calloc(LTH, sizeof(R)); if (!*small[i]) return -1; }
    m->c_rms1 = (R *)calloc(L * T, sizeof(R)); m->c_rms2 = (R *)calloc(L * T, sizeof(R));
    m->c_Sb = (R *)calloc(T * st, sizeof(R)); m->c_hp = (R *)calloc(L * T * F, sizeof(R)); m->c_h = (R *)calloc(L * T * F, sizeof(R)); m->c_dhp = (R *)calloc(L * T * F, sizeof(R));
    m->c_xpre = (R *)calloc(T * d, sizeof(R)); m->c_xf = (R *)calloc(T * d, sizeof(R)); m->c_dxf = (R *)calloc(T * d, sizeof(R)); m->c_rmsf = (R *)calloc(T, sizeof(R)); m->c_prob = (R *)calloc(T * (size_t)V, sizeof(R));
    m->c_dnprev = (R *)calloc((size_t)L * d, sizeof(R));
    return (m->S && m->dS && m->nprev && m->conv && m->nprev0 && m->conv0 && m->c_rms1 && m->c_rms2 && m->c_Sb && m->c_hp && m->c_h && m->c_dhp && m->c_xpre && m->c_xf && m->c_dxf && m->c_rmsf && m->c_prob && m->c_dnprev) ? 0 : -1;
}

static cnet_vsa_rlm *layout(const cnet_vsa_rlm_cfg *cfg) {
    if (!cfg || cfg->vocab < 4 || cfg->d_model < 4 || cfg->d_model > MAXD || cfg->n_head < 1 || cfg->d_model % cfg->n_head || cfg->d_model / cfg->n_head > MAXDH || cfg->n_layer < 1 || cfg->chunk < 1 || cfg->mixer < 0 || cfg->mixer > 2) return NULL;
    cnet_vsa_rlm *m = (cnet_vsa_rlm *)calloc(1, sizeof(*m)); if (!m) return NULL;
    m->cfg = *cfg; m->d = cfg->d_model; m->H = cfg->n_head; m->dh = m->d / m->H; m->L = cfg->n_layer; m->F = cfg->d_ff > 0 ? cfg->d_ff : 2 * m->d;
    if (m->F > MAXF) { free(m); return NULL; }
    m->V = cfg->vocab; m->T = cfg->chunk; m->K = CNET_VSA_RLM_CONV_K; m->rng = 0x9E3779B97F4A7C15ULL ^ ((uint64_t)cfg->seed + 1) * 0xD1B54A32D192ED03ULL;
    const int d = m->d, L = m->L, H = m->H, F = m->F, V = m->V, K = m->K;
    m->n1 = slots(L); m->n2 = slots(L); m->wq = slots(L); m->wk = slots(L); m->wv = slots(L); m->wg = slots(L); m->wo = slots(L); m->w1 = slots(L); m->w2 = slots(L);
    m->wb = slots(L); m->wa = slots(L); m->mu = slots(L); m->ww = slots(L); m->wkap = slots(L); m->wa7 = slots(L);
    m->wu = slots(L); m->cw = slots(L); m->cb = slots(L); m->wd = slots(L); m->alog = slots(L); m->dskip = slots(L);
    addslot(m, &m->emb, (size_t)V * d); addslot(m, &m->normf, d);
    for (int l = 0; l < L; ++l) {
        addslot(m, &m->n1[l], d); addslot(m, &m->n2[l], d);
        addslot(m, &m->wq[l], (size_t)d * d); addslot(m, &m->wk[l], (size_t)d * d); addslot(m, &m->wv[l], (size_t)d * d);
        addslot(m, &m->wg[l], (size_t)d * d); addslot(m, &m->wo[l], (size_t)d * d);
        addslot(m, &m->w1[l], (size_t)F * d); addslot(m, &m->w2[l], (size_t)d * F);
        if (cfg->mixer == CNET_VSA_RLM_MIXER_DELTANET) { addslot(m, &m->wu[l], (size_t)d * d); addslot(m, &m->cw[l], (size_t)d * K); addslot(m, &m->cb[l], d); addslot(m, &m->wb[l], (size_t)H * (d + 1)); addslot(m, &m->wa[l], (size_t)H * (d + 1)); }
        else if (cfg->mixer == CNET_VSA_RLM_MIXER_RWKV7) { addslot(m, &m->mu[l], d); addslot(m, &m->ww[l], (size_t)d * (d + 1)); addslot(m, &m->wkap[l], (size_t)d * d); addslot(m, &m->wa7[l], (size_t)d * (d + 1)); }
        else { addslot(m, &m->wu[l], (size_t)d * d); addslot(m, &m->cw[l], (size_t)d * K); addslot(m, &m->cb[l], d); addslot(m, &m->wd[l], (size_t)H * (d + 1)); addslot(m, &m->alog[l], H); addslot(m, &m->dskip[l], d); }
    }
    m->g = (R *)calloc(m->np, sizeof(R));
    if (!m->g || alloc_work(m) != 0) { cnet_vsa_rlm_free(m); return NULL; }
    return m;
}

/* conv tap initialisation: uniform 1/K (default) or CNET_VSA_RLM_CONV_INIT=prev (half current token, half previous) */
static void conv_init(R *cw, R *cb, int d, int K) {
    const char *e = getenv("CNET_VSA_RLM_CONV_INIT"); int prev = e && !strcmp(e, "prev");
    for (int i = 0; i < d; ++i) { cb[i] = 0; for (int j = 0; j < K; ++j) cw[(size_t)i * K + j] = prev ? (j >= K - 2 ? (R)0.5 : 0) : (R)(1.0 / K); }
}
cnet_vsa_rlm *cnet_vsa_rlm_create(const cnet_vsa_rlm_cfg *cfg) {
    cnet_vsa_rlm *m = layout(cfg); if (!m) return NULL;
    m->p = (R *)calloc(m->np, sizeof(R)); m->am = (R *)calloc(m->np, sizeof(R)); m->av = (R *)calloc(m->np, sizeof(R));
    if (!m->p || !m->am || !m->av) { cnet_vsa_rlm_free(m); return NULL; }
    const int d = m->d, L = m->L, H = m->H, K = m->K;
    for (size_t i = 0; i < m->np; ++i) m->p[i] = randn(&m->rng) * (R)0.02;
    for (int i = 0; i < d; ++i) P(m->normf)[i] = 1;
    for (int l = 0; l < L; ++l) {
        for (int i = 0; i < d; ++i) { P(m->n1[l])[i] = 1; P(m->n2[l])[i] = 1; }
        if (cfg->mixer == CNET_VSA_RLM_MIXER_DELTANET) { for (int h = 0; h < H; ++h) { P(m->wb[l])[(size_t)h * (d + 1) + d] = 0; P(m->wa[l])[(size_t)h * (d + 1) + d] = -3; }
                                                          conv_init(P(m->cw[l]), P(m->cb[l]), d, K); }
        else if (cfg->mixer == CNET_VSA_RLM_MIXER_RWKV7) { for (int i = 0; i < d; ++i) { P(m->mu[l])[i] = (R)0.5; P(m->ww[l])[(size_t)i * (d + 1) + d] = -3; P(m->wa7[l])[(size_t)i * (d + 1) + d] = 0; } }
        else { for (int i = 0; i < d; ++i) P(m->dskip[l])[i] = 1; conv_init(P(m->cw[l]), P(m->cb[l]), d, K);
               for (int h = 0; h < H; ++h) { P(m->wd[l])[(size_t)h * (d + 1) + d] = -2; P(m->alog[l])[h] = (R)log(1.0 + h); } }
    }
    return m;
}

cnet_vsa_rlm *cnet_vsa_rlm_clone_shared(cnet_vsa_rlm *master) {
    if (!master) return NULL;
    cnet_vsa_rlm *m = layout(&master->cfg); if (!m) return NULL;
    m->p = master->p; m->shared = 1;
    return m;
}
void cnet_vsa_rlm_reduce_grad(cnet_vsa_rlm *master, cnet_vsa_rlm *worker, double scale) {
    if (!master || !worker || master->np != worker->np) return;
    axpy(master->g, worker->g, (R)scale, (int)master->np);
}

void cnet_vsa_rlm_free(cnet_vsa_rlm *m) {
    if (!m) return;
    if (!m->shared) free(m->p);
    R **arrs[] = { &m->g, &m->am, &m->av, &m->S, &m->dS, &m->nprev, &m->conv, &m->nprev0, &m->conv0, &m->c_xin, &m->c_n, &m->c_rms1, &m->c_xt, &m->c_u, &m->c_c, &m->c_q, &m->c_k, &m->c_kn, &m->c_v, &m->c_bet, &m->c_alp, &m->c_w, &m->c_a, &m->c_kap, &m->c_dl, &m->c_Sb, &m->c_o, &m->c_gp, &m->c_m, &m->c_xmid, &m->c_n2, &m->c_rms2, &m->c_hp, &m->c_h, &m->c_xpre, &m->c_xf, &m->c_dxf, &m->c_rmsf, &m->c_prob,
                  &m->c_dxout, &m->c_dhp, &m->c_dxmid, &m->c_dgp, &m->c_dq, &m->c_dk, &m->c_dv, &m->c_du, &m->c_dzb, &m->c_dza, &m->c_dzw, &m->c_dza7, &m->c_dkr, &m->c_dzd, &m->c_dnprev };
    for (size_t i = 0; i < sizeof(arrs) / sizeof(arrs[0]); ++i) free(*arrs[i]);
    Slot **sl[] = { &m->n1, &m->n2, &m->wq, &m->wk, &m->wv, &m->wg, &m->wo, &m->w1, &m->w2, &m->wb, &m->wa, &m->mu, &m->ww, &m->wkap, &m->wa7, &m->wu, &m->cw, &m->cb, &m->wd, &m->alog, &m->dskip };
    for (size_t i = 0; i < sizeof(sl) / sizeof(sl[0]); ++i) free(*sl[i]);
    free(m);
}
size_t cnet_vsa_rlm_param_count(const cnet_vsa_rlm *m) { return m ? m->np : 0; }
const cnet_vsa_rlm_cfg *cnet_vsa_rlm_config(const cnet_vsa_rlm *m) { return &m->cfg; }
void cnet_vsa_rlm_reset_state(cnet_vsa_rlm *m) { memset(m->S, 0, sizeof(R) * (size_t)m->L * m->d * m->dh); memset(m->nprev, 0, sizeof(R) * (size_t)m->L * m->d); memset(m->conv, 0, sizeof(R) * (size_t)m->L * m->d * (m->K - 1)); }
void cnet_vsa_rlm_zero_grad(cnet_vsa_rlm *m) { memset(m->g, 0, sizeof(R) * m->np); }

/* ---------------------------------------------------------------- forward ------------- */
static void layer_forward(cnet_vsa_rlm *m, int l, int t, const R *xin, R *xout) {
    const int d = m->d, H = m->H, dh = m->dh, F = m->F, K = m->K, mix = m->cfg.mixer;
    R *xin_c = C(m->c_xin, l, t, d), *n = C(m->c_n, l, t, d), *xt = C(m->c_xt, l, t, d), *q = C(m->c_q, l, t, d), *k = C(m->c_k, l, t, d), *kn = C(m->c_kn, l, t, d), *v = C(m->c_v, l, t, d);
    R *o = C(m->c_o, l, t, d), *gp = C(m->c_gp, l, t, d), *mm = C(m->c_m, l, t, d), *xmid = C(m->c_xmid, l, t, d), *n2 = C(m->c_n2, l, t, d), *hp = C(m->c_hp, l, t, F), *h = C(m->c_h, l, t, F);
    R *Sb = C(m->c_Sb, l, t, d * dh), *S = m->S + (size_t)l * d * dh;
    R y[MAXD], tmp[MAXDH * MAXDH];
    memcpy(xin_c, xin, sizeof(R) * d);
    m->c_rms1[(size_t)l * m->T + t] = rmsnorm(xin, P(m->n1[l]), n, d);
    memcpy(Sb, S, sizeof(R) * d * dh);
    if (mix == CNET_VSA_RLM_MIXER_RWKV7) {
        R *np = m->nprev + (size_t)l * d; const R *mu = P(m->mu[l]);
        for (int i = 0; i < d; ++i) xt[i] = mu[i] * n[i] + (1 - mu[i]) * np[i];
        memcpy(np, n, sizeof(R) * d);
    } else {   /* deltanet and ssd: projection, depthwise causal conv (K taps), silu */
        R *u = C(m->c_u, l, t, d), *c = C(m->c_c, l, t, d); const R *cw = P(m->cw[l]), *cb = P(m->cb[l]); R *cv = m->conv + (size_t)l * d * (K - 1);
        matvec(P(m->wu[l]), n, u, d, d, d);
        for (int i = 0; i < d; ++i) {
            R acc = cb[i] + cw[(size_t)i * K + (K - 1)] * u[i];
            for (int j = 0; j < K - 1; ++j) acc += cw[(size_t)i * K + j] * cv[(size_t)i * (K - 1) + j];   /* history, oldest first */
            c[i] = acc; xt[i] = silu(acc);
            for (int j = 0; j < K - 2; ++j) cv[(size_t)i * (K - 1) + j] = cv[(size_t)i * (K - 1) + j + 1];
            cv[(size_t)i * (K - 1) + (K - 2)] = u[i];
        }
    }
    matvec(P(m->wq[l]), xt, q, d, d, d);
    matvec(P(m->wk[l]), xt, k, d, d, d);
    if (mix != CNET_VSA_RLM_MIXER_SSD) matvec(P(m->wv[l]), xt, v, d, d, d);
    matvec(P(m->wg[l]), xt, gp, d, d, d);
    for (int hh = 0; hh < H; ++hh) {
        R *Sh = S + (size_t)hh * dh * dh; const R *qh = q + hh * dh, *kh = k + hh * dh; R *vh = v + hh * dh, *knh = kn + hh * dh, *oh = o + hh * dh;
        if (mix == CNET_VSA_RLM_MIXER_DELTANET) {
            R inv = (R)(1.0 / sqrt((double)dot(kh, kh, dh) + 1e-8));
            for (int i = 0; i < dh; ++i) knh[i] = kh[i] * inv;
            const R *wb = P(m->wb[l]) + (size_t)hh * (d + 1), *wa = P(m->wa[l]) + (size_t)hh * (d + 1);
            R bet = sigm(dot(wb, xt, d) + wb[d]), alp = (R)exp(-(double)softplus(dot(wa, xt, d) + wa[d]));
            C(m->c_bet, l, t, H)[hh] = bet; C(m->c_alp, l, t, H)[hh] = alp;
            for (int i = 0; i < dh; ++i) { R u = dot(Sh + i * dh, knh, dh), s = alp * bet * u - bet * vh[i]; R *row = Sh + i * dh;
SIMD
                for (int j = 0; j < dh; ++j) row[j] = alp * row[j] - s * knh[j]; }
        } else if (mix == CNET_VSA_RLM_MIXER_RWKV7) {
            R *wv = C(m->c_w, l, t, d) + hh * dh, *av = C(m->c_a, l, t, d) + hh * dh, *kap = C(m->c_kap, l, t, d) + hh * dh;
            for (int i = 0; i < dh; ++i) {
                int ch = hh * dh + i; const R *rw = P(m->ww[l]) + (size_t)ch * (d + 1), *ra = P(m->wa7[l]) + (size_t)ch * (d + 1), *rk = P(m->wkap[l]) + (size_t)ch * d;
                wv[i] = (R)exp(-(double)softplus(dot(rw, xt, d) + rw[d])); av[i] = sigm(dot(ra, xt, d) + ra[d]); kap[i] = dot(rk, xt, d);
            }
            R inv = (R)(1.0 / sqrt((double)dot(kap, kap, dh) + 1e-8)); for (int i = 0; i < dh; ++i) kap[i] *= inv;
            /* S <- S D + v k^T, D = diag(w) - kap (a o kap)^T */
            for (int i = 0; i < dh; ++i) { R *row = Sh + i * dh; R dotk = dot(row, kap, dh), vi = vh[i];
SIMD
                for (int j = 0; j < dh; ++j) tmp[i * dh + j] = row[j] * wv[j] - dotk * av[j] * kap[j] + vi * kh[j]; }
            memcpy(Sh, tmp, sizeof(R) * dh * dh);
        } else {
            const R *wd = P(m->wd[l]) + (size_t)hh * (d + 1);
            R dl = softplus(dot(wd, xt, d) + wd[d]), A = (R)exp((double)P(m->alog[l])[hh]), alp = (R)exp(-(double)dl * A);
            C(m->c_dl, l, t, H)[hh] = dl; C(m->c_alp, l, t, H)[hh] = alp;
            for (int i = 0; i < dh; ++i) vh[i] = dl * xt[hh * dh + i];
            for (int i = 0; i < dh; ++i) { R *row = Sh + i * dh, vi = vh[i];
SIMD
                for (int j = 0; j < dh; ++j) row[j] = alp * row[j] + vi * kh[j]; }
        }
        for (int i = 0; i < dh; ++i) oh[i] = dot(Sh + i * dh, qh, dh);
        if (mix == CNET_VSA_RLM_MIXER_SSD) for (int i = 0; i < dh; ++i) oh[i] += P(m->dskip[l])[hh * dh + i] * xt[hh * dh + i];
    }
    for (int i = 0; i < d; ++i) mm[i] = silu(gp[i]) * o[i];
    matvec(P(m->wo[l]), mm, y, d, d, d);
    for (int i = 0; i < d; ++i) xmid[i] = xin[i] + y[i];
    m->c_rms2[(size_t)l * m->T + t] = rmsnorm(xmid, P(m->n2[l]), n2, d);
    matvec(P(m->w1[l]), n2, hp, F, d, d);
    for (int i = 0; i < F; ++i) h[i] = silu(hp[i]);
    matvec(P(m->w2[l]), h, y, d, F, F);
    for (int i = 0; i < d; ++i) xout[i] = xmid[i] + y[i];
}

/* ---------------------------------------------------------------- backward ------------ */
/* Token-level backward: everything that flows through the recurrence and the residual stream. Output
 * gradients of the linear maps are cached; their matrix gradients are accumulated in layer_grads. */
static void layer_backward(cnet_vsa_rlm *m, int l, int t, const R *dxout, R *dxin) {
    const int d = m->d, H = m->H, dh = m->dh, F = m->F, K = m->K, mix = m->cfg.mixer;
    const R *xin = C(m->c_xin, l, t, d), *n = C(m->c_n, l, t, d), *xt = C(m->c_xt, l, t, d), *q = C(m->c_q, l, t, d), *k = C(m->c_k, l, t, d), *kn = C(m->c_kn, l, t, d), *v = C(m->c_v, l, t, d);
    const R *o = C(m->c_o, l, t, d), *gp = C(m->c_gp, l, t, d), *xmid = C(m->c_xmid, l, t, d), *hp = C(m->c_hp, l, t, F);
    const R *Sb = C(m->c_Sb, l, t, d * dh);
    const R *Sa = (t + 1 < m->last_n) ? C(m->c_Sb, l, t + 1, d * dh) : m->S + (size_t)l * d * dh;   /* state after this token */
    R *dSr = m->dS + (size_t)l * d * dh;
    R *dxmid = C(m->c_dxmid, l, t, d), *dhp = C(m->c_dhp, l, t, F), *dgp = C(m->c_dgp, l, t, d), *dq = C(m->c_dq, l, t, d), *dk = C(m->c_dk, l, t, d), *dv = C(m->c_dv, l, t, d);
    R dn2[MAXD], dm[MAXD], do_[MAXD], dxt[MAXD], dn[MAXD], dprev[MAXDH * MAXDH];
    memset(dn2, 0, sizeof(R) * d); memset(dm, 0, sizeof(R) * d); memset(dxt, 0, sizeof(R) * d); memset(dq, 0, sizeof(R) * d); memset(dk, 0, sizeof(R) * d); memset(dv, 0, sizeof(R) * d); memset(dn, 0, sizeof(R) * d);
    /* MLP */
    memcpy(C(m->c_dxout, l, t, d), dxout, sizeof(R) * d);
    memcpy(dxmid, dxout, sizeof(R) * d);
    memset(dhp, 0, sizeof(R) * F); matvec_dx(P(m->w2[l]), dxout, dhp, d, F, F);
    for (int i = 0; i < F; ++i) dhp[i] *= dsilu(hp[i]);
    matvec_dx(P(m->w1[l]), dhp, dn2, F, d, d);
    rmsnorm_bwd(xmid, P(m->n2[l]), G(m->n2[l]), m->c_rms2[(size_t)l * m->T + t], dn2, dxmid, d);
    /* output projection and gate */
    memcpy(dxin, dxmid, sizeof(R) * d);
    matvec_dx(P(m->wo[l]), dxmid, dm, d, d, d);
    for (int i = 0; i < d; ++i) { do_[i] = dm[i] * silu(gp[i]); dgp[i] = dm[i] * o[i] * dsilu(gp[i]); }
    matvec_dx(P(m->wg[l]), dgp, dxt, d, d, d);
    /* recurrence, per head */
    for (int hh = 0; hh < H; ++hh) {
        const R *Sbh = Sb + (size_t)hh * dh * dh, *Sah = Sa + (size_t)hh * dh * dh, *qh = q + hh * dh, *kh = k + hh * dh, *knh = kn + hh * dh, *vh = v + hh * dh;
        R *dS = dSr + (size_t)hh * dh * dh, *dqh = dq + hh * dh, *dkh = dk + hh * dh, *dvh = dv + hh * dh; const R *doh = do_ + hh * dh;
        if (mix == CNET_VSA_RLM_MIXER_SSD) for (int i = 0; i < dh; ++i) { G(m->dskip[l])[hh * dh + i] += doh[i] * xt[hh * dh + i]; dxt[hh * dh + i] += doh[i] * P(m->dskip[l])[hh * dh + i]; }
        /* o = S_after q */
        for (int i = 0; i < dh; ++i) { axpy(dqh, Sah + i * dh, doh[i], dh); axpy(dS + i * dh, qh, doh[i], dh); }
        if (mix == CNET_VSA_RLM_MIXER_DELTANET) {
            R bet = C(m->c_bet, l, t, H)[hh], alp = C(m->c_alp, l, t, H)[hh], u[MAXDH], z[MAXDH], dkn[MAXDH], dalp = 0, dbet = 0;
            memset(dkn, 0, sizeof(R) * dh);
            for (int i = 0; i < dh; ++i) { u[i] = dot(Sbh + i * dh, knh, dh); z[i] = dot(dS + i * dh, knh, dh); }
            for (int i = 0; i < dh; ++i) {
                const R *ds = dS + i * dh, *sb = Sbh + i * dh; R *dp = dprev + i * dh, zi = z[i], ui = u[i], vi = vh[i];
                dvh[i] += bet * zi; dbet += zi * (vi - alp * ui);
                dalp += dot(ds, sb, dh) - bet * ui * dot(ds, knh, dh);
SIMD
                for (int j = 0; j < dh; ++j) dp[j] = alp * ds[j] - alp * bet * zi * knh[j];
                /* dk (unit key): -alpha beta (S'^T z + dS^T u) + beta dS^T v */
                axpy(dkn, sb, -alp * bet * zi, dh); axpy(dkn, ds, bet * vi - alp * bet * ui, dh);
            }
            R inv = (R)(1.0 / sqrt((double)dot(kh, kh, dh) + 1e-8)), dd = dot(dkn, knh, dh);
            for (int i = 0; i < dh; ++i) dkh[i] += (dkn[i] - dd * knh[i]) * inv;
            R dzb = dbet * bet * (1 - bet), dza = dalp * alp * (-(1 - alp));   /* alpha = exp(-softplus(za)) => sigm(za) = 1 - alpha */
            C(m->c_dzb, l, t, H)[hh] = dzb; C(m->c_dza, l, t, H)[hh] = dza;
            axpy(dxt, P(m->wb[l]) + (size_t)hh * (d + 1), dzb, d); axpy(dxt, P(m->wa[l]) + (size_t)hh * (d + 1), dza, d);
        } else if (mix == CNET_VSA_RLM_MIXER_RWKV7) {
            const R *wv = C(m->c_w, l, t, d) + hh * dh, *av = C(m->c_a, l, t, d) + hh * dh, *kap = C(m->c_kap, l, t, d) + hh * dh;
            R Sk[MAXDH], dSka[MAXDH], ka[MAXDH], dkap[MAXDH], dka[MAXDH], dw[MAXDH];
            memset(dkap, 0, sizeof(R) * dh); memset(dka, 0, sizeof(R) * dh); memset(dw, 0, sizeof(R) * dh);
            for (int j = 0; j < dh; ++j) ka[j] = av[j] * kap[j];
            /* S = S' D + v k^T with D = diag(w) - kap ka^T, all in O(dh^2):
             *   dS'_ip = dS_ip w_p - (dS ka)_i kap_p;  dw_j = sum_i S'_ij dS_ij;
             *   dkap_p = -(S'^T (dS ka))_p;  dka_j = -(dS^T (S' kap))_j;  dv = dS k;  dk = dS^T v */
            for (int i = 0; i < dh; ++i) { const R *ds = dS + i * dh, *sb = Sbh + i * dh; Sk[i] = dot(sb, kap, dh); dSka[i] = dot(ds, ka, dh); dvh[i] += dot(ds, kh, dh); }
            for (int i = 0; i < dh; ++i) {
                const R *ds = dS + i * dh, *sb = Sbh + i * dh; R *dp = dprev + i * dh;
                axpy(dkh, ds, vh[i], dh);
SIMD
                for (int j = 0; j < dh; ++j) { dw[j] += sb[j] * ds[j]; dp[j] = ds[j] * wv[j] - dSka[i] * kap[j]; }
                axpy(dkap, sb, -dSka[i], dh); axpy(dka, ds, -Sk[i], dh);
            }
            for (int j = 0; j < dh; ++j) dkap[j] += dka[j] * av[j];
            R dd = dot(dkap, kap, dh); double nn = 0;
            for (int i = 0; i < dh; ++i) { R zk = dot(P(m->wkap[l]) + (size_t)(hh * dh + i) * d, xt, d); nn += (double)zk * zk; }
            R inv = (R)(1.0 / sqrt(nn + 1e-8));
            for (int i = 0; i < dh; ++i) {
                int ch = hh * dh + i;
                R dkr = (dkap[i] - dd * kap[i]) * inv, dzw = dw[i] * wv[i] * (-(1 - wv[i])), dza = dka[i] * kap[i] * av[i] * (1 - av[i]);
                C(m->c_dzw, l, t, d)[ch] = dzw; C(m->c_dza7, l, t, d)[ch] = dza; C(m->c_dkr, l, t, d)[ch] = dkr;
                axpy(dxt, P(m->ww[l]) + (size_t)ch * (d + 1), dzw, d); axpy(dxt, P(m->wa7[l]) + (size_t)ch * (d + 1), dza, d); axpy(dxt, P(m->wkap[l]) + (size_t)ch * d, dkr, d);
            }
        } else {
            R dl = C(m->c_dl, l, t, H)[hh], alp = C(m->c_alp, l, t, H)[hh], A = (R)exp((double)P(m->alog[l])[hh]), dalp = 0, ddl = 0;
            for (int i = 0; i < dh; ++i) { const R *ds = dS + i * dh; dalp += dot(ds, Sbh + i * dh, dh); dvh[i] += dot(ds, kh, dh); axpy(dkh, ds, vh[i], dh); R *dp = dprev + i * dh;
SIMD
                for (int j = 0; j < dh; ++j) dp[j] = alp * ds[j]; }
            for (int i = 0; i < dh; ++i) { ddl += dvh[i] * xt[hh * dh + i]; dxt[hh * dh + i] += dvh[i] * dl; }
            ddl += dalp * alp * (-A); G(m->alog[l])[hh] += dalp * alp * (-dl) * A;
            R dzd = ddl * (1 - (R)exp(-(double)dl));   /* softplus'(zd) = sigm(zd) = 1 - exp(-dl) */
            C(m->c_dzd, l, t, H)[hh] = dzd;
            axpy(dxt, P(m->wd[l]) + (size_t)hh * (d + 1), dzd, d);
        }
        memcpy(dS, dprev, sizeof(R) * dh * dh);
    }
    matvec_dx(P(m->wq[l]), dq, dxt, d, d, d);
    matvec_dx(P(m->wk[l]), dk, dxt, d, d, d);
    if (mix != CNET_VSA_RLM_MIXER_SSD) matvec_dx(P(m->wv[l]), dv, dxt, d, d, d);
    if (mix == CNET_VSA_RLM_MIXER_RWKV7) {
        const R *mu = P(m->mu[l]); R *gmu = G(m->mu[l]); const R *np = (t > 0) ? C(m->c_n, l, t - 1, d) : m->nprev0 + (size_t)l * d; R *carry = m->c_dnprev + (size_t)l * d;
        for (int i = 0; i < d; ++i) { gmu[i] += dxt[i] * (n[i] - np[i]); dn[i] += dxt[i] * mu[i] + carry[i]; carry[i] = dxt[i] * (1 - mu[i]); }
    } else {
        const R *c = C(m->c_c, l, t, d), *cw = P(m->cw[l]), *cv0 = m->conv0 + (size_t)l * d * (K - 1); R *gcw = G(m->cw[l]), *gcb = G(m->cb[l]), *du = C(m->c_du, l, t, d);
        for (int i = 0; i < d; ++i) {
            R dc = dxt[i] * dsilu(c[i]); gcb[i] += dc;
            for (int j = 0; j < K; ++j) {
                int ts = t - (K - 1) + j;
                if (ts >= 0) { gcw[(size_t)i * K + j] += dc * C(m->c_u, l, ts, d)[i]; C(m->c_du, l, ts, d)[i] += dc * cw[(size_t)i * K + j]; }
                else gcw[(size_t)i * K + j] += dc * cv0[(size_t)i * (K - 1) + (t + j)];   /* chunk-start history: no gradient to u */
            }
        }
        matvec_dx(P(m->wu[l]), du, dn, d, d, d);
    }
    rmsnorm_bwd(xin, P(m->n1[l]), G(m->n1[l]), m->c_rms1[(size_t)l * m->T + t], dn, dxin, d);
}

/* chunk-level matrix gradients for one layer */
static void layer_grads(cnet_vsa_rlm *m, int l, int n) {
    const int d = m->d, H = m->H, F = m->F, mix = m->cfg.mixer;
    gemm_acc(G(m->w2[l]), C(m->c_dxout, l, 0, d), d, C(m->c_h, l, 0, F), F, d, F, F, n);
    gemm_acc(G(m->w1[l]), C(m->c_dhp, l, 0, F), F, C(m->c_n2, l, 0, d), d, F, d, d, n);
    gemm_acc(G(m->wo[l]), C(m->c_dxmid, l, 0, d), d, C(m->c_m, l, 0, d), d, d, d, d, n);
    gemm_acc(G(m->wg[l]), C(m->c_dgp, l, 0, d), d, C(m->c_xt, l, 0, d), d, d, d, d, n);
    gemm_acc(G(m->wq[l]), C(m->c_dq, l, 0, d), d, C(m->c_xt, l, 0, d), d, d, d, d, n);
    gemm_acc(G(m->wk[l]), C(m->c_dk, l, 0, d), d, C(m->c_xt, l, 0, d), d, d, d, d, n);
    if (mix != CNET_VSA_RLM_MIXER_SSD) gemm_acc(G(m->wv[l]), C(m->c_dv, l, 0, d), d, C(m->c_xt, l, 0, d), d, d, d, d, n);
    if (mix != CNET_VSA_RLM_MIXER_RWKV7) gemm_acc(G(m->wu[l]), C(m->c_du, l, 0, d), d, C(m->c_n, l, 0, d), d, d, d, d, n);
    if (mix == CNET_VSA_RLM_MIXER_DELTANET) {
        gemm_acc(G(m->wb[l]), C(m->c_dzb, l, 0, H), H, C(m->c_xt, l, 0, d), d, H, d, d + 1, n);
        gemm_acc(G(m->wa[l]), C(m->c_dza, l, 0, H), H, C(m->c_xt, l, 0, d), d, H, d, d + 1, n);
    } else if (mix == CNET_VSA_RLM_MIXER_RWKV7) {
        gemm_acc(G(m->ww[l]), C(m->c_dzw, l, 0, d), d, C(m->c_xt, l, 0, d), d, d, d, d + 1, n);
        gemm_acc(G(m->wa7[l]), C(m->c_dza7, l, 0, d), d, C(m->c_xt, l, 0, d), d, d, d, d + 1, n);
        gemm_acc(G(m->wkap[l]), C(m->c_dkr, l, 0, d), d, C(m->c_xt, l, 0, d), d, d, d, d, n);
    } else {
        gemm_acc(G(m->wd[l]), C(m->c_dzd, l, 0, H), H, C(m->c_xt, l, 0, d), d, H, d, d + 1, n);
    }
}

double cnet_vsa_rlm_chunk(cnet_vsa_rlm *m, const int *tokens, int n, int do_backward) { return cnet_vsa_rlm_chunk_masked(m, tokens, n, NULL, do_backward); }
double cnet_vsa_rlm_chunk_masked(cnet_vsa_rlm *m, const int *tokens, int n, const unsigned char *mask, int do_backward) {
    if (!m || !tokens || n < 1 || n > m->T) return -1;
    int cnt = 0; if (mask) { for (int t = 0; t < n; ++t) cnt += mask[t] != 0; if (!cnt) return 0; } else cnt = n;
    const int d = m->d, V = m->V, L = m->L;
    m->last_n = n;
    memcpy(m->nprev0, m->nprev, sizeof(R) * (size_t)L * d); memcpy(m->conv0, m->conv, sizeof(R) * (size_t)L * d * (m->K - 1));
    double loss = 0;
    R x[MAXD], y[MAXD];
    for (int t = 0; t < n; ++t) {
        memcpy(x, P(m->emb) + (size_t)tokens[t] * d, sizeof(R) * d);
        for (int l = 0; l < L; ++l) { layer_forward(m, l, t, x, y); memcpy(x, y, sizeof(R) * d); }
        memcpy(m->c_xpre + (size_t)t * d, x, sizeof(R) * d);
        m->c_rmsf[t] = rmsnorm(x, P(m->normf), m->c_xf + (size_t)t * d, d);
    }
    gemm_out(P(m->emb), m->c_xf, m->c_prob, V, d, n);
    for (int t = 0; t < n; ++t) {
        R *pr = m->c_prob + (size_t)t * V;
        R mx = pr[0]; for (int v = 1; v < V; ++v) if (pr[v] > mx) mx = pr[v];
        double sum = 0; for (int v = 0; v < V; ++v) { pr[v] = (R)exp((double)(pr[v] - mx)); sum += pr[v]; }
        for (int v = 0; v < V; ++v) pr[v] = (R)(pr[v] / sum);
        if (!mask || mask[t]) loss += -log((double)pr[tokens[t + 1]] + 1e-30);
    }
    loss /= cnt;
    if (!do_backward) return loss;
    memset(m->dS, 0, sizeof(R) * (size_t)L * d * m->dh);
    memset(m->c_du, 0, sizeof(R) * (size_t)L * m->T * d); memset(m->c_dnprev, 0, sizeof(R) * (size_t)L * d);
    for (int t = 0; t < n; ++t) { R *pr = m->c_prob + (size_t)t * V;   /* dlogits = (p - onehot) / count, zero off the mask */
        if (mask && !mask[t]) memset(pr, 0, sizeof(R) * V); else { pr[tokens[t + 1]] -= 1; for (int v = 0; v < V; ++v) pr[v] /= (R)cnt; } }
    memset(m->c_dxf, 0, sizeof(R) * (size_t)n * d);
    gemm_dx(P(m->emb), m->c_prob, m->c_dxf, V, d, n);
    R dx[MAXD], dxin[MAXD];
    for (int t = n - 1; t >= 0; --t) {
        memset(dx, 0, sizeof(R) * d);
        rmsnorm_bwd(m->c_xpre + (size_t)t * d, P(m->normf), G(m->normf), m->c_rmsf[t], m->c_dxf + (size_t)t * d, dx, d);
        for (int l = L - 1; l >= 0; --l) { layer_backward(m, l, t, dx, dxin); memcpy(dx, dxin, sizeof(R) * d); }
        axpy(G(m->emb) + (size_t)tokens[t] * d, dx, 1, d);
    }
    gemm_acc(G(m->emb), m->c_prob, V, m->c_xf, d, V, d, d, n);
    for (int l = 0; l < L; ++l) layer_grads(m, l, n);
    return loss;
}

double cnet_vsa_rlm_grad_clip(cnet_vsa_rlm *m, double max_norm) {
    double ss = 0; for (size_t i = 0; i < m->np; ++i) ss += (double)m->g[i] * m->g[i];
    double nrm = sqrt(ss);
    if (max_norm > 0 && nrm > max_norm) { R s = (R)(max_norm / nrm); for (size_t i = 0; i < m->np; ++i) m->g[i] *= s; }
    return nrm;
}

void cnet_vsa_rlm_adam(cnet_vsa_rlm *m, float lr, int step) {
    if (m->shared || !m->am) return;
    const double b1 = 0.9, b2 = 0.999, e = 1e-8, c1 = 1.0 - pow(b1, step), c2 = 1.0 - pow(b2, step);
    OMP(omp parallel for)
    for (size_t i = 0; i < m->np; ++i) {
        double gg = m->g[i]; m->am[i] = (R)(b1 * m->am[i] + (1 - b1) * gg); m->av[i] = (R)(b2 * m->av[i] + (1 - b2) * gg * gg);
        m->p[i] -= (R)(lr * ((m->am[i] / c1) / (sqrt(m->av[i] / c2) + e)));
    }
    if (m->cfg.weight_decay > 0) {   /* decoupled, matrices only */
        R f = (R)(1.0 - lr * m->cfg.weight_decay);
        for (size_t i = 0; i < m->emb.n; ++i) m->p[m->emb.off + i] *= f;
        for (int l = 0; l < m->L; ++l) {
            Slot *ms[9]; int k = 0; ms[k++] = &m->wq[l]; ms[k++] = &m->wk[l]; ms[k++] = &m->wv[l]; ms[k++] = &m->wg[l]; ms[k++] = &m->wo[l]; ms[k++] = &m->w1[l]; ms[k++] = &m->w2[l];
            if (m->cfg.mixer == CNET_VSA_RLM_MIXER_RWKV7) ms[k++] = &m->wkap[l];
            if (m->cfg.mixer != CNET_VSA_RLM_MIXER_RWKV7) ms[k++] = &m->wu[l];
            for (int s = 0; s < k; ++s) for (size_t i = 0; i < ms[s]->n; ++i) m->p[ms[s]->off + i] *= f;
        }
    }
}

void cnet_vsa_rlm_predict(cnet_vsa_rlm *m, const int *tokens, int n, rlm_real *logits) {
    const int d = m->d, V = m->V, L = m->L;
    R x[MAXD], y[MAXD], xf[MAXD];
    m->last_n = 1;
    for (int t = 0; t < n; ++t) {
        memcpy(x, P(m->emb) + (size_t)tokens[t] * d, sizeof(R) * d);
        for (int l = 0; l < L; ++l) { layer_forward(m, l, 0, x, y); memcpy(x, y, sizeof(R) * d); }
    }
    rmsnorm(x, P(m->normf), xf, d);
    matvec(P(m->emb), xf, logits, V, d, d);
}

double cnet_vsa_rlm_grad_check(cnet_vsa_rlm *m, const int *tokens, int n, double eps) {
    const size_t st = (size_t)m->L * m->d * m->dh, nl = (size_t)m->L * m->d, nc = (size_t)m->L * m->d * (m->K - 1);
    R *dir = (R *)malloc(sizeof(R) * m->np), *S0 = (R *)malloc(sizeof(R) * st), *np0 = (R *)malloc(sizeof(R) * nl), *cv0 = (R *)malloc(sizeof(R) * nc);
    uint64_t s = 12345; double nn = 0;
    for (size_t i = 0; i < m->np; ++i) { dir[i] = randn(&s); nn += (double)dir[i] * dir[i]; }
    for (size_t i = 0; i < m->np; ++i) dir[i] = (R)(dir[i] / sqrt(nn));
    memcpy(S0, m->S, sizeof(R) * st); memcpy(np0, m->nprev, sizeof(R) * nl); memcpy(cv0, m->conv, sizeof(R) * nc);
#define RESTORE() do { memcpy(m->S, S0, sizeof(R) * st); memcpy(m->nprev, np0, sizeof(R) * nl); memcpy(m->conv, cv0, sizeof(R) * nc); } while (0)
    cnet_vsa_rlm_zero_grad(m); RESTORE(); cnet_vsa_rlm_chunk(m, tokens, n, 1);
    double gd = 0; for (size_t i = 0; i < m->np; ++i) gd += (double)m->g[i] * dir[i];
    for (size_t i = 0; i < m->np; ++i) m->p[i] += (R)(eps * dir[i]);
    RESTORE(); double lp = cnet_vsa_rlm_chunk(m, tokens, n, 0);
    for (size_t i = 0; i < m->np; ++i) m->p[i] -= (R)(2 * eps * dir[i]);
    RESTORE(); double lm = cnet_vsa_rlm_chunk(m, tokens, n, 0);
    for (size_t i = 0; i < m->np; ++i) m->p[i] += (R)(eps * dir[i]);
    RESTORE();
#undef RESTORE
    double fd = (lp - lm) / (2 * eps);
    free(dir); free(S0); free(np0); free(cv0);
    return fabs(fd - gd) / (fabs(gd) + 1e-12);
}

int cnet_vsa_rlm_save(const cnet_vsa_rlm *m, const char *path) {
    FILE *f = fopen(path, "wb"); if (!f) return -1;
    uint32_t magic = 0x4D4C5243u, rs = (uint32_t)sizeof(R); uint64_t np = m->np;
    int ok = fwrite(&magic, 4, 1, f) == 1 && fwrite(&rs, 4, 1, f) == 1 && fwrite(&m->cfg, sizeof(m->cfg), 1, f) == 1 && fwrite(&np, 8, 1, f) == 1 && fwrite(m->p, sizeof(R), m->np, f) == m->np;
    fclose(f); return ok ? 0 : -2;
}
cnet_vsa_rlm *cnet_vsa_rlm_load(const char *path) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    uint32_t magic = 0, rs = 0; cnet_vsa_rlm_cfg cfg; uint64_t np = 0;
    if (fread(&magic, 4, 1, f) != 1 || fread(&rs, 4, 1, f) != 1 || fread(&cfg, sizeof(cfg), 1, f) != 1 || fread(&np, 8, 1, f) != 1 || magic != 0x4D4C5243u || rs != sizeof(R)) { fclose(f); return NULL; }
    cnet_vsa_rlm *m = cnet_vsa_rlm_create(&cfg); if (!m || m->np != np) { fclose(f); cnet_vsa_rlm_free(m); return NULL; }
    if (fread(m->p, sizeof(R), m->np, f) != m->np) { fclose(f); cnet_vsa_rlm_free(m); return NULL; }
    fclose(f); return m;
}
