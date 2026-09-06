/* Persistent-handle AMD math bench: CPU vs f32 FMA vs gfx12 WMMA.
 * No PyTorch. Does not CERT. Copies only.
 *
 * Build via `make amdmath_bench`. Args: [M] [N] [K] [repeats]
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

static void cpu_gemm(const float *A, const float *B, float *C, size_t m, size_t n, size_t k)
{
    for (size_t r = 0; r < m; r++) {
        for (size_t c = 0; c < n; c++) {
            float acc = 0.f;
            for (size_t t = 0; t < k; t++)
                acc += A[r * k + t] * B[t * n + c];
            C[r * n + c] = acc;
        }
    }
}

int main(int argc, char **argv)
{
    size_t M = argc > 1 ? (size_t)atoi(argv[1]) : 1024;
    size_t N = argc > 2 ? (size_t)atoi(argv[2]) : 1024;
    size_t K = argc > 3 ? (size_t)atoi(argv[3]) : 1024;
    int reps = argc > 4 ? atoi(argv[4]) : 5;
    if (reps < 1)
        reps = 1;

    char name[256];
    cce_amdmath *h = cce_amdmath_open(name, sizeof name);
    if (!h) {
        fprintf(stderr, "no discrete GPU\n");
        return 1;
    }
    printf("device %s  M=%zu N=%zu K=%zu reps=%d  version %s\n", name, M, N, K, reps,
           cce_amdmath_version());

    float *A = fill(M * K, 1);
    float *B = fill(K * N, 2);
    float *C = (float *)calloc(M * N, sizeof(float));
    float *R = (float *)calloc(M * N, sizeof(float));
    double flop = 2.0 * (double)M * (double)N * (double)K;

    double t0 = now_s();
    cpu_gemm(A, B, R, M, N, K);
    double tcpu = now_s() - t0;
    volatile float sink = 0.f;
    for (size_t i = 0; i < M * N; i += 17)
        sink += R[i];
    printf("CPU f32          %8.3f s  %8.1f GFLOP/s  checksum=%g\n", tcpu,
           flop * 1e-9 / (tcpu > 1e-6 ? tcpu : 1e-6), (double)sink);

    /* warmup */
    (void)cce_amdmath_gemm_f32(h, A, B, C, M, N, K);
    (void)cce_amdmath_gemm_f32_wmma(h, A, B, C, M, N, K);

    t0 = now_s();
    for (int i = 0; i < reps; i++) {
        if (cce_amdmath_gemm_f32(h, A, B, C, M, N, K) != 0) {
            printf("gemm_f32 failed: %s\n", cce_amdmath_last_error(h));
            return 1;
        }
    }
    double tfma = (now_s() - t0) / reps;
    printf("GPU f32 FMA      %8.3f s  %8.1f GFLOP/s  vsCPU %.1fx\n", tfma,
           flop * 1e-9 / tfma, tcpu / tfma);

    t0 = now_s();
    for (int i = 0; i < reps; i++) {
        if (cce_amdmath_gemm_f32_wmma(h, A, B, C, M, N, K) != 0) {
            printf("gemm_f32_wmma failed: %s\n", cce_amdmath_last_error(h));
            return 1;
        }
    }
    double tw = (now_s() - t0) / reps;
    printf("GPU bf16 WMMA    %8.3f s  %8.1f GFLOP/s  vsCPU %.1fx\n", tw, flop * 1e-9 / tw,
           tcpu / (tw > 1e-9 ? tw : 1e-9));

    {
        float mabs = 0.f;
        size_t nn = M * N;
        for (size_t i = 0; i < nn; i++) {
            float d = C[i] - R[i];
            if (d < 0)
                d = -d;
            if (d > mabs)
                mabs = d;
            sink += C[i];
        }
        printf("WMMA vs CPU max|Δ|=%g  checksum=%g\n", mabs, (double)sink);
    }

    printf("AMDMATH_BENCH_PASS\n");
    free(A);
    free(B);
    free(C);
    free(R);
    cce_amdmath_close(h);
    return 0;
}
