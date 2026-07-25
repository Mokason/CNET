/* moe_xf — train the per-token transformer-MoE block (attention + MoE FFN).
 *
 * Task: each sequence is a random period-3 pattern (tokens[i] = tokens[i-3]), so
 * predicting the next token requires ATTENDING three positions back and copying —
 * an attention task, not something the FFN alone can do. Validates the block with
 * a gradient check, checks GPU==CPU equivalence, then trains (GPU if available)
 * and reports the loss curve + tokens/s.
 */

#include "../include/cce/cce_moe_xf.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static unsigned S = 0x1234u;
static unsigned rr(void) { S = S * 1664525u + 1013904223u; return S; }
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

static void gen_seq(int *tok, int n, int V) {
    for (int i = 0; i < n; i++) tok[i] = i < 3 ? (int)(rr() % V) : tok[i - 3];
}

int main(int argc, char **argv) {
    int steps = argc > 1 ? atoi(argv[1]) : 3000;
    float lr = argc > 2 ? (float)atof(argv[2]) : 0.003f;

    /* ---- 1. gradient check (small, CPU) ---- */
    {
        cce_moe_xf_cfg c = {0}; c.vocab = 32; c.d_model = 16; c.seq = 12; c.n_expert = 4; c.top_k = 4; c.d_ff = 12; c.lb_coef = 0.05f; c.seed = 3;
        cce_moe_xf *m = cce_moe_xf_create(&c);
        int tok[13]; gen_seq(tok, 13, c.vocab);
        double err = cce_moe_xf_grad_check(m, tok);
        printf("gradient check (fully differentiable): max rel err = %.2e  (%s)\n", err, err < 2e-3 ? "PASS" : "FAIL");
        cce_moe_xf_free(m);
        if (!(err < 2e-3)) { printf("\nMOE_XF_FAIL (backward incorrect)\n"); return 1; }
    }

    /* ---- 2. training config (small, single-GPU target) ---- */
    cce_moe_xf_cfg c = {0};
    c.vocab = 256; c.d_model = 192; c.seq = 96; c.n_expert = 8; c.top_k = 2; c.d_ff = 512;
    c.lb_coef = 0.02f; c.weight_decay = 1e-3f; c.seed = 1;
    cce_moe_xf *m = cce_moe_xf_create(&c);
    printf("\nmodel: d=%d seq=%d experts=%d top-%d d_ff=%d vocab=%d\n", c.d_model, c.seq, c.n_expert, c.top_k, c.d_ff, c.vocab);

    /* GPU equivalence: same seq on CPU vs GPU should give ~same loss */
    int tok[97]; gen_seq(tok, 97, c.vocab);
    cce_moe_xf_use_gpu(m, 0); double lcpu = cce_moe_xf_seq(m, tok, 0, NULL, NULL);
    int want_cpu = getenv("MOE_XF_CPU") != NULL;
    int on = want_cpu ? 0 : cce_moe_xf_use_gpu(m, 1);
    printf("device: %s\n", cce_moe_xf_device(m));
    if (on) { double lgpu = cce_moe_xf_seq(m, tok, 0, NULL, NULL);
        printf("GPU==CPU equivalence: cpu=%.5f gpu=%.5f  (rel %.1e)\n", lcpu, lgpu, fabs(lcpu - lgpu) / (fabs(lcpu) + 1e-9));
        /* validate the GPU BACKWARD too: directional grad check on the live device */
        double gerr = cce_moe_xf_grad_check(m, tok);
        printf("GPU gradient check: max rel err = %.2e  (%s)\n", gerr, gerr < 2e-3 ? "PASS" : "FAIL");
        if (!(gerr < 2e-3)) { printf("\nMOE_XF_FAIL (GPU backward incorrect)\n"); cce_moe_xf_free(m); return 1; }
    }
    else printf("(training on CPU%s)\n", want_cpu ? ", forced" : "; GPU unavailable");

    /* ---- 3. train ---- */
    const int BS = 8;                                 /* sequences per step */
    double t0 = now(), ce = 0, aux = 0; long toks = 0; int step;
    printf("\ntraining %d steps x %d seqs x %d tokens (lr=%.4g):\n", steps, BS, c.seq, lr);
    for (step = 1; step <= steps; step++) {
        cce_moe_xf_zero_grad(m);
        double bce = 0;
        for (int b = 0; b < BS; b++) { gen_seq(tok, c.seq + 1, c.vocab); cce_moe_xf_seq(m, tok, 1, &ce, &aux); bce += ce; toks += c.seq; }
        cce_moe_xf_adam(m, lr, step);
        if (step % (steps / 15 > 0 ? steps / 15 : 1) == 0 || step == 1) {
            double dt = now() - t0;
            printf("  step %5d  ce=%.4f  aux=%.4f  %.0f tok/s  (%.1fs)\n", step, bce / BS, aux, toks / dt, dt);
            fflush(stdout);
        }
    }
    double dt = now() - t0;
    printf("\ndone: final ce=%.4f  over %ld tokens in %.1fs = %.0f tok/s\n", ce, toks, dt, toks / dt);
    printf("(uniform CE = %.3f; period-3 copy is learnable only by attending 3 back)\n", log((double)c.vocab));
    cce_moe_xf_free(m);
    printf("\nMOE_XF_PASS\n");
    return 0;
}
