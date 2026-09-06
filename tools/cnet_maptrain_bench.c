#ifndef CCE_MAPTRAIN_CPU_ONLY
#include "cce/cce_amdmath.h"
#endif
#include "cce/cce_maptrain.h"

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

static void fill32(float *a, size_t n, unsigned s)
{
    for (size_t i = 0; i < n; i++)
        a[i] = (float)((s + (unsigned)i * 17u) % 200) / 100.f - 1.f;
}

static void fill64(double *a, size_t n, unsigned s)
{
    for (size_t i = 0; i < n; i++)
        a[i] = (double)((s + (unsigned)i * 17u) % 200) / 100.0 - 1.0;
}

static void run_case(cce_amdmath *gpu, const char *tag, size_t E, size_t N, size_t in_dim,
                     size_t out_dim, int repeats)
{
    size_t nw = E * out_dim * in_dim, nx = E * N * in_dim, ny = E * N * out_dim;
    double flops = 2.0 * (double)E * (double)N * (double)in_dim * (double)out_dim * (double)repeats;
    printf("case %s  E=%zu N=%zu in=%zu out=%zu repeats=%d\n", tag, E, N, in_dim, out_dim,
           repeats);

    float *Wf = (float *)malloc(nw * sizeof(float));
    float *Xf = (float *)malloc(nx * sizeof(float));
    float *Yf = (float *)malloc(ny * sizeof(float));
    fill32(Wf, nw, 1);
    fill32(Xf, nx, 2);
    fill32(Yf, ny, 3);
    double t0 = now_s();
    for (int r = 0; r < repeats; r++)
        cce_maptrain_sgd(NULL, CCE_MAPTRAIN_F32, Wf, Xf, Yf, E, N, in_dim, out_dim, 1e-4);
    double tcpu32 = now_s() - t0;
    printf("  cpu f32  %.4f s  %.1f GFLOP/s\n", tcpu32, (flops / tcpu32) / 1e9);

    double tgpu32 = -1;
    if (gpu) {
        fill32(Wf, nw, 1);
        t0 = now_s();
        for (int r = 0; r < repeats; r++)
            cce_maptrain_sgd(gpu, CCE_MAPTRAIN_F32, Wf, Xf, Yf, E, N, in_dim, out_dim, 1e-4);
        tgpu32 = now_s() - t0;
        printf("  gpu f32  %.4f s  %.1f GFLOP/s  vsCPU %.2fx\n", tgpu32,
               (flops / tgpu32) / 1e9, tcpu32 / tgpu32);
    }
    free(Wf);
    free(Xf);
    free(Yf);

    double *Wd = (double *)malloc(nw * sizeof(double));
    double *Xd = (double *)malloc(nx * sizeof(double));
    double *Yd = (double *)malloc(ny * sizeof(double));
    fill64(Wd, nw, 1);
    fill64(Xd, nx, 2);
    fill64(Yd, ny, 3);
    t0 = now_s();
    for (int r = 0; r < repeats; r++)
        cce_maptrain_sgd(NULL, CCE_MAPTRAIN_F64, Wd, Xd, Yd, E, N, in_dim, out_dim, 1e-4);
    double tcpu64 = now_s() - t0;
    printf("  cpu f64  %.4f s  %.1f GFLOP/s  vs_f32 %.2fx\n", tcpu64, (flops / tcpu64) / 1e9,
           tcpu32 / tcpu64);

    if (gpu) {
        fill64(Wd, nw, 1);
        t0 = now_s();
        for (int r = 0; r < repeats; r++)
            cce_maptrain_sgd(gpu, CCE_MAPTRAIN_F64, Wd, Xd, Yd, E, N, in_dim, out_dim, 1e-4);
        double tgpu64 = now_s() - t0;
        printf("  gpu f64  %.4f s  %.1f GFLOP/s  vsCPU %.2fx  vs_gpu_f32 %.2fx\n", tgpu64,
               (flops / tgpu64) / 1e9, tcpu64 / tgpu64,
               tgpu32 > 0 ? tgpu32 / tgpu64 : 0.0);
    }
    free(Wd);
    free(Xd);
    free(Yd);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("cce_maptrain %s  (hashtable-style stacked SGD, f32|f64)\n",
           cce_maptrain_version());
    cce_amdmath *gpu = NULL;
#ifndef CCE_MAPTRAIN_CPU_ONLY
    char name[128];
    gpu = cce_amdmath_open_device(0, name, sizeof name);
    if (gpu)
        printf("gpu %s\n", name);
    else
        printf("gpu none — CPU only\n");
#endif
    /* Same-shape pieces as chess BACKEND: tiny in/out, many N. */
    run_case(gpu, "hashtable-tiny", 8, 8000, 10, 5, 4);
    /* Fused fat maps: GPU f32 should win; f64 stays rate-limited. */
    run_case(gpu, "fused-fat", 8, 256, 256, 256, 2);
    if (gpu)
        cce_amdmath_close(gpu);
    printf("MAPTRAIN_BENCH_PASS\n");
    return 0;
}
