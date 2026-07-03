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
#include "../include/cce/cce_clgemm.h"

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

/* PAIR: argmax over V of P(v | [t, w_prev, w_cur]) — 3-token context. */
static int cce_cond_pair(const double *in, double *out, void *ctx) {
    CceOracleCtx *c = (CceOracleCtx *)ctx;
    size_t wp = 0, wc = 0, i, best = 0;
    int tokens[3];
    float bl;

    for (i = 0; i < c->v; ++i)
        if (in[i] > 0.5) wp = i;
    for (i = 0; i < c->v; ++i)
        if (in[c->v + i] > 0.5) wc = i;
    tokens[0] = c->t;
    tokens[1] = c->vocab[wp];
    tokens[2] = c->vocab[wc];
    c->m->cur_pos = 0;
    if (cce_gguf_qwen2_forward(c->m, tokens, 3, c->logits,
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

/* TOPK: the model's ordered top-3 within V, as 3 one-hot fields (the soul). */
static int cce_cond_topk(const double *in, double *out, void *ctx) {
    CceOracleCtx *c = (CceOracleCtx *)ctx;
    size_t w = 0, i, r, best;
    int tokens[2];
    int taken[4096] = {0};

    for (i = 0; i < c->v; ++i)
        if (in[i] > 0.5) w = i;
    tokens[0] = c->t;
    tokens[1] = c->vocab[w];
    c->m->cur_pos = 0;
    if (cce_gguf_qwen2_forward(c->m, tokens, 2, c->logits,
                               c->m->vocab_size) != CCE_OK) {
        return -1;
    }
    for (i = 0; i < 3u * c->v; ++i) out[i] = 0.0;
    for (r = 0; r < 3; ++r) {
        float bl;
        best = (size_t)-1;
        bl = 0.0f;
        for (i = 0; i < c->v; ++i) {
            if (taken[i]) continue;
            if (best == (size_t)-1 || c->logits[c->vocab[i]] > bl) {
                bl = c->logits[c->vocab[i]];
                best = i;
            }
        }
        taken[best] = 1;
        out[r * c->v + best] = 1.0;
    }
    return 0;
}

static CnetOracleFn cce_task_fn = cce_cond_next;   /* set by main per mode */

/* PAIR window discovery. A window of arbitrary ids yields CONSTANT
   conditional slices (the model's attractor tokens live outside it; measured
   on the gemma MTP draft: 4/4 class_imbalance defers). Build the window from
   the model's OWN most frequent full-vocab argmax choices over a
   deterministic probe spread, so the restricted argmax actually varies.
   Returns the number of discovered tokens placed (rest keep their defaults). */
static size_t discover_pair_window(cce_gguf_qwen2 *m, float *logits,
                                   int *vocab, size_t V) {
    unsigned *hist;
    size_t i, placed = 0;
    int probes = 128;

    hist = (unsigned *)calloc((size_t)m->vocab_size, sizeof *hist);
    if (!hist) return 0;
    for (i = 0; i < (size_t)probes; ++i) {
        int tokens[3];
        size_t a, best = 0;
        tokens[0] = 2000 + (int)((i * 37u) % 4096u);
        tokens[1] = 2000 + (int)((i * 91u + 17u) % 4096u);
        tokens[2] = 2000 + (int)((i * 53u + 5u) % 4096u);
        m->cur_pos = 0;
        if (cce_gguf_qwen2_forward(m, tokens, 3, logits,
                                   m->vocab_size) != CCE_OK) continue;
        for (a = 1; a < (size_t)m->vocab_size; ++a)
            if (logits[a] > logits[best]) best = a;
        hist[best]++;
    }
    while (placed < V) {
        size_t a, best = 0;
        for (a = 1; a < (size_t)m->vocab_size; ++a)
            if (hist[a] > hist[best]) best = a;
        if (hist[best] == 0) break;      /* fewer distinct choices than V */
        vocab[placed++] = (int)best;
        hist[best] = 0;
    }
    free(hist);
    return placed;
}

static int cce_maker(void *maker_ctx, size_t k, int token_id,
                     FlagshipOracle *out) {
    CceOracleCtx *c = (CceOracleCtx *)maker_ctx;
    (void)k;
    c->t = token_id;
    out->fn = cce_task_fn;
    out->ctx = c;
    return 0;
}

/* KV-state-leak guard: the same query must answer identically before and
   after an unrelated query. A leak here would poison every mined exemplar.
   Task-aware: PAIR has a 2-field input, TOPK a 3-field output. */
static int determinism_spot_check(CceOracleCtx *c, FlagshipTask task) {
    static double in[8192], out1[12288], out2[12288];
    size_t i;
    size_t in_total = (task == FLAGSHIP_TASK_PAIR) ? 2 * c->v : c->v;
    size_t out_total = (task == FLAGSHIP_TASK_TOPK) ? 3 * c->v : c->v;

    c->t = c->vocab[0];
    memset(in, 0, in_total * sizeof *in);
    in[0] = 1.0;
    if (task == FLAGSHIP_TASK_PAIR) in[c->v] = 1.0;
    if (cce_task_fn(in, out1, c) != 0) return -1;

    memset(in, 0, in_total * sizeof *in);      /* unrelated query in between */
    in[c->v - 1] = 1.0;
    if (task == FLAGSHIP_TASK_PAIR) in[in_total - 1] = 1.0;
    if (cce_task_fn(in, out2, c) != 0) return -1;

    memset(in, 0, in_total * sizeof *in);      /* repeat the first */
    in[0] = 1.0;
    if (task == FLAGSHIP_TASK_PAIR) in[c->v] = 1.0;
    if (cce_task_fn(in, out2, c) != 0) return -1;

    for (i = 0; i < out_total; ++i)
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

    FlagshipTask task = FLAGSHIP_TASK_ARGMAX;

    if (argc < 2) {
        fprintf(stderr, "usage: %s <model> [V] [max_units] [temp_C] [duty] "
                        "[wall_s] [base.cnb] [argmax|pair|topk]\n", argv[0]);
        return 2;
    }
    model_path = argv[1];
    if (argc > 2) V = (size_t)atoi(argv[2]);
    if (argc > 3) max_units = (size_t)atoi(argv[3]);
    if (argc > 4) temp_c = atoi(argv[4]);
    if (argc > 5) duty = atof(argv[5]);
    if (argc > 6) wall = atof(argv[6]);
    if (argc > 7) base_path = argv[7];
    if (argc > 8) {
        if (strcmp(argv[8], "pair") == 0) task = FLAGSHIP_TASK_PAIR;
        else if (strcmp(argv[8], "topk") == 0) task = FLAGSHIP_TASK_TOPK;
        else if (strcmp(argv[8], "argmax") != 0) {
            fprintf(stderr, "unknown task %s\n", argv[8]);
            return 2;
        }
    }
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

    flagship_config_defaults(&cfg);
    cfg.task = task;
    cfg.vocab_tokens = vocab;
    cfg.vocab_size = V;
    cfg.max_units = max_units;
    cfg.base_path = base_path;
    snprintf(ledger_path, sizeof ledger_path, "%s.gaps.txt", base_path);
    cfg.ledger_path = ledger_path;
    cfg.gpu_temp_limit_c = temp_c;
    cfg.duty_fraction = duty;
    cfg.max_wall_seconds = wall;
    switch (task) {
    case FLAGSHIP_TASK_PAIR:
        cce_task_fn = cce_cond_pair;
        /* memorization-scale student for entangled pair lookups (the one
           documented tuning pass; certify_failed rates are the RESULT) */
        cfg.acq.init_hidden = 64;
        cfg.acq.max_hidden = 256;
        cfg.acq.max_epochs = 12000;
        cfg.acq.growth_window = 400;
        cfg.acq.holdout_fraction = 0.0;  /* exactness-on-sample is the bar */
        {
            size_t placed = discover_pair_window(am->transformer, ctx.logits,
                                                 vocab, V);
            printf("pair window: %lu/%lu tokens from the model's own "
                   "argmax distribution\n",
                   (unsigned long)placed, (unsigned long)V);
        }
        break;
    case FLAGSHIP_TASK_TOPK:
        cce_task_fn = cce_cond_topk;
        break;
    default:
        cce_task_fn = cce_cond_next;
        break;
    }

    /* CNET_GPU=1: OpenCL forward, gated by an in-process equivalence sweep —
       a diverging oracle silently changes the meaning of extracted knowledge,
       so any decision mismatch refuses GPU mining outright. */
    if (getenv("CNET_GPU") && strcmp(getenv("CNET_GPU"), "1") == 0) {
        char dev[128] = {0};
        cce_clgemm *gpu = cce_clgemm_open(NULL, dev, sizeof dev);
        if (!gpu) {
            fprintf(stderr, "CNET_GPU=1 but no OpenCL GPU available\n");
            return 1;
        }
        {
            float *lg = (float *)malloc(
                (size_t)am->transformer->vocab_size * sizeof *lg);
            size_t jj, mismatch = 0;
            if (!lg) return 1;
            for (jj = 0; jj < 16 && !mismatch; ++jj) {
                int tk[2];
                size_t a, am_c = 0, am_g = 0;
                tk[0] = 2000 + (int)((jj * 37u) % 4096u);
                tk[1] = 2000 + (int)((jj * 91u + 17u) % 4096u);
                am->transformer->cur_pos = 0;
                cce_gguf_qwen2_forward(am->transformer, tk, 2, ctx.logits,
                                       am->transformer->vocab_size);
                cce_gguf_set_clgemm(gpu);
                am->transformer->cur_pos = 0;
                cce_gguf_qwen2_forward(am->transformer, tk, 2, lg,
                                       am->transformer->vocab_size);
                cce_gguf_set_clgemm(NULL);
                for (a = 1; a < (size_t)am->transformer->vocab_size; ++a) {
                    if (ctx.logits[a] > ctx.logits[am_c]) am_c = a;
                    if (lg[a] > lg[am_g]) am_g = a;
                }
                if (am_c != am_g) mismatch = 1;
            }
            free(lg);
            if (mismatch) {
                fprintf(stderr, "GPU EQUIVALENCE FAILED — refusing to mine "
                                "with --gpu (run gpu_equiv for details)\n");
                cce_clgemm_close(gpu);
                return 1;
            }
        }
        cce_gguf_set_clgemm(gpu);
        printf("gpu: %s (equivalence sweep OK, %0.1f MB will go resident)\n",
               dev, 0.0);
    }

    if (determinism_spot_check(&ctx, task) != 0) {
        fprintf(stderr, "DETERMINISM CHECK FAILED: the forward pass is not "
                        "reproducible across calls (KV state leak?) — refusing "
                        "to mine from a nondeterministic oracle\n");
        return 1;
    }
    printf("determinism spot check: OK\n");

    printf("run: task=%s V=%lu max_units=%lu temp<=%dC duty=%.2f wall=%.0fs base=%s\n",
           task == FLAGSHIP_TASK_PAIR ? "pair"
           : task == FLAGSHIP_TASK_TOPK ? "topk" : "argmax",
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
