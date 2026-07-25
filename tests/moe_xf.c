/* moe_xf — train the per-token transformer-MoE block (attention + MoE FFN).
 *
 * Two tasks (env MOE_XF_TASK):
 *   copy3   (default) — each sequence is a random period-3 pattern
 *                       tokens[i] = tokens[i-3]; predicting the next token needs
 *                       ATTENDING three positions back and copying. Deterministic,
 *                       floor 0, converges fast — the CI smoke test.
 *   markov2           — an order-2 Markov language source with a fixed random
 *                       conditional distribution and a KNOWN entropy floor. Every
 *                       sequence is freshly sampled, so the model trains on an
 *                       infinite stream and cannot memorize: CE descending toward
 *                       the oracle floor is genuine distribution learning. Uses the
 *                       current token (own embedding) + the previous token (1-back
 *                       attention) + the MoE to map the pair to its distribution.
 *
 * Validates the block with CPU + GPU gradient checks and CPU/GPU forward
 * equivalence, then trains (GPU if available) and reports the loss curve + tok/s.
 *
 * args: ./moe_xf <steps> <lr>   env: MOE_XF_TASK={copy3|markov2}  MOE_XF_CPU=1
 */

#include "../include/cce/cce_moe_xf.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static unsigned S = 0x1234u;
static unsigned rr(void) { S = S * 1664525u + 1013904223u; return S; }
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

/* ---- task: period-3 copy (deterministic, attention-only, floor 0) ---- */
static void gen_copy3(int *tok, int n, int V) {
    for (int i = 0; i < n; i++) tok[i] = i < 3 ? (int)(rr() % V) : tok[i - 3];
}

/* ---- task: order-2 Markov LM (stochastic, known-entropy floor) ----
 * Layered source so learning is progressive (partial credit exists):
 *   logit(next | prev2, prev1) = A[prev1][next] + B[prev2][next]
 * A (dominant, indexed by the CURRENT token) is an order-1 signal the FFN/head
 * can learn immediately; B (weaker, indexed by the PREVIOUS token) is an order-2
 * correction the model can only capture by attending one position back. So CE
 * descends in two stages: uniform -> H1 (order-1 plateau) -> H2 (the floor). */
#define MK_V 64                          /* source == model alphabet */
static float mk_A[MK_V * MK_V];          /* prev1 -> next  (order-1, dominant) */
static float mk_B[MK_V * MK_V];          /* prev2 -> next  (order-2, via attention) */
static float mk_marg[MK_V * MK_V];       /* P(next | prev1), averaged over prev2 */
static void mk_gauss_fill(float *t, int n, unsigned *s, float sc) {
    for (int k = 0; k < n; k++) { double g = 0; for (int q = 0; q < 12; q++) { *s = *s * 1664525u + 1013904223u; g += (*s >> 8) / 16777216.0; } t[k] = (float)((g - 6.0) * sc); }
}
static void mk_logits(int prev2, int prev1, float *out) {   /* out[MK_V] */
    const float *a = mk_A + (size_t)prev1 * MK_V, *b = mk_B + (size_t)prev2 * MK_V;
    for (int j = 0; j < MK_V; j++) out[j] = a[j] + b[j];
}
static void mk_init(void) {
    unsigned s = 0x9E3779B9u;
    mk_gauss_fill(mk_A, MK_V * MK_V, &s, 1.9f);              /* order-1: strong */
    mk_gauss_fill(mk_B, MK_V * MK_V, &s, 1.1f);              /* order-2: weaker */
    for (int p1 = 0; p1 < MK_V; p1++) {                      /* P(next|prev1) = mean over prev2 */
        float *mg = mk_marg + (size_t)p1 * MK_V, lg[MK_V];
        for (int j = 0; j < MK_V; j++) mg[j] = 0;
        for (int p2 = 0; p2 < MK_V; p2++) { mk_logits(p2, p1, lg);
            float mx = -1e30f; for (int j = 0; j < MK_V; j++) if (lg[j] > mx) mx = lg[j];
            double Z = 0; for (int j = 0; j < MK_V; j++) Z += exp((double)(lg[j] - mx));
            for (int j = 0; j < MK_V; j++) mg[j] += (float)(exp((double)(lg[j] - mx)) / Z / MK_V); }
    }
}
static void mk_gen(int *tok, int n) {
    for (int i = 0; i < n; i++) {
        if (i < 2) { tok[i] = (int)((rr() >> 8) % MK_V); continue; }
        float lg[MK_V]; mk_logits(tok[i - 2], tok[i - 1], lg);
        float mx = -1e30f; for (int j = 0; j < MK_V; j++) if (lg[j] > mx) mx = lg[j];
        double Z = 0; for (int j = 0; j < MK_V; j++) Z += exp((double)(lg[j] - mx));
        double r = ((rr() >> 8) / 16777216.0) * Z, acc = 0; int pick = MK_V - 1;
        for (int j = 0; j < MK_V; j++) { acc += exp((double)(lg[j] - mx)); if (acc >= r) { pick = j; break; } }
        tok[i] = pick;
    }
}
/* oracle CE over a sequence's scored targets tok[1..seq] (matches model averaging).
   full=1: H2 (true order-2 dist); full=0: H1 (best order-1 predictor P(next|prev1)).
   Positions 0,1 are uniform (unpredictable) -> log(MK_V). */
static double mk_oracle(const int *tok, int seq, int full) {
    double sum = 0;
    for (int t = 0; t < seq; t++) { int i = t + 1;
        if (i < 2) { sum += log((double)MK_V); continue; }
        double p;
        if (full) { float lg[MK_V]; mk_logits(tok[i - 2], tok[i - 1], lg);
            float mx = -1e30f; for (int j = 0; j < MK_V; j++) if (lg[j] > mx) mx = lg[j];
            double Z = 0; for (int j = 0; j < MK_V; j++) Z += exp((double)(lg[j] - mx));
            p = exp((double)(lg[tok[i]] - mx)) / Z;
        } else p = mk_marg[(size_t)tok[i - 1] * MK_V + tok[i]];
        sum += -log(p + 1e-30);
    }
    return sum / seq;
}

int main(int argc, char **argv) {
    int steps = argc > 1 ? atoi(argv[1]) : 3000;
    float lr = argc > 2 ? (float)atof(argv[2]) : 0.003f;
    const char *task = getenv("MOE_XF_TASK");
    int markov = task && strcmp(task, "markov2") == 0;

    /* ---- 1. gradient check (small, CPU, task-independent) ---- */
    {
        cce_moe_xf_cfg c = {0}; c.vocab = 32; c.d_model = 16; c.seq = 12; c.n_expert = 4; c.top_k = 4; c.d_ff = 12; c.lb_coef = 0.05f; c.seed = 3;
        cce_moe_xf *m = cce_moe_xf_create(&c);
        int tok[13]; gen_copy3(tok, 13, c.vocab);
        double err = cce_moe_xf_grad_check(m, tok);
        printf("gradient check (fully differentiable): max rel err = %.2e  (%s)\n", err, err < 2e-3 ? "PASS" : "FAIL");
        cce_moe_xf_free(m);
        if (!(err < 2e-3)) { printf("\nMOE_XF_FAIL (backward incorrect)\n"); return 1; }
    }

    /* ---- 2. training config ---- */
    cce_moe_xf_cfg c = {0};
    c.vocab = markov ? MK_V : 256; c.d_model = 192; c.seq = 96; c.n_expert = 8; c.top_k = 2; c.d_ff = 512;
    c.lb_coef = 0.02f; c.weight_decay = 1e-3f; c.seed = 1;
    cce_moe_xf *m = cce_moe_xf_create(&c);
    double uniform = log((double)c.vocab), floor_ce = 0, floor_h1 = 0;
    printf("\ntask: %s   model: d=%d seq=%d experts=%d top-%d d_ff=%d vocab=%d\n",
           markov ? "markov2 (order-2 Markov LM, known-entropy floor)" : "copy3 (period-3 copy)",
           c.d_model, c.seq, c.n_expert, c.top_k, c.d_ff, c.vocab);

    int tok[97];
    if (markov) {
        mk_init();
        int NF = 4000; for (int k = 0; k < NF; k++) { mk_gen(tok, c.seq + 1); floor_ce += mk_oracle(tok, c.seq, 1); floor_h1 += mk_oracle(tok, c.seq, 0); }
        floor_ce /= NF; floor_h1 /= NF;
        printf("source: order-2 Markov (logit = A[prev1] + B[prev2]), alphabet %d\n", MK_V);
        printf("  uniform %.4f  ->  order-1 plateau H1 %.4f (current token only)  ->  floor H2 %.4f (needs 1-back attention)\n",
               uniform, floor_h1, floor_ce);
    }
#define GEN(t) do { if (markov) mk_gen((t), c.seq + 1); else gen_copy3((t), c.seq + 1, c.vocab); } while (0)

    /* GPU equivalence + GPU backward check on one sequence */
    GEN(tok);
    cce_moe_xf_use_gpu(m, 0); double lcpu = cce_moe_xf_seq(m, tok, 0, NULL, NULL);
    int want_cpu = getenv("MOE_XF_CPU") != NULL;
    int on = want_cpu ? 0 : cce_moe_xf_use_gpu(m, 1);
    printf("device: %s\n", cce_moe_xf_device(m));
    if (on) { double lgpu = cce_moe_xf_seq(m, tok, 0, NULL, NULL);
        printf("GPU==CPU equivalence: cpu=%.5f gpu=%.5f  (rel %.1e)\n", lcpu, lgpu, fabs(lcpu - lgpu) / (fabs(lcpu) + 1e-9));
        double gerr = cce_moe_xf_grad_check(m, tok);
        printf("GPU gradient check: max rel err = %.2e  (%s)\n", gerr, gerr < 2e-3 ? "PASS" : "FAIL");
        if (!(gerr < 2e-3)) { printf("\nMOE_XF_FAIL (GPU backward incorrect)\n"); cce_moe_xf_free(m); return 1; }
    }
    else printf("(training on CPU%s)\n", want_cpu ? ", forced" : "; GPU unavailable");

    /* ---- 3. train ---- */
    const int BS = 8;
    double t0 = now(), ce = 0, aux = 0; long toks = 0; int step; double last_ce = uniform;
    printf("\ntraining %d steps x %d seqs x %d tokens (lr=%.4g):\n", steps, BS, c.seq, lr);
    for (step = 1; step <= steps; step++) {
        cce_moe_xf_zero_grad(m);
        double bce = 0;
        for (int b = 0; b < BS; b++) { GEN(tok); cce_moe_xf_seq(m, tok, 1, &ce, &aux); bce += ce; toks += c.seq; }
        cce_moe_xf_adam(m, lr, step);
        last_ce = bce / BS;
        if (step % (steps / 20 > 0 ? steps / 20 : 1) == 0 || step == 1) {
            double dt = now() - t0;
            if (markov) printf("  step %6d  ce=%.4f  (floor %.4f, gap %+.4f)  aux=%.4f  %.0f tok/s  (%.0fs)\n",
                               step, last_ce, floor_ce, last_ce - floor_ce, aux, toks / dt, dt);
            else printf("  step %6d  ce=%.4f  aux=%.4f  %.0f tok/s  (%.0fs)\n", step, last_ce, aux, toks / dt, dt);
            fflush(stdout);
        }
    }
    double dt = now() - t0;
    /* clean final metric: mean CE over fresh sequences (no gradient), de-noised */
    double eval = 0; int NE = 64;
    for (int k = 0; k < NE; k++) { double e; GEN(tok); cce_moe_xf_seq(m, tok, 0, &e, NULL); eval += e; }
    eval /= NE;
    printf("\ndone: eval ce=%.4f  (last batch %.4f)  over %ld tokens in %.1fs = %.0f tok/s\n", eval, last_ce, toks, dt, toks / dt);
    if (markov) printf("(uniform %.4f; order-1 plateau H1 %.4f; floor H2 %.4f — crossing H1 means the attention learned the order-2 signal)\n", uniform, floor_h1, floor_ce);
    else printf("(uniform CE = %.3f; period-3 copy is learnable only by attending 3 back)\n", uniform);
    cce_moe_xf_free(m);

    /* did it actually learn?  copy3: near-perfect copy; markov: clearly past the order-1 signal */
    int learned = markov ? (eval < uniform - 0.6) : (eval < 0.5);
    printf("\n%s\n", learned ? "MOE_XF_PASS" : "MOE_XF_FAIL (did not learn)");
    return learned ? 0 : 1;
}
