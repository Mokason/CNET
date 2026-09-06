/* Dual-runtime: GPU-A trains (SGD), GPU-B evals + BitNet absmean ternary.
 * Independent HIP processes (HIP_VISIBLE_DEVICES). No RCCL. Not CERT.
 *
 *   HIP_VISIBLE_DEVICES=0 ./bin/cnet_amdmath_dual_runtime train [seconds]
 *   HIP_VISIBLE_DEVICES=1 ./bin/cnet_amdmath_dual_runtime eval  [seconds]
 */
#include "cce/cce_amdmath.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static float *fill(size_t n, unsigned seed)
{
    float *p = (float *)malloc(n * sizeof(float));
    unsigned s = seed;
    for (size_t i = 0; i < n; i++) {
        s = s * 1664525u + 1013904223u;
        p[i] = ((int)(s >> 16) % 2001 - 1000) / 500.0f;
    }
    return p;
}

/* Host ternary: per-output absmean, codes in {-1,0,1} (BitNet b1.58 / CCE). */
static size_t ternary_pack_host(const float *W, int8_t *codes, float *gamma, size_t out,
                                size_t in_dim)
{
    size_t nz = 0;
    for (size_t o = 0; o < out; o++) {
        double s = 0.0;
        const float *row = W + o * in_dim;
        for (size_t i = 0; i < in_dim; i++)
            s += fabs((double)row[i]);
        float g = in_dim ? (float)(s / (double)in_dim) : 0.f;
        gamma[o] = g;
        for (size_t i = 0; i < in_dim; i++) {
            int code = 0;
            if (g > 0.f) {
                float r = roundf(row[i] / g);
                if (r > 1)
                    r = 1;
                if (r < -1)
                    r = -1;
                code = (int)r;
            }
            codes[o * in_dim + i] = (int8_t)code;
            if (code)
                nz++;
        }
    }
    return nz;
}

static int run_train(cce_amdmath *h, double seconds)
{
    const size_t E = 32, dim = 1024, N = 128;
    const size_t wbytes = dim * dim * sizeof(float);
    float **W = (float **)calloc(E, sizeof *W);
    for (size_t i = 0; i < E; i++)
        W[i] = fill(dim * dim, (unsigned)(10 + i));
    float *X = fill(N * dim, 1);
    float *dY = fill(N * dim, 2);
    cce_amdmath_pool *p = cce_amdmath_pool_open(h, E * wbytes, E * wbytes);
    if (!p)
        return 1;
    for (size_t i = 0; i < E; i++)
        if (cce_amdmath_pool_bind(p, (int)i, W[i], wbytes, CCE_AMDMATH_TIER_HOT))
            return 1;
    for (size_t i = 0; i < E; i++)
        (void)cce_amdmath_pool_touch(p, (int)i);
    int steps = 0;
    double t0 = now_s();
    while (now_s() - t0 < seconds) {
        if (cce_amdmath_pool_sgd_f32(p, steps % (int)E, X, dY, N, dim, dim, 0.01f)) {
            fprintf(stderr, "train sgd fail: %s\n", cce_amdmath_last_error(h));
            return 1;
        }
        steps++;
    }
    double sec = now_s() - t0;
    printf("TRAIN steps=%d  %.2fs  %.1f step/s  dim=%zu E=%zu\n", steps, sec,
           steps / sec, dim, E);
    cce_amdmath_pool_close(p);
    for (size_t i = 0; i < E; i++)
        free(W[i]);
    free(W);
    free(X);
    free(dY);
    return 0;
}

static int run_eval(cce_amdmath *h, double seconds)
{
    const size_t dim = 1024, N = 128;
    float *W = fill(dim * dim, 7);
    float *X = fill(N * dim, 8);
    float *Y = (float *)calloc(N * dim, sizeof(float));
    int8_t *codes = (int8_t *)malloc(dim * dim);
    float *gamma = (float *)malloc(dim * sizeof(float));
    int rounds = 0;
    size_t last_nz = 0;
    double t0 = now_s();
    double t_tern = 0, t_eval = 0;
    while (now_s() - t0 < seconds) {
        double a = now_s();
        int rc = cce_amdmath_linear_f32(h, X, W, Y, 1, N, dim, dim);
        t_eval += now_s() - a;
        if (rc == CCE_AMDMATH_FLOOR) {
            /* dim 1024 N 128 is above floor; if not, bump */
        } else if (rc != 0) {
            fprintf(stderr, "eval linear fail rc=%d %s\n", rc, cce_amdmath_last_error(h));
            return 1;
        }
        a = now_s();
        last_nz = ternary_pack_host(W, codes, gamma, dim, dim);
        t_tern += now_s() - a;
        /* mutate W slightly so ternary isn't a dead store */
        W[(size_t)rounds % (dim * dim)] += 0.001f;
        rounds++;
    }
    double sec = now_s() - t0;
    printf("EVAL  rounds=%d  %.2fs  %.1f round/s  eval=%.3fs tern=%.3fs  nz=%zu/%zu\n",
           rounds, sec, rounds / sec, t_eval, t_tern, last_nz, dim * dim);
    free(W);
    free(X);
    free(Y);
    free(codes);
    free(gamma);
    return 0;
}

int main(int argc, char **argv)
{
    const char *role = argc > 1 ? argv[1] : "eval";
    double seconds = argc > 2 ? atof(argv[2]) : 8.0;
    if (seconds < 1)
        seconds = 1;
    char name[256];
    cce_amdmath *h = cce_amdmath_open(name, sizeof name);
    if (!h) {
        fprintf(stderr, "no discrete GPU\n");
        return 1;
    }
    size_t fr = 0, tot = 0;
    cce_amdmath_vram_info(h, &fr, &tot);
    printf("role=%s device=%s free=%.2fGiB tot=%.2fGiB  %s\n", role, name,
           fr / (1024. * 1024. * 1024.), tot / (1024. * 1024. * 1024.),
           cce_amdmath_version());
    int rc = 0;
    if (strcmp(role, "train") == 0)
        rc = run_train(h, seconds);
    else
        rc = run_eval(h, seconds);
    cce_amdmath_close(h);
    return rc;
}
