/* Minimal adversarial unit test for cce_clgemm: synthetic deterministic
 * matrices vs the exact CPU reference (same k-ascending accumulation), many
 * repetitions, mixed shapes (splittable and not), interleaved calls — the
 * shapes the transformer seam actually produces. Any mismatch prints the
 * first offending element. Exit 0 = every element of every call EXACTLY
 * matches the reference (bit-identical is the contract).
 *
 * Usage: clgemm_unit [reps]   (env knobs as usual: CNET_GPU_COUNT etc.)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "../include/cce/cce_clgemm.h"

/* deterministic pseudo-random floats in [-1, 1) */
static float prf(unsigned *st) {
    *st = *st * 1664525u + 1013904223u;
    return ((float)(*st >> 8) / (float)(1u << 24)) * 2.0f - 1.0f;
}

typedef struct {
    const char *name;
    size_t T, K, N;
} Shape;

int main(int argc, char **argv) {
    /* the seam's real shapes (draft model) + edge shapes */
    Shape shapes[] = {
        { "qkv 1024x1024", 2, 1024, 1024 },
        { "ffn up 1024x8192", 2, 1024, 8192 },   /* 32MB: splits at default */
        { "ffn down 8192x1024", 2, 8192, 1024 },
        { "head 1024x32768", 1, 1024, 32768 },   /* 128MB: splits */
        { "odd 640x1000", 3, 640, 1000 },        /* N%2, N%512 nonzero */
    };
    size_t n_shapes = sizeof shapes / sizeof shapes[0];
    int reps = (argc > 1) ? atoi(argv[1]) : 20;
    char dev[128] = {0};
    cce_clgemm *h;
    float **W, **B, **A, **Cg, **Cr;
    size_t s;
    unsigned seed = 12345;
    size_t total_calls = 0, fails = 0;

    h = cce_clgemm_open(NULL, dev, sizeof dev);
    if (!h) { fprintf(stderr, "no GPU\n"); return 1; }
    printf("device: %s (%lu)\n", dev,
           (unsigned long)cce_clgemm_device_count(h));

    W = malloc(n_shapes * sizeof *W);
    B = malloc(n_shapes * sizeof *B);
    A = malloc(n_shapes * sizeof *A);
    Cg = malloc(n_shapes * sizeof *Cg);
    Cr = malloc(n_shapes * sizeof *Cr);
    for (s = 0; s < n_shapes; ++s) {
        size_t i;
        W[s] = malloc(shapes[s].K * shapes[s].N * sizeof(float));
        B[s] = malloc(shapes[s].N * sizeof(float));
        A[s] = malloc(shapes[s].T * shapes[s].K * sizeof(float));
        Cg[s] = malloc(shapes[s].T * shapes[s].N * sizeof(float));
        Cr[s] = malloc(shapes[s].T * shapes[s].N * sizeof(float));
        if (!W[s] || !B[s] || !A[s] || !Cg[s] || !Cr[s]) return 1;
        for (i = 0; i < shapes[s].K * shapes[s].N; ++i) W[s][i] = prf(&seed);
        for (i = 0; i < shapes[s].N; ++i) B[s][i] = prf(&seed);
    }

    for (int r = 0; r < reps; ++r) {
        for (s = 0; s < n_shapes; ++s) {
            size_t T = shapes[s].T, K = shapes[s].K, N = shapes[s].N;
            size_t t, n, k, i;
            const float *bias = (r % 3 == 0) ? NULL : B[s];
            /* fresh activations every call (weights stay resident) */
            for (i = 0; i < T * K; ++i) A[s][i] = prf(&seed);
            memset(Cg[s], 0, T * N * sizeof(float));
            if (cce_clgemm_matmul(h, A[s], T, K, W[s], bias, N,
                                  Cg[s]) != 0) {
                fprintf(stderr, "rep %d %s: matmul returned -1\n", r,
                        shapes[s].name);
                return 1;
            }
            /* exact CPU reference, same accumulation order */
            for (t = 0; t < T; ++t)
                for (n = 0; n < N; ++n) {
                    float acc = bias ? bias[n] : 0.0f;
                    for (k = 0; k < K; ++k)
                        acc += A[s][t * K + k] * W[s][k * N + n];
                    Cr[s][t * N + n] = acc;
                }
            total_calls++;
            for (i = 0; i < T * N; ++i) {
                if (Cg[s][i] != Cr[s][i]) {
                    if (fails < 8)
                        printf("rep %d %s: MISMATCH at [t=%lu n=%lu] gpu=%.9g "
                               "ref=%.9g (nan gpu=%d)\n", r, shapes[s].name,
                               (unsigned long)(i / N),
                               (unsigned long)(i % N), (double)Cg[s][i],
                               (double)Cr[s][i], Cg[s][i] != Cg[s][i]);
                    fails++;
                    break;
                }
            }
        }
    }

    printf("%lu calls, %lu mismatched calls\n", (unsigned long)total_calls,
           (unsigned long)fails);
    cce_clgemm_close(h);
    if (fails == 0) { printf("CLGEMM UNIT: PASS (bit-identical)\n"); return 0; }
    printf("CLGEMM UNIT: FAIL\n");
    return 1;
}
