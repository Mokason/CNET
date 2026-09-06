/* N-gram skip in front of fat GPU SGD (cce_amdmath HOT pool).
 * Not CERT. Bonsai stays on GPU1 — run with HIP_VISIBLE_DEVICES=0.
 *
 * Modes:
 *   gpu-all   every token → GPU batch SGD
 *   hybrid    skip GPU when n-gram top-1 == target and p >= tau
 */
#include "cce/cce_amdmath.h"
#include "cce/cce_ngram.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <x86intrin.h>

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static uint64_t cycles_now(void)
{
    unsigned aux;
    return __rdtscp(&aux);
}

static unsigned rng_u(unsigned *s)
{
    *s = *s * 1664525u + 1013904223u;
    return *s;
}

static int ngram_hit(cce_ngram *ng, const uint32_t *ctx, int ctx_n, uint32_t target,
                     float tau)
{
    uint32_t tok[4];
    float p[4];
    int k = 0;
    if (cce_ngram_predict(ng, ctx, ctx_n, tok, p, 1, &k) != CCE_NGRAM_OK || k < 1)
        return 0;
    return tok[0] == target && p[0] >= tau;
}

typedef struct {
    const char *name;
    long tokens;
    long gpu_steps;
    long skipped;
    double sec;
    uint64_t cyc_gpu;
    uint64_t cyc_ng;
} Run;

static int run_mode(cce_amdmath *h, const uint32_t *toks, int ntok, int V, int D, int N,
                    int epochs, int skip, float tau, const float *E, Run *out)
{
    size_t wbytes = (size_t)D * (size_t)D * sizeof(float);
    float *W = (float *)malloc(wbytes);
    if (!W)
        return 1;
    unsigned s = 7;
    for (size_t i = 0; i < (size_t)D * D; i++)
        W[i] = ((int)(rng_u(&s) >> 16) % 2001 - 1000) / 2000.0f;

    cce_amdmath_pool *pool = cce_amdmath_pool_open(h, wbytes, wbytes);
    if (!pool) {
        free(W);
        return 1;
    }
    if (cce_amdmath_pool_bind(pool, 0, W, wbytes, CCE_AMDMATH_TIER_HOT) ||
        cce_amdmath_pool_touch(pool, 0)) {
        fprintf(stderr, "pool: %s\n", cce_amdmath_last_error(h));
        cce_amdmath_pool_close(pool);
        free(W);
        return 1;
    }

    cce_ngram *ng = cce_ngram_open(3, 1u << 15);
    float *Xb = (float *)calloc((size_t)N * D, sizeof(float));
    float *Yb = (float *)calloc((size_t)N * D, sizeof(float));
    int fill = 0;
    long gpu = 0, skipped = 0;
    uint64_t cg = 0, cn = 0;
    double t0 = now_s();

    for (int ep = 0; ep < epochs; ep++) {
        for (int i = 2; i < ntok; i++) {
            uint32_t ctx[2] = {toks[i - 2], toks[i - 1]};
            uint32_t tgt = toks[i];
            uint64_t a = cycles_now();
            int hit = skip && ep > 0 && ngram_hit(ng, ctx, 2, tgt, tau);
            cn += cycles_now() - a;
            (void)cce_ngram_add(ng, ctx, 2, tgt, 1);
            (void)cce_ngram_add(ng, ctx + 1, 1, tgt, 1);
            (void)cce_ngram_add(ng, NULL, 0, tgt, 1);
            if (hit) {
                skipped++;
                continue;
            }
            memcpy(Xb + (size_t)fill * D, E + (size_t)(tgt % (uint32_t)V) * D,
                   (size_t)D * sizeof(float));
            memcpy(Yb + (size_t)fill * D, E + (size_t)ctx[1] * D, (size_t)D * sizeof(float));
            fill++;
            if (fill == N) {
                a = cycles_now();
                int rc = cce_amdmath_pool_sgd_f32(pool, 0, Xb, Yb, (size_t)N, (size_t)D,
                                                  (size_t)D, 0.01f);
                cg += cycles_now() - a;
                if (rc != 0) {
                    fprintf(stderr, "sgd rc=%d %s\n", rc, cce_amdmath_last_error(h));
                    cce_ngram_close(ng);
                    cce_amdmath_pool_close(pool);
                    free(W);
                    free(Xb);
                    free(Yb);
                    return 1;
                }
                gpu++;
                fill = 0;
            }
        }
    }
    double sec = now_s() - t0;
    out->tokens = (long)(ntok - 2) * epochs;
    out->gpu_steps = gpu;
    out->skipped = skipped;
    out->sec = sec;
    out->cyc_gpu = cg;
    out->cyc_ng = cn;
    cce_ngram_close(ng);
    cce_amdmath_pool_close(pool);
    free(W);
    free(Xb);
    free(Yb);
    (void)V;
    return 0;
}

int main(int argc, char **argv)
{
    int D = argc > 1 ? atoi(argv[1]) : 1024;
    int N = argc > 2 ? atoi(argv[2]) : 128;
    int ntok = argc > 3 ? atoi(argv[3]) : 8192;
    int epochs = argc > 4 ? atoi(argv[4]) : 3;
    if (D < 64)
        D = 64;
    if (N < 16)
        N = 16;
    int V = 64;
    float tau = 0.70f;

    char name[256];
    cce_amdmath *h = cce_amdmath_open(name, sizeof name);
    if (!h) {
        fprintf(stderr, "no discrete GPU\n");
        return 1;
    }
    size_t fr = 0, tot = 0;
    cce_amdmath_vram_info(h, &fr, &tot);
    printf("hybrid-gpu device=%s free=%.2fGiB  D=%d N=%d toks=%d epochs=%d  %s\n", name,
           fr / (1024. * 1024. * 1024.), D, N, ntok, epochs, cce_amdmath_version());

    float *E = (float *)malloc((size_t)V * D * sizeof(float));
    unsigned s = 1;
    for (int i = 0; i < V * D; i++)
        E[i] = ((int)(rng_u(&s) >> 16) % 2001 - 1000) / 1000.0f;

    uint32_t *toks = (uint32_t *)malloc((size_t)ntok * sizeof(uint32_t));
    /* 80% repeating 0,1,2,3 formula; 20% random 16..V-1 */
    for (int i = 0; i < ntok; i++) {
        if ((rng_u(&s) % 100) < 80)
            toks[i] = (uint32_t)(i % 4);
        else
            toks[i] = 16u + (rng_u(&s) % (uint32_t)(V - 16));
    }

    Run all, hy;
    memset(&all, 0, sizeof all);
    memset(&hy, 0, sizeof hy);
    all.name = "gpu-all";
    hy.name = "hybrid-skip";

    if (run_mode(h, toks, ntok, V, D, N, epochs, 0, tau, E, &all))
        return 1;
    if (run_mode(h, toks, ntok, V, D, N, epochs, 1, tau, E, &hy))
        return 1;

    printf("%-12s  toks=%ld  gpu_steps=%ld  skipped=%ld  %.3fs  gpu_cyc=%.3e  "
           "ng_cyc=%.3e\n",
           all.name, all.tokens, all.gpu_steps, all.skipped, all.sec, (double)all.cyc_gpu,
           (double)all.cyc_ng);
    printf("%-12s  toks=%ld  gpu_steps=%ld  skipped=%ld  %.3fs  gpu_cyc=%.3e  "
           "ng_cyc=%.3e\n",
           hy.name, hy.tokens, hy.gpu_steps, hy.skipped, hy.sec, (double)hy.cyc_gpu,
           (double)hy.cyc_ng);
    double step_r = (double)hy.gpu_steps / (double)(all.gpu_steps ? all.gpu_steps : 1);
    double time_r = hy.sec / (all.sec > 1e-9 ? all.sec : 1e-9);
    double cyc_r = (double)(hy.cyc_gpu + hy.cyc_ng) /
                   (double)((all.cyc_gpu + all.cyc_ng) ? (all.cyc_gpu + all.cyc_ng) : 1);
    printf("ratios  gpu_steps=%.3f  wall=%.3f  cycles=%.3f  (want < 1)\n", step_r, time_r,
           cyc_r);

    cce_amdmath_close(h);
    free(E);
    free(toks);
    int pass = hy.gpu_steps < all.gpu_steps && hy.sec < all.sec;
    if (!pass) {
        printf("HYBRID_GPU_FAIL\n");
        return 1;
    }
    printf("HYBRID_GPU_PASS\n");
    return 0;
}
