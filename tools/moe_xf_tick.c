/* moe_xf_tick — one bounded, resumable training tick for the 24/7 loop.
 *
 * The gap lane's unit is a one-hot -> one-hot table row certified by exact
 * replay: it cannot generalise, because a one-hot input space has no metric to
 * interpolate over. This drives the transformer-MoE block instead, and swaps
 * the certification predicate accordingly:
 *
 *     certified := held-out CE improved on the incumbent, and sits below H1
 *
 * H1 is the analytic order-1 plateau of the markov2 source; beating it requires
 * the attention to actually use the previous token, so it cannot be reached by
 * memorising the marginal. The source is sampled fresh forever, so there is no
 * training set to overfit — the held-out set is a FIXED sample drawn from its
 * own RNG stream and never trained on, which keeps CE comparable tick to tick.
 *
 * Prints one JSON line (MOE_TICK) for the caller to record. Never promotes:
 * it writes a candidate checkpoint and reports; scripts/cnet_moe_tick.sh
 * decides. Exit 0 on a completed tick (improved or not), non-zero on error.
 *
 *   env: MOE_CKPT      incumbent checkpoint to resume (created if absent)
 *        MOE_CKPT_OUT  where to write the candidate (default $MOE_CKPT.cand)
 *        MOE_STEPS     training steps this tick (default 400)
 *        MOE_LR        learning rate (default 0.003)
 *        MOE_EVAL_N    held-out sequences (default 64)
 *        MOE_CPU=1     force the CPU reference path
 */
#include "../include/cce/cce_moe_xf.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MK_V 64
#define SEQ 96

/* ---- markov2 source: identical tables to tests/moe_xf.c (fixed seed) ---- */
static float mk_A[MK_V * MK_V], mk_B[MK_V * MK_V], mk_marg[MK_V * MK_V];

static void mk_gauss_fill(float *t, int n, unsigned *s, float sc) {
    int k, q;
    for (k = 0; k < n; k++) {
        double g = 0;
        for (q = 0; q < 12; q++) { *s = *s * 1664525u + 1013904223u; g += (*s >> 8) / 16777216.0; }
        t[k] = (float)((g - 6.0) * sc);
    }
}
static void mk_logits(int prev2, int prev1, float *out) {
    const float *a = mk_A + (size_t)prev1 * MK_V, *b = mk_B + (size_t)prev2 * MK_V;
    int j;
    for (j = 0; j < MK_V; j++) out[j] = a[j] + b[j];
}
static void mk_init(void) {
    unsigned s = 0x9E3779B9u;
    int p1, p2, j;
    mk_gauss_fill(mk_A, MK_V * MK_V, &s, 1.9f);
    mk_gauss_fill(mk_B, MK_V * MK_V, &s, 1.1f);
    for (p1 = 0; p1 < MK_V; p1++) {
        float *mg = mk_marg + (size_t)p1 * MK_V, lg[MK_V];
        for (j = 0; j < MK_V; j++) mg[j] = 0;
        for (p2 = 0; p2 < MK_V; p2++) {
            float mx = -1e30f; double Z = 0;
            mk_logits(p2, p1, lg);
            for (j = 0; j < MK_V; j++) if (lg[j] > mx) mx = lg[j];
            for (j = 0; j < MK_V; j++) Z += exp((double)(lg[j] - mx));
            for (j = 0; j < MK_V; j++) mg[j] += (float)(exp((double)(lg[j] - mx)) / Z / MK_V);
        }
    }
}
/* Independent RNG streams: training must never draw the held-out sequences. */
static unsigned g_train_rng, g_eval_rng;
static unsigned rr(unsigned *s) { *s = *s * 1664525u + 1013904223u; return *s; }

static void mk_gen(int *tok, int n, unsigned *rng) {
    int i, j;
    for (i = 0; i < n; i++) {
        float lg[MK_V], mx = -1e30f;
        double Z = 0, r, acc = 0;
        int pick = MK_V - 1;
        if (i < 2) { tok[i] = (int)((rr(rng) >> 8) % MK_V); continue; }
        mk_logits(tok[i - 2], tok[i - 1], lg);
        for (j = 0; j < MK_V; j++) if (lg[j] > mx) mx = lg[j];
        for (j = 0; j < MK_V; j++) Z += exp((double)(lg[j] - mx));
        r = ((rr(rng) >> 8) / 16777216.0) * Z;
        for (j = 0; j < MK_V; j++) { acc += exp((double)(lg[j] - mx)); if (acc >= r) { pick = j; break; } }
        tok[i] = pick;
    }
}
static double mk_oracle(const int *tok, int seq, int full) {
    double sum = 0;
    int t, j;
    for (t = 0; t < seq; t++) {
        int i = t + 1;
        double p;
        if (i < 2) { sum += log((double)MK_V); continue; }
        if (full) {
            float lg[MK_V], mx = -1e30f; double Z = 0;
            mk_logits(tok[i - 2], tok[i - 1], lg);
            for (j = 0; j < MK_V; j++) if (lg[j] > mx) mx = lg[j];
            for (j = 0; j < MK_V; j++) Z += exp((double)(lg[j] - mx));
            p = exp((double)(lg[tok[i]] - mx)) / Z;
        } else p = mk_marg[(size_t)tok[i - 1] * MK_V + tok[i]];
        sum += -log(p + 1e-30);
    }
    return sum / seq;
}

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}
static int envi(const char *k, int d) { const char *v = getenv(k); return v && v[0] ? atoi(v) : d; }

int main(void) {
    const char *ckpt = getenv("MOE_CKPT");
    const char *out = getenv("MOE_CKPT_OUT");
    char out_buf[1200];
    int steps = envi("MOE_STEPS", 400);
    int eval_n = envi("MOE_EVAL_N", 64);
    float lr = (float)(getenv("MOE_LR") && getenv("MOE_LR")[0] ? atof(getenv("MOE_LR")) : 0.003);
    cce_moe_xf_cfg c;
    cce_moe_xf *m = NULL;
    int step0 = 0, s, resumed = 0, k;
    int *eval_tok = NULL;
    double h1 = 0, h2 = 0, ce_eval = 0, train_ce = 0, t0;
    double uniform = log((double)MK_V);

    if (!ckpt || !ckpt[0]) { fprintf(stderr, "MOE_CKPT not set\n"); return 2; }
    if (!out || !out[0]) {
        if (snprintf(out_buf, sizeof out_buf, "%s.cand", ckpt) >= (int)sizeof out_buf) return 2;
        out = out_buf;
    }

    mk_init();

    memset(&c, 0, sizeof c);
    c.vocab = MK_V; c.d_model = 192; c.seq = SEQ; c.n_expert = 8; c.top_k = 2;
    c.d_ff = 512; c.lb_coef = 0.02f; c.weight_decay = 1e-3f; c.seed = 1;

    m = cce_moe_xf_load(ckpt, &step0);
    if (m) {
        resumed = 1;
    } else {
        m = cce_moe_xf_create(&c);
        step0 = 0;
        if (!m) { fprintf(stderr, "create failed\n"); return 3; }
    }

    /* Held-out set: fixed seed, generated identically every tick, never trained
       on. If this drifted per tick, CE differences would be sampling noise and
       "improvement" would be unfalsifiable. */
    g_eval_rng = 0xE7A15EEDu;
    eval_tok = (int *)malloc((size_t)eval_n * (SEQ + 1) * sizeof(int));
    if (!eval_tok) { cce_moe_xf_free(m); return 3; }
    for (k = 0; k < eval_n; k++) {
        int *t = eval_tok + (size_t)k * (SEQ + 1);
        mk_gen(t, SEQ + 1, &g_eval_rng);
        h2 += mk_oracle(t, SEQ, 1);
        h1 += mk_oracle(t, SEQ, 0);
    }
    h1 /= eval_n; h2 /= eval_n;

    /* Training stream: seeded from the resumed step so a resume does not replay
       the same sequences it already trained on. */
    g_train_rng = 0x1234u + (unsigned)step0 * 2654435761u;

    if (!getenv("MOE_CPU")) (void)cce_moe_xf_use_gpu(m, 1);

    t0 = now();
    {
        int tok[SEQ + 1];
        for (s = 0; s < steps; s++) {
            double ce = 0, aux = 0;
            mk_gen(tok, SEQ + 1, &g_train_rng);
            cce_moe_xf_zero_grad(m);
            (void)cce_moe_xf_seq(m, tok, 1, &ce, &aux);
            cce_moe_xf_adam(m, lr, step0 + s + 1);
            train_ce += ce;
        }
    }
    train_ce /= (steps > 0 ? steps : 1);

    for (k = 0; k < eval_n; k++) {
        double ce = 0;
        (void)cce_moe_xf_seq(m, eval_tok + (size_t)k * (SEQ + 1), 0, &ce, NULL);
        ce_eval += ce;
    }
    ce_eval /= eval_n;

    if (cce_moe_xf_save(m, out, step0 + steps) != 0) {
        fprintf(stderr, "checkpoint save failed: %s\n", out);
        free(eval_tok); cce_moe_xf_free(m);
        return 4;
    }

    printf("MOE_TICK {\"resumed\":%d,\"step_from\":%d,\"step_to\":%d,\"steps\":%d,"
           "\"lr\":%.6f,\"train_ce\":%.6f,\"heldout_ce\":%.6f,\"h1\":%.6f,\"h2\":%.6f,"
           "\"uniform\":%.6f,\"gap_to_floor\":%.6f,\"below_h1\":%d,\"eval_n\":%d,"
           "\"device\":\"%s\",\"secs\":%.2f,\"candidate\":\"%s\"}\n",
           resumed, step0, step0 + steps, steps, lr, train_ce, ce_eval, h1, h2,
           uniform, ce_eval - h2, ce_eval < h1 ? 1 : 0, eval_n,
           cce_moe_xf_device(m), now() - t0, out);

    free(eval_tok);
    cce_moe_xf_free(m);
    return 0;
}
