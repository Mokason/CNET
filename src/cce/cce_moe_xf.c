/* Per-token transformer MoE block trainer (see header). Single-head causal
 * attention + dense-expert MoE FFN; every heavy matmul dispatches to a CPU
 * reference or cce_clgemm on the GPU. Explicit forward+backward, AdamW. */

#include "../../include/cce/cce_moe_xf.h"
#include "../../include/cce/cce_clgemm.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { float *w, *g, *m, *v; int n; } Param;

struct cce_moe_xf {
    cce_moe_xf_cfg c;
    float *emb, *Wq, *Wk, *Wv, *Wo, *Wr, *W1, *b1, *W2, *Wh, *bh, *pos;
    Param p[12]; int np;
    /* caches (per sequence) */
    float *X, *Q, *K, *V, *ATT, *CTX, *X1, *G, *He, *Ue, *X2, *P;
    float usage[64];
    /* scratch */
    float *WT, *tmpTN, *tmpTK, *tmpTf, *sd, *sE;
    int cap_seq;
    cce_clgemm *gpu; char dev[96];
    uint32_t rng;
};

static uint32_t xr(struct cce_moe_xf *m) { m->rng = m->rng * 1664525u + 1013904223u; return m->rng; }
static float gauss(struct cce_moe_xf *m) { float s = 0; int i; for (i = 0; i < 4; i++) s += (float)(xr(m) >> 8) / (float)(1u << 24); return (s - 2.0f) * 1.7320508f; }
static void reg(struct cce_moe_xf *m, float **slot, int n, float sc) {
    Param p; p.w = calloc((size_t)n, 4); p.g = calloc((size_t)n, 4); p.m = calloc((size_t)n, 4); p.v = calloc((size_t)n, 4); p.n = n;
    int i; for (i = 0; i < n; i++) p.w[i] = sc > 0 ? gauss(m) * sc : 0.0f;
    m->p[m->np++] = p; *slot = p.w;
}

cce_moe_xf *cce_moe_xf_create(const cce_moe_xf_cfg *cfg) {
    if (!cfg || cfg->vocab < 2 || cfg->d_model < 1 || cfg->seq < 1 || cfg->n_expert < 1 ||
        cfg->top_k < 1 || cfg->top_k > cfg->n_expert || cfg->d_ff < 1 || cfg->n_expert > 64) return NULL;
    struct cce_moe_xf *m = calloc(1, sizeof *m);
    m->c = *cfg; m->rng = cfg->seed ? cfg->seed : 0x9e3779b9u;
    int V = cfg->vocab, d = cfg->d_model, E = cfg->n_expert, f = cfg->d_ff, T = cfg->seq;
    float s1 = 1.0f / sqrtf((float)d), sf = 1.0f / sqrtf((float)f);
    reg(m, &m->emb, V * d, 0.1f);
    reg(m, &m->Wq, d * d, s1); reg(m, &m->Wk, d * d, s1); reg(m, &m->Wv, d * d, s1); reg(m, &m->Wo, d * d, s1);
    reg(m, &m->Wr, d * E, s1);
    reg(m, &m->W1, E * d * f, s1); reg(m, &m->b1, E * f, 0.0f); reg(m, &m->W2, E * f * d, sf);
    reg(m, &m->Wh, d * V, s1); reg(m, &m->bh, V, 0.0f);
    reg(m, &m->pos, T * d, 0.02f);   /* learned positional embedding (param 11) */
    m->X = calloc((size_t)T * d, 4); m->Q = calloc((size_t)T * d, 4); m->K = calloc((size_t)T * d, 4); m->V = calloc((size_t)T * d, 4);
    m->ATT = calloc((size_t)T * T, 4); m->CTX = calloc((size_t)T * d, 4); m->X1 = calloc((size_t)T * d, 4);
    m->G = calloc((size_t)T * E, 4); m->He = calloc((size_t)T * E * f, 4); m->Ue = calloc((size_t)T * E * d, 4);
    m->X2 = calloc((size_t)T * d, 4); m->P = calloc((size_t)T * V, 4);
    int mx = d > V ? d : V; if (f > mx) mx = f;
    m->WT = calloc((size_t)(d > f ? d : f) * (d > V ? d : V), 4);
    m->tmpTN = calloc((size_t)T * mx, 4); m->tmpTK = calloc((size_t)T * mx, 4); m->tmpTf = calloc((size_t)T * f, 4);
    m->sd = calloc((size_t)mx, 4); m->sE = calloc((size_t)E, 4);
    m->cap_seq = T; strcpy(m->dev, "cpu");
    return m;
}

void cce_moe_xf_free(cce_moe_xf *m) {
    int i;
    if (!m) return;
    for (i = 0; i < m->np; i++) { free(m->p[i].w); free(m->p[i].g); free(m->p[i].m); free(m->p[i].v); }
    free(m->X); free(m->Q); free(m->K); free(m->V); free(m->ATT); free(m->CTX); free(m->X1);
    free(m->G); free(m->He); free(m->Ue); free(m->X2); free(m->P);
    free(m->WT); free(m->tmpTN); free(m->tmpTK); free(m->tmpTf); free(m->sd); free(m->sE);
    if (m->gpu) cce_clgemm_close(m->gpu);
    free(m);
}

int cce_moe_xf_use_gpu(cce_moe_xf *m, int on) {
    if (!on) { if (m->gpu) { cce_clgemm_close(m->gpu); m->gpu = NULL; } strcpy(m->dev, "cpu"); return 0; }
    if (!m->gpu) { m->gpu = cce_clgemm_open(NULL, m->dev, sizeof m->dev); if (!m->gpu) { strcpy(m->dev, "cpu"); return 0; } }
    return 1;
}
const char *cce_moe_xf_device(const cce_moe_xf *m) { return m->dev; }

/* ---- matmul dispatch (CPU reference / GPU cce_clgemm) --------------------- */
/* C[T,N] = A[T,K] . W[K,N] (+ bias) */
static void mm(cce_moe_xf *m, const float *A, int T, int K, const float *W, const float *bias, int N, float *C) {
    if (m->gpu && (long)T * K * N >= 1 << 15) { if (cce_clgemm_matmul_ephemeral(m->gpu, A, T, K, W, bias, N, C) == 0) return; }
    int t, k, n;
    for (t = 0; t < T; t++) for (n = 0; n < N; n++) { float s = bias ? bias[n] : 0.0f; const float *a = A + (size_t)t * K; for (k = 0; k < K; k++) s += a[k] * W[k * N + n]; C[(size_t)t * N + n] = s; }
}
/* dX[T,K] = dY[T,N] . W[K,N]^T  (W transposed) */
static void mmT(cce_moe_xf *m, const float *dY, int T, int N, const float *W, int K, float *dX) {
    if (m->gpu && (long)T * K * N >= 1 << 15) {
        int k, n; for (k = 0; k < K; k++) for (n = 0; n < N; n++) m->WT[(size_t)n * K + k] = W[(size_t)k * N + n]; /* WT[N,K] */
        if (cce_clgemm_matmul_ephemeral(m->gpu, dY, T, N, m->WT, NULL, K, dX) == 0) return;
    }
    int t, k, n;
    for (t = 0; t < T; t++) for (k = 0; k < K; k++) { float s = 0; const float *dy = dY + (size_t)t * N; for (n = 0; n < N; n++) s += dy[n] * W[(size_t)k * N + n]; dX[(size_t)t * K + k] = s; }
}
/* dW[K,N] += A[T,K]^T . dY[T,N] */
static void wg(cce_moe_xf *m, const float *A, int T, int K, const float *dY, int N, float *dW) {
    if (m->gpu && (long)T * K * N >= 1 << 15) {
        if (cce_clgemm_ste_XT_dY(m->gpu, A, dY, T, K, N, m->WT) == 0) { int i, tot = K * N; for (i = 0; i < tot; i++) dW[i] += m->WT[i]; return; }
    }
    int t, k, n;
    for (k = 0; k < K; k++) for (n = 0; n < N; n++) { float s = 0; for (t = 0; t < T; t++) s += A[(size_t)t * K + k] * dY[(size_t)t * N + n]; dW[(size_t)k * N + n] += s; }
}

static void softmax(float *v, int n) { float mx = -1e30f, s = 0; int i; for (i = 0; i < n; i++) if (v[i] > mx) mx = v[i]; for (i = 0; i < n; i++) { v[i] = expf(v[i] - mx); s += v[i]; } for (i = 0; i < n; i++) v[i] /= s; }

double cce_moe_xf_seq(cce_moe_xf *m, const int *tokens, int do_backward, double *ce_out, double *aux_out) {
    const int V = m->c.vocab, d = m->c.d_model, E = m->c.n_expert, K = m->c.top_k, f = m->c.d_ff, T = m->c.seq;
    const float scale = 1.0f / sqrtf((float)d);
    int t, s, i, j, e, kk;
    float *X = m->X, *Q = m->Q, *Kk = m->K, *Vv = m->V, *ATT = m->ATT, *CTX = m->CTX, *X1 = m->X1;
    float *G = m->G, *He = m->He, *Ue = m->Ue, *X2 = m->X2, *P = m->P;

    /* ---- forward ---- */
    for (t = 0; t < T; t++) { const float *e0 = m->emb + (size_t)tokens[t] * d, *pp = m->pos + (size_t)t * d;
        float *xt = X + (size_t)t * d; for (i = 0; i < d; i++) xt[i] = e0[i] + pp[i]; }   /* token + positional */
    mm(m, X, T, d, m->Wq, NULL, d, Q); mm(m, X, T, d, m->Wk, NULL, d, Kk); mm(m, X, T, d, m->Wv, NULL, d, Vv);
    for (t = 0; t < T; t++) {                              /* causal single-head attention */
        float *a = ATT + (size_t)t * T; const float *q = Q + (size_t)t * d;
        for (s = 0; s <= t; s++) { float sc = 0; const float *k = Kk + (size_t)s * d; for (i = 0; i < d; i++) sc += q[i] * k[i]; a[s] = sc * scale; }
        softmax(a, t + 1);
        float *ct = CTX + (size_t)t * d; for (i = 0; i < d; i++) ct[i] = 0;
        for (s = 0; s <= t; s++) { float w = a[s]; const float *vv = Vv + (size_t)s * d; for (i = 0; i < d; i++) ct[i] += w * vv[i]; }
    }
    mm(m, CTX, T, d, m->Wo, NULL, d, m->tmpTN);            /* ao = ctx@Wo */
    for (i = 0; i < T * d; i++) X1[i] = X[i] + m->tmpTN[i]; /* residual */

    /* MoE (dense experts, sparse top-k gating) */
    mm(m, X1, T, d, m->Wr, NULL, E, G);                    /* router logits */
    for (e = 0; e < E; e++) m->usage[e] = 0;
    double aux = 0; float imp[64] = {0}, load[64] = {0};
    for (e = 0; e < E; e++) {                              /* all experts over all tokens */
        mm(m, X1, T, d, m->W1 + (size_t)e * d * f, m->b1 + (size_t)e * f, f, m->tmpTf);
        float *h = He + (size_t)e * T * f; for (i = 0; i < T * f; i++) h[i] = m->tmpTf[i] > 0 ? m->tmpTf[i] : 0;
        mm(m, h, T, f, m->W2 + (size_t)e * f * d, NULL, d, Ue + (size_t)e * T * d);
    }
    for (t = 0; t < T; t++) {                              /* softmax gates + top-k + combine */
        float *g = G + (size_t)t * E; softmax(g, E);
        int chosen[64] = {0};
        for (kk = 0; kk < K; kk++) { int best = -1; float bv = -1; for (e = 0; e < E; e++) if (!chosen[e] && g[e] > bv) { bv = g[e]; best = e; } chosen[best] = 1; }
        for (e = 0; e < E; e++) imp[e] += g[e];
        for (e = 0; e < E; e++) if (!chosen[e]) g[e] = 0; else load[e] += 1, m->usage[e] += 1;
        float *x2 = X2 + (size_t)t * d, *x1 = X1 + (size_t)t * d;
        for (i = 0; i < d; i++) x2[i] = x1[i];
        for (e = 0; e < E; e++) if (g[e] > 0) { const float *u = Ue + ((size_t)e * T + t) * d; for (i = 0; i < d; i++) x2[i] += g[e] * u[i]; }
    }
    for (e = 0; e < E; e++) { imp[e] /= T; load[e] /= T; m->usage[e] /= T; aux += (double)imp[e] * load[e]; }
    aux *= (double)m->c.lb_coef * E;

    mm(m, X2, T, d, m->Wh, m->bh, V, P);                  /* logits */
    double ce = 0;
    for (t = 0; t < T; t++) { float *pt = P + (size_t)t * V; softmax(pt, V); ce += -log((double)pt[tokens[t + 1]] + 1e-30); }
    ce /= T;
    if (ce_out) *ce_out = ce;
    if (aux_out) *aux_out = aux;
    if (!do_backward) return ce + aux;

    /* ---- backward ---- */
    float *dX2 = m->tmpTK, *dlog = m->tmpTN;               /* reuse scratch: dlog[T,V], dX2[T,d] */
    for (t = 0; t < T; t++) { float *dl = dlog + (size_t)t * V, *pt = P + (size_t)t * V; int tgt = tokens[t + 1];
        for (j = 0; j < V; j++) { dl[j] = (pt[j] - (j == tgt ? 1.0f : 0.0f)) / (float)T; m->p[10].g[j] += dl[j]; } }
    wg(m, X2, T, d, dlog, V, m->p[9].g);                  /* dWh */
    mmT(m, dlog, T, V, m->Wh, d, dX2);                    /* dX2 = dlog @ Wh^T */

    /* MoE backward. dX1 accumulates: residual (dX2) + router + experts. Its own
       buffer — CTX is still needed by the attention backward below. */
    static float *dX1buf = NULL; static int dX1cap = 0;
    if (dX1cap < T * d) { free(dX1buf); dX1buf = calloc((size_t)T * d, 4); dX1cap = T * d; }
    float *dX1 = dX1buf;
    for (i = 0; i < T * d; i++) dX1[i] = dX2[i];          /* residual x2 = x1 + moe */

    /* gates gradient (moe part + aux), then router + experts. The (masked) gate
       values g[t][e] used in forward are still in G. */
    {
        float *dg = m->sE; /* per-token [E] */
        for (t = 0; t < T; t++) {
            float *g = G + (size_t)t * E, *dx1 = dX1 + (size_t)t * d, *dmoe = dX2 + (size_t)t * d;
            for (e = 0; e < E; e++) {
                float ge = g[e];
                float s2 = 0; const float *u = Ue + ((size_t)e * T + t) * d;
                for (i = 0; i < d; i++) s2 += dmoe[i] * u[i];
                dg[e] = s2 + m->c.lb_coef * E * (m->usage[e]) / (float)T; /* moe + aux(importance) ; load treated const */
                /* dU_e[t] = ge * dmoe ; but we need per-expert dU over all tokens for the GEMM -> stash into Ue-grad buffer */
                float *du = m->Ue + ((size_t)e * T + t) * d; /* overwrite Ue with dU (Ue no longer needed) */
                for (i = 0; i < d; i++) du[i] = ge * dmoe[i];
            }
            /* router softmax backward over full E (dg includes non-selected via aux) */
            float gd = 0; float *gg = G + (size_t)t * E; /* NOTE: gg is masked (zeros); need full softmax g. Recompute. */
            (void)gd; (void)gg; (void)dx1;
            /* store dg into a per-token router-grad row (reuse P buffer row) */
            memcpy(m->P + (size_t)t * V, dg, (size_t)E * 4); /* stash dg (E<=V) */
        }
    }
    /* experts backward via GEMMs (dU stored in Ue): dW2, dHe, relu, dW1, dX1 */
    for (e = 0; e < E; e++) {
        float *du = m->Ue + (size_t)e * T * d, *h = He + (size_t)e * T * f;
        wg(m, h, T, f, du, d, m->p[8].g + (size_t)e * f * d);         /* dW2_e = h^T du */
        mmT(m, du, T, d, m->W2 + (size_t)e * f * d, f, m->tmpTf);     /* dHe = du @ W2^T  [T,f] */
        for (i = 0; i < T * f; i++) m->tmpTf[i] = h[i] > 0 ? m->tmpTf[i] : 0; /* relu grad (h post-relu) */
        wg(m, X1, T, d, m->tmpTf, f, m->p[6].g + (size_t)e * d * f);  /* dW1_e = X1^T dHpre */
        for (t = 0; t < T; t++) for (j = 0; j < f; j++) m->p[7].g[(size_t)e * f + j] += m->tmpTf[(size_t)t * f + j]; /* db1 */
        mmT(m, m->tmpTf, T, f, m->W1 + (size_t)e * d * f, d, m->tmpTK);/* dX1 += dHpre @ W1^T */
        for (i = 0; i < T * d; i++) dX1[i] += m->tmpTK[i];
    }
    /* router backward: recompute full softmax gates, dr = g*(dg - <g,dg>) into tmpTN[T,E]; dWr, dX1 */
    for (t = 0; t < T; t++) {
        float gfull[64]; const float *x1 = X1 + (size_t)t * d; const float *dg = m->P + (size_t)t * V;
        for (e = 0; e < E; e++) { float sc = 0; for (i = 0; i < d; i++) sc += x1[i] * m->Wr[i * E + e]; gfull[e] = sc; }
        softmax(gfull, E);
        float gd = 0; for (e = 0; e < E; e++) gd += gfull[e] * dg[e];
        float *dr = m->tmpTN + (size_t)t * E; for (e = 0; e < E; e++) dr[e] = gfull[e] * (dg[e] - gd);
    }
    wg(m, X1, T, d, m->tmpTN, E, m->p[5].g);              /* dWr */
    mmT(m, m->tmpTN, T, E, m->Wr, d, m->tmpTK); for (i = 0; i < T * d; i++) dX1[i] += m->tmpTK[i];

    /* attention backward. dX1 -> residual (dX) + ao. dAo = dX1 ; dX = dX1 */
    float *dX = m->tmpTK; for (i = 0; i < T * d; i++) dX[i] = dX1[i];   /* residual x1 = x + ao */
    wg(m, CTX, T, d, dX1, d, m->p[4].g);                  /* dWo = CTX^T dAo (dAo=dX1) */
    float *dCTX = m->Q + 0; /* reuse? Q needed for attn bwd. use fresh */
    static float *dCTXb = NULL, *dQb = NULL, *dKb = NULL, *dVb = NULL; static int abcap = 0;
    if (abcap < T * d) { free(dCTXb); free(dQb); free(dKb); free(dVb); dCTXb = calloc((size_t)T * d, 4); dQb = calloc((size_t)T * d, 4); dKb = calloc((size_t)T * d, 4); dVb = calloc((size_t)T * d, 4); abcap = T * d; }
    dCTX = dCTXb; float *dQ = dQb, *dK = dKb, *dV = dVb;
    mmT(m, dX1, T, d, m->Wo, d, dCTX);                    /* dCTX = dAo @ Wo^T */
    memset(dQ, 0, (size_t)T * d * 4); memset(dK, 0, (size_t)T * d * 4); memset(dV, 0, (size_t)T * d * 4);
    for (t = 0; t < T; t++) {
        const float *a = ATT + (size_t)t * T, *dct = dCTX + (size_t)t * d, *q = Q + (size_t)t * d;
        float da[512]; /* dATT[t][s] for s<=t ; seq<=512 */
        for (s = 0; s <= t; s++) { const float *vv = Vv + (size_t)s * d; float sda = 0; for (i = 0; i < d; i++) { sda += dct[i] * vv[i]; dV[(size_t)s * d + i] += a[s] * dct[i]; } da[s] = sda; }
        float adot = 0; for (s = 0; s <= t; s++) adot += a[s] * da[s];      /* softmax backward */
        for (s = 0; s <= t; s++) { float dsc = a[s] * (da[s] - adot) * scale; const float *k = Kk + (size_t)s * d;
            for (i = 0; i < d; i++) { dQ[(size_t)t * d + i] += dsc * k[i]; dK[(size_t)s * d + i] += dsc * q[i]; } }
    }
    wg(m, X, T, d, dQ, d, m->p[1].g); mmT(m, dQ, T, d, m->Wq, d, m->tmpTN); for (i = 0; i < T * d; i++) dX[i] += m->tmpTN[i];
    wg(m, X, T, d, dK, d, m->p[2].g); mmT(m, dK, T, d, m->Wk, d, m->tmpTN); for (i = 0; i < T * d; i++) dX[i] += m->tmpTN[i];
    wg(m, X, T, d, dV, d, m->p[3].g); mmT(m, dV, T, d, m->Wv, d, m->tmpTN); for (i = 0; i < T * d; i++) dX[i] += m->tmpTN[i];
    for (t = 0; t < T; t++) { float *eg = m->p[0].g + (size_t)tokens[t] * d, *pg = m->p[11].g + (size_t)t * d;
        const float *dx = dX + (size_t)t * d; for (i = 0; i < d; i++) { eg[i] += dx[i]; pg[i] += dx[i]; } }  /* emb + pos grad */
    return ce + aux;
}

void cce_moe_xf_zero_grad(cce_moe_xf *m) { int i; for (i = 0; i < m->np; i++) memset(m->p[i].g, 0, (size_t)m->p[i].n * 4); }

void cce_moe_xf_adam(cce_moe_xf *m, float lr, int step) {
    const float b1 = 0.9f, b2 = 0.999f, eps = 1e-8f, wd = m->c.weight_decay;
    float c1 = 1 - powf(b1, (float)step), c2 = 1 - powf(b2, (float)step); int pi, i;
    for (pi = 0; pi < m->np; pi++) { Param *p = &m->p[pi]; int decay = (pi != 7 && pi != 10 && pi != 11);
        for (i = 0; i < p->n; i++) { float g = p->g[i];
            p->m[i] = b1 * p->m[i] + (1 - b1) * g; p->v[i] = b2 * p->v[i] + (1 - b2) * g * g;
            p->w[i] -= lr * (p->m[i] / c1) / (sqrtf(p->v[i] / c2) + eps);
            if (wd > 0 && decay) p->w[i] -= lr * wd * p->w[i]; } }
}

/* ---- checkpointing (see header) ---- */

#define XF_CKPT_MAGIC "CCEMOEXF"
#define XF_CKPT_VERSION 1u

static unsigned long long xf_fnv(const void *buf, size_t n, unsigned long long h) {
    const unsigned char *p = (const unsigned char *)buf;
    size_t i;
    for (i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ULL; }
    return h;
}

/* write + accumulate the checksum in one place so save and load cannot drift */
static int xf_w(FILE *f, const void *buf, size_t n, unsigned long long *h) {
    *h = xf_fnv(buf, n, *h);
    return fwrite(buf, 1, n, f) == n ? 0 : -1;
}
static int xf_r(FILE *f, void *buf, size_t n, unsigned long long *h) {
    if (fread(buf, 1, n, f) != n) return -1;
    *h = xf_fnv(buf, n, *h);
    return 0;
}

int cce_moe_xf_save(const cce_moe_xf *m, const char *path, int step) {
    FILE *f;
    unsigned long long h = 14695981039346656037ULL;
    unsigned ver = XF_CKPT_VERSION;
    int pi, rc = 0;
    char tmp[1200];
    if (!m || !path || !path[0]) return -1;
    if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp) return -1;
    f = fopen(tmp, "wb");
    if (!f) return -2;
    rc |= xf_w(f, XF_CKPT_MAGIC, 8, &h);
    rc |= xf_w(f, &ver, sizeof ver, &h);
    rc |= xf_w(f, &m->c, sizeof m->c, &h);
    rc |= xf_w(f, &step, sizeof step, &h);
    rc |= xf_w(f, &m->rng, sizeof m->rng, &h);
    rc |= xf_w(f, &m->np, sizeof m->np, &h);
    for (pi = 0; pi < m->np && !rc; pi++) {
        const Param *p = &m->p[pi];
        size_t bytes = (size_t)p->n * sizeof(float);
        rc |= xf_w(f, &p->n, sizeof p->n, &h);
        rc |= xf_w(f, p->w, bytes, &h);
        rc |= xf_w(f, p->m, bytes, &h);   /* Adam first moment  */
        rc |= xf_w(f, p->v, bytes, &h);   /* Adam second moment */
    }
    if (!rc && fwrite(&h, sizeof h, 1, f) != 1) rc = -1;
    if (fclose(f) != 0) rc = -1;
    if (rc) { remove(tmp); return -3; }
    /* atomic publish: a crash mid-write must never leave a torn checkpoint */
    if (rename(tmp, path) != 0) { remove(tmp); return -4; }
    return 0;
}

cce_moe_xf *cce_moe_xf_load(const char *path, int *step_out) {
    FILE *f;
    unsigned long long h = 14695981039346656037ULL, stored = 0;
    unsigned ver = 0;
    char magic[8];
    cce_moe_xf_cfg cfg;
    cce_moe_xf *m = NULL;
    int step = 0, np = 0, pi;
    if (!path || !path[0]) return NULL;
    f = fopen(path, "rb");
    if (!f) return NULL;
    if (xf_r(f, magic, 8, &h) || memcmp(magic, XF_CKPT_MAGIC, 8) != 0) goto bad;
    if (xf_r(f, &ver, sizeof ver, &h) || ver != XF_CKPT_VERSION) goto bad;
    if (xf_r(f, &cfg, sizeof cfg, &h)) goto bad;
    if (xf_r(f, &step, sizeof step, &h)) goto bad;
    m = cce_moe_xf_create(&cfg);
    if (!m) goto bad;
    if (xf_r(f, &m->rng, sizeof m->rng, &h)) goto bad;
    if (xf_r(f, &np, sizeof np, &h) || np != m->np) goto bad;
    for (pi = 0; pi < np; pi++) {
        Param *p = &m->p[pi];
        int n = 0;
        size_t bytes;
        if (xf_r(f, &n, sizeof n, &h) || n != p->n) goto bad;
        bytes = (size_t)p->n * sizeof(float);
        if (xf_r(f, p->w, bytes, &h)) goto bad;
        if (xf_r(f, p->m, bytes, &h)) goto bad;
        if (xf_r(f, p->v, bytes, &h)) goto bad;
    }
    if (fread(&stored, sizeof stored, 1, f) != 1 || stored != h) goto bad;
    fclose(f);
    if (step_out) *step_out = step;
    return m;
bad:
    if (m) cce_moe_xf_free(m);
    fclose(f);
    return NULL;
}

/* Directional finite-difference check on the CURRENT device (call with GPU on to
   validate the GPU backward against the GPU forward). */
double cce_moe_xf_grad_check(cce_moe_xf *m, const int *tokens) {
    const double eps = 1e-4; int pi, i;
    cce_moe_xf_zero_grad(m); (void)cce_moe_xf_seq(m, tokens, 1, NULL, NULL);
    double gn2 = 0; for (pi = 0; pi < m->np; pi++) for (i = 0; i < m->p[pi].n; i++) gn2 += (double)m->p[pi].g[i] * m->p[pi].g[i];
    double gnorm = sqrt(gn2); if (gnorm < 1e-8) return 0;
    double sc = eps / gnorm;
    for (pi = 0; pi < m->np; pi++) for (i = 0; i < m->p[pi].n; i++) m->p[pi].w[i] += (float)(sc * m->p[pi].g[i]);
    double lp = cce_moe_xf_seq(m, tokens, 0, NULL, NULL);
    for (pi = 0; pi < m->np; pi++) for (i = 0; i < m->p[pi].n; i++) m->p[pi].w[i] -= (float)(2 * sc * m->p[pi].g[i]);
    double lm = cce_moe_xf_seq(m, tokens, 0, NULL, NULL);
    for (pi = 0; pi < m->np; pi++) for (i = 0; i < m->p[pi].n; i++) m->p[pi].w[i] += (float)(sc * m->p[pi].g[i]);
    double num = (lp - lm) / (2 * eps);
    return fabs(num - gnorm) / (fabs(num) + gnorm + 1e-12);
}
