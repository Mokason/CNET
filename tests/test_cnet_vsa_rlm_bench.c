/* Recurrent LM trainer gate. Marker: CNET_VSA_RLM_BENCH_PASS
 *  1. Gradient check, double precision, every mixer: analytic directional derivative vs central differences
 *     on a random chunk with a carried state (relative error < 1e-4).
 *  2. Learning: a 2-state Markov token stream; after a short run the chunk loss falls well below the unigram
 *     entropy for every mixer, and two runs with one seed give identical losses. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>
#include "cnet_vsa_rlm.h"

static uint64_t g = 0x1234567ULL;
static uint32_t rnd(void) { g ^= g << 13; g ^= g >> 7; g ^= g << 17; return (uint32_t)g; }
static const char *names[3] = { "deltanet", "rwkv7", "ssd" };

int main(void) {
    printf("=================================================================\n");
    printf(" CNET Recurrent LM Bench (%s precision)\n", sizeof(rlm_real) == 8 ? "double" : "single");
    printf("=================================================================\n\n");
    printf("[1/2] Gradient check per mixer (d=16, 2 heads, 2 layers, vocab 23, chunk 7, state carried)\n");
    for (int mx = 0; mx < 3; ++mx) {
        cnet_vsa_rlm_cfg cfg = { 23, 16, 2, 2, 32, mx, 8, 7u, 0.0f };
        cnet_vsa_rlm *m = cnet_vsa_rlm_create(&cfg); assert(m);
        int warm[9], toks[8]; for (int i = 0; i < 9; ++i) warm[i] = (int)(rnd() % 23); for (int i = 0; i < 8; ++i) toks[i] = (int)(rnd() % 23);
        cnet_vsa_rlm_chunk(m, warm, 8, 0);   /* carried state from a previous chunk */
        double err = cnet_vsa_rlm_grad_check(m, toks, 7, sizeof(rlm_real) == 8 ? 1e-5 : 1e-2);
        printf("  %-9s relative error %.2e  (%zu parameters)\n", names[mx], err, cnet_vsa_rlm_param_count(m));
        assert(err < (sizeof(rlm_real) == 8 ? 1e-4 : 5e-2));
        cnet_vsa_rlm_free(m);
    }
    printf("  PASS\n\n[2/2] Learning a 2-state Markov stream (vocab 8), 300 chunks of 32\n");
    /* stream: state A emits tokens 0..3 (biased), state B emits 4..7; switches with p=0.1 */
    int N = 300 * 32 + 1; int *stream = (int *)malloc(sizeof(int) * N); int st = 0;
    double H_uni = 0; { int cnt[8] = {0};
        for (int i = 0; i < N; ++i) { if (rnd() % 10 == 0) st ^= 1; int r = rnd() % 10; stream[i] = st * 4 + (r < 6 ? 0 : r < 8 ? 1 : r < 9 ? 2 : 3); cnt[stream[i]]++; }
        for (int v = 0; v < 8; ++v) if (cnt[v]) { double p = (double)cnt[v] / N; H_uni -= p * log(p); } }
    printf("  unigram entropy %.3f nats; a model that tracks the state can reach about %.3f\n", H_uni, 0.1 * log(10.0) + 0.9 * log(1.0 / 0.9) + (-(0.6 * log(0.6) + 0.2 * log(0.2) + 0.1 * log(0.1) + 0.1 * log(0.1))));
    for (int mx = 0; mx < 3; ++mx) {
        double final_a = 0, final_b = 0;
        for (int rep = 0; rep < 2; ++rep) {
            cnet_vsa_rlm_cfg cfg = { 8, 32, 2, 1, 64, mx, 32, 3u, 0.0f };
            cnet_vsa_rlm *m = cnet_vsa_rlm_create(&cfg); assert(m);
            double acc = 0; int step = 0;
            for (int c = 0; c < 300; ++c) {
                cnet_vsa_rlm_zero_grad(m);
                double l = cnet_vsa_rlm_chunk(m, stream + c * 32, 32, 1);
                cnet_vsa_rlm_adam(m, 3e-3f, ++step);
                if (c >= 270) acc += l;
            }
            if (rep == 0) final_a = acc / 30; else final_b = acc / 30;
            cnet_vsa_rlm_free(m);
        }
        printf("  %-9s mean loss over the last 30 chunks: %.3f (repeat with the same seed: %.3f)\n", names[mx], final_a, final_b);
        assert(final_a < H_uni - 0.15);
        assert(fabs(final_a - final_b) < 1e-9);
    }
    free(stream);
    printf("  PASS\n\n=================================================================\n CNET_VSA_RLM_BENCH_PASS\n=================================================================\n");
    return 0;
}
