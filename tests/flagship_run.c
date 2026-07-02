/* The REAL flagship run: a CCE-loaded transformer as the acquisition oracle.
 *
 * Extracts conditional next-token slices (V-restricted greedy argmax) from
 * the model into certified, sealed units in a unified base — duty-cycled,
 * thermally governed, crash-resumable (re-run the same command to resume;
 * create <base>.stop to stop cleanly).
 *
 * Usage:
 *   flagship_run <model> [V] [max_units] [temp_C] [duty] [wall_s] [base.cnb]
 * Defaults: V=64, max_units=8 (smoke-sized), temp 80C, duty 0.75, wall 0
 * (unbounded), base flagship.cnb. Vocab = token ids 2000..2000+V-1.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/flagship.h"
#include "../include/cce/cce_detect.h"

typedef struct {
    cce_gguf_qwen2 *m;
    const int *vocab;
    size_t v;
    int t;           /* conditioning token of the CURRENT unit */
    float *logits;   /* model vocab_size scratch */
} CceOracleCtx;

static int cce_cond_next(const double *in, double *out, void *ctx) {
    CceOracleCtx *c = (CceOracleCtx *)ctx;
    size_t w = 0, i, best = 0;
    int tokens[2];
    float bl;

    for (i = 0; i < c->v; ++i)
        if (in[i] > 0.5) w = i;
    tokens[0] = c->t;
    tokens[1] = c->vocab[w];
    c->m->cur_pos = 0;   /* restart the KV cache: each call is a fresh context */
    if (cce_gguf_qwen2_forward(c->m, tokens, 2, c->logits,
                               c->m->vocab_size) != CCE_OK) {
        return -1;
    }
    bl = c->logits[c->vocab[0]];
    for (i = 1; i < c->v; ++i) {
        if (c->logits[c->vocab[i]] > bl) {
            bl = c->logits[c->vocab[i]];
            best = i;
        }
    }
    for (i = 0; i < c->v; ++i) out[i] = (i == best) ? 1.0 : 0.0;
    return 0;
}

static int cce_maker(void *maker_ctx, size_t k, int token_id,
                     FlagshipOracle *out) {
    CceOracleCtx *c = (CceOracleCtx *)maker_ctx;
    (void)k;
    c->t = token_id;
    out->fn = cce_cond_next;
    out->ctx = c;
    return 0;
}

/* KV-state-leak guard: the same query must answer identically before and
   after an unrelated query. A leak here would poison every mined exemplar. */
static int determinism_spot_check(CceOracleCtx *c) {
    double in[4096], out1[4096], out2[4096];
    size_t i;
    memset(in, 0, c->v * sizeof *in);
    in[0] = 1.0;
    c->t = c->vocab[0];
    if (cce_cond_next(in, out1, c) != 0) return -1;
    in[0] = 0.0;
    in[c->v - 1] = 1.0;          /* unrelated query in between */
    if (cce_cond_next(in, out2, c) != 0) return -1;
    in[c->v - 1] = 0.0;
    in[0] = 1.0;
    if (cce_cond_next(in, out2, c) != 0) return -1;
    for (i = 0; i < c->v; ++i)
        if (out1[i] != out2[i]) return -1;
    return 0;
}

int main(int argc, char **argv) {
    const char *model_path;
    size_t V = 64, max_units = 8;
    int temp_c = 80;
    double duty = 0.75, wall = 0.0;
    const char *base_path = "flagship.cnb";
    char ledger_path[512];
    cce_anymodel *am = NULL;
    CceOracleCtx ctx;
    int *vocab;
    size_t i;
    FlagshipConfig cfg;
    FlagshipReport rep;

    if (argc < 2) {
        fprintf(stderr, "usage: %s <model> [V] [max_units] [temp_C] [duty] "
                        "[wall_s] [base.cnb]\n", argv[0]);
        return 2;
    }
    model_path = argv[1];
    if (argc > 2) V = (size_t)atoi(argv[2]);
    if (argc > 3) max_units = (size_t)atoi(argv[3]);
    if (argc > 4) temp_c = atoi(argv[4]);
    if (argc > 5) duty = atof(argv[5]);
    if (argc > 6) wall = atof(argv[6]);
    if (argc > 7) base_path = argv[7];
    if (V < 16 || V > 4096) { fprintf(stderr, "V must be 16..4096\n"); return 2; }

    if (cce_anymodel_open(&am, model_path) != CCE_OK || am->transformer == NULL) {
        fprintf(stderr, "cannot open a transformer runner for %s "
                        "(see: make detect FILE=%s)\n", model_path, model_path);
        return 1;
    }
    printf("model: %s  layers=%d hidden=%d vocab=%d\n", model_path,
           am->transformer->n_layer, am->transformer->n_embd,
           am->transformer->vocab_size);
    if ((int)(2000 + V) >= am->transformer->vocab_size) {
        fprintf(stderr, "vocab window exceeds model vocab\n");
        return 1;
    }

    vocab = (int *)malloc(V * sizeof *vocab);
    for (i = 0; i < V; ++i) vocab[i] = 2000 + (int)i;

    memset(&ctx, 0, sizeof ctx);
    ctx.m = am->transformer;
    ctx.vocab = vocab;
    ctx.v = V;
    ctx.logits = (float *)malloc((size_t)am->transformer->vocab_size *
                                 sizeof *ctx.logits);
    if (!ctx.logits) { fprintf(stderr, "oom\n"); return 1; }

    if (determinism_spot_check(&ctx) != 0) {
        fprintf(stderr, "DETERMINISM CHECK FAILED: the forward pass is not "
                        "reproducible across calls (KV state leak?) — refusing "
                        "to mine from a nondeterministic oracle\n");
        return 1;
    }
    printf("determinism spot check: OK\n");

    flagship_config_defaults(&cfg);
    cfg.vocab_tokens = vocab;
    cfg.vocab_size = V;
    cfg.max_units = max_units;
    cfg.base_path = base_path;
    snprintf(ledger_path, sizeof ledger_path, "%s.gaps.txt", base_path);
    cfg.ledger_path = ledger_path;
    cfg.gpu_temp_limit_c = temp_c;
    cfg.duty_fraction = duty;
    cfg.max_wall_seconds = wall;

    printf("run: V=%lu max_units=%lu temp<=%dC duty=%.2f wall=%.0fs base=%s\n",
           (unsigned long)V, (unsigned long)max_units, temp_c, duty, wall,
           base_path);
    printf("stop cleanly at any time:  echo stop > %s.stop\n", base_path);

    if (flagship_run(&cfg, cce_maker, &ctx, &rep) != 0) {
        fprintf(stderr, "flagship_run failed to start\n");
        return 1;
    }
    flagship_print_report(&rep, stdout);

    /* the decisive number */
    if (rep.attempted > 0) {
        printf("extraction rate this run: %lu/%lu (%.1f%%)\n",
               (unsigned long)rep.acquired, (unsigned long)rep.attempted,
               100.0 * (double)rep.acquired / (double)rep.attempted);
    }

    free(ctx.logits);
    free(vocab);
    cce_anymodel_free(am);
    return 0;
}
