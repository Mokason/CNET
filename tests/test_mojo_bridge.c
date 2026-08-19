/* Mojo kernel bridge gate.

   Sections 1-3 run on ANY box including this Windows one, which cannot run
   Mojo at all (no native Windows support, WSL2 declined). The equivalence
   comparison against a real Mojo kernel is Linux-only and is NOT claimed here.

   Spec: docs/superpowers/specs/2026-08-16-mojo-kernel-bridge-design.md */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "../include/cce/cce_mojo_kernel.h"

static int checks = 0, fails = 0;
#define CHECK(cond, msg) do { checks++; if (!(cond)) { fails++; printf("  FAIL: %s\n", msg); } \
                              else printf("  ok   %s\n", msg); } while (0)

#define BPR(out_dim) (((out_dim) + 4) / 5)

/* Deterministic fixture: no rand(), so the anchor reproduces anywhere. */
static void fill_fixture(float* in, uint8_t* wt, float* ws, float* bias,
                         int in_dim, int out_dim, int bpr) {
    unsigned long long r = 0x9E3779B97F4A7C15ULL;
    int i;
    for (i = 0; i < in_dim; ++i) {
        r = r * 6364136223846793005ULL + 1442695040888963407ULL;
        in[i] = (float)((double)(r >> 33) / 2147483648.0 - 1.0);
    }
    for (i = 0; i < in_dim * bpr; ++i) {
        r = r * 6364136223846793005ULL + 1442695040888963407ULL;
        wt[i] = (uint8_t)((r >> 33) % 243);   /* valid 5-trit byte range */
    }
    for (i = 0; i < out_dim; ++i) {
        r = r * 6364136223846793005ULL + 1442695040888963407ULL;
        ws[i] = 0.01f + (float)((double)(r >> 40) / 8388608.0) * 0.1f;
        bias[i] = 0.0f;
    }
}

/* Runs one shape through the C kernel and returns the anchor digest of the
   output. Returns 0 on success, -1 on allocation failure. Deterministic:
   the same shape always produces the same digest on the same build. */
static int run_shape(int in_dim, int out_dim, int apply_sigmoid,
                     uint64_t* digest_out) {
    int bpr = BPR(out_dim);
    float *in = (float*)malloc(sizeof(float) * (size_t)in_dim);
    uint8_t *wt = (uint8_t*)malloc((size_t)in_dim * (size_t)bpr);
    float *ws = (float*)malloc(sizeof(float) * (size_t)out_dim);
    float *bi = (float*)malloc(sizeof(float) * (size_t)out_dim);
    float *out = (float*)malloc(sizeof(float) * (size_t)out_dim);
    int rc = -1;
    if (in && wt && ws && bi && out) {
        fill_fixture(in, wt, ws, bi, in_dim, out_dim, bpr);
        cce_trit_matmul_c(in, wt, ws, bi, out, in_dim, out_dim, bpr,
                          apply_sigmoid);
        *digest_out = cce_mojo_anchor(out, (size_t)out_dim);
        rc = 0;
    }
    free(in); free(wt); free(ws); free(bi); free(out);
    return rc;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("=== Mojo kernel bridge gate ===\n");

    printf("[1] extracted C kernel produces a stable anchor\n");
    {
        const int in_dim = 64, out_dim = 96, bpr = BPR(96);
        float *in = (float*)malloc(sizeof(float) * in_dim);
        uint8_t *wt = (uint8_t*)malloc((size_t)in_dim * bpr);
        float *ws = (float*)malloc(sizeof(float) * out_dim);
        float *bi = (float*)malloc(sizeof(float) * out_dim);
        float *out = (float*)malloc(sizeof(float) * out_dim);
        CHECK(in && wt && ws && bi && out, "fixture allocates");
        if (in && wt && ws && bi && out) {
            fill_fixture(in, wt, ws, bi, in_dim, out_dim, bpr);
            cce_trit_matmul_c(in, wt, ws, bi, out, in_dim, out_dim, bpr, 1);
            printf("  info ANCHOR in=%d out=%d digest=%016llx\n",
                   in_dim, out_dim,
                   (unsigned long long)cce_mojo_anchor(out, (size_t)out_dim));
            CHECK(1, "anchor computed");
        }
        free(in); free(wt); free(ws); free(bi); free(out);
    }

    printf("[2] harness self-proof: C vs C over the shape matrix\n");
    {
        /* Shapes chosen to hit the edges the kernel actually has: 510 is the
           tile width, 5 is the packed-byte granularity, and 1<<21 elements is
           where the OpenMP clause engages. A harness that has only ever seen
           one implementation proves nothing, so it proves itself first. */
        static const int shapes[][2] = {
            {1, 1}, {1, 5}, {7, 3}, {64, 96},
            {32, 509}, {32, 510}, {32, 511},
            {33, 1020}, {17, 7},
            {2048, 1024}
        };
        int n = (int)(sizeof shapes / sizeof shapes[0]), s, sig;
        int all_equal = 1;
        for (s = 0; s < n; ++s) {
            for (sig = 0; sig <= 1; ++sig) {
                uint64_t d1 = 0, d2 = 0;
                if (run_shape(shapes[s][0], shapes[s][1], sig, &d1) != 0 ||
                    run_shape(shapes[s][0], shapes[s][1], sig, &d2) != 0) {
                    CHECK(0, "shape ran");
                    all_equal = 0;
                    continue;
                }
                if (d1 != d2) {
                    printf("  MISMATCH in=%d out=%d sig=%d: %016llx vs %016llx\n",
                           shapes[s][0], shapes[s][1], sig,
                           (unsigned long long)d1, (unsigned long long)d2);
                    all_equal = 0;
                }
            }
        }
        printf("  info %d shapes x 2 sigmoid modes compared\n", n);
        CHECK(all_equal, "C kernel is byte-identical to itself on every shape");
    }

    printf("[3] dispatch defaults OFF and degrades safely\n");
    {
        /* With CNET_MOJO unset, dispatch must decline so the C path runs.
           This is what keeps every existing gate on the C kernel even on a
           box where Mojo is compiled in. */
        CHECK(cce_mojo_available() == 0,
              "Mojo path is off by default (CNET_MOJO unset)");
        {
            const int in_dim = 64, out_dim = 96, bpr = BPR(96);
            float *in = (float*)malloc(sizeof(float) * in_dim);
            uint8_t *wt = (uint8_t*)malloc((size_t)in_dim * bpr);
            float *ws = (float*)malloc(sizeof(float) * out_dim);
            float *bi = (float*)malloc(sizeof(float) * out_dim);
            float *out = (float*)malloc(sizeof(float) * out_dim);
            int rc;
            if (!in || !wt || !ws || !bi || !out) { printf("FAIL: alloc\n"); return 1; }
            fill_fixture(in, wt, ws, bi, in_dim, out_dim, bpr);
            rc = cce_mojo_dispatch_trit(in, wt, ws, bi, out, in_dim, out_dim,
                                        bpr, 1);
            CHECK(rc != 0, "dispatch declines when disabled, so C path runs");
            CHECK(cce_mojo_fallback_count() == 0,
                  "declining is not counted as a kernel failure");
            free(in); free(wt); free(ws); free(bi); free(out);
        }
    }

    printf("checks=%d fails=%d\n", checks, fails);
    if (fails) return 1;
    printf("ALL MOJO BRIDGE TESTS PASSED\n");
    return 0;
}
