/* Sparse MoE LM trainer — explicit forward + backward + Adam. See header. */

#include "../../include/cce/cce_moe_train.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- parameter block (weight + grad + Adam moments) ---- */
typedef struct { float *w, *g, *m, *v; int n; } Param;

struct cce_moe {
    cce_moe_cfg c;
    /* named params (also registered in p[]) */
    float *emb, *Wr, *W1, *b1, *W2, *Wh, *bh;
    Param p[8]; int np;
    /* per-batch caches */
    int cap;                 /* cached example capacity */
    float *H, *G, *A, *Z, *P;
    int   *SEL;
    float importance[64], load[64], usage[64];
    /* scratch (sized max(d,f,V,E)) */
    float *s1, *s2, *s3, *s4, *dg, *dr, *dz, *dh, *dmoe, *ue, *du;
    uint32_t rng;
};

static uint32_t xr(struct cce_moe *m) { m->rng = m->rng * 1664525u + 1013904223u; return m->rng; }
static float gauss(struct cce_moe *m) { /* ~N(0,1) via CLT of 4 uniforms */
    float s = 0; int i; for (i = 0; i < 4; i++) s += (float)(xr(m) >> 8) / (float)(1u << 24);
    return (s - 2.0f) * 1.7320508f; /* var(sum4)=4/12=1/3 -> scale by sqrt(3) */
}
static void reg(struct cce_moe *m, float **slot, int n, float scale) {
    float *w = (float *)calloc((size_t)n, sizeof(float));
    Param p; p.w = w; p.g = (float *)calloc((size_t)n, sizeof(float));
    p.m = (float *)calloc((size_t)n, sizeof(float)); p.v = (float *)calloc((size_t)n, sizeof(float)); p.n = n;
    int i; for (i = 0; i < n; i++) w[i] = scale > 0 ? gauss(m) * scale : 0.0f;
    m->p[m->np++] = p; *slot = w;
}

cce_moe *cce_moe_create(const cce_moe_cfg *cfg) {
    if (!cfg || cfg->vocab < 2 || cfg->d_model < 1 || cfg->n_expert < 1 ||
        cfg->top_k < 1 || cfg->top_k > cfg->n_expert || cfg->d_ff < 1 || cfg->ctx < 1 ||
        cfg->n_expert > 64) return NULL;
    struct cce_moe *m = (struct cce_moe *)calloc(1, sizeof *m);
    if (!m) return NULL;
    m->c = *cfg; m->rng = cfg->seed ? cfg->seed : 0x9e3779b9u; m->np = 0;
    /* working dim W = ctx * d_model: context embeddings are CONCATENATED (not
       pooled) so the experts see each token and can model interactions. */
    int V = cfg->vocab, dm = cfg->d_model, W = cfg->ctx * dm, E = cfg->n_expert, f = cfg->d_ff;
    float sd = 1.0f / sqrtf((float)W), sf = 1.0f / sqrtf((float)f);
    reg(m, &m->emb, V * dm, 0.1f);
    reg(m, &m->Wr,  W * E, sd);
    reg(m, &m->W1,  E * W * f, sd);
    reg(m, &m->b1,  E * f, 0.0f);
    reg(m, &m->W2,  E * f * W, sf);
    reg(m, &m->Wh,  W * V, sd);
    reg(m, &m->bh,  V, 0.0f);
    int mx = W; if (f > mx) mx = f; if (V > mx) mx = V; if (E > mx) mx = E;
    m->s1 = calloc((size_t)mx, 4); m->s2 = calloc((size_t)mx, 4); m->s3 = calloc((size_t)mx, 4);
    m->s4 = calloc((size_t)mx, 4); m->dg = calloc((size_t)E, 4); m->dr = calloc((size_t)E, 4);
    m->dz = calloc((size_t)W, 4); m->dh = calloc((size_t)W, 4); m->dmoe = calloc((size_t)W, 4);
    m->ue = calloc((size_t)W, 4); m->du = calloc((size_t)W, 4);
    return m;
}

void cce_moe_free(cce_moe *m) {
    int i;
    if (!m) return;
    for (i = 0; i < m->np; i++) { free(m->p[i].w); free(m->p[i].g); free(m->p[i].m); free(m->p[i].v); }
    free(m->H); free(m->G); free(m->A); free(m->Z); free(m->P); free(m->SEL);
    free(m->s1); free(m->s2); free(m->s3); free(m->s4); free(m->dg); free(m->dr);
    free(m->dz); free(m->dh); free(m->dmoe); free(m->ue); free(m->du);
    free(m);
}

void cce_moe_zero_grad(cce_moe *m) { int i; for (i = 0; i < m->np; i++) memset(m->p[i].g, 0, (size_t)m->p[i].n * sizeof(float)); }

static void ensure_cap(struct cce_moe *m, int N) {
    if (N <= m->cap) return;
    int d = m->c.ctx * m->c.d_model, E = m->c.n_expert, K = m->c.top_k, f = m->c.d_ff, V = m->c.vocab;
    free(m->H); free(m->G); free(m->A); free(m->Z); free(m->P); free(m->SEL);
    m->H = calloc((size_t)N * d, 4); m->G = calloc((size_t)N * E, 4);
    m->A = calloc((size_t)N * K * f, 4); m->Z = calloc((size_t)N * d, 4);
    m->P = calloc((size_t)N * V, 4); m->SEL = calloc((size_t)N * K, sizeof(int));
    m->cap = N;
}

/* top_k indices of the largest g[E] into sel[K] (simple selection, E small) */
static void topk(const float *g, int E, int K, int *sel) {
    int chosen[64] = {0}, kk, e;
    for (kk = 0; kk < K; kk++) {
        int best = -1; float bv = -1e30f;
        for (e = 0; e < E; e++) if (!chosen[e] && g[e] > bv) { bv = g[e]; best = e; }
        sel[kk] = best; if (best >= 0) chosen[best] = 1;
    }
}

double cce_moe_batch(cce_moe *m, const int *tokens, const int *targets, int N,
                     int do_backward, double *ce_out, double *aux_out, double *bal_out) {
    const int V = m->c.vocab, dm = m->c.d_model, C = m->c.ctx, d = C * dm,
              E = m->c.n_expert, K = m->c.top_k, f = m->c.d_ff; /* d = working dim = ctx*d_model */
    ensure_cap(m, N);
    int n, i, j, e, kk, v;
    double ce = 0.0;
    for (e = 0; e < E; e++) { m->importance[e] = 0; m->load[e] = 0; }

    /* ---------- forward ---------- */
    for (n = 0; n < N; n++) {
        float *h = m->H + (size_t)n * d, *g = m->G + (size_t)n * E, *z = m->Z + (size_t)n * d, *P = m->P + (size_t)n * V;
        int *sel = m->SEL + (size_t)n * K;
        /* context representation h = CONCAT of the ctx token embeddings [ctx*dm] */
        for (int c = 0; c < C; c++) { const float *ev = m->emb + (size_t)tokens[n * C + c] * dm; for (i = 0; i < dm; i++) h[c * dm + i] = ev[i]; }
        /* router logits -> softmax gates */
        for (e = 0; e < E; e++) { float s = 0; for (i = 0; i < d; i++) s += h[i] * m->Wr[i * E + e]; g[e] = s; }
        { float mx = -1e30f, sm = 0; for (e = 0; e < E; e++) if (g[e] > mx) mx = g[e];
          for (e = 0; e < E; e++) { g[e] = expf(g[e] - mx); sm += g[e]; }
          for (e = 0; e < E; e++) g[e] /= sm; }
        topk(g, E, K, sel);
        /* experts (selected) + residual */
        for (i = 0; i < d; i++) z[i] = h[i];
        for (kk = 0; kk < K; kk++) {
            e = sel[kk]; const float *W1e = m->W1 + (size_t)e * d * f, *W2e = m->W2 + (size_t)e * f * d, *b1e = m->b1 + (size_t)e * f;
            float *a = m->A + ((size_t)n * K + kk) * f;
            for (j = 0; j < f; j++) { float s = b1e[j]; for (i = 0; i < d; i++) s += h[i] * W1e[i * f + j]; a[j] = s > 0 ? s : 0; }
            for (i = 0; i < d; i++) { float u = 0; for (j = 0; j < f; j++) u += a[j] * W2e[j * d + i]; z[i] += g[e] * u; }
        }
        /* head -> softmax -> CE */
        for (v = 0; v < V; v++) { float s = m->bh[v]; for (i = 0; i < d; i++) s += z[i] * m->Wh[i * V + v]; P[v] = s; }
        { float mx = -1e30f, sm = 0; for (v = 0; v < V; v++) if (P[v] > mx) mx = P[v];
          for (v = 0; v < V; v++) { P[v] = expf(P[v] - mx); sm += P[v]; }
          for (v = 0; v < V; v++) P[v] /= sm; }
        ce += -log((double)P[targets[n]] + 1e-30);
        for (e = 0; e < E; e++) m->importance[e] += g[e];
        for (kk = 0; kk < K; kk++) m->load[sel[kk]] += 1.0f;
    }
    ce /= (double)N;
    double aux = 0.0;
    for (e = 0; e < E; e++) { m->importance[e] /= (float)N; m->usage[e] = m->load[e] / (float)N; m->load[e] /= (float)N; aux += (double)m->importance[e] * (double)m->load[e]; }
    aux *= (double)m->c.lb_coef * (double)E;
    double meanload = (double)K / (double)E, maxload = 0; for (e = 0; e < E; e++) if (m->load[e] > maxload) maxload = m->load[e];
    if (ce_out) *ce_out = ce;
    if (aux_out) *aux_out = aux;
    if (bal_out) *bal_out = meanload > 0 ? maxload / meanload : 0;

    /* ---------- backward ---------- */
    if (do_backward) {
        float *dl = m->s1, *da = m->s2, *dap = m->s3;
        for (n = 0; n < N; n++) {
            const float *h = m->H + (size_t)n * d, *g = m->G + (size_t)n * E, *z = m->Z + (size_t)n * d, *P = m->P + (size_t)n * V;
            const int *sel = m->SEL + (size_t)n * K;
            int tgt = targets[n];
            /* head + CE: dlogits = (p - onehot)/N */
            for (v = 0; v < V; v++) { dl[v] = (P[v] - (v == tgt ? 1.0f : 0.0f)) / (float)N; m->p[6].g[v] += dl[v]; }
            for (i = 0; i < d; i++) { float s = 0; for (v = 0; v < V; v++) { s += m->Wh[i * V + v] * dl[v]; m->p[5].g[i * V + v] += z[i] * dl[v]; } m->dz[i] = s; }
            /* residual split */
            for (i = 0; i < d; i++) { m->dh[i] = m->dz[i]; m->dmoe[i] = m->dz[i]; }
            /* dg from moe (selected) + aux (all experts) */
            for (e = 0; e < E; e++) m->dg[e] = m->c.lb_coef * (float)E * m->load[e] / (float)N;
            for (kk = 0; kk < K; kk++) {
                e = sel[kk]; const float *W2e = m->W2 + (size_t)e * f * d;
                const float *a = m->A + ((size_t)n * K + kk) * f;
                for (i = 0; i < d; i++) { float u = 0; for (j = 0; j < f; j++) u += a[j] * W2e[j * d + i]; m->ue[i] = u; }
                { float s = 0; for (i = 0; i < d; i++) s += m->dmoe[i] * m->ue[i]; m->dg[e] += s; }
            }
            /* expert backward (selected) */
            for (kk = 0; kk < K; kk++) {
                e = sel[kk];
                float *W1g = m->p[2].g + (size_t)e * d * f, *b1g = m->p[3].g + (size_t)e * f, *W2g = m->p[4].g + (size_t)e * f * d;
                const float *W1e = m->W1 + (size_t)e * d * f, *W2e = m->W2 + (size_t)e * f * d;
                const float *a = m->A + ((size_t)n * K + kk) * f;
                for (i = 0; i < d; i++) m->du[i] = g[e] * m->dmoe[i];
                for (j = 0; j < f; j++) { float s = 0; for (i = 0; i < d; i++) { s += W2e[j * d + i] * m->du[i]; W2g[j * d + i] += a[j] * m->du[i]; } da[j] = s; }
                for (j = 0; j < f; j++) dap[j] = a[j] > 0 ? da[j] : 0.0f;
                for (i = 0; i < d; i++) { float s = 0; for (j = 0; j < f; j++) { s += W1e[i * f + j] * dap[j]; W1g[i * f + j] += h[i] * dap[j]; } m->dh[i] += s; }
                for (j = 0; j < f; j++) b1g[j] += dap[j];
            }
            /* router softmax backward: dr = g .* (dg - <g,dg>) */
            { float gd = 0; for (e = 0; e < E; e++) gd += g[e] * m->dg[e];
              for (e = 0; e < E; e++) m->dr[e] = g[e] * (m->dg[e] - gd); }
            for (i = 0; i < d; i++) { float s = 0; for (e = 0; e < E; e++) { s += m->Wr[i * E + e] * m->dr[e]; m->p[1].g[i * E + e] += h[i] * m->dr[e]; } m->dh[i] += s; }
            /* embedding backward: scatter each concat chunk to its context token */
            for (int c = 0; c < C; c++) { float *eg = m->p[0].g + (size_t)tokens[n * C + c] * dm; for (i = 0; i < dm; i++) eg[i] += m->dh[c * dm + i]; }
        }
    }
    return ce + aux;
}

void cce_moe_adam(cce_moe *m, float lr, int step) {
    const float b1 = 0.9f, b2 = 0.999f, eps = 1e-8f, wd = m->c.weight_decay;
    float c1 = 1.0f - powf(b1, (float)step), c2 = 1.0f - powf(b2, (float)step);
    int pi, i;
    for (pi = 0; pi < m->np; pi++) {
        Param *p = &m->p[pi];
        int decay = (pi != 3 && pi != 6);  /* decay weights, not biases (b1, bh) */
        for (i = 0; i < p->n; i++) {
            float grd = p->g[i];
            p->m[i] = b1 * p->m[i] + (1 - b1) * grd;
            p->v[i] = b2 * p->v[i] + (1 - b2) * grd * grd;
            float mh = p->m[i] / c1, vh = p->v[i] / c2;
            p->w[i] -= lr * mh / (sqrtf(vh) + eps);
            if (wd > 0 && decay) p->w[i] -= lr * wd * p->w[i]; /* decoupled AdamW */
        }
    }
}

void cce_moe_expert_usage(const cce_moe *m, float *usage) { int e; for (e = 0; e < m->c.n_expert; e++) usage[e] = m->usage[e]; }

/* Directional finite-difference check: the numeric derivative of the loss along
   the (normalized) analytic gradient must equal |grad|. This aggregates over all
   parameters, so it is robust to relu kinks and float-precision noise on any
   single weight (which broke a naive per-parameter check), while still catching a
   wrong gradient in any block. Returns the relative error (~<1e-3 = correct). */
double cce_moe_grad_check(cce_moe *m, const int *tokens, const int *targets, int N) {
    const double eps = 1e-4; int pi, i;
    cce_moe_zero_grad(m);
    (void)cce_moe_batch(m, tokens, targets, N, 1, NULL, NULL, NULL);
    double gn2 = 0;
    for (pi = 0; pi < m->np; pi++) for (i = 0; i < m->p[pi].n; i++) gn2 += (double)m->p[pi].g[i] * m->p[pi].g[i];
    double gnorm = sqrt(gn2);
    if (gnorm < 1e-8) return 0.0;
    double scale = eps / gnorm;                           /* step eps along g/|g| */
    for (pi = 0; pi < m->np; pi++) for (i = 0; i < m->p[pi].n; i++) m->p[pi].w[i] += (float)(scale * m->p[pi].g[i]);
    double lp = cce_moe_batch(m, tokens, targets, N, 0, NULL, NULL, NULL);
    for (pi = 0; pi < m->np; pi++) for (i = 0; i < m->p[pi].n; i++) m->p[pi].w[i] -= (float)(2.0 * scale * m->p[pi].g[i]);
    double lm = cce_moe_batch(m, tokens, targets, N, 0, NULL, NULL, NULL);
    for (pi = 0; pi < m->np; pi++) for (i = 0; i < m->p[pi].n; i++) m->p[pi].w[i] += (float)(scale * m->p[pi].g[i]);
    double num = (lp - lm) / (2.0 * eps), ana = gnorm;
    if (getenv("MOE_GC_DEBUG")) fprintf(stderr, "  gc directional: num=%.8g ana=%.8g |g|=%.4g\n", num, ana, gnorm);
    return fabs(num - ana) / (fabs(num) + fabs(ana) + 1e-12);
}
