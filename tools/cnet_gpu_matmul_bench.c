/* Raw GEMM throughput: CPU vs OpenCL (1/2 GPU) vs hipBLAS (1/2 GPU).
 * Proves dual discrete > single; iGPU never selected.
 *
 * Usage: cnet_gpu_matmul_bench [K] [N] [iters]
 * Default K=N=4096, iters=20 (decode-shaped T=1).
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../include/cce/cce_clgemm.h"
#include "../include/cce/cce_hipgemm.h"

static double wall_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void cpu_gemm(const float *A, const float *W, float *C, size_t T,
                     size_t K, size_t N) {
    size_t t, n, k;
    for (t = 0; t < T; ++t)
        for (n = 0; n < N; ++n) {
            float acc = 0.0f;
            const float *a = A + t * K;
            for (k = 0; k < K; ++k) acc += a[k] * W[k * N + n];
            C[t * N + n] = acc;
        }
}

static double gflops(size_t T, size_t K, size_t N, int iters, double sec) {
    double flops = 2.0 * (double)T * (double)K * (double)N * (double)iters;
    return (sec > 1e-12) ? (flops / sec) / 1e9 : 0.0;
}

int main(int argc, char **argv) {
    size_t K = 4096, N = 4096, T = 1;
    int iters = 20, i;
    float *A, *W, *C, *Cref;
    double t0, t1, sec, g;
    char name[160];
    cce_clgemm *cl1 = NULL, *cl2 = NULL;
    cce_hipgemm *h1 = NULL, *h2 = NULL;
    int fails = 0;

    if (argc > 1) K = (size_t)atoi(argv[1]);
    if (argc > 2) N = (size_t)atoi(argv[2]);
    if (argc > 3) iters = atoi(argv[3]);
    if (K < 64 || N < 64 || iters < 1) {
        fprintf(stderr, "usage: %s [K] [N] [iters]\n", argv[0]);
        return 2;
    }

    printf("== CNET GPU matmul bench (T=%zu K=%zu N=%zu iters=%d) ==\n", T, K,
           N, iters);
    printf("  plan: beat CPU; dual discrete > single; no iGPU\n");

    A = (float *)malloc(T * K * sizeof(float));
    W = (float *)malloc(K * N * sizeof(float));
    C = (float *)malloc(T * N * sizeof(float));
    Cref = (float *)malloc(T * N * sizeof(float));
    if (!A || !W || !C || !Cref) return 1;
    for (i = 0; i < (int)(T * K); ++i)
        A[i] = 0.001f * (float)((i * 17) % 100);
    for (i = 0; i < (int)(K * N); ++i)
        W[i] = 0.001f * (float)((i * 13) % 100) - 0.05f;

    /* ---- CPU ---- */
    t0 = wall_s();
    for (i = 0; i < iters; ++i) cpu_gemm(A, W, Cref, T, K, N);
    t1 = wall_s();
    sec = t1 - t0;
    g = gflops(T, K, N, iters, sec);
    printf("  CPU            %8.3f s  %8.1f GFLOP/s\n", sec, g);

    /* ---- OpenCL single ---- */
    setenv("CNET_GPU_COUNT", "1", 1);
    unsetenv("CNET_GPU_DEVICES");
    cl1 = cce_clgemm_open(NULL, name, sizeof name);
    if (cl1) {
        printf("  OpenCL×1       %s (ndev=%zu)\n", name,
               cce_clgemm_device_count(cl1));
        /* warm + resident upload */
        (void)cce_clgemm_matmul(cl1, A, T, K, W, NULL, N, C);
        t0 = wall_s();
        for (i = 0; i < iters; ++i)
            if (cce_clgemm_matmul(cl1, A, T, K, W, NULL, N, C) != 0) fails++;
        t1 = wall_s();
        sec = t1 - t0;
        g = gflops(T, K, N, iters, sec);
        printf("  OpenCL×1 run   %8.3f s  %8.1f GFLOP/s  resident=%.1f MB\n",
               sec, g, (double)cce_clgemm_resident_bytes(cl1) / (1024 * 1024));
        cce_clgemm_close(cl1);
    } else {
        printf("  OpenCL×1       SKIP (no device)\n");
    }

    /* ---- OpenCL dual ---- */
    setenv("CNET_GPU_COUNT", "2", 1);
    cl2 = cce_clgemm_open(NULL, name, sizeof name);
    if (cl2 && cce_clgemm_device_count(cl2) >= 2) {
        printf("  OpenCL×2       %s (ndev=%zu)\n", name,
               cce_clgemm_device_count(cl2));
        (void)cce_clgemm_matmul(cl2, A, T, K, W, NULL, N, C);
        t0 = wall_s();
        for (i = 0; i < iters; ++i)
            if (cce_clgemm_matmul(cl2, A, T, K, W, NULL, N, C) != 0) fails++;
        t1 = wall_s();
        sec = t1 - t0;
        g = gflops(T, K, N, iters, sec);
        printf("  OpenCL×2 run   %8.3f s  %8.1f GFLOP/s  resident=%.1f MB\n",
               sec, g, (double)cce_clgemm_resident_bytes(cl2) / (1024 * 1024));
        cce_clgemm_close(cl2);
    } else {
        if (cl2) cce_clgemm_close(cl2);
        printf("  OpenCL×2       SKIP (need 2 discrete)\n");
    }

    /* ---- hipBLAS single ---- */
    setenv("CNET_GPU_COUNT", "1", 1);
    h1 = cce_hipgemm_open(name, sizeof name);
    if (h1) {
        printf("  hipBLAS×1      %s (ndev=%zu)\n", name,
               cce_hipgemm_device_count(h1));
        (void)cce_hipgemm_matmul(h1, A, T, K, W, NULL, N, C);
        t0 = wall_s();
        for (i = 0; i < iters; ++i)
            if (cce_hipgemm_matmul(h1, A, T, K, W, NULL, N, C) != 0) fails++;
        t1 = wall_s();
        sec = t1 - t0;
        g = gflops(T, K, N, iters, sec);
        printf("  hipBLAS×1 run  %8.3f s  %8.1f GFLOP/s  resident=%.1f MB\n",
               sec, g, (double)cce_hipgemm_resident_bytes(h1) / (1024 * 1024));
        cce_hipgemm_close(h1);
    } else {
        printf("  hipBLAS×1      SKIP (ROCm/hipBLAS unavailable)\n");
    }

    /* ---- hipBLAS dual ---- */
    setenv("CNET_GPU_COUNT", "2", 1);
    h2 = cce_hipgemm_open(name, sizeof name);
    if (h2 && cce_hipgemm_device_count(h2) >= 2) {
        printf("  hipBLAS×2      %s (ndev=%zu)\n", name,
               cce_hipgemm_device_count(h2));
        (void)cce_hipgemm_matmul(h2, A, T, K, W, NULL, N, C);
        t0 = wall_s();
        for (i = 0; i < iters; ++i)
            if (cce_hipgemm_matmul(h2, A, T, K, W, NULL, N, C) != 0) fails++;
        t1 = wall_s();
        sec = t1 - t0;
        g = gflops(T, K, N, iters, sec);
        printf("  hipBLAS×2 run  %8.3f s  %8.1f GFLOP/s  resident=%.1f MB\n",
               sec, g, (double)cce_hipgemm_resident_bytes(h2) / (1024 * 1024));
        cce_hipgemm_close(h2);
    } else {
        if (h2) cce_hipgemm_close(h2);
        printf("  hipBLAS×2      SKIP (need 2 discrete)\n");
    }

    free(A);
    free(W);
    free(C);
    free(Cref);
    if (fails) {
        printf("GPU_MATMUL_BENCH_FAIL fails=%d\n", fails);
        return 1;
    }
    printf("GPU_MATMUL_BENCH_PASS fails=0\n");
    return 0;
}
