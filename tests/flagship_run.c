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

#ifdef _OPENMP
#include <omp.h>
#endif

#include "../include/flagship.h"
#include "../include/cce/cce_detect.h"
#include "../include/cce/cce_clgemm.h"
#include "window_discover.h"

#define FS_MAX_LANES 8

/* One oracle LANE: a model instance + logits scratch (+ its own GPU handle
   in pool mode). Lanes share nothing mutable — no queue, no KV cache — so
   the mining loop can call the oracle width-wide, one lane per GPU. */
typedef struct {
    cce_anymodel *am;      /* owned; NULL for lane 0 (borrows main's model) */
    cce_gguf_qwen2 *m;
    float *logits;         /* model vocab_size scratch */
    cce_clgemm *gpu;       /* per-lane handle in pool mode; NULL otherwise */
    int prefix_token;      /* token whose KV occupies row 0; -1 = none */
} OracleLane;

typedef struct {
    OracleLane lane[FS_MAX_LANES];
    size_t nlanes;
    int inner_threads;     /* per-lane CPU team for the forward's OMP tiles */
    int force_lane;        /* >= 0: pin a lane (serial checks); -1 = by thread */
    const int *vocab;
    size_t v;
    int t;                 /* conditioning token of the CURRENT unit */
    double margin_eps;     /* >0: margin-aware certification. A point whose
                              decision margin (smallest logit gap that would
                              change the ordered answer) is below this is
                              ABSTAINED (oracle returns +1) — the teacher's
                              own coin-flips are excluded from the certified
                              domain instead of poisoning training. 0 = off. */
} CceOracleCtx;

/* Smallest logit gap whose crossing would change the ordered top-`depth`
   window answer (gaps rank0-1, 1-2, ..., depth-1..depth). A tiny value means
   the teacher is nearly tied at some decision boundary — a point no student
   can be expected to memorize because the model itself is choosing at random
   there. Returns +inf-ish (large) when the window is smaller than depth+1. */
static double fs_decision_margin(const float *logits, const int *vocab,
                                 size_t V, int depth) {
    int taken[4096] = {0};
    double prev = 0.0, minfg = 1e30;
    int r;
    if ((size_t)(depth + 1) > V) return 1e30;
    for (r = 0; r <= depth; ++r) {
        size_t i, best = (size_t)-1;
        double bl = 0.0;
        for (i = 0; i < V; ++i) {
            if (taken[i]) continue;
            if (best == (size_t)-1 || (double)logits[vocab[i]] > bl) {
                bl = (double)logits[vocab[i]];
                best = i;
            }
        }
        taken[best] = 1;
        if (r > 0) { double g = prev - bl; if (g < minfg) minfg = g; }
        prev = bl;
    }
    return minfg;
}

/* Lane for THIS call: pinned when force_lane is set, else by OMP thread id
   (the mining loop runs one thread per lane). Also scopes the calling
   thread's nested-OMP team so `lanes x inner` matches the machine. */
static OracleLane *fs_lane(CceOracleCtx *c) {
    size_t li = 0;
#ifdef _OPENMP
    if (c->force_lane >= 0) li = (size_t)c->force_lane % c->nlanes;
    else li = (size_t)omp_get_thread_num() % c->nlanes;
    if (c->inner_threads > 0) omp_set_num_threads(c->inner_threads);
#endif
    return &c->lane[li];
}

/* All calls of one unit share conditioning token t. Its KV row is a pure
   function of the token and the weights, so computing it ONCE per lane per
   unit and letting every call attend to the cached row 0 is BIT-IDENTICAL
   to recomputing it inside each call — while removing 1 of 2 (argmax/topk)
   or 1 of 3 (pair) tokens from every layer traversal. Suffix calls write
   KV rows >= 1 only; row 0 stays pinned until t changes.
   CNET_ORACLE_PREFIX=0 disables (A/B; both paths are gated by the digest
   audit and the determinism spot check either way). */
static int fs_prefix_off(void) {
    const char *e = getenv("CNET_ORACLE_PREFIX");
    return e && e[0] == '0';
}

/* BOS anchor (default ON, CNET_ORACLE_BOS=0 opts out): gemma-family models
   collapse to <pad> on BOS-less raw-token contexts — measured 1 distinct
   full-vocab argmax over 128 probes on BOTH real models, which made every
   window degenerate and every mined unit constant. Anchored contexts are
   [bos, t, ...suffix]; with prefix reuse the anchor costs nothing per call. */
static int fs_bos(const cce_gguf_qwen2 *m) {
    const char *e = getenv("CNET_ORACLE_BOS");
    if (e && e[0] == '0') return -1;
    return m->bos_token_id;
}

static int fs_prefix(OracleLane *L, int t) {
    int toks[2];
    int n = 0, bos = fs_bos(L->m);
    if (L->prefix_token == t) return 0;
    L->prefix_token = -1;                 /* invalid until fully computed */
    if (bos >= 0) toks[n++] = bos;
    toks[n++] = t;
    L->m->cur_pos = 0;
    if (cce_gguf_qwen2_forward(L->m, toks, n, L->logits,
                               L->m->vocab_size) != CCE_OK) {
        return -1;
    }
    L->prefix_token = t;
    return 0;
}

/* prefix length = rows pinned in the KV cache = where each suffix starts */
static int fs_prefix_len(const cce_gguf_qwen2 *m) {
    return (fs_bos(m) >= 0) ? 2 : 1;
}

static int cce_cond_next(const double *in, double *out, void *ctx) {
    CceOracleCtx *c = (CceOracleCtx *)ctx;
    OracleLane *L = fs_lane(c);
    size_t w = 0, i, best = 0;
    int tokens[2];
    float bl;

    for (i = 0; i < c->v; ++i)
        if (in[i] > 0.5) w = i;
    if (fs_prefix_off()) {
        int n = 0, bos = fs_bos(L->m);
        int toks[3];
        if (bos >= 0) toks[n++] = bos;
        toks[n++] = c->t;
        toks[n++] = c->vocab[w];
        L->m->cur_pos = 0;   /* fresh context: prefix recomputed every call */
        if (cce_gguf_qwen2_forward(L->m, toks, n, L->logits,
                                   L->m->vocab_size) != CCE_OK) {
            return -1;
        }
    } else {
        if (fs_prefix(L, c->t) != 0) return -1;
        tokens[0] = c->vocab[w];
        L->m->cur_pos = fs_prefix_len(L->m);   /* attend to the pinned prefix */
        if (cce_gguf_qwen2_forward(L->m, tokens, 1, L->logits,
                                   L->m->vocab_size) != CCE_OK) {
            return -1;
        }
    }
    if (c->margin_eps > 0.0 &&
        fs_decision_margin(L->logits, c->vocab, c->v, 1) < c->margin_eps)
        return 1;   /* teacher-ambiguous argmax: abstain */
    bl = L->logits[c->vocab[0]];
    for (i = 1; i < c->v; ++i) {
        if (L->logits[c->vocab[i]] > bl) {
            bl = L->logits[c->vocab[i]];
            best = i;
        }
    }
    for (i = 0; i < c->v; ++i) out[i] = (i == best) ? 1.0 : 0.0;
    return 0;
}

/* PAIR: argmax over V of P(v | [t, w_prev, w_cur]) — 3-token context. */
static int cce_cond_pair(const double *in, double *out, void *ctx) {
    CceOracleCtx *c = (CceOracleCtx *)ctx;
    OracleLane *L = fs_lane(c);
    size_t wp = 0, wc = 0, i, best = 0;
    int tokens[3];
    float bl;

    for (i = 0; i < c->v; ++i)
        if (in[i] > 0.5) wp = i;
    for (i = 0; i < c->v; ++i)
        if (in[c->v + i] > 0.5) wc = i;
    if (fs_prefix_off()) {
        int n = 0, bos = fs_bos(L->m);
        int toks[4];
        if (bos >= 0) toks[n++] = bos;
        toks[n++] = c->t;
        toks[n++] = c->vocab[wp];
        toks[n++] = c->vocab[wc];
        L->m->cur_pos = 0;
        if (cce_gguf_qwen2_forward(L->m, toks, n, L->logits,
                                   L->m->vocab_size) != CCE_OK) {
            return -1;
        }
    } else {
        if (fs_prefix(L, c->t) != 0) return -1;
        tokens[0] = c->vocab[wp];
        tokens[1] = c->vocab[wc];
        L->m->cur_pos = fs_prefix_len(L->m);
        if (cce_gguf_qwen2_forward(L->m, tokens, 2, L->logits,
                                   L->m->vocab_size) != CCE_OK) {
            return -1;
        }
    }
    if (c->margin_eps > 0.0 &&
        fs_decision_margin(L->logits, c->vocab, c->v, 1) < c->margin_eps)
        return 1;   /* teacher-ambiguous argmax: abstain */
    bl = L->logits[c->vocab[0]];
    for (i = 1; i < c->v; ++i) {
        if (L->logits[c->vocab[i]] > bl) {
            bl = L->logits[c->vocab[i]];
            best = i;
        }
    }
    for (i = 0; i < c->v; ++i) out[i] = (i == best) ? 1.0 : 0.0;
    return 0;
}

/* TOPK: the model's ordered top-3 within V, as 3 one-hot fields (the soul). */
static int cce_cond_topk(const double *in, double *out, void *ctx) {
    CceOracleCtx *c = (CceOracleCtx *)ctx;
    OracleLane *L = fs_lane(c);
    size_t w = 0, i, r, best;
    int tokens[2];
    int taken[4096] = {0};

    for (i = 0; i < c->v; ++i)
        if (in[i] > 0.5) w = i;
    if (fs_prefix_off()) {
        int n = 0, bos = fs_bos(L->m);
        int toks[3];
        if (bos >= 0) toks[n++] = bos;
        toks[n++] = c->t;
        toks[n++] = c->vocab[w];
        L->m->cur_pos = 0;
        if (cce_gguf_qwen2_forward(L->m, toks, n, L->logits,
                                   L->m->vocab_size) != CCE_OK) {
            return -1;
        }
    } else {
        if (fs_prefix(L, c->t) != 0) return -1;
        tokens[0] = c->vocab[w];
        L->m->cur_pos = fs_prefix_len(L->m);
        if (cce_gguf_qwen2_forward(L->m, tokens, 1, L->logits,
                                   L->m->vocab_size) != CCE_OK) {
            return -1;
        }
    }
    if (c->margin_eps > 0.0 &&
        fs_decision_margin(L->logits, c->vocab, c->v, 3) < c->margin_eps)
        return 1;   /* teacher-ambiguous top-3: abstain from the cert domain */
    for (i = 0; i < 3u * c->v; ++i) out[i] = 0.0;
    for (r = 0; r < 3; ++r) {
        float bl;
        best = (size_t)-1;
        bl = 0.0f;
        for (i = 0; i < c->v; ++i) {
            if (taken[i]) continue;
            if (best == (size_t)-1 || L->logits[c->vocab[i]] > bl) {
                bl = L->logits[c->vocab[i]];
                best = i;
            }
        }
        taken[best] = 1;
        out[r * c->v + best] = 1.0;
    }
    return 0;
}

static CnetOracleFn cce_task_fn = cce_cond_next;   /* set by main per mode */

static int cce_maker(void *maker_ctx, size_t k, int token_id,
                     FlagshipOracle *out) {
    CceOracleCtx *c = (CceOracleCtx *)maker_ctx;
    (void)k;
    c->t = token_id;
    out->fn = cce_task_fn;
    out->ctx = c;
    out->width = c->nlanes;   /* pool mode: mining fans out one thread/lane */
    return 0;
}

/* KV-state-leak guard: the same query must answer identically before and
   after an unrelated query. A leak here would poison every mined exemplar.
   Task-aware: PAIR has a 2-field input, TOPK a 3-field output.
   Runs per LANE, and additionally requires every lane's answer to be
   identical to lane 0's — divergent lanes would make mined knowledge depend
   on scheduling, which is exactly what the digests must never do. */
static int determinism_spot_check(CceOracleCtx *c, FlagshipTask task) {
    static double in[8192], out1[12288], out2[12288], out_ref[12288];
    size_t i, lane;
    size_t in_total = (task == FLAGSHIP_TASK_PAIR) ? 2 * c->v : c->v;
    size_t out_total = (task == FLAGSHIP_TASK_TOPK) ? 3 * c->v : c->v;
    int rc = 0;
    /* determinism, not certification: never abstain here (a probe point that
       happens to be teacher-ambiguous would return +1 and read as failure). */
    double saved_eps = c->margin_eps;
    c->margin_eps = 0.0;

    c->t = c->vocab[0];
    for (lane = 0; lane < c->nlanes && rc == 0; ++lane) {
        c->force_lane = (int)lane;

        memset(in, 0, in_total * sizeof *in);
        in[0] = 1.0;
        if (task == FLAGSHIP_TASK_PAIR) in[c->v] = 1.0;
        if (cce_task_fn(in, out1, c) != 0) { rc = -1; break; }

        memset(in, 0, in_total * sizeof *in);  /* unrelated query in between */
        in[c->v - 1] = 1.0;
        if (task == FLAGSHIP_TASK_PAIR) in[in_total - 1] = 1.0;
        if (cce_task_fn(in, out2, c) != 0) { rc = -1; break; }

        memset(in, 0, in_total * sizeof *in);  /* repeat the first */
        in[0] = 1.0;
        if (task == FLAGSHIP_TASK_PAIR) in[c->v] = 1.0;
        if (cce_task_fn(in, out2, c) != 0) { rc = -1; break; }

        for (i = 0; i < out_total && rc == 0; ++i)
            if (out1[i] != out2[i]) rc = -1;

        if (lane == 0) {
            memcpy(out_ref, out1, out_total * sizeof *out_ref);
        } else {
            for (i = 0; i < out_total && rc == 0; ++i)
                if (out1[i] != out_ref[i]) rc = -1;   /* cross-lane divergence */
        }
    }
    c->force_lane = -1;
    c->margin_eps = saved_eps;
    return rc;
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
    int window_from_file;
    FlagshipConfig cfg;
    FlagshipReport rep;

    FlagshipTask task = FLAGSHIP_TASK_ARGMAX;

    /* Line-buffer stdout: mining runs live for hours under service managers
       with stdout redirected to a file; block buffering hides every
       progress line until exit. */
    setvbuf(stdout, NULL, _IOLBF, 0);

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
    {
        size_t li0;
        for (li0 = 0; li0 < FS_MAX_LANES; ++li0)
            ctx.lane[li0].prefix_token = -1;
    }
    ctx.lane[0].m = am->transformer;
    ctx.nlanes = 1;
    ctx.force_lane = -1;
    /* margin-aware certification: CNET_CERT_MARGIN=<logit gap>. A domain
       point whose decision margin (smallest logit gap that would change the
       ordered answer) falls below this is ABSTAINED — the model's own
       near-ties are excluded from the certified domain instead of poisoning
       training and blocking certification. 0 (default) = off. */
    ctx.margin_eps = getenv("CNET_CERT_MARGIN")
                         ? atof(getenv("CNET_CERT_MARGIN")) : 0.0;
#ifdef _OPENMP
    ctx.inner_threads = omp_get_max_threads();
#endif
    ctx.vocab = vocab;
    ctx.v = V;
    ctx.lane[0].logits = (float *)malloc((size_t)am->transformer->vocab_size *
                                         sizeof *ctx.lane[0].logits);
    if (!ctx.lane[0].logits) { fprintf(stderr, "oom\n"); return 1; }

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
            size_t placed = cnet_window_discover(am->transformer,
                                                 ctx.lane[0].logits,
                                                 vocab, V, 3,
                                                 fs_bos(am->transformer));
            printf("pair window: %lu/%lu tokens from the model's own "
                   "argmax distribution\n",
                   (unsigned long)placed, (unsigned long)V);
        }
        break;
    case FLAGSHIP_TASK_TOPK:
        cce_task_fn = cce_cond_topk;
        /* memorization-scale student (same documented precedent as PAIR):
           a REAL oracle's top-3 slices are entangled ~40-class lookups —
           the lean-teacher default (hidden<=64, 4000 epochs) certify_fails
           on every unit. The NaN-era oracle "certified" only because its
           units were constant functions. */
        cfg.acq.init_hidden = 64;
        cfg.acq.max_hidden = 256;
        cfg.acq.max_epochs = 12000;
        cfg.acq.growth_window = 400;
        break;
    default:
        cce_task_fn = cce_cond_next;
        cfg.acq.init_hidden = 64;      /* same reasoning as TOPK above */
        cfg.acq.max_hidden = 256;
        cfg.acq.max_epochs = 12000;
        cfg.acq.growth_window = 400;
        break;
    }

    /* Certification tier. Default (exhaustive/PROVEN) demands EXACT match on
       every domain point — measured on the real gemma4 oracle, students
       reach 254-255/256, blocked from PROVEN by the model's OWN near-tie
       points (worst_margin ~0.001: irreducible residue, the doc's
       "no sharp boundary" wall). CNET_CERT_SAMPLED=1 engages the SAMPLED
       tier: mine a strict subset of the domain so coverage is SAMPLED, then
       certify with a Wilson lower bound (min_accuracy_bound, default 0.95) —
       high-confidence units instead of exact proofs. Exactness-on-sample is
       the bar (holdout 0), the PAIR precedent. */
    if (getenv("CNET_CERT_SAMPLED") && getenv("CNET_CERT_SAMPLED")[0] == '1') {
        size_t samp = (size_t)((double)V * 0.8);   /* 80% subsample -> SAMPLED */
        /* CNET_CERT_SAMPLE_COUNT: explicit subsample size. Wilson >= 0.95
           needs only ~96 all-correct points; the 0.8V default (204 at
           V=256) both doubles the oracle probing cost AND plants more of
           the teacher's coin-flip points in the sample than the bound
           requires. Smaller samples are faster and pass more often, at a
           lower (still >= min_accuracy_bound) statistical floor. */
        if (getenv("CNET_CERT_SAMPLE_COUNT")) {
            long sc = atol(getenv("CNET_CERT_SAMPLE_COUNT"));
            if (sc >= 16 && (size_t)sc < V) samp = (size_t)sc;
        }
        if (samp < 16) samp = (V < 16 ? V : 16);
        cfg.acq.sample_count = samp;
        cfg.acq.mine_budget = (samp > 1) ? samp - 1 : 1;  /* card>budget => sampled */
        cfg.acq.holdout_fraction = 0.0;
        printf("cert tier: SAMPLED (Wilson >= %.2f over a %lu/%lu-point "
               "subsample; exactness-on-sample)\n",
               cfg.acq.min_accuracy_bound, (unsigned long)samp,
               (unsigned long)V);
    }

    /* Student-capacity overrides (sweep the representational limit without
       recompiling): CNET_ACQ_HIDDEN / CNET_ACQ_MAXHIDDEN / CNET_ACQ_EPOCHS.
       Diagnostic for whether cert failures are the student's ceiling (they
       shrink with capacity) or the oracle's ambiguity (they don't). */
    if (getenv("CNET_ACQ_HIDDEN"))    cfg.acq.init_hidden = (size_t)atoi(getenv("CNET_ACQ_HIDDEN"));
    if (getenv("CNET_ACQ_MAXHIDDEN")) cfg.acq.max_hidden  = (size_t)atoi(getenv("CNET_ACQ_MAXHIDDEN"));
    if (getenv("CNET_ACQ_EPOCHS"))    cfg.acq.max_epochs  = (size_t)atoi(getenv("CNET_ACQ_EPOCHS"));
    /* Student seed override: the default (42) is FIXED, so re-running a
       deferred unit reproduces the identical near-miss deterministically.
       Retry passes over deferred units need a different seed per pass to
       give each student a fresh draw at exactness-on-sample. */
    if (getenv("CNET_ACQ_SEED"))      cfg.acq.seed = (unsigned)atoi(getenv("CNET_ACQ_SEED"));
    if (getenv("CNET_ACQ_HIDDEN") || getenv("CNET_ACQ_MAXHIDDEN") ||
        getenv("CNET_ACQ_EPOCHS"))
        printf("student: init_hidden=%lu max_hidden=%lu max_epochs=%lu\n",
               (unsigned long)cfg.acq.init_hidden,
               (unsigned long)cfg.acq.max_hidden,
               (unsigned long)cfg.acq.max_epochs);

    /* Explicit window: CNET_WINDOW_FILE=<path> supplies the V token ids
       (one decimal id per line) instead of argmax discovery — e.g. an
       English word window so downstream claim verification covers English
       text instead of the model's multilingual argmax attractors. Ids must
       be unique and inside the model vocab; the run aborts on a malformed
       file rather than silently mining the wrong units. ARGMAX/TOPK only
       (PAIR windows are always discovered). */
    window_from_file = 0;
    if (task != FLAGSHIP_TASK_PAIR && getenv("CNET_WINDOW_FILE")) {
        const char *wf = getenv("CNET_WINDOW_FILE");
        FILE *f = fopen(wf, "r");
        size_t got = 0, j;
        if (!f) {
            fprintf(stderr, "CNET_WINDOW_FILE %s: cannot open\n", wf);
            return 1;
        }
        while (got < V && fscanf(f, "%d", &vocab[got]) == 1) {
            if (vocab[got] < 0 ||
                vocab[got] >= am->transformer->vocab_size) {
                fprintf(stderr, "CNET_WINDOW_FILE: id %d outside model "
                                "vocab\n", vocab[got]);
                fclose(f);
                return 1;
            }
            got++;
        }
        fclose(f);
        if (got != V) {
            fprintf(stderr, "CNET_WINDOW_FILE: need %lu ids, got %lu\n",
                    (unsigned long)V, (unsigned long)got);
            return 1;
        }
        for (i = 0; i < V; ++i)
            for (j = i + 1; j < V; ++j)
                if (vocab[i] == vocab[j]) {
                    fprintf(stderr, "CNET_WINDOW_FILE: duplicate id %d\n",
                            vocab[i]);
                    return 1;
                }
        window_from_file = 1;
        printf("window: %lu tokens from %s (first: %d %d %d %d)\n",
               (unsigned long)V, wf, vocab[0], vocab[1], vocab[2], vocab[3]);
    }

    /* Window discovery for ARGMAX/TOPK (PAIR always discovered): the fixed
       2000..2000+V window is a MEASURED constant slice on both real models
       (window_discover.h) — every unit mined from it is one constant
       function. Discovery derives the window from the model's own argmax
       attractors, deterministically (resume-safe: same window, same tags).
       CNET_WINDOW_DISCOVER=0 restores the fixed window. */
    if (task != FLAGSHIP_TASK_PAIR && !window_from_file &&
        !(getenv("CNET_WINDOW_DISCOVER") &&
          getenv("CNET_WINDOW_DISCOVER")[0] == '0')) {
        size_t placed = cnet_window_discover(am->transformer,
                                             ctx.lane[0].logits, vocab, V, 2,
                                             fs_bos(am->transformer));
        printf("window: %lu/%lu tokens from the model's own argmax "
               "distribution (first: %d %d %d %d)\n", (unsigned long)placed,
               (unsigned long)V, vocab[0], vocab[1], vocab[2], vocab[3]);
        if (placed < V / 2)
            fprintf(stderr, "WARNING: only %lu distinct attractor tokens — "
                            "window padded with defaults beyond that\n",
                    (unsigned long)placed);
    }

    /* CNET_GPU=1: OpenCL forward, gated by an in-process equivalence sweep —
       a diverging oracle silently changes the meaning of extracted knowledge,
       so any decision mismatch refuses GPU mining outright.
       With >1 discrete GPU (and an OpenMP build), an oracle POOL is built:
       one model instance pinned per GPU, and mining calls the oracle
       width-wide (CNET_ORACLE_LANES caps it; 1 restores the single path). */
    if (getenv("CNET_GPU") && strcmp(getenv("CNET_GPU"), "1") == 0) {
        char dev[128] = {0};
        size_t lanes = 1;
        cce_clgemm *gpu = cce_clgemm_open(NULL, dev, sizeof dev);
        if (!gpu) {
            fprintf(stderr, "CNET_GPU=1 but no OpenCL GPU available\n");
            return 1;
        }
#ifdef _OPENMP
        lanes = cce_clgemm_device_count(gpu);
        if (getenv("CNET_ORACLE_LANES")) {
            long wl = atol(getenv("CNET_ORACLE_LANES"));
            if (wl >= 1 && wl < (long)lanes) lanes = (size_t)wl;
        }
        if (lanes > FS_MAX_LANES) lanes = FS_MAX_LANES;
#endif
        if (lanes > 1) {
            /* pool mode: per-lane single-device handles; the probe handle
               (which spans ALL devices) is not used for mining */
            size_t li;
            cce_clgemm_close(gpu);
            gpu = NULL;
            for (li = 0; li < lanes; ++li) {
                char dn[128] = {0};
                ctx.lane[li].gpu =
                    cce_clgemm_open_device(NULL, (int)li, dn, sizeof dn);
                if (!ctx.lane[li].gpu) {
                    fprintf(stderr, "lane %lu: cannot open GPU device\n",
                            (unsigned long)li);
                    return 1;
                }
                if (li > 0) {
                    cce_anymodel *am2 = NULL;
                    if (cce_anymodel_open(&am2, model_path) != CCE_OK ||
                        am2->transformer == NULL) {
                        fprintf(stderr, "lane %lu: cannot re-open %s\n",
                                (unsigned long)li, model_path);
                        return 1;
                    }
                    ctx.lane[li].am = am2;
                    ctx.lane[li].m = am2->transformer;
                    ctx.lane[li].logits = (float *)malloc(
                        (size_t)am2->transformer->vocab_size *
                        sizeof *ctx.lane[li].logits);
                    if (!ctx.lane[li].logits) { fprintf(stderr, "oom\n"); return 1; }
                }
                printf("lane %lu: %s\n", (unsigned long)li, dn);
            }
            ctx.nlanes = lanes;
        }
        {
            /* per-lane sweep: every lane's argmax must match the CPU path
               (CPU reference computed BEFORE any handle is attached) */
            float *lg = (float *)malloc(
                (size_t)am->transformer->vocab_size * sizeof *lg);
            size_t cpu_am[16];
            size_t jj, li, mismatch = 0;
            if (!lg) return 1;
            for (jj = 0; jj < 16; ++jj) {
                int tk[3];
                int nt = 0;
                size_t a, am_c = 0;
                if (fs_bos(am->transformer) >= 0)
                    tk[nt++] = fs_bos(am->transformer);
                tk[nt++] = 2000 + (int)((jj * 37u) % 4096u);
                tk[nt++] = 2000 + (int)((jj * 91u + 17u) % 4096u);
                am->transformer->cur_pos = 0;
                cce_gguf_qwen2_forward(am->transformer, tk, nt,
                                       ctx.lane[0].logits,
                                       am->transformer->vocab_size);
                for (a = 0; a < (size_t)am->transformer->vocab_size; ++a) {
                    if (ctx.lane[0].logits[a] != ctx.lane[0].logits[a]) {
                        fprintf(stderr, "NaN logit in CPU reference — "
                                        "forward is broken; refusing to "
                                        "mine\n");
                        return 1;
                    }
                    if (ctx.lane[0].logits[a] > ctx.lane[0].logits[am_c])
                        am_c = a;
                }
                cpu_am[jj] = am_c;
            }
            /* attach handles (pool: per instance; single: process-global) */
            if (ctx.nlanes > 1) {
                for (li = 0; li < ctx.nlanes; ++li)
                    cce_gguf_qwen2_set_clgemm(ctx.lane[li].m,
                                              ctx.lane[li].gpu);
            } else {
                cce_gguf_set_clgemm(gpu);
            }
            for (li = 0; li < ctx.nlanes && !mismatch; ++li) {
                for (jj = 0; jj < 16 && !mismatch; ++jj) {
                    int tk[3];
                    int nt = 0;
                    size_t a, am_g = 0;
                    if (fs_bos(ctx.lane[li].m) >= 0)
                        tk[nt++] = fs_bos(ctx.lane[li].m);
                    tk[nt++] = 2000 + (int)((jj * 37u) % 4096u);
                    tk[nt++] = 2000 + (int)((jj * 91u + 17u) % 4096u);
                    ctx.lane[li].m->cur_pos = 0;
                    cce_gguf_qwen2_forward(ctx.lane[li].m, tk, nt, lg,
                                           ctx.lane[li].m->vocab_size);
                    for (a = 0; a < (size_t)ctx.lane[li].m->vocab_size; ++a) {
                        if (lg[a] != lg[a]) {
                            fprintf(stderr, "NaN logit on lane %lu — "
                                            "forward is broken; refusing "
                                            "to mine\n", (unsigned long)li);
                            return 1;
                        }
                        if (lg[a] > lg[am_g]) am_g = a;
                    }
                    if (am_g != cpu_am[jj]) mismatch = 1;
                }
            }
            free(lg);
            if (mismatch) {
                fprintf(stderr, "GPU EQUIVALENCE FAILED — refusing to mine "
                                "with --gpu (run gpu_equiv for details)\n");
                return 1;
            }
        }
#ifdef _OPENMP
        if (ctx.nlanes > 1) {
            /* nested teams: `lanes` outer oracle threads x `inner` CPU tile
               threads per forward — sized to the machine, not stacked */
            omp_set_max_active_levels(2);
            ctx.inner_threads = omp_get_max_threads() / (int)ctx.nlanes;
            if (ctx.inner_threads < 1) ctx.inner_threads = 1;
        }
#endif
        if (ctx.nlanes > 1)
            printf("gpu: %s — oracle pool of %lu lanes (equivalence sweep OK "
                   "on every lane, inner CPU teams %d)\n",
                   dev, (unsigned long)ctx.nlanes, ctx.inner_threads);
        else
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

    /* Restrict every lane's head to the mined window now that the full-head
       startup gates (equivalence sweep, determinism check) have passed:
       mining forwards then skip the [D x 262144] head GEMM and compute only
       the |V| window logits the oracle reads — bit-identical, ~1/4 off each
       forward. CNET_HEAD_WINDOW=0 keeps the full head. */
    if (!(getenv("CNET_HEAD_WINDOW") && getenv("CNET_HEAD_WINDOW")[0] == '0')) {
        size_t li;
        for (li = 0; li < ctx.nlanes; ++li)
            cce_gguf_qwen2_set_head_window(ctx.lane[li].m, vocab, (int)V);
        printf("head window: restricted to %lu mined tokens (full-vocab head "
               "off for mining)\n", (unsigned long)V);
    }

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

    {
        size_t li;
        for (li = 1; li < ctx.nlanes; ++li) {
            free(ctx.lane[li].logits);
            if (ctx.lane[li].am) cce_anymodel_free(ctx.lane[li].am);
        }
        for (li = 0; li < ctx.nlanes; ++li)
            if (ctx.lane[li].gpu) cce_clgemm_close(ctx.lane[li].gpu);
    }
    free(ctx.lane[0].logits);
    free(vocab);
    cce_anymodel_free(am);
    return 0;
}
