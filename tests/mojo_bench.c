/* C vs Mojo ternary matvec timing on identical shapes.

   This is the number that decides whether the bridge earns its place, and
   per the project's own call, whether it gets pushed.

   Runs C-only where the Mojo toolchain is absent — which includes the Windows
   box, since Mojo has no native Windows support. That mode is not a
   degradation: it verifies the harness before it ever sees a Mojo kernel.

   Expect the stage-1 Mojo kernel to be SLOWER than C: it is a naive triple
   loop written to match the C accumulation order exactly, while the C kernel
   is tiled, LUT-decoded and OpenMP'd. Its job is to prove the bridge, not to
   win. A loss here is information, not failure.

   Spec: docs/superpowers/specs/2026-08-16-mojo-kernel-bridge-design.md */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../include/cce/cce_mojo_kernel.h"

#define BPR(out_dim) (((out_dim) + 4) / 5)

static void fill(float* in, uint8_t* wt, float* ws, float* bi,
                 int in_dim, int out_dim, int bpr) {
    unsigned long long r = 0x9E3779B97F4A7C15ULL;
    int i;
    for (i = 0; i < in_dim; ++i) {
        r = r * 6364136223846793005ULL + 1442695040888963407ULL;
        in[i] = (float)((double)(r >> 33) / 2147483648.0 - 1.0);
    }
    for (i = 0; i < in_dim * bpr; ++i) {
        r = r * 6364136223846793005ULL + 1442695040888963407ULL;
        wt[i] = (uint8_t)((r >> 33) % 243);
    }
    for (i = 0; i < out_dim; ++i) {
        r = r * 6364136223846793005ULL + 1442695040888963407ULL;
        ws[i] = 0.01f + (float)((double)(r >> 40) / 8388608.0) * 0.1f;
        bi[i] = 0.0f;
    }
}

int main(void) {
    static const int shapes[][2] = {
        {512, 512}, {1024, 1024}, {2048, 2048}, {4096, 4096}
    };
    int n = (int)(sizeof shapes / sizeof shapes[0]), s;

    setvbuf(stdout, NULL, _IONBF, 0);
    printf("=== Mojo trit matvec benchmark ===\n");
    printf("mojo available: %s\n",
           cce_mojo_available() ? "yes" : "no (C only)");
    printf("%-14s %12s %12s %9s\n", "shape", "C ms", "mojo ms", "speedup");

    for (s = 0; s < n; ++s) {
        int in_dim = shapes[s][0], out_dim = shapes[s][1], bpr = BPR(out_dim);
        float *in = (float*)malloc(sizeof(float) * (size_t)in_dim);
        uint8_t *wt = (uint8_t*)malloc((size_t)in_dim * (size_t)bpr);
        float *ws = (float*)malloc(sizeof(float) * (size_t)out_dim);
        float *bi = (float*)malloc(sizeof(float) * (size_t)out_dim);
        float *out = (float*)malloc(sizeof(float) * (size_t)out_dim);
        int it, iters;
        clock_t t0, t1;
        double c_ms, m_ms = -1.0;
        char shape[32];
        /* Time a minimum DURATION, not a fixed count: clock() resolution is
           coarse enough that 20 iterations of a small shape reads as 0.000 ms
           and makes the speedup column meaningless. */
        const double min_ms = 50.0;

        if (!in || !wt || !ws || !bi || !out) { printf("alloc failed\n"); return 1; }
        fill(in, wt, ws, bi, in_dim, out_dim, bpr);

        /* warm the caches so the first shape is not penalised */
        cce_trit_matmul_c(in, wt, ws, bi, out, in_dim, out_dim, bpr, 1);
        iters = 0;
        t0 = clock();
        do {
            for (it = 0; it < 20; ++it)
                cce_trit_matmul_c(in, wt, ws, bi, out, in_dim, out_dim, bpr, 1);
            iters += 20;
            t1 = clock();
        } while (1000.0 * (double)(t1 - t0) / CLOCKS_PER_SEC < min_ms);
        c_ms = 1000.0 * (double)(t1 - t0) / CLOCKS_PER_SEC / iters;

        if (cce_mojo_available()) {
            if (cce_mojo_dispatch_trit(in, wt, ws, bi, out, in_dim, out_dim,
                                       bpr, 1) == 0) {
                int m_iters = 0;
                t0 = clock();
                do {
                    for (it = 0; it < 20; ++it)
                        (void)cce_mojo_dispatch_trit(in, wt, ws, bi, out,
                                                     in_dim, out_dim, bpr, 1);
                    m_iters += 20;
                    t1 = clock();
                } while (1000.0 * (double)(t1 - t0) / CLOCKS_PER_SEC < min_ms);
                m_ms = 1000.0 * (double)(t1 - t0) / CLOCKS_PER_SEC / m_iters;
            }
        }

        snprintf(shape, sizeof shape, "%dx%d", in_dim, out_dim);
        if (m_ms > 0.0)
            printf("%-14s %12.3f %12.3f %8.2fx\n", shape, c_ms, m_ms,
                   c_ms / m_ms);
        else
            printf("%-14s %12.3f %12s %9s\n", shape, c_ms, "-", "-");

        free(in); free(wt); free(ws); free(bi); free(out);
    }

    if (cce_mojo_fallback_count())
        printf("\nWARNING: %lu Mojo calls fell back to C — the kernel was "
               "enabled but declined or failed.\n", cce_mojo_fallback_count());
    printf("MOJO BENCH DONE\n");
    return 0;
}
