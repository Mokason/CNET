/* GPU equivalence gate: the OpenCL forward must be DECISION-identical to the
 * CPU forward before any --gpu mining is allowed. Runs N deterministic
 * contexts through both paths and reports:
 *   - max |delta logit| over the full vocab (printed; ulp-level expected)
 *   - full-vocab argmax agreement          (MUST be 100%)
 *   - window-restricted top-3 agreement    (MUST be 100%, ordered)
 *   - forwards/s on both paths (the speedup number)
 * Exit 0 only on 100% decision agreement.
 *
 * Usage: gpu_equiv <model> [V window] [N contexts]
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../include/cce/cce_detect.h"
#include "../include/cce/cce_clgemm.h"

/* WALL time. clock() is wall-ish on Windows (kept there — timespec_get is
   absent from MSVCRT-based MinGW) but PROCESS CPU TIME on Linux, where N
   driver threads spin-waiting on N queues overcount N-fold. */
static double wall_s(void) {
#ifdef _WIN32
    return (double)clock() / CLOCKS_PER_SEC;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
#endif
}

static void top3_window(const float *logits, const int *vocab, size_t V,
                        size_t *r) {
    size_t i, k;
    int taken[4096] = {0};
    for (k = 0; k < 3; ++k) {
        size_t best = (size_t)-1;
        float bl = 0.0f;
        for (i = 0; i < V; ++i) {
            if (taken[i]) continue;
            if (best == (size_t)-1 || logits[vocab[i]] > bl) {
                bl = logits[vocab[i]];
                best = i;
            }
        }
        taken[best] = 1;
        r[k] = best;
    }
}

int main(int argc, char **argv) {
    size_t V = 64, N = 64, i, j;
    cce_anymodel *am = NULL;
    cce_clgemm *gpu;
    char dev[128] = {0};
    int *vocab;
    float *lc, *lg;
    double max_dl = 0.0;
    size_t argmax_mismatch = 0, top3_mismatch = 0;
    double t_cpu, t_gpu, t0;

    if (argc < 2) {
        fprintf(stderr, "usage: %s <model> [V] [N]\n", argv[0]);
        return 2;
    }
    if (argc > 2) V = (size_t)atoi(argv[2]);
    if (argc > 3) N = (size_t)atoi(argv[3]);
    if (V < 4 || V > 4096) { fprintf(stderr, "V out of range\n"); return 2; }

    if (cce_anymodel_open(&am, argv[1]) != CCE_OK || am->transformer == NULL) {
        fprintf(stderr, "cannot open transformer for %s\n", argv[1]);
        return 1;
    }
    gpu = cce_clgemm_open(NULL, dev, sizeof dev);
    if (!gpu) {
        fprintf(stderr, "no OpenCL GPU available — nothing to gate\n");
        return 1;
    }
    printf("gpu device: %s\n", dev);

    vocab = (int *)malloc(V * sizeof *vocab);
    lc = (float *)malloc((size_t)am->transformer->vocab_size * sizeof *lc);
    lg = (float *)malloc((size_t)am->transformer->vocab_size * sizeof *lg);
    if (!vocab || !lc || !lg) return 1;
    for (i = 0; i < V; ++i) vocab[i] = 2000 + (int)i;

    /* CPU pass (also warms nothing — the CPU path has no state) */
    t0 = wall_s();
    for (j = 0; j < N; ++j) {
        int tokens[2];
        tokens[0] = 2000 + (int)((j * 37u) % 4096u);
        tokens[1] = 2000 + (int)((j * 91u + 17u) % 4096u);
        am->transformer->cur_pos = 0;
        if (cce_gguf_qwen2_forward(am->transformer, tokens, 2, lc,
                                   am->transformer->vocab_size) != CCE_OK) {
            fprintf(stderr, "cpu forward failed at %lu\n", (unsigned long)j);
            return 1;
        }
    }
    t_cpu = wall_s() - t0;

    /* GPU pass + comparison (re-run CPU per context for the diff) */
    cce_gguf_set_clgemm(gpu);
    t0 = wall_s();
    for (j = 0; j < N; ++j) {
        int tokens[2];
        tokens[0] = 2000 + (int)((j * 37u) % 4096u);
        tokens[1] = 2000 + (int)((j * 91u + 17u) % 4096u);
        am->transformer->cur_pos = 0;
        if (cce_gguf_qwen2_forward(am->transformer, tokens, 2, lg,
                                   am->transformer->vocab_size) != CCE_OK) {
            fprintf(stderr, "gpu forward failed at %lu\n", (unsigned long)j);
            return 1;
        }
    }
    t_gpu = wall_s() - t0;
    cce_gguf_set_clgemm(NULL);

    /* comparison pass: per context, CPU vs GPU logits + decisions */
    for (j = 0; j < N; ++j) {
        int tokens[2];
        size_t am_c = 0, am_g = 0, r_c[3], r_g[3];
        int vs;
        tokens[0] = 2000 + (int)((j * 37u) % 4096u);
        tokens[1] = 2000 + (int)((j * 91u + 17u) % 4096u);

        am->transformer->cur_pos = 0;
        cce_gguf_qwen2_forward(am->transformer, tokens, 2, lc,
                               am->transformer->vocab_size);
        cce_gguf_set_clgemm(gpu);
        am->transformer->cur_pos = 0;
        cce_gguf_qwen2_forward(am->transformer, tokens, 2, lg,
                               am->transformer->vocab_size);
        cce_gguf_set_clgemm(NULL);

        vs = am->transformer->vocab_size;
        for (i = 0; i < (size_t)vs; ++i) {
            double d = fabs((double)lc[i] - (double)lg[i]);
            if (d > max_dl) max_dl = d;
            if (lc[i] > lc[am_c]) am_c = i;
            if (lg[i] > lg[am_g]) am_g = i;
        }
        if (am_c != am_g) argmax_mismatch++;
        top3_window(lc, vocab, V, r_c);
        top3_window(lg, vocab, V, r_g);
        if (memcmp(r_c, r_g, sizeof r_c) != 0) top3_mismatch++;
    }

    printf("contexts: %lu\n", (unsigned long)N);
    printf("max |delta logit|: %.6e\n", max_dl);
    printf("argmax agreement: %lu/%lu\n", (unsigned long)(N - argmax_mismatch),
           (unsigned long)N);
    printf("window top-3 agreement: %lu/%lu\n",
           (unsigned long)(N - top3_mismatch), (unsigned long)N);
    printf("cpu: %.2f fwd/s   gpu: %.2f fwd/s   speedup: %.2fx\n",
           (double)N / t_cpu, (double)N / t_gpu, t_cpu / t_gpu);
    printf("gpu resident weights: %.1f MB\n",
           (double)cce_clgemm_resident_bytes(gpu) / (1024.0 * 1024.0));

    cce_clgemm_close(gpu);
    cce_anymodel_free(am);
    free(vocab);
    free(lc);
    free(lg);

    if (argmax_mismatch == 0 && top3_mismatch == 0) {
        printf("EQUIVALENCE: PASS (decision-identical)\n");
        return 0;
    }
    printf("EQUIVALENCE: FAIL — do NOT mine with --gpu\n");
    return 1;
}
