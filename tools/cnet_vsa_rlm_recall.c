/* Synthetic associative recall with re-binding: does the mixer write, keep and overwrite key/value pairs?
 *   sequence: <eos> (k v){P pairs, keys distinct} (k v'){R re-bindings of already used keys} (? k v){Q queries}
 *   loss and accuracy only on the value positions after each query; a query asks for the LATEST value of its key.
 * Usage: cnet_vsa_rlm_recall [--pairs 16] [--rebind 8] [--queries 16] [--keys 32] [--d 64] [--heads 2] [--layers 2]
 *                            [--steps 3000] [--batch 8] [--lr 3e-3] [--eval 200] [--seed 1] [--mixer all|deltanet|rwkv7|ssd]
 * Each step trains on --batch fresh sequences (data-parallel workers, OpenMP). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#ifdef _OPENMP
#include <omp.h>
#endif
#include "cnet_vsa_rlm.h"
static uint64_t g = 0x2545F4914F6CDD1DULL;
static uint32_t rnd(void) { g ^= g << 13; g ^= g >> 7; g ^= g << 17; return (uint32_t)(g >> 11); }
static double now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec * 1e-9; }
static const char *arg(int argc, char **argv, const char *k, const char *def) { for (int i = 1; i + 1 < argc; ++i) if (!strcmp(argv[i], k)) return argv[i + 1]; return def; }
/* builds one sequence; returns its length; mask marks positions whose NEXT token is a queried value */
static int make_seq(int *seq, unsigned char *mask, int *qpos, int nkeys, int P, int R, int Q) {
    int KEY = 2, VAL = 2 + nkeys, QM = 2 + 2 * nkeys, n = 0, perm[256], latest[256];
    for (int i = 0; i < nkeys; ++i) perm[i] = i;
    for (int i = nkeys - 1; i > 0; --i) { int j = rnd() % (i + 1), t = perm[i]; perm[i] = perm[j]; perm[j] = t; }
    seq[n++] = 1;
    for (int i = 0; i < P; ++i) { int k = perm[i], v = rnd() % nkeys; latest[k] = v; seq[n++] = KEY + k; seq[n++] = VAL + v; }
    for (int i = 0; i < R; ++i) { int k = perm[rnd() % P], v = rnd() % nkeys; latest[k] = v; seq[n++] = KEY + k; seq[n++] = VAL + v; }
    for (int i = 0; i < Q; ++i) { int k = perm[rnd() % P]; seq[n++] = QM; seq[n++] = KEY + k; qpos[i] = n - 1; seq[n++] = VAL + latest[k]; }
    memset(mask, 0, (size_t)n); for (int i = 0; i < Q; ++i) mask[qpos[i]] = 1;
    return n;
}
int main(int argc, char **argv) {
    int P = atoi(arg(argc, argv, "--pairs", "16")), R = atoi(arg(argc, argv, "--rebind", "8")), Q = atoi(arg(argc, argv, "--queries", "16")), nkeys = atoi(arg(argc, argv, "--keys", "32"));
    int d = atoi(arg(argc, argv, "--d", "64")), H = atoi(arg(argc, argv, "--heads", "2")), L = atoi(arg(argc, argv, "--layers", "2")), steps = atoi(arg(argc, argv, "--steps", "3000")), neval = atoi(arg(argc, argv, "--eval", "200"));
    double lr = atof(arg(argc, argv, "--lr", "3e-3")); int B = atoi(arg(argc, argv, "--batch", "8")); unsigned seed = (unsigned)atoi(arg(argc, argv, "--seed", "1")); const char *which = arg(argc, argv, "--mixer", "all");
    int V = 3 + 2 * nkeys, len = 1 + 2 * (P + R) + 3 * Q;
    if (len > 512 || nkeys > 200) { fprintf(stderr, "sequence too long\n"); return 2; }
    printf("associative recall: %d keys, %d pairs, %d re-bindings, %d queries (latest value), sequence %d tokens, vocab %d; d=%d heads=%d layers=%d, %d steps x batch %d, lr %.1e, eval on %d fresh sequences\n", nkeys, P, R, Q, len, V, d, H, L, steps, B, lr, neval);
    const char *names[3] = { "deltanet", "rwkv7", "ssd" };
    for (int mx = 0; mx < 3; ++mx) {
        if (strcmp(which, "all") && strcmp(which, names[mx])) continue;
        cnet_vsa_rlm_cfg cfg = { V, d, H, L, 0, mx, len, seed, 0.0f };
        cnet_vsa_rlm *m = cnet_vsa_rlm_create(&cfg); if (!m) { fprintf(stderr, "bad config\n"); return 2; }
        int seq[520], qpos[256]; unsigned char mask[520]; g = 0x2545F4914F6CDD1DULL ^ seed; double t0 = now(), run = 0;
        cnet_vsa_rlm **wk = (cnet_vsa_rlm **)malloc(sizeof(void *) * B); for (int b = 0; b < B; ++b) wk[b] = cnet_vsa_rlm_clone_shared(m);
        int *bseq = (int *)malloc(sizeof(int) * 520 * B), *bn = (int *)malloc(sizeof(int) * B); unsigned char *bmask = (unsigned char *)malloc((size_t)520 * B); double *bl = (double *)malloc(sizeof(double) * B);
        for (int s = 1; s <= steps; ++s) {
            for (int b = 0; b < B; ++b) bn[b] = make_seq(bseq + 520 * b, bmask + 520 * b, qpos, nkeys, P, R, Q);
#pragma omp parallel for schedule(static, 1)
            for (int b = 0; b < B; ++b) { cnet_vsa_rlm_reset_state(wk[b]); cnet_vsa_rlm_zero_grad(wk[b]); bl[b] = cnet_vsa_rlm_chunk_masked(wk[b], bseq + 520 * b, bn[b] - 1, bmask + 520 * b, 1); }
            cnet_vsa_rlm_zero_grad(m); double l = 0; for (int b = 0; b < B; ++b) { cnet_vsa_rlm_reduce_grad(m, wk[b], 1.0 / B); l += bl[b] / B; }
            cnet_vsa_rlm_grad_clip(m, 1.0); cnet_vsa_rlm_adam(m, (float)(lr * (s < 100 ? s / 100.0 : 1.0)), s);
            run = s == 1 ? l : 0.98 * run + 0.02 * l;
            if (s % 500 == 0) { printf("  %-9s step %5d  query loss (ema) %.3f\n", names[mx], s, run); fflush(stdout); }
        }
        /* evaluation: greedy prediction at every query position on fresh sequences */
        long correct = 0, total = 0, correct_rebound = 0, total_rebound = 0; rlm_real *logits = (rlm_real *)malloc(sizeof(rlm_real) * V);
        for (int e = 0; e < neval; ++e) {
            make_seq(seq, mask, qpos, nkeys, P, R, Q);
            cnet_vsa_rlm_reset_state(m);
            /* find keys that were re-bound (appear more than once before the queries) */
            int seen[256] = {0}, rebound[256] = {0};
            for (int i = 1; i < 1 + 2 * (P + R); i += 2) { int k = seq[i] - 2; if (seen[k]) rebound[k] = 1; seen[k] = 1; }
            int pos = 0;
            for (int qi = 0; qi < Q; ++qi) {
                cnet_vsa_rlm_predict(m, seq + pos, qpos[qi] + 1 - pos, logits); pos = qpos[qi] + 1;
                int best = 0; for (int v = 1; v < V; ++v) if (logits[v] > logits[best]) best = v;
                int k = seq[qpos[qi]] - 2, ok = best == seq[qpos[qi] + 1];
                total++; correct += ok; if (rebound[k]) { total_rebound++; correct_rebound += ok; }
            }
        }
        printf("%-9s query accuracy %.1f%% (%ld/%ld); on re-bound keys %.1f%% (%ld/%ld); %zu params; %.0f s\n", names[mx], 100.0 * correct / total, correct, total, total_rebound ? 100.0 * correct_rebound / total_rebound : 0.0, correct_rebound, total_rebound, cnet_vsa_rlm_param_count(m), now() - t0);
        fflush(stdout); free(logits); for (int b = 0; b < B; ++b) cnet_vsa_rlm_free(wk[b]); free(wk); free(bseq); free(bn); free(bmask); free(bl); cnet_vsa_rlm_free(m);
    }
    return 0;
}
