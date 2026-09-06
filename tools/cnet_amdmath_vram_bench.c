/* 3-tier VRAM exposure vs training throughput.
 *
 * Corpus of expert maps on host. Each step SGD-updates one expert.
 * Sweep HOT budget: how much W we keep resident vs stream.
 * Leaves ≥8 GiB free on the card. Not CERT.
 *
 * Args: [experts] [dim] [N] [steps]
 *   experts default 256, dim 1024, N 128, steps 80
 */
#include "../include/cce/cce_amdmath.h"

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

static float *fill_align(size_t n, unsigned seed)
{
    float *p = NULL;
    if (posix_memalign((void **)&p, 4096, n * sizeof(float)) != 0)
        return NULL;
    unsigned s = seed;
    for (size_t i = 0; i < n; i++) {
        s = s * 1664525u + 1013904223u;
        p[i] = ((int)(s >> 16) % 2001 - 1000) / 500.0f;
    }
    return p;
}

static int zipf_pick(unsigned *rng, int n_hot, int n_total)
{
    *rng = *rng * 1664525u + 1013904223u;
    /* 80% of steps hit the first n_hot experts */
    if (((*rng >> 16) % 100) < 80)
        return (int)((*rng >> 8) % (unsigned)n_hot);
    return n_hot + (int)((*rng >> 4) % (unsigned)(n_total - n_hot));
}

static int run_mode(cce_amdmath *h, float **W, int nexp, size_t wbytes, size_t dim,
                    size_t N, int steps, size_t hot_bytes, size_t warm_bytes,
                    const char *tag, int zipf)
{
    cce_amdmath_pool *p = cce_amdmath_pool_open(h, hot_bytes, warm_bytes);
    if (!p) {
        printf("FAIL pool_open %s\n", tag);
        return 1;
    }
    int pref = (warm_bytes > 0) ? CCE_AMDMATH_TIER_WARM : CCE_AMDMATH_TIER_COLD;
    for (int i = 0; i < nexp; i++) {
        if (cce_amdmath_pool_bind(p, i, W[i], wbytes, pref) != 0) {
            printf("FAIL bind %d %s: %s\n", i, tag, cce_amdmath_last_error(h));
            cce_amdmath_pool_close(p);
            return 1;
        }
    }
    float *X = fill_align(N * dim, 99);
    float *dY = fill_align(N * dim, 100);
    if (!X || !dY) {
        printf("FAIL X/dY alloc\n");
        free(X);
        free(dY);
        cce_amdmath_pool_close(p);
        return 1;
    }
    int n_hot_set = nexp / 5;
    if (n_hot_set < 1)
        n_hot_set = 1;
    unsigned rng = 1;
    /* warmup */
    (void)cce_amdmath_pool_sgd_f32(p, 0, X, dY, N, dim, dim, 0.01f);
    double t0 = now_s();
    for (int s = 0; s < steps; s++) {
        int id = zipf ? zipf_pick(&rng, n_hot_set, nexp) : (int)((rng = rng * 1664525u + 1013904223u) % (unsigned)nexp);
        if (cce_amdmath_pool_sgd_f32(p, id, X, dY, N, dim, dim, 0.01f) != 0) {
            printf("FAIL sgd step %d %s: %s\n", s, tag, cce_amdmath_last_error(h));
            free(X);
            free(dY);
            cce_amdmath_pool_close(p);
            return 1;
        }
    }
    double sec = now_s() - t0;
    cce_amdmath_pool_stats st;
    cce_amdmath_pool_get_stats(p, &st);
    double flop = (double)steps * 2.0 * (double)N * (double)dim * (double)dim; /* SGD is one GEMM-shaped */
    printf("%-18s  %7.3fs  %7.1f step/s  %7.1f GFLOP/s  hot=%5.0fMiB  "
           "hits h/w/c=%llu/%llu/%llu  evicts=%llu  vfree=%.1fGiB\n",
           tag, sec, (double)steps / sec, flop * 1e-9 / sec,
           st.hot_used / (1024. * 1024.), (unsigned long long)st.hits_hot,
           (unsigned long long)st.hits_warm, (unsigned long long)st.hits_cold,
           (unsigned long long)st.evicts, st.vram_free / (1024. * 1024. * 1024.));
    free(X);
    free(dY);
    cce_amdmath_pool_close(p);
    return 0;
}

int main(int argc, char **argv)
{
    int nexp = argc > 1 ? atoi(argv[1]) : 256;
    int dim = argc > 2 ? atoi(argv[2]) : 1024;
    int N = argc > 3 ? atoi(argv[3]) : 128;
    int steps = argc > 4 ? atoi(argv[4]) : 80;
    if (nexp < 8)
        nexp = 8;
    if (nexp > 1024)
        nexp = 1024;
    if (dim < 64)
        dim = 64;

    char name[256];
    cce_amdmath *h = cce_amdmath_open(name, sizeof name);
    if (!h) {
        fprintf(stderr, "no discrete GPU\n");
        return 1;
    }
    size_t vfree = 0, vtot = 0;
    cce_amdmath_vram_info(h, &vfree, &vtot);
    const size_t keep_free = (size_t)8 << 30; /* leave 8 GiB */
    size_t usable = (vfree > keep_free) ? vfree - keep_free : vfree / 4;
    size_t wbytes = (size_t)dim * (size_t)dim * sizeof(float);
    size_t corpus = (size_t)nexp * wbytes;
    printf("device %s  vram tot=%.2fGiB free=%.2fGiB usable<=%.2fGiB\n", name,
           vtot / (1024. * 1024. * 1024.), vfree / (1024. * 1024. * 1024.),
           usable / (1024. * 1024. * 1024.));
    printf("corpus %d experts × %d² f32 = %.2f GiB  N=%d steps=%d\n", nexp, dim,
           corpus / (1024. * 1024. * 1024.), N, steps);
    printf("cce_amdmath %s\n", cce_amdmath_version());

    float **W = (float **)calloc((size_t)nexp, sizeof *W);
    for (int i = 0; i < nexp; i++) {
        W[i] = fill_align((size_t)dim * (size_t)dim, (unsigned)(1000 + i));
        if (!W[i]) {
            fprintf(stderr, "OOM host expert %d\n", i);
            return 1;
        }
    }

    const size_t mib = 1024ull * 1024ull;
    struct {
        const char *tag;
        size_t hot;
        size_t warm;
        int zipf;
    } cases[] = {
        {"cold-uniform", 0, 0, 0},
        {"cold-zipf", 0, 0, 1},
        {"warm-zipf", 0, corpus, 1},
        {"hot64Mi-zipf", 64 * mib, corpus, 1},
        {"hot256Mi-zipf", 256 * mib, corpus, 1},
        {"hot1Gi-zipf", 1ull << 30, corpus, 1},
        {"hot-all-zipf", corpus, corpus, 1},
        {"hot-all-uniform", corpus, corpus, 0},
    };
    int rc = 0;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        size_t hot = cases[i].hot;
        if (hot > usable)
            hot = usable;
        if (hot > 0 && hot < wbytes)
            hot = wbytes;
        if (run_mode(h, W, nexp, wbytes, (size_t)dim, (size_t)N, steps, hot, cases[i].warm,
                     cases[i].tag, cases[i].zipf)) {
            rc = 1;
            break;
        }
    }

    for (int i = 0; i < nexp; i++)
        free(W[i]);
    free(W);
    cce_amdmath_close(h);
    if (rc)
        return 1;
    printf("AMDMATH_VRAM_PASS\n");
    return 0;
}
