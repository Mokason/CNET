/* Progressive specialist conversion ladder — see cce_spec_ladder.h */
#include "../../include/cce/cce_spec_ladder.h"
#include "../../include/cce/cce_tensor.h"
#include "../../include/cce/cce_clgemm.h"
#include "../../include/cce/cce_hipgemm.h"
#include "../../include/cce/cce_trit_lut.h"

#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Optional GPU handles for STE (OpenCL + hipBLAS; both T≤8 — we tile).
 * Thread-local override for parallel specialists (one device per worker). */
static cce_clgemm *g_ladder_cl = NULL;
static cce_hipgemm *g_ladder_hip = NULL;
static __thread cce_clgemm *g_ladder_cl_tls = NULL;
/* Telemetry: why we were stuck at 100% CPU before ephemeral matmul. */
static unsigned long g_gpu_ok = 0, g_gpu_fail = 0;
static pthread_mutex_t g_gpu_stat_mu = PTHREAD_MUTEX_INITIALIZER;
/* Serialize host ladder_parallel when multiple specialists run. */
static pthread_mutex_t g_host_par_mu = PTHREAD_MUTEX_INITIALIZER;

static cce_clgemm *ladder_cl(void) {
    return g_ladder_cl_tls ? g_ladder_cl_tls : g_ladder_cl;
}

static void gpu_stat_ok(void) {
    pthread_mutex_lock(&g_gpu_stat_mu);
    g_gpu_ok++;
    pthread_mutex_unlock(&g_gpu_stat_mu);
}
static void gpu_stat_fail(void) {
    pthread_mutex_lock(&g_gpu_stat_mu);
    g_gpu_fail++;
    pthread_mutex_unlock(&g_gpu_stat_mu);
}
/* Host STE threads: ternize / Adam / CPU matmul. Env CNET_LADDER_THREADS.
 * Persistent pool — create/join-per-call wasted ~all multi-core benefit. */
static int g_ladder_threads = -1;
#define LADDER_POOL_MAX 64
static pthread_t g_pool_th[LADDER_POOL_MAX];
static int g_pool_n = 0;          /* workers (not counting caller) */
static int g_pool_stop = 0;
static int g_pool_ready = 0;
static unsigned g_pool_gen = 0;   /* bumps each parallel dispatch */
static unsigned g_pool_done = 0;  /* workers finished this gen */
static void (*g_pool_fn)(int begin, int end, void *ctx) = NULL;
static void *g_pool_ctx = NULL;
static int g_pool_n_items = 0;
static int g_pool_nt = 0;         /* total participants this dispatch */
static pthread_mutex_t g_pool_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_pool_cv = PTHREAD_COND_INITIALIZER;
static pthread_cond_t g_pool_done_cv = PTHREAD_COND_INITIALIZER;

void cce_ladder_set_clgemm(struct cce_clgemm *cl) { g_ladder_cl = cl; }
struct cce_clgemm *cce_ladder_get_clgemm(void) { return g_ladder_cl; }
void cce_ladder_set_hipgemm(struct cce_hipgemm *h) { g_ladder_hip = h; }
struct cce_hipgemm *cce_ladder_get_hipgemm(void) { return g_ladder_hip; }

void cce_ladder_gpu_stats(unsigned long *ok, unsigned long *fail) {
    if (ok) *ok = g_gpu_ok;
    if (fail) *fail = g_gpu_fail;
}

int cce_ladder_nthreads(void) {
    if (g_ladder_threads > 0) return g_ladder_threads;
    {
        const char *e = getenv("CNET_LADDER_THREADS");
        int n = (e && e[0]) ? atoi(e) : 0;
        if (n <= 0) {
            long c = (long)cnet_cpu_count();
            n = (c > 1) ? (int)c : 1;
            if (n > 24) n = 24;
        }
        if (n < 1) n = 1;
        if (n > LADDER_POOL_MAX) n = LADDER_POOL_MAX;
        g_ladder_threads = n;
    }
    return g_ladder_threads;
}

static void *ladder_pool_worker(void *arg) {
    int wid = (int)(intptr_t)arg;
    unsigned my_gen = 0;
    for (;;) {
        int b, e, chunk, nt, n;
        void (*fn)(int, int, void *);
        void *ctx;
        pthread_mutex_lock(&g_pool_mu);
        while (!g_pool_stop && g_pool_gen == my_gen)
            pthread_cond_wait(&g_pool_cv, &g_pool_mu);
        if (g_pool_stop) {
            pthread_mutex_unlock(&g_pool_mu);
            return NULL;
        }
        my_gen = g_pool_gen;
        fn = g_pool_fn;
        ctx = g_pool_ctx;
        n = g_pool_n_items;
        nt = g_pool_nt;
        pthread_mutex_unlock(&g_pool_mu);

        /* wid in [0, g_pool_n); active slots are 0..nt-2 (caller is nt-1) */
        if (fn && n > 0 && wid < nt - 1) {
            chunk = (n + nt - 1) / nt;
            b = wid * chunk;
            e = b + chunk;
            if (e > n) e = n;
            if (b < e) fn(b, e, ctx);
        }

        pthread_mutex_lock(&g_pool_mu);
        g_pool_done++;
        if (g_pool_done >= (unsigned)g_pool_n)
            pthread_cond_signal(&g_pool_done_cv);
        pthread_mutex_unlock(&g_pool_mu);
    }
}

static void ladder_pool_ensure(void) {
    int nt, i;
    if (g_pool_ready) return;
    nt = cce_ladder_nthreads();
    if (nt <= 1) {
        g_pool_ready = 1;
        g_pool_n = 0;
        return;
    }
    /* nt total including caller → nt-1 workers */
    g_pool_n = nt - 1;
    if (g_pool_n > LADDER_POOL_MAX) g_pool_n = LADDER_POOL_MAX;
    g_pool_stop = 0;
    g_pool_gen = 0;
    g_pool_done = 0;
    for (i = 0; i < g_pool_n; ++i) {
        if (pthread_create(&g_pool_th[i], NULL, ladder_pool_worker,
                           (void *)(intptr_t)i) != 0) {
            g_pool_n = i;
            break;
        }
    }
    g_pool_ready = 1;
}

/* Split [0, n) across pool workers + caller. fn(begin,end,ctx).
 * Always waits for ALL pool workers (barrier) so idle workers cannot
 * race the done count ahead of workers still in fn().
 * Mutex: safe when parallel specialists also call host STE. */
static void ladder_parallel(int n, void *ctx,
                            void (*fn)(int begin, int end, void *ctx)) {
    int nt, chunk, b, e;

    if (n < 1 || !fn) return;
    pthread_mutex_lock(&g_host_par_mu);
    ladder_pool_ensure();
    if (g_pool_n < 1 || n < 256) {
        fn(0, n, ctx);
        pthread_mutex_unlock(&g_host_par_mu);
        return;
    }
    /* participants = workers + caller */
    nt = g_pool_n + 1;
    if (nt > n) nt = n;
    if (nt < 2) {
        fn(0, n, ctx);
        pthread_mutex_unlock(&g_host_par_mu);
        return;
    }

    pthread_mutex_lock(&g_pool_mu);
    g_pool_fn = fn;
    g_pool_ctx = ctx;
    g_pool_n_items = n;
    g_pool_nt = nt;
    g_pool_done = 0;
    g_pool_gen++;
    pthread_cond_broadcast(&g_pool_cv);
    pthread_mutex_unlock(&g_pool_mu);

    /* Caller always takes the last slot among the nt participants */
    chunk = (n + nt - 1) / nt;
    b = (nt - 1) * chunk;
    e = b + chunk;
    if (e > n) e = n;
    if (b < e) fn(b, e, ctx);

    pthread_mutex_lock(&g_pool_mu);
    while (g_pool_done < (unsigned)g_pool_n)
        pthread_cond_wait(&g_pool_done_cv, &g_pool_mu);
    g_pool_fn = NULL;
    g_pool_ctx = NULL;
    pthread_mutex_unlock(&g_pool_mu);
    pthread_mutex_unlock(&g_host_par_mu);
}

int cce_ladder_spec_parallel(void) {
    const char *e = getenv("CNET_LADDER_PARALLEL");
    int n;
    if (e && e[0]) {
        n = atoi(e);
        if (n < 1) n = 1;
        if (n > 4) n = 4;
        return n;
    }
    /* Default: 2 when dual OpenCL available, else 1 */
    if (ladder_cl() && cce_clgemm_device_count(ladder_cl()) >= 2) return 2;
    if (g_ladder_cl && cce_clgemm_device_count(g_ladder_cl) >= 2) return 2;
    return 1;
}

void cce_ladder_cfg_default(cce_ladder_cfg *cfg) {
    if (!cfg) return;
    memset(cfg, 0, sizeof *cfg);
    cfg->mode = CCE_LADDER_STE;
    cfg->cert_relerr = 0.20f;
    cfg->ste_steps = 48;
    cfg->ste_lr = 1e-2f;
    cfg->pack_trits = 1;
    cfg->n_calib = 256;
    cfg->n_holdout = 64;
    cfg->obq_max_in = 512;
    cfg->seed = 0xC0FFEEu;
}

cce_ladder_mode cce_ladder_mode_parse(const char *s) {
    if (!s || !s[0]) return CCE_LADDER_STE;
    if (strcmp(s, "posthoc") == 0 || strcmp(s, "ptq") == 0) return CCE_LADDER_POSTHOC;
    if (strcmp(s, "obq") == 0 || strcmp(s, "data") == 0) return CCE_LADDER_OBQ;
    if (strcmp(s, "ste") == 0 || strcmp(s, "qat") == 0) return CCE_LADDER_STE;
    return CCE_LADDER_STE;
}

static void clear_quant(cce_block *blk) {
    if (!blk) return;
    free(blk->w_q);
    free(blk->w_scale);
    free(blk->w_trit);
    blk->w_q = NULL;
    blk->w_scale = NULL;
    blk->w_trit = NULL;
    blk->w_trit_bpr = 0;
}

static uint64_t rng_u(uint64_t *s) {
    *s = *s * 6364136223846793005ULL + 1442695040888963407ULL;
    return *s;
}
static float rng_n(uint64_t *s) {
    /* Box-Muller */
    double u1 = ((rng_u(s) >> 11) & ((1ULL << 53) - 1)) / (double)(1ULL << 53);
    double u2 = ((rng_u(s) >> 11) & ((1ULL << 53) - 1)) / (double)(1ULL << 53);
    if (u1 < 1e-12) u1 = 1e-12;
    return (float)(sqrt(-2.0 * log(u1)) * cos(6.283185307179586 * u2));
}

/* Y[n][out] = X[n][in] @ W[in][out]  (W row-major in×out). Multi-core by row. */
typedef struct {
    const float *X, *W;
    float *Y;
    int in, out;
} matmul_cpu_ctx;

static void matmul_cpu_chunk(int r0, int r1, void *v) {
    matmul_cpu_ctx *c = (matmul_cpu_ctx *)v;
    int r, o, i;
    for (r = r0; r < r1; ++r) {
        const float *xr = c->X + (size_t)r * c->in;
        float *yr = c->Y + (size_t)r * c->out;
        for (o = 0; o < c->out; ++o) yr[o] = 0.f;
        for (i = 0; i < c->in; ++i) {
            float xv = xr[i];
            const float *wi = c->W + (size_t)i * c->out;
            for (o = 0; o < c->out; ++o) yr[o] += xv * wi[o];
        }
    }
}

static void matmul_cpu(const float *X, const float *W, int n, int in, int out,
                       float *Y) {
    matmul_cpu_ctx c = {X, W, Y, in, out};
    ladder_parallel(n, &c, matmul_cpu_chunk);
}

/* One T×K · K×N matmul on GPU (T≤8). Prefer OpenCL then hip.
 * Ephemeral fallback when no STE bind (single-shot). */
static int matmul_gpu_tn(const float *A, int T, int K, const float *W, int N,
                         float *C) {
    int rc = -1;
    cce_clgemm *cl = ladder_cl();
    if (T < 1 || K < 1 || N < 1 || T > 8) return -1;
    if (cl)
        rc = cce_clgemm_matmul_ephemeral(cl, A, (size_t)T, (size_t)K, W, NULL,
                                         (size_t)N, C);
    if (rc != 0 && g_ladder_hip)
        rc = cce_hipgemm_matmul_ephemeral(g_ladder_hip, A, (size_t)T, (size_t)K,
                                          W, NULL, (size_t)N, C);
    if (rc == 0)
        gpu_stat_ok();
    else
        gpu_stat_fail();
    return rc;
}

/* Forward: bind Weff once, tile A only (big win vs re-upload Weff each tile). */
static void matmul(const float *X, const float *W, int n, int in, int out,
                   float *Y) {
    int off = 0;
    cce_clgemm *cl = ladder_cl();
    if (n < 1 || in < 1 || out < 1) return;

    if (cl && cce_clgemm_ste_bind_W(cl, W, (size_t)in, (size_t)out) == 0) {
        while (off < n) {
            int tb = n - off;
            if (tb > 8) tb = 8;
            if (cce_clgemm_ste_matmul(cl, X + (size_t)off * in, (size_t)tb,
                                      Y + (size_t)off * out) == 0) {
                gpu_stat_ok();
            } else {
                gpu_stat_fail();
                matmul_cpu(X + (size_t)off * in, W, tb, in, out,
                           Y + (size_t)off * out);
            }
            off += tb;
        }
        cce_clgemm_ste_unbind_W(cl);
        return;
    }

    if (!cl && !g_ladder_hip) {
        matmul_cpu(X, W, n, in, out, Y);
        return;
    }
    /* hip / ephemeral path */
    while (off < n) {
        int tb = n - off;
        if (tb > 8) tb = 8;
        if (matmul_gpu_tn(X + (size_t)off * in, tb, in, W, out,
                          Y + (size_t)off * out) != 0)
            matmul_cpu(X + (size_t)off * in, W, tb, in, out,
                       Y + (size_t)off * out);
        off += tb;
    }
}

/* CPU dW path: parallel over input rows t (independent). */
typedef struct {
    const float *X, *dY;
    float *dW;
    int n, in, out;
} dW_cpu_ctx;

static void dW_cpu_chunk(int t0, int t1, void *v) {
    dW_cpu_ctx *c = (dW_cpu_ctx *)v;
    int t, r, o;
    for (t = t0; t < t1; ++t) {
        float *dw = c->dW + (size_t)t * c->out;
        for (o = 0; o < c->out; ++o) dw[o] = 0.f;
        for (r = 0; r < c->n; ++r) {
            float xv = c->X[(size_t)r * c->in + t];
            const float *yr = c->dY + (size_t)r * c->out;
            for (o = 0; o < c->out; ++o) dw[o] += xv * yr[o];
        }
    }
}

/* dW[in,out] = X^T[in,n] @ dY[n,out].
 * OpenCL: dY resident once + dual-GPU strip pipeline.
 * Fallback: ephemeral tiles or multi-core CPU. */
static void matmul_XT_dY(const float *X, const float *dY, int n, int in,
                         int out, float *dW) {
    float At[8 * 512];
    float *A = At;
    float *A_heap = NULL;
    int i0, r, t, o;
    cce_clgemm *cl = ladder_cl();

    if (cl &&
        cce_clgemm_ste_XT_dY(cl, X, dY, (size_t)n, (size_t)in, (size_t)out,
                             dW) == 0) {
        /* Count ~ceil(in/8) successful strip waves as ok (pipeline). */
        gpu_stat_ok();
        return;
    }

    if (n > 512) {
        A_heap = (float *)malloc((size_t)8 * n * sizeof(float));
        A = A_heap;
    }
    if ((!cl && !g_ladder_hip) || !A) {
        dW_cpu_ctx c = {X, dY, dW, n, in, out};
        ladder_parallel(in, &c, dW_cpu_chunk);
        free(A_heap);
        return;
    }
    memset(dW, 0, (size_t)in * out * sizeof(float));
    for (i0 = 0; i0 < in; i0 += 8) {
        int tb = in - i0;
        if (tb > 8) tb = 8;
        for (t = 0; t < tb; ++t)
            for (r = 0; r < n; ++r)
                A[(size_t)t * n + r] = X[(size_t)r * in + (i0 + t)];
        if (matmul_gpu_tn(A, tb, n, dY, out, dW + (size_t)i0 * out) != 0) {
            for (t = 0; t < tb; ++t) {
                float *dw = dW + (size_t)(i0 + t) * out;
                for (o = 0; o < out; ++o) dw[o] = 0.f;
                for (r = 0; r < n; ++r) {
                    float xv = X[(size_t)r * in + (i0 + t)];
                    const float *yr = dY + (size_t)r * out;
                    for (o = 0; o < out; ++o) dw[o] += xv * yr[o];
                }
            }
        }
    }
    free(A_heap);
}

static double relerr(const float *A, const float *B, size_t n) {
    double num = 0, den = 0;
    size_t k;
    for (k = 0; k < n; ++k) {
        double d = (double)A[k] - (double)B[k];
        num += d * d;
        den += (double)B[k] * (double)B[k];
    }
    return den > 0 ? sqrt(num / den) : 0.0;
}

/* Per-output absmean ternary of W into Weff (same layout). Parallel over out. */
typedef struct {
    const float *W;
    float *Weff;
    int in, out;
} tern_ctx;

static void ternize_chunk(int o0, int o1, void *v) {
    tern_ctx *c = (tern_ctx *)v;
    int o, i;
    for (o = o0; o < o1; ++o) {
        double s = 0;
        float g;
        for (i = 0; i < c->in; ++i)
            s += fabs((double)c->W[(size_t)i * c->out + o]);
        g = (float)(s / (c->in > 0 ? c->in : 1));
        if (g <= 0.f) g = 1.f;
        for (i = 0; i < c->in; ++i) {
            float w = c->W[(size_t)i * c->out + o];
            float r = roundf(w / g);
            if (r > 1.f) r = 1.f;
            if (r < -1.f) r = -1.f;
            c->Weff[(size_t)i * c->out + o] = g * r;
        }
    }
}

static void ternize(const float *W, int in, int out, float *Weff) {
    tern_ctx c = {W, Weff, in, out};
    ladder_parallel(out, &c, ternize_chunk);
}

/* Adam step: independent per-weight. Parallel over weight index. */
typedef struct {
    float *shadow, *dW, *m, *v;
    float inv, bc1, bc2, step_lr, b1, b2, eps;
} adam_ctx;

static void adam_chunk(int k0, int k1, void *v) {
    adam_ctx *c = (adam_ctx *)v;
    int k;
    for (k = k0; k < k1; ++k) {
        float g = c->dW[k] * c->inv;
        float mh, vh;
        c->m[k] = c->b1 * c->m[k] + (1.f - c->b1) * g;
        c->v[k] = c->b2 * c->v[k] + (1.f - c->b2) * g * g;
        mh = c->m[k] / (c->bc1 > 1e-12f ? c->bc1 : 1.f);
        vh = c->v[k] / (c->bc2 > 1e-12f ? c->bc2 : 1.f);
        c->shadow[k] -= c->step_lr * mh / (sqrtf(vh) + c->eps);
    }
}

/* STE QAT on calib rows; select checkpoint by HOLDOUT relerr (not train MSE).
 * Prior bug: train loss → 0 while holdout got *worse* than posthoc (overfit).
 * L2 pull toward W0 keeps shadow near the teacher weights. */
static void ste_qat(float *shadow, const float *X, const float *Yt, int n_cal,
                    int n_ho, int in, int out, int steps, float lr,
                    const float *W0, float *loss0, float *loss1) {
    float *Weff = (float *)malloc((size_t)in * out * sizeof(float));
    float *Y = (float *)malloc((size_t)n_cal * out * sizeof(float));
    float *Yh = (float *)malloc((size_t)(n_ho > 0 ? n_ho : 1) * out *
                                sizeof(float));
    float *dW = (float *)calloc((size_t)in * out, sizeof(float));
    float *m = (float *)calloc((size_t)in * out, sizeof(float));
    float *v = (float *)calloc((size_t)in * out, sizeof(float));
    const float b1 = 0.9f, b2 = 0.999f, eps = 1e-8f;
    /* Mild L2 toward W0 — stops unbounded calib overfitting */
    const float l2 = 1e-4f;
    int t, n_tot_ho = n_ho > 0 ? n_ho : 0;
    double best_ho = 1e300;
    float *best_shadow = NULL;
    float base_lr = lr > 0.f ? lr : 1e-2f;

    if (!Weff || !Y || !Yh || !dW || !m || !v) {
        free(Weff);
        free(Y);
        free(Yh);
        free(dW);
        free(m);
        free(v);
        return;
    }
    best_shadow = (float *)malloc((size_t)in * out * sizeof(float));
    if (best_shadow)
        memcpy(best_shadow, shadow, (size_t)in * out * sizeof(float));

    for (t = 0; t < steps; ++t) {
        double L = 0, Lho;
        size_t k, N = (size_t)n_cal * out;
        size_t Nw = (size_t)in * out;
        float cos_lr, step_lr;
        ternize(shadow, in, out, Weff);
        matmul(X, Weff, n_cal, in, out, Y);
        for (k = 0; k < N; ++k) {
            double d = (double)Y[k] - (double)Yt[k];
            L += d * d;
        }
        L /= (double)(N > 0 ? N : 1);
        if (t == 0 && loss0) *loss0 = (float)L;
        if (t == steps - 1 && loss1) *loss1 = (float)L;

        /* Holdout selection metric (what cert uses) */
        if (n_tot_ho > 0) {
            ternize(shadow, in, out, Weff);
            matmul(X + (size_t)n_cal * in, Weff, n_tot_ho, in, out, Yh);
            Lho = relerr(Yh, Yt + (size_t)n_cal * out,
                         (size_t)n_tot_ho * out);
            if (Lho < best_ho && best_shadow) {
                best_ho = Lho;
                memcpy(best_shadow, shadow, (size_t)in * out * sizeof(float));
            }
        } else if (L < best_ho && best_shadow) {
            best_ho = L;
            memcpy(best_shadow, shadow, (size_t)in * out * sizeof(float));
        }

        {
            float *dY = Y;
            size_t Ndy = (size_t)n_cal * out;
            for (k = 0; k < Ndy; ++k) dY[k] = Y[k] - Yt[k];
            matmul_XT_dY(X, dY, n_cal, in, out, dW);
            /* L2 grad: dW += l2 * (shadow - W0) */
            if (W0 && l2 > 0.f) {
                for (k = 0; k < Nw; ++k)
                    dW[k] += l2 * (shadow[k] - W0[k]) * (float)n_cal;
            }
        }
        {
            float u = steps > 1 ? (float)t / (float)(steps - 1) : 1.f;
            cos_lr = 0.5f * (1.f + cosf(3.14159265f * u));
            step_lr = base_lr * (0.05f + 0.95f * cos_lr);
        }
        {
            adam_ctx ac;
            ac.shadow = shadow;
            ac.dW = dW;
            ac.m = m;
            ac.v = v;
            ac.inv = 1.f / (float)(n_cal > 0 ? n_cal : 1);
            ac.bc1 = 1.f - powf(b1, (float)(t + 1));
            ac.bc2 = 1.f - powf(b2, (float)(t + 1));
            ac.step_lr = step_lr;
            ac.b1 = b1;
            ac.b2 = b2;
            ac.eps = eps;
            ladder_parallel((int)Nw, &ac, adam_chunk);
        }
    }
    if (best_shadow) {
        memcpy(shadow, best_shadow, (size_t)in * out * sizeof(float));
        if (loss1 && n_tot_ho > 0) {
            ternize(shadow, in, out, Weff);
            matmul(X + (size_t)n_cal * in, Weff, n_tot_ho, in, out, Yh);
            *loss1 = (float)relerr(Yh, Yt + (size_t)n_cal * out,
                                   (size_t)n_tot_ho * out);
        }
        free(best_shadow);
    }
    free(Weff);
    free(Y);
    free(Yh);
    free(dW);
    free(m);
    free(v);
}

/* Lightweight OBQ (few rounds) when in_dim is small. */
static void obq_ternary(const float *W, const float *X, int n, int in, int out,
                        float *Weff) {
    double *Sig = (double *)calloc((size_t)in * in, sizeof(double));
    int i, j, r, o;
    if (!Sig) {
        ternize(W, in, out, Weff);
        return;
    }
    for (i = 0; i < in; ++i)
        for (j = 0; j < in; ++j) {
            double s = 0;
            for (r = 0; r < n; ++r)
                s += (double)X[(size_t)r * in + i] *
                     (double)X[(size_t)r * in + j];
            Sig[(size_t)i * in + j] = s;
        }
    {
        double md = 0;
        for (i = 0; i < in; ++i) md += Sig[(size_t)i * in + i];
        md /= (in > 0 ? in : 1);
        for (i = 0; i < in; ++i) Sig[(size_t)i * in + i] += 0.01 * md;
    }
    for (o = 0; o < out; ++o) {
        double *w = (double *)malloc((size_t)in * sizeof(double));
        double *q = (double *)malloc((size_t)in * sizeof(double));
        double *Sw = (double *)malloc((size_t)in * sizeof(double));
        double *g = (double *)malloc((size_t)in * sizeof(double));
        double *rr = (double *)malloc((size_t)in * sizeof(double));
        double gam = 0;
        int rstep, sweep, cc;
        if (!w || !q || !Sw || !g || !rr) {
            free(w);
            free(q);
            free(Sw);
            free(g);
            free(rr);
            continue;
        }
        for (i = 0; i < in; ++i) w[i] = (double)W[(size_t)i * out + o];
        for (i = 0; i < in; ++i) {
            double s = 0;
            const double *Si = Sig + (size_t)i * in;
            for (j = 0; j < in; ++j) s += Si[j] * w[j];
            Sw[i] = s;
            gam += fabs(w[i]);
        }
        gam /= (in > 0 ? in : 1);
        for (i = 0; i < in; ++i) {
            double qi = gam > 0 ? round(w[i] / gam) : 0;
            if (qi > 1) qi = 1;
            if (qi < -1) qi = -1;
            q[i] = qi;
        }
        for (rstep = 0; rstep < 3; ++rstep) {
            double num = 0, den = 0;
            for (i = 0; i < in; ++i) {
                double sq = 0;
                const double *Si = Sig + (size_t)i * in;
                for (j = 0; j < in; ++j) sq += Si[j] * q[j];
                num += q[i] * Sw[i];
                den += q[i] * sq;
            }
            if (den > 1e-12) {
                double gn = num / den;
                if (gn > 0) gam = gn;
            }
            for (i = 0; i < in; ++i) rr[i] = gam * q[i] - w[i];
            for (i = 0; i < in; ++i) {
                double s = 0;
                const double *Si = Sig + (size_t)i * in;
                for (j = 0; j < in; ++j) s += Si[j] * rr[j];
                g[i] = s;
            }
            for (sweep = 0; sweep < 2; ++sweep)
                for (i = 0; i < in; ++i) {
                    double Sii = Sig[(size_t)i * in + i], bdE = 0, bd = 0,
                           bc = q[i];
                    for (cc = -1; cc <= 1; ++cc) {
                        double delta = gam * ((double)cc - q[i]);
                        double dE;
                        if (delta == 0) continue;
                        dE = 2 * delta * g[i] + delta * delta * Sii;
                        if (dE < bdE) {
                            bdE = dE;
                            bd = delta;
                            bc = cc;
                        }
                    }
                    if (bd != 0) {
                        rr[i] += bd;
                        q[i] = bc;
                        {
                            const double *Si = Sig + (size_t)i * in;
                            for (j = 0; j < in; ++j) g[j] += bd * Si[j];
                        }
                    }
                }
        }
        for (i = 0; i < in; ++i)
            Weff[(size_t)i * out + o] = (float)(gam * q[i]);
        free(w);
        free(q);
        free(Sw);
        free(g);
        free(rr);
    }
    free(Sig);
}

/* Measure ternary(W) forward relerr vs Yt on rows of X. */
static float measure_tern_relerr(const float *W, const float *X, const float *Yt,
                                 int n, int in, int out) {
    float *Weff = (float *)malloc((size_t)in * out * sizeof(float));
    float *Y = (float *)malloc((size_t)n * out * sizeof(float));
    float e = 1e9f;
    if (!Weff || !Y) {
        free(Weff);
        free(Y);
        return e;
    }
    ternize(W, in, out, Weff);
    matmul(X, Weff, n, in, out, Y);
    e = (float)relerr(Y, Yt, (size_t)n * out);
    free(Weff);
    free(Y);
    return e;
}

cce_result cce_ladder_convert_block(cce_block *blk, const char *name,
                                    const float *X_in, int n_rows,
                                    const cce_ladder_cfg *cfg_in,
                                    cce_ladder_report *rep) {
    cce_ladder_cfg cfg;
    int in, out, n_cal, n_ho, n_tot;
    float *X = NULL, *Yt = NULL, *W0 = NULL, *shadow = NULL;
    int own_X = 0;
    uint64_t rng;
    cce_ladder_mode mode;

    if (rep) memset(rep, 0, sizeof *rep);
    if (!blk || !blk->weights.data || blk->weights.ndim != 2)
        return CCE_ERR_INVALID_ARG;
    if (blk->type != CCE_BLOCK_LINEAR && blk->type != CCE_BLOCK_LINEAR_HEAD)
        return CCE_ERR_UNSUPPORTED;

    if (cfg_in)
        cfg = *cfg_in;
    else
        cce_ladder_cfg_default(&cfg);

    in = blk->weights.shape[0];
    out = blk->weights.shape[1];
    if (in < 1 || out < 1) return CCE_ERR_INVALID_ARG;

    if (rep) {
        snprintf(rep->name, sizeof rep->name, "%s", name ? name : "(block)");
        rep->in_dim = in;
        rep->out_dim = out;
        rep->mode_used = (int)cfg.mode;
        rep->used_real_acts = (X_in && n_rows > 0) ? 1 : 0;
    }

    n_cal = cfg.n_calib > 0 ? cfg.n_calib : 256;
    n_ho = cfg.n_holdout > 0 ? cfg.n_holdout : 64;
    n_tot = n_cal + n_ho;
    if (X_in && n_rows >= n_tot) {
        X = (float *)X_in; /* const cast ok for read */
        n_tot = n_rows;
        if (n_tot > n_cal + n_ho) {
            /* use last n_ho as holdout */
            n_cal = n_tot - n_ho;
        }
    } else {
        int r, i;
        own_X = 1;
        X = (float *)malloc((size_t)n_tot * in * sizeof(float));
        if (!X) return CCE_ERR_OOM;
        rng = cfg.seed ? cfg.seed : 1u;
        for (r = 0; r < n_tot; ++r)
            for (i = 0; i < in; ++i)
                X[(size_t)r * in + i] = rng_n(&rng) * 0.5f;
    }

    W0 = (float *)malloc((size_t)in * out * sizeof(float));
    shadow = (float *)malloc((size_t)in * out * sizeof(float));
    Yt = (float *)malloc((size_t)n_tot * out * sizeof(float));
    if (!W0 || !shadow || !Yt) {
        free(W0);
        free(shadow);
        free(Yt);
        if (own_X) free(X);
        return CCE_ERR_OOM;
    }
    memcpy(W0, blk->weights.data, (size_t)in * out * sizeof(float));
    memcpy(shadow, W0, (size_t)in * out * sizeof(float));
    matmul(X, W0, n_tot, in, out, Yt);

    /* Diagnostic: naive posthoc relerr on holdout */
    if (rep)
        rep->relerr_posthoc = measure_tern_relerr(
            W0, X + (size_t)n_cal * in, Yt + (size_t)n_cal * out, n_ho, in,
            out);

    mode = cfg.mode;
    if (mode == CCE_LADDER_OBQ && in > cfg.obq_max_in) mode = CCE_LADDER_STE;

    if (mode == CCE_LADDER_STE) {
        float l0 = 0, l1 = 0;
        float *ste_shadow =
            (float *)malloc((size_t)in * out * sizeof(float));
        if (ste_shadow) {
            memcpy(ste_shadow, W0, (size_t)in * out * sizeof(float));
            ste_qat(ste_shadow, X, Yt, n_cal, n_ho, in, out,
                    cfg.ste_steps > 0 ? cfg.ste_steps : 48,
                    cfg.ste_lr > 0 ? cfg.ste_lr : 1e-2f, W0, &l0, &l1);
            if (rep) {
                rep->ste_loss0 = l0;
                rep->ste_loss1 = l1; /* holdout relerr of best STE shadow */
            }
            /* Never ship STE if it loses to plain posthoc on holdout */
            {
                float e_post = rep ? rep->relerr_posthoc
                                   : measure_tern_relerr(
                                         W0, X + (size_t)n_cal * in,
                                         Yt + (size_t)n_cal * out, n_ho, in,
                                         out);
                float e_ste = measure_tern_relerr(
                    ste_shadow, X + (size_t)n_cal * in,
                    Yt + (size_t)n_cal * out, n_ho, in, out);
                if (e_ste < e_post)
                    memcpy(shadow, ste_shadow, (size_t)in * out * sizeof(float));
                else
                    memcpy(shadow, W0, (size_t)in * out * sizeof(float));
            }
            free(ste_shadow);
        }
    } else if (mode == CCE_LADDER_OBQ) {
        float *Weff = (float *)malloc((size_t)in * out * sizeof(float));
        if (Weff) {
            obq_ternary(W0, X, n_cal, in, out, Weff);
            memcpy(shadow, Weff, (size_t)in * out * sizeof(float));
            free(Weff);
        }
    }
    /* POSTHOC: shadow stays W0 */

    /* Install shadow into block, clear any prior quant, ternary quantize */
    clear_quant(blk);
    memcpy(blk->weights.data, shadow, (size_t)in * out * sizeof(float));
    if (cce_block_quantize_ternary(blk) != CCE_OK) {
        memcpy(blk->weights.data, W0, (size_t)in * out * sizeof(float));
        if (own_X) free(X);
        free(W0);
        free(shadow);
        free(Yt);
        return CCE_ERR_UNSUPPORTED;
    }

    /* Certify: reconstruct from stored ternary codes (raw matmul). */
    {
        float e = 1e9f;
        if (blk->w_q && blk->w_scale) {
            float *Weff = (float *)malloc((size_t)in * out * sizeof(float));
            float *Yq = (float *)malloc((size_t)n_ho * out * sizeof(float));
            int i, o;
            if (Weff && Yq) {
                for (i = 0; i < in; ++i)
                    for (o = 0; o < out; ++o)
                        Weff[(size_t)i * out + o] =
                            blk->w_scale[o] *
                            (float)blk->w_q[(size_t)i * out + o];
                matmul(X + (size_t)n_cal * in, Weff, n_ho, in, out, Yq);
                e = (float)relerr(Yq, Yt + (size_t)n_cal * out,
                                  (size_t)n_ho * out);
            }
            free(Weff);
            free(Yq);
        }
        if (rep) rep->relerr_final = e;

        if (e <= cfg.cert_relerr) {
            if (rep) rep->certified = 1;
            if (cfg.pack_trits) {
                if (cce_block_pack_trits(blk) == CCE_OK && rep) rep->packed = 1;
            } else if (rep) {
                rep->packed = 1; /* w_q kept for GPU */
            }
            cce_block_freeze(blk);
        } else {
            /* FAIL-CLOSED: restore FP, drop quant */
            clear_quant(blk);
            memcpy(blk->weights.data, W0, (size_t)in * out * sizeof(float));
            if (rep) {
                rep->certified = 0;
                rep->packed = 0;
            }
        }
    }

    if (own_X) free(X);
    free(W0);
    free(shadow);
    free(Yt);
    return CCE_OK;
}

int cce_ladder_convert_forest(cce_forest *f, const char *family_sub,
                              const cce_ladder_cfg *cfg,
                              cce_ladder_report *reps, int max_reps) {
    return cce_ladder_convert_forest_bank(f, family_sub, NULL, cfg, reps,
                                          max_reps);
}

/* ---- capture bank ---- */

cce_result cce_ladder_bank_init(cce_ladder_bank *bank, int max_specs,
                                int max_rows_per) {
    if (!bank) return CCE_ERR_INVALID_ARG;
    memset(bank, 0, sizeof *bank);
    if (max_specs < 1) max_specs = 64;
    if (max_rows_per < 16) max_rows_per = 256;
    bank->specs = (cce_ladder_spec_cap *)calloc((size_t)max_specs,
                                                sizeof(cce_ladder_spec_cap));
    if (!bank->specs) return CCE_ERR_OOM;
    bank->max_specs = max_specs;
    bank->max_rows_per = max_rows_per;
    return CCE_OK;
}

void cce_ladder_bank_free(cce_ladder_bank *bank) {
    int i;
    if (!bank) return;
    if (bank->specs) {
        for (i = 0; i < bank->n_specs; ++i) free(bank->specs[i].rows);
        free(bank->specs);
    }
    memset(bank, 0, sizeof *bank);
}

void cce_ladder_bank_on_capture(const char *spec_name, const float *rows,
                                int n_rows, int in_dim, void *uctx) {
    cce_ladder_bank *bank = (cce_ladder_bank *)uctx;
    cce_ladder_spec_cap *sc = NULL;
    int i, need, copy_n;
    if (!bank || !spec_name || !rows || n_rows < 1 || in_dim < 1) return;
    for (i = 0; i < bank->n_specs; ++i) {
        if (strcmp(bank->specs[i].name, spec_name) == 0) {
            sc = &bank->specs[i];
            break;
        }
    }
    if (!sc) {
        if (bank->n_specs >= bank->max_specs) return;
        sc = &bank->specs[bank->n_specs++];
        memset(sc, 0, sizeof *sc);
        snprintf(sc->name, sizeof sc->name, "%s", spec_name);
        sc->in_dim = in_dim;
        sc->cap_rows = bank->max_rows_per;
        sc->rows = (float *)malloc((size_t)sc->cap_rows * in_dim * sizeof(float));
        if (!sc->rows) {
            bank->n_specs--;
            return;
        }
    }
    if (sc->in_dim != in_dim || !sc->rows) return;
    need = sc->n_rows + n_rows;
    if (need > sc->cap_rows) {
        /* keep most recent window */
        int keep = sc->cap_rows / 2;
        if (keep < 1) keep = 1;
        if (sc->n_rows > keep) {
            memmove(sc->rows, sc->rows + (size_t)(sc->n_rows - keep) * in_dim,
                    (size_t)keep * in_dim * sizeof(float));
            sc->n_rows = keep;
        }
    }
    copy_n = n_rows;
    if (sc->n_rows + copy_n > sc->cap_rows) copy_n = sc->cap_rows - sc->n_rows;
    if (copy_n < 1) return;
    memcpy(sc->rows + (size_t)sc->n_rows * in_dim, rows,
           (size_t)copy_n * in_dim * sizeof(float));
    sc->n_rows += copy_n;
}

const cce_ladder_spec_cap *cce_ladder_bank_find(const cce_ladder_bank *bank,
                                                const char *name) {
    int i;
    if (!bank || !name) return NULL;
    for (i = 0; i < bank->n_specs; ++i)
        if (strcmp(bank->specs[i].name, name) == 0) return &bank->specs[i];
    for (i = 0; i < bank->n_specs; ++i)
        if (strstr(bank->specs[i].name, name) || strstr(name, bank->specs[i].name))
            return &bank->specs[i];
    return NULL;
}

cce_result cce_ladder_capture_model(cce_gguf_qwen2 *m, cce_ladder_bank *bank,
                                    const int *tokens, int n_seq, int seq_len) {
    float *logits = NULL;
    int s, V;
    if (!m || !bank || !tokens || n_seq < 1 || seq_len < 1)
        return CCE_ERR_INVALID_ARG;
    V = m->vocab_size > 0 ? m->vocab_size : 1;
    logits = (float *)malloc((size_t)V * sizeof(float));
    if (!logits) return CCE_ERR_OOM;
    cce_gguf_set_capture_hook(cce_ladder_bank_on_capture, bank);
    for (s = 0; s < n_seq; ++s) {
        m->cur_pos = 0;
        (void)cce_gguf_qwen2_forward(m, tokens + (size_t)s * seq_len, seq_len,
                                     logits, V);
    }
    cce_gguf_set_capture_hook(NULL, NULL);
    free(logits);
    return CCE_OK;
}

/* Work unit for parallel specialist convert. */
typedef struct {
    cce_block *blk;
    char full[180];
    const float *X;
    int n_rows;
    cce_ladder_report *rp;
    int slot; /* index into reps / result order */
} ladder_job_item;

typedef struct {
    ladder_job_item *jobs;
    int n_jobs;
    int next;
    const cce_ladder_cfg *cfg;
    cce_clgemm *lane_cl[4];
    int n_lanes;
    pthread_mutex_t mu;
    int done_ok;
} ladder_par_spec_ctx;

static void *ladder_spec_worker(void *arg) {
    ladder_par_spec_ctx *ctx = (ladder_par_spec_ctx *)arg;
    for (;;) {
        ladder_job_item *job;
        int ji, L;
        pthread_mutex_lock(&ctx->mu);
        if (ctx->next >= ctx->n_jobs) {
            pthread_mutex_unlock(&ctx->mu);
            break;
        }
        ji = ctx->next++;
        job = &ctx->jobs[ji];
        pthread_mutex_unlock(&ctx->mu);

        L = (ctx->n_lanes > 0) ? (ji % ctx->n_lanes) : 0;
        g_ladder_cl_tls =
            (ctx->n_lanes > 0 && ctx->lane_cl[L]) ? ctx->lane_cl[L] : NULL;

        fprintf(stderr, "[ladder] convert %s (lane %d) …\n", job->full, L);
        fflush(stderr);
        if (cce_ladder_convert_block(job->blk, job->full, job->X, job->n_rows,
                                     ctx->cfg, job->rp) == CCE_OK) {
            if (job->rp) {
                fprintf(stderr,
                        "[ladder]   %s cert=%d packed=%d relerr=%.4f\n",
                        job->full, job->rp->certified, job->rp->packed,
                        job->rp->relerr_final);
                if (job->rp->packed)
                    fprintf(stderr, "[ladder] NEW_PACK %s relerr=%.4f\n",
                            job->full, job->rp->relerr_final);
                else
                    fprintf(stderr, "[ladder] NO_PACK %s relerr=%.4f\n",
                            job->full, job->rp->relerr_final);
            }
            fflush(stderr);
            pthread_mutex_lock(&ctx->mu);
            ctx->done_ok++;
            pthread_mutex_unlock(&ctx->mu);
        }
        g_ladder_cl_tls = NULL;
    }
    return NULL;
}

/* Optional denylist: CNET_LADDER_SKIP=path, one specialist name per line. */
static int ladder_is_skipped(const char *full) {
    const char *path = getenv("CNET_LADDER_SKIP");
    FILE *fp;
    char line[200];
    if (!path || !path[0] || !full) return 0;
    fp = fopen(path, "r");
    if (!fp) return 0;
    while (fgets(line, (int)sizeof line, fp)) {
        size_t L = strlen(line);
        while (L > 0 && (line[L - 1] == '\n' || line[L - 1] == '\r'))
            line[--L] = 0;
        if (L == 0 || line[0] == '#') continue;
        if (strcmp(line, full) == 0) {
            fclose(fp);
            return 1;
        }
    }
    fclose(fp);
    return 0;
}

int cce_ladder_convert_forest_bank(cce_forest *f, const char *family_sub,
                                   const cce_ladder_bank *bank,
                                   const cce_ladder_cfg *cfg,
                                   cce_ladder_report *reps, int max_reps) {
    int n, i, done = 0, n_work = 0;
    ladder_job_item *jobs = NULL;
    int jobs_cap = 0;
    int np;

    if (!f || !family_sub || !family_sub[0]) return 0;
    n = cce_forest_branch_count(f);

    /* Pass 1: collect work + handle already-packed skips.
     * max_reps limits NEW converts only (skips/denylist do not consume the
     * budget — required for MAX=1 greedy e2e resume). */
    for (i = 0; i < n && n_work < max_reps; ++i) {
        char name[160];
        cce_cascade *cas;
        int b;
        const cce_ladder_spec_cap *sc = NULL;
        if (cce_forest_branch_name(f, i, name, (int)sizeof name) != CCE_OK)
            continue;
        if (!strstr(name, family_sub)) continue;
        cas = cce_forest_get_resident(f, name);
        if (!cas) continue;
        if (bank) sc = cce_ladder_bank_find(bank, name);
        for (b = 0; b < cas->num_blocks && n_work < max_reps; ++b) {
            cce_block *blk = &cas->blocks[b];
            cce_ladder_report *rp;
            char full[180];
            const float *X = NULL;
            int n_rows = 0;
            if (!blk->weights.data && !blk->w_trit) continue;
            snprintf(full, sizeof full, "%s#%d", name, b);
            if (ladder_is_skipped(full)) {
                fprintf(stderr, "[ladder] skip denylist %s\n", full);
                fflush(stderr);
                continue;
            }
            if (blk->w_trit && blk->w_scale) {
                rp = reps ? &reps[done] : NULL;
                if (rp) {
                    snprintf(rp->name, sizeof rp->name, "%s#%d", name, b);
                    rp->in_dim = blk->weights.ndim == 2 ? blk->weights.shape[0]
                                                        : 0;
                    rp->out_dim = blk->weights.ndim == 2 ? blk->weights.shape[1]
                                                         : 0;
                    rp->certified = 1;
                    rp->packed = 1;
                    rp->mode_used = cfg ? (int)cfg->mode : 0;
                    rp->used_real_acts = 0;
                    rp->relerr_final = 0.f;
                    rp->relerr_posthoc = 0.f;
                }
                fprintf(stderr, "[ladder] skip already packed %s\n", full);
                fflush(stderr);
                done++;
                continue;
            }
            if (!blk->weights.data) continue;
            if (sc && sc->rows && sc->n_rows > 0 &&
                sc->in_dim == blk->weights.shape[0]) {
                X = sc->rows;
                n_rows = sc->n_rows;
            }
            if (n_work >= jobs_cap) {
                int nc = jobs_cap ? jobs_cap * 2 : 32;
                ladder_job_item *nj =
                    (ladder_job_item *)realloc(jobs, (size_t)nc * sizeof *jobs);
                if (!nj) break;
                jobs = nj;
                jobs_cap = nc;
            }
            jobs[n_work].blk = blk;
            snprintf(jobs[n_work].full, sizeof jobs[n_work].full, "%s", full);
            jobs[n_work].X = X;
            jobs[n_work].n_rows = n_rows;
            jobs[n_work].slot = n_work;
            jobs[n_work].rp =
                reps ? &reps[done + n_work] : NULL; /* after skips */
            n_work++;
        }
    }

    /* Fix report pointers: skips took [0..done), work gets [done..done+n_work) */
    for (i = 0; i < n_work; ++i)
        jobs[i].rp = reps ? &reps[done + i] : NULL;

    np = cce_ladder_spec_parallel();
    if (np > n_work) np = n_work;
    if (np < 1) np = 1;

    if (n_work == 0) {
        free(jobs);
        return done;
    }

    if (np <= 1) {
        for (i = 0; i < n_work; ++i) {
            fprintf(stderr, "[ladder] convert %s …\n", jobs[i].full);
            fflush(stderr);
            if (cce_ladder_convert_block(jobs[i].blk, jobs[i].full, jobs[i].X,
                                         jobs[i].n_rows, cfg, jobs[i].rp) ==
                CCE_OK) {
                if (jobs[i].rp) {
                    fprintf(stderr,
                            "[ladder]   %s cert=%d packed=%d relerr=%.4f\n",
                            jobs[i].full, jobs[i].rp->certified,
                            jobs[i].rp->packed, jobs[i].rp->relerr_final);
                    /* Machine-parseable marker for greedy e2e scripts */
                    if (jobs[i].rp->packed)
                        fprintf(stderr, "[ladder] NEW_PACK %s relerr=%.4f\n",
                                jobs[i].full, jobs[i].rp->relerr_final);
                    else
                        fprintf(stderr, "[ladder] NO_PACK %s relerr=%.4f\n",
                                jobs[i].full, jobs[i].rp->relerr_final);
                }
                fflush(stderr);
                done++;
            }
        }
        free(jobs);
        return done;
    }

    /* Parallel specialists: one OpenCL device per lane (no shared queue). */
    {
        ladder_par_spec_ctx ctx;
        pthread_t th[4];
        int w, n_th;
        char nm[64];
        memset(&ctx, 0, sizeof ctx);
        ctx.jobs = jobs;
        ctx.n_jobs = n_work;
        ctx.next = 0;
        ctx.cfg = cfg;
        ctx.done_ok = 0;
        pthread_mutex_init(&ctx.mu, NULL);

        ctx.n_lanes = np;
        for (w = 0; w < np; ++w) {
            ctx.lane_cl[w] =
                cce_clgemm_open_device(NULL, w, nm, sizeof nm);
            if (!ctx.lane_cl[w] && g_ladder_cl && w == 0) {
                /* fallback: share global (serialized by OpenCL) */
                ctx.lane_cl[w] = g_ladder_cl;
            }
            if (ctx.lane_cl[w])
                fprintf(stderr, "[ladder] parallel lane %d: %s\n", w,
                        nm[0] ? nm : "cl");
        }
        /* If open_device failed for some, shrink */
        while (ctx.n_lanes > 1 && !ctx.lane_cl[ctx.n_lanes - 1])
            ctx.n_lanes--;
        if (ctx.n_lanes < 1 || !ctx.lane_cl[0]) {
            /* No lanes — serial with global */
            pthread_mutex_destroy(&ctx.mu);
            for (i = 0; i < n_work; ++i) {
                fprintf(stderr, "[ladder] convert %s …\n", jobs[i].full);
                fflush(stderr);
                if (cce_ladder_convert_block(jobs[i].blk, jobs[i].full,
                                             jobs[i].X, jobs[i].n_rows, cfg,
                                             jobs[i].rp) == CCE_OK) {
                    if (jobs[i].rp) {
                        fprintf(stderr,
                                "[ladder]   %s cert=%d packed=%d relerr=%.4f\n",
                                jobs[i].full, jobs[i].rp->certified,
                                jobs[i].rp->packed, jobs[i].rp->relerr_final);
                        if (jobs[i].rp->packed)
                            fprintf(stderr,
                                    "[ladder] NEW_PACK %s relerr=%.4f\n",
                                    jobs[i].full, jobs[i].rp->relerr_final);
                        else
                            fprintf(stderr,
                                    "[ladder] NO_PACK %s relerr=%.4f\n",
                                    jobs[i].full, jobs[i].rp->relerr_final);
                    }
                    fflush(stderr);
                    done++;
                }
            }
            free(jobs);
            return done;
        }

        fprintf(stderr,
                "[ladder] parallel specialists np=%d jobs=%d (device-pinned)\n",
                ctx.n_lanes, n_work);
        fflush(stderr);

        n_th = ctx.n_lanes;
        for (w = 0; w < n_th; ++w) {
            if (pthread_create(&th[w], NULL, ladder_spec_worker, &ctx) != 0) {
                /* remaining work will be done by fewer workers */
                n_th = w;
                break;
            }
        }
        if (n_th == 0) {
            /* create failed — run on caller */
            ladder_spec_worker(&ctx);
        } else {
            for (w = 0; w < n_th; ++w) pthread_join(th[w], NULL);
        }

        done += ctx.done_ok;

        for (w = 0; w < ctx.n_lanes; ++w) {
            if (ctx.lane_cl[w] && ctx.lane_cl[w] != g_ladder_cl)
                cce_clgemm_close(ctx.lane_cl[w]);
        }
        pthread_mutex_destroy(&ctx.mu);
    }

    free(jobs);
    return done;
}

/* Quality-first schedule: bars under typical posthoc (~0.51) so only STE
 * wins (or unusually easy mats) pack. Fail-closed leaves FP. */
static const cce_ladder_stage g_default_sched[] = {
    {"gate_proj", 0.42f, CCE_LADDER_STE, 96},
    {"ffn_gate", 0.42f, CCE_LADDER_STE, 96},
    {"up_proj", 0.42f, CCE_LADDER_STE, 96},
    {"ffn_up", 0.42f, CCE_LADDER_STE, 96},
    {"down_proj", 0.38f, CCE_LADDER_STE, 128},
    {"ffn_down", 0.38f, CCE_LADDER_STE, 128},
    {"o_proj", 0.40f, CCE_LADDER_STE, 96},
    {"attn_output", 0.40f, CCE_LADDER_STE, 96},
    {"q_proj", 0.40f, CCE_LADDER_STE, 96},
    {"attn_q", 0.40f, CCE_LADDER_STE, 96},
    {"k_proj", 0.40f, CCE_LADDER_STE, 96},
    {"attn_k", 0.40f, CCE_LADDER_STE, 96},
    {"v_proj", 0.40f, CCE_LADDER_STE, 96},
    {"attn_v", 0.40f, CCE_LADDER_STE, 96},
};

const cce_ladder_stage *cce_ladder_default_schedule(int *n_out) {
    if (n_out)
        *n_out = (int)(sizeof g_default_sched / sizeof g_default_sched[0]);
    return g_default_sched;
}

int cce_ladder_run_schedule(cce_gguf_qwen2 *m, const cce_ladder_bank *bank,
                            const cce_ladder_stage *stages, int n_stages,
                            const cce_ladder_cfg *base_cfg,
                            cce_ladder_report *reps, int max_reps,
                            const char *checkpoint_export) {
    int s, total = 0, n_st = n_stages;
    const cce_ladder_stage *st = stages;
    cce_ladder_cfg cfg;
    if (!m || !m->forest || max_reps < 1) return 0;
    if (!st) st = cce_ladder_default_schedule(&n_st);
    if (base_cfg)
        cfg = *base_cfg;
    else
        cce_ladder_cfg_default(&cfg);
    /* Base STE knobs from cfg; stage may only tighten steps if larger.
     * Prefer lean global steps when env/cfg sets them lower. */
    for (s = 0; s < n_st && total < max_reps; ++s) {
        int got, nw;
        cce_ladder_cfg sc = cfg;
        sc.mode = st[s].mode;
        sc.cert_relerr = st[s].cert_relerr;
        /* Use min(stage, cfg) so campaign can lean-down without fighting schedule */
        if (st[s].ste_steps > 0) {
            if (cfg.ste_steps > 0 && cfg.ste_steps < st[s].ste_steps)
                sc.ste_steps = cfg.ste_steps;
            else
                sc.ste_steps = st[s].ste_steps;
        }
        fprintf(stderr, "[ladder] === stage %d/%d family=%s cert=%.2f steps=%d ===\n",
                s + 1, n_st, st[s].family, sc.cert_relerr, sc.ste_steps);
        fflush(stderr);
        got = cce_ladder_convert_forest_bank(
            m->forest, st[s].family, bank, &sc, reps ? reps + total : NULL,
            max_reps - total);
        total += got;
        {
            unsigned long ok = 0, fail = 0;
            cce_ladder_gpu_stats(&ok, &fail);
            fprintf(stderr,
                    "[ladder] === stage %s done (+%d, total=%d) "
                    "gpu_matmul ok=%lu fail=%lu ===\n",
                    st[s].family, got, total, ok, fail);
        }
        fflush(stderr);
        if (checkpoint_export && checkpoint_export[0]) {
            nw = cce_ladder_export_certified(m->forest, checkpoint_export);
            fprintf(stderr, "[ladder] checkpoint export %s written=%d\n",
                    checkpoint_export, nw);
            fflush(stderr);
        }
    }
    return total;
}

/* ---- certified export / import (magic LDTR)
 * v1: branch name → blocks[0] only
 * v2: branch name + block_idx for every packed block in the cascade
 */
#define LADDER_MAGIC 0x5254444Cu /* 'LDTR' LE */
#define LADDER_VER 2
#define LADDER_VER_MIN 1

static void ladder_expand_trit_to_wq(cce_block *blk, const uint8_t *trit,
                                     int in, int out, int bpr) {
    int8_t *wq;
    int i, o;
    if (!blk || !trit || in < 1 || out < 1 || bpr < 1) return;
    wq = (int8_t *)malloc((size_t)in * out * sizeof(int8_t));
    if (!wq) return;
    for (i = 0; i < in; ++i) {
        const uint8_t *pr = trit + (size_t)i * bpr;
        for (o = 0; o < out; ++o) {
            int byte = o / 5;
            int k = o % 5;
            wq[(size_t)i * out + o] = cce_trit_lut[pr[byte]][k];
        }
    }
    free(blk->w_q);
    blk->w_q = wq;
}

int cce_ladder_export_certified(const cce_forest *f, const char *path) {
    FILE *fp;
    int n, i, b, written = 0;
    uint32_t magic = LADDER_MAGIC, ver = LADDER_VER;
    long count_pos;
    if (!f || !path) return -1;
    fp = fopen(path, "wb");
    if (!fp) return -1;
    fwrite(&magic, 4, 1, fp);
    fwrite(&ver, 4, 1, fp);
    n = cce_forest_branch_count(f);
    count_pos = ftell(fp);
    fwrite(&written, sizeof(int), 1, fp); /* placeholder ncert */
    for (i = 0; i < n; ++i) {
        char name[160];
        cce_cascade *cas;
        int nl, one = 1;
        if (cce_forest_branch_name(f, i, name, (int)sizeof name) != CCE_OK)
            continue;
        cas = cce_forest_get_resident((cce_forest *)f, name);
        if (!cas) continue;
        nl = (int)strlen(name);
        for (b = 0; b < cas->num_blocks; ++b) {
            cce_block *blk = &cas->blocks[b];
            int in, out, bpr;
            if (!blk->w_trit || !blk->w_scale || blk->weights.ndim != 2)
                continue;
            in = blk->weights.shape[0];
            out = blk->weights.shape[1];
            bpr = blk->w_trit_bpr;
            fwrite(&one, sizeof(int), 1, fp);
            fwrite(&nl, sizeof(int), 1, fp);
            fwrite(name, 1, (size_t)nl, fp);
            fwrite(&b, sizeof(int), 1, fp); /* block index (v2) */
            fwrite(&in, sizeof(int), 1, fp);
            fwrite(&out, sizeof(int), 1, fp);
            fwrite(&bpr, sizeof(int), 1, fp);
            fwrite(blk->w_trit, 1, (size_t)in * bpr, fp);
            fwrite(blk->w_scale, sizeof(float), (size_t)out, fp);
            if (blk->bias.data && blk->bias.numel == (size_t)out)
                fwrite(blk->bias.data, sizeof(float), (size_t)out, fp);
            else {
                float *zz = (float *)calloc((size_t)out, sizeof(float));
                if (zz) {
                    fwrite(zz, sizeof(float), (size_t)out, fp);
                    free(zz);
                }
            }
            written++;
        }
    }
    fseek(fp, count_pos, SEEK_SET);
    fwrite(&written, sizeof(int), 1, fp);
    fclose(fp);
    return written;
}

int cce_ladder_import_certified(cce_forest *f, const char *path) {
    FILE *fp;
    uint32_t magic = 0, ver = 0;
    int ncert = 0, loaded = 0, i;
    if (!f || !path) return -1;
    fp = fopen(path, "rb");
    if (!fp) return -1;
    if (fread(&magic, 4, 1, fp) != 1 || magic != LADDER_MAGIC) {
        fclose(fp);
        return -1;
    }
    if (fread(&ver, 4, 1, fp) != 1 || ver < LADDER_VER_MIN || ver > LADDER_VER) {
        fclose(fp);
        return -1;
    }
    if (fread(&ncert, sizeof(int), 1, fp) != 1) {
        fclose(fp);
        return -1;
    }
    for (i = 0; i < ncert; ++i) {
        int one = 0, nl = 0, in = 0, out = 0, bpr = 0, bidx = 0;
        char name[160];
        cce_cascade *cas;
        cce_block *blk;
        uint8_t *trit;
        float *scale, *bias;
        if (fread(&one, sizeof(int), 1, fp) != 1 || !one) break;
        if (fread(&nl, sizeof(int), 1, fp) != 1 || nl < 1 || nl >= 160) break;
        if (fread(name, 1, (size_t)nl, fp) != (size_t)nl) break;
        name[nl] = 0;
        if (ver >= 2) {
            if (fread(&bidx, sizeof(int), 1, fp) != 1) break;
        } else {
            bidx = 0; /* v1: only blocks[0] was written */
        }
        if (fread(&in, sizeof(int), 1, fp) != 1) break;
        if (fread(&out, sizeof(int), 1, fp) != 1) break;
        if (fread(&bpr, sizeof(int), 1, fp) != 1) break;
        trit = (uint8_t *)malloc((size_t)in * bpr);
        scale = (float *)malloc((size_t)out * sizeof(float));
        bias = (float *)malloc((size_t)out * sizeof(float));
        if (!trit || !scale || !bias ||
            fread(trit, 1, (size_t)in * bpr, fp) != (size_t)in * bpr ||
            fread(scale, sizeof(float), (size_t)out, fp) != (size_t)out ||
            fread(bias, sizeof(float), (size_t)out, fp) != (size_t)out) {
            free(trit);
            free(scale);
            free(bias);
            break;
        }
        cas = cce_forest_get_resident(f, name);
        if (cas && bidx >= 0 && bidx < cas->num_blocks) {
            blk = &cas->blocks[bidx];
            if (blk->weights.ndim == 2 && blk->weights.shape[0] == in &&
                blk->weights.shape[1] == out) {
                clear_quant(blk);
                blk->w_trit = trit;
                blk->w_trit_bpr = bpr;
                blk->w_scale = scale;
                ladder_expand_trit_to_wq(blk, trit, in, out, bpr);
                if (blk->bias.data && blk->bias.numel == (size_t)out)
                    memcpy(blk->bias.data, bias, (size_t)out * sizeof(float));
                free(bias);
                bias = NULL;
                cce_block_freeze(blk);
                trit = NULL;
                scale = NULL;
                loaded++;
            }
        }
        free(trit);
        free(scale);
        free(bias);
    }
    fclose(fp);
    return loaded;
}
