/* gap_lane_run — the 24/7 gap-lane daemon.
 *
 *   ./bin/gap_lane_run <base.cnb> <ledger.txt> [model.gguf]
 *
 * Detect -> acquire -> persist forever: ingests the serving gap inbox,
 * runs the health pass, bridges unhealed demotions, and drains open gaps
 * by teaching from the local model. Without a model it is the maintenance
 * lane (detect + heal + checkpoint); gaps then wait for a teacher.
 *
 * Model teaching semantics (matches the flagship mining convention): for a
 * gap shaped ONEHOT[W] -> ONEHOT[W] x k, the input one-hot selects token
 * (TOKEN_BASE + i) as the bare context; the teacher returns the model's
 * ordered top-k next tokens WITHIN the window as k one-hot fields, and
 * abstains when the k/k+1 logit margin is under MARGIN_EPS (the model's
 * own near-ties are declined, never guessed). Oracle entries are rebuilt
 * per tick for up to ACQUIRE_MAX_ORACLES open gaps — NO_PLAN gaps carry
 * their ports; rebuild gaps take the incumbent's real ports.
 *
 * Environment (all optional):
 *   CNET_GAP_INBOX            inbox path       (default <base>.inbox)
 *   CNET_LANE_INTERVAL_SEC    tick interval    (default 60)
 *   CNET_LANE_LOW_REL         reliability floor gap trigger (default 0=off)
 *   CNET_LANE_LOW_REL_MIN_EV  evidence needed for the floor (default 32)
 *   CNET_WINDOW_FILE          corpus-drawn window: token ids, one per line
 *                             (flagship's convention, e.g.
 *                             english_window_256.txt); input one-hot i maps
 *                             to ids[i] and outputs range over the same ids.
 *                             Unset = contiguous [TOKEN_BASE, TOKEN_BASE+W)
 *   CNET_LANE_CONTEXT_FILE    default corpus-drawn teaching context: token
 *                             ids of a real prefix, pinned in the KV; every
 *                             teaching call is then an independent probe of
 *                             prefix+token (cce_gguf_qwen2_forward_probes,
 *                             KV never modified). Unset = bare context.
 *                             Requires CNET_MAX_CTX > prefix length.
 *   CNET_LANE_CONTEXT_DIR     multiple pinned contexts: every <name>.ids in
 *                             the directory is a context; a gap whose goal
 *                             tag starts with "<name>_" is taught under it
 *                             (longest name wins), all others under the
 *                             default. One KV holds one prefix, so the
 *                             teacher re-pins on context switch — the drain
 *                             works gap-by-gap, so at most one re-pin per
 *                             gap. Each context carries its own provenance
 *                             fingerprint into the ledger.
 *   CNET_LANE_TOKEN_BASE      window base token id (default 0)
 *   CNET_LANE_MARGIN_EPS      teacher abstention margin (default 1e-4)
 *   CNET_ACQ_HIDDEN / CNET_ACQ_MAXHIDDEN / CNET_ACQ_EPOCHS
 *                             student structure budget (dynamic growth)
 *   CNET_ACQ_TARGET_LOSS / CNET_ACQ_MIN_IMPROVEMENT
 *                             training exactness bar: certification demands
 *                             per-point exactness, and a near-constant map
 *                             plateaus at the AVERAGE-loss default with one
 *                             stubborn point wrong (255/256) — 1e-7 trains
 *                             through it (measured: 41 s, margin 0.9881)
 * Stop file: <base>.stop (same convention as the flagship harness).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <dirent.h>

#include "../include/gap_lane.h"
#include "../include/nn.h"
#include "../include/router.h"
#include "../include/cce/cce_detect.h"
#include "../include/cce/cce_gguf.h"

#define LM_MAX_K 8

typedef struct {
    cce_gguf_qwen2 *m;
    float *logits;
    int vocab;
    const int *ids;  /* window token ids, ids[0..width) */
    int width;       /* W */
    int k;           /* ordered top-k fields */
    double eps;      /* abstention margin */
    const void *ctx_sel;              /* selected LmContext for this gap */
} LmTask;

#define LM_WINDOW_MAX 4096
#define LM_CTX_MAX 512
static int lm_window[LM_WINDOW_MAX];
static int lm_window_n;       /* 0 = synth from TOKEN_BASE per gap width */
static int lm_synth[LM_WINDOW_MAX];  /* TOKEN_BASE fallback alphabet */

/* Batched-teach cache: a gap's mining probes the SAME window (all tokens) under
   one pinned context, so instead of one GEMV forward per mined point we probe
   the whole window once as wide GEMMs (streaming the 12B weights once, not W
   times) and serve every point from the cache. Cached window-logits are
   bit-identical to serial probes (cce_gguf_qwen2_forward_probes guarantees
   per-row identity), so the mined table and every certification digest are
   unchanged — a startup equivalence gate proves it. Only the corpus-window
   path caches (all tasks share lm_window, stable); synth falls back to serial. */
static float *lm_probe_buf;   /* LM_PROBE_CHUNK x vocab, reused per chunk */
static float *lm_cache;       /* window_n x window_n window-logits */
static float lm_scratch[LM_WINDOW_MAX];  /* synth-path window logits */
static const void *lm_cache_ctx = (const void *)-1;  /* which context filled */
static int lm_cache_w;
static int lm_probe_chunk = 32;  /* CNET_LANE_PROBE_BATCH; probes per forward */

/* Named teaching contexts. One KV cache holds one pinned prefix, so
   lm_pinned tracks the occupant and lm_teach re-pins on switch. fp is the
   "c<fnv8>" provenance over window+context ids, carried into the ledger's
   permanent oracle column — WHICH corpus taught a gap is provenance. */
#define LM_CTX_SLOTS 16
typedef struct {
    char name[32];            /* "" for the default context */
    int ids[LM_CTX_MAX];
    int n;                    /* 0 = bare context */
    char fp[16];
    unsigned long long fp64;  /* context ids only: the retrieval snapshot */
} LmContext;
static unsigned long long lm_window_fp64;  /* window ids only: the config */
static unsigned long long lm_model_fp64;   /* model artifact BYTES */
static unsigned long long lm_toolchain_fp64; /* the teaching stack's build */
static LmContext lm_contexts[LM_CTX_SLOTS];
static int lm_context_count;
static LmContext lm_default_ctx;
static const LmContext *lm_pinned;

static unsigned long long lm_fnv_port(unsigned long long h, Port p) {
    const char *t = p.tag;
    h ^= (unsigned long long)p.family; h *= 1099511628211ULL;
    h ^= (unsigned long long)p.field_width; h *= 1099511628211ULL;
    h ^= (unsigned long long)p.field_count; h *= 1099511628211ULL;
    while (*t) { h ^= (unsigned char)*t++; h *= 1099511628211ULL; }
    return h;
}

static unsigned long long lm_fnv_ids(const int *ids, int n,
                                     unsigned long long salt) {
    unsigned long long h = 1469598103934665603ULL ^ salt;
    int i;
    for (i = 0; i < n; i++) {
        h ^= (unsigned long long)ids[i];
        h *= 1099511628211ULL;
    }
    return h;
}

/* Toolchain identity: what the build can attest about the stack that
   computes the teaching forwards (cce is compiled into this binary by the
   same invocation): compiler version, oracle ABI, pointer width, PLUS the
   build FLAGS and SOURCE REVISION injected by the Makefile
   (-DCNET_TOOLCHAIN_CFLAGS / -DCNET_SOURCE_REV; "unattested" when built
   outside it — visibly weaker identity, not silently equal). Still not a
   full hermetic transcript: linked system libraries and any GPU kernels
   are not captured. */
#ifndef CNET_TOOLCHAIN_CFLAGS
#define CNET_TOOLCHAIN_CFLAGS "unattested"
#endif
#ifndef CNET_SOURCE_REV
#define CNET_SOURCE_REV "unattested"
#endif
static unsigned long long lm_toolchain_identity(void) {
    unsigned long long h = 1469598103934665603ULL;
    const char *v =
#if defined(_MSC_VER)
        "msc";
#elif defined(__VERSION__)
        __VERSION__;
#else
        "unknown_compiler";
#endif
    const char *flags = CNET_TOOLCHAIN_CFLAGS;
    const char *rev = CNET_SOURCE_REV;
    while (*v) { h ^= (unsigned char)*v++; h *= 1099511628211ULL; }
#if defined(_MSC_VER)
    h ^= (unsigned long long)_MSC_FULL_VER; h *= 1099511628211ULL;
#endif
    h ^= 0xffULL; h *= 1099511628211ULL;   /* field separator */
    while (*flags) { h ^= (unsigned char)*flags++; h *= 1099511628211ULL; }
    h ^= 0xffULL; h *= 1099511628211ULL;
    while (*rev) { h ^= (unsigned char)*rev++; h *= 1099511628211ULL; }
    h ^= (unsigned long long)CNET_ORACLE_ABI_VERSION; h *= 1099511628211ULL;
    h ^= (unsigned long long)sizeof(void *); h *= 1099511628211ULL;
    return h;
}

static void lm_fingerprint(LmContext *c) {
    unsigned long long h = lm_fnv_ids(lm_window, lm_window_n, 0);
    int i;
    for (i = 0; i < c->n; i++) {
        h ^= (unsigned long long)c->ids[i] ^ 0x8000000000ULL;
        h *= 1099511628211ULL;
    }
    c->fp64 = c->n ? lm_fnv_ids(c->ids, c->n, 0x8000000000ULL) : 0;
    if (lm_window_n || c->n)
        snprintf(c->fp, sizeof c->fp, "c%08x", (unsigned)(h ^ (h >> 32)));
    else
        c->fp[0] = '\0';
}

/* tag-prefix convention: goal tag "<name>_..." selects context <name>.ids;
   the longest matching name wins; no match = the default context */
static const LmContext *lm_select_context(const char *tag) {
    const LmContext *best = &lm_default_ctx;
    size_t best_len = 0;
    int i;
    for (i = 0; i < lm_context_count; i++) {
        size_t n = strlen(lm_contexts[i].name);
        if (n > best_len && strncmp(tag, lm_contexts[i].name, n) == 0 &&
            tag[n] == '_') {
            best = &lm_contexts[i];
            best_len = n;
        }
    }
    return best;
}

static LmTask lm_tasks[ACQUIRE_MAX_ORACLES];
static size_t lm_task_count;

/* Pin this gap's context in the KV if another occupies it (one cache, one
   prefix). A context switch invalidates the batched-teach cache. Returns 0
   or -1 (prefix forward failed). */
static int lm_pin(LmTask *t, const LmContext *want) {
    if (lm_pinned == want) return 0;
    /* Invalidate before touching the shared KV. A failed prefix forward may
       have partially mutated it; the same context must retry rather than be
       mistaken for a valid pinned prefix. */
    lm_pinned = NULL;
    lm_cache_ctx = (const void *)-1;   /* KV changed: cached logits are stale */
    t->m->cur_pos = 0;
    if (want && want->n > 0 &&
        cce_gguf_qwen2_forward(t->m, want->ids, want->n, t->logits,
                               t->vocab) != CCE_OK)
        return -1;
    lm_pinned = want;
    return 0;
}

/* Fill lm_cache with every window token's window-restricted logits under the
   currently pinned context, probing lm_probe_chunk tokens per wide forward
   (GEMV -> GEMM: the model weights stream once per chunk, not once per token).
   Bit-identical per row to serial probes. Returns 0 or -1. */
static int lm_fill_cache(LmTask *t) {
    int base, b, j, w = t->width, vocab = t->vocab;
    if (!lm_cache || !lm_probe_buf) return -1;
    for (base = 0; base < w; base += lm_probe_chunk) {
        int cnt = (w - base < lm_probe_chunk) ? (w - base) : lm_probe_chunk;
        if (cce_gguf_qwen2_forward_probes(t->m, &t->ids[base], cnt,
                                          lm_probe_buf, vocab) != CCE_OK)
            return -1;
        for (b = 0; b < cnt; b++)
            for (j = 0; j < w; j++)
                lm_cache[(size_t)(base + b) * w + j] =
                    lm_probe_buf[(size_t)b * vocab + t->ids[j]];
    }
    return 0;
}

/* ordered top-k within the window from window-logits wl[0..width); margin-aware
   abstention on the teacher's own near-tie at the k/k+1 boundary. Writes k
   one-hot output fields, or returns 1 to abstain. */
static int lm_topk(LmTask *t, const float *wl, double *out) {
    int used[LM_MAX_K], picked[LM_MAX_K];
    float kth = 0.0f, next_best = -1e30f;
    int r, j, i;
    for (r = 0; r < t->k; r++) {
        int best = -1;
        for (j = 0; j < t->width; j++) {
            int taken = 0, u;
            for (u = 0; u < r; u++) if (used[u] == j) taken = 1;
            if (taken) continue;
            if (best < 0 || wl[j] > wl[best]) best = j;
        }
        used[r] = best;
        picked[r] = best;
        kth = wl[best];
    }
    for (j = 0; j < t->width; j++) {
        int taken = 0, u;
        for (u = 0; u < t->k; u++) if (used[u] == j) taken = 1;
        if (taken) continue;
        if (wl[j] > next_best) next_best = wl[j];
    }
    if (t->width > t->k && (double)(kth - next_best) < t->eps)
        return 1;  /* abstain: the model itself is undecided here */
    memset(out, 0, (size_t)t->width * (size_t)t->k * sizeof *out);
    for (i = 0; i < t->k; i++) out[(size_t)i * t->width + picked[i]] = 1.0;
    return 0;
}

static int lm_teach(const double *in, double *out, void *ctx) {
    LmTask *t = (LmTask *)ctx;
    const LmContext *want;
    const float *wl;
    int i, hot = 0, j;
    if (!t || !t->m) return -1;
    for (i = 1; i < t->width; i++) if (in[i] > in[hot]) hot = i;
    want = (const LmContext *)t->ctx_sel;
    if (lm_pin(t, want) != 0) return -1;
    /* Window logits for the hot token. Corpus-window mode (all tasks share the
       stable lm_window) serves from the batched cache, filled once per context;
       synth mode falls back to a serial probe. Either way `wl` is width floats. */
    if (lm_window_n > 0 && t->ids == lm_window && lm_cache) {
        if (lm_cache_ctx != want || lm_cache_w != t->width) {
            if (lm_fill_cache(t) != 0) return -1;
            lm_cache_ctx = want;
            lm_cache_w = t->width;
        }
        wl = &lm_cache[(size_t)hot * t->width];
    } else {
        int tok = t->ids[hot];
        if (cce_gguf_qwen2_forward_probes(t->m, &tok, 1, t->logits,
                                          t->vocab) != CCE_OK)
            return -1;
        for (j = 0; j < t->width; j++) lm_scratch[j] = t->logits[t->ids[j]];
        wl = lm_scratch;
    }
    return lm_topk(t, wl, out);
}

static int lm_shape_ok(Port in, Port goal, int vocab, int base) {
    if (!(in.family == PORT_ONEHOT && in.field_count == 1 &&
          goal.family == PORT_ONEHOT &&
          goal.field_width == in.field_width &&
          goal.field_count >= 1 && goal.field_count <= LM_MAX_K))
        return 0;
    if (lm_window_n > 0)
        return (int)in.field_width == lm_window_n;  /* corpus window mode */
    return in.field_width <= LM_WINDOW_MAX &&
           base + (int)in.field_width <= vocab;
}

/* Rebuild the oracle registry for this tick: one teacher per open gap
   whose shape the model can teach. */
static size_t bind_model_teachers(GapLane *L, cce_gguf_qwen2 *m,
                                  float *logits, int vocab, int base,
                                  double eps) {
    size_t g, bound = 0;
    memset(&L->oracles, 0, sizeof L->oracles);
    lm_task_count = 0;
    for (g = 0; g < L->ledger.count && bound < ACQUIRE_MAX_ORACLES; g++) {
        const GapRecord *gap = &L->ledger.gaps[g];
        Port in, goal;
        char name[ACQUIRE_NAME_MAX];
        LmTask *t;
        if (gap->status != GAP_OPEN) continue;
        if (gap->kind == GAP_NO_PLAN) {
            in = gap->input_port;
            goal = gap->goal_port;
        } else {
            /* rebuild: take the incumbent's real ports */
            size_t k;
            const RegistryEntry *e = NULL;
            for (k = 0; k < L->reg.count; ++k)
                if (L->reg.entries[k].name &&
                    strcmp(L->reg.entries[k].name, gap->subject) == 0)
                    e = &L->reg.entries[k];
            if (!e || !e->btn || e->btn->input_port_count != 1 ||
                e->btn->output_port_count != 1) continue;
            in = e->btn->input_ports[0];
            goal = e->btn->output_ports[0];
        }
        if (!lm_shape_ok(in, goal, vocab, base)) continue;
        {
            const LmContext *sel =
                lm_select_context(goal.tag[0] ? goal.tag : "");
            if (sel->fp[0])
                snprintf(name, sizeof name, "lm_%s_%.44s", sel->fp,
                         goal.tag[0] ? goal.tag : "untagged");
            else
                snprintf(name, sizeof name, "lm_%.56s",
                         goal.tag[0] ? goal.tag : "untagged");
            t = &lm_tasks[lm_task_count];
            t->ctx_sel = sel;
        }
        t->m = m; t->logits = logits; t->vocab = vocab;
        if (lm_window_n > 0) {
            t->ids = lm_window;
        } else {
            int w;
            for (w = 0; w < (int)in.field_width; w++)
                lm_synth[w] = base + w;
            t->ids = lm_synth;
        }
        t->width = (int)in.field_width;
        t->k = (int)goal.field_count;
        t->eps = eps;
        if (acquire_oracle_register(&L->oracles, name, in, goal,
                                    lm_teach, t) == 0) {
            /* the teacher's identity IS the unit's provenance: artifact =
               the model, config = the window, retrieval snapshot = the
               pinned teaching context. gap_lane_drain persists it as a
               base descriptor when this teacher closes a gap. */
            OracleEntry *oe = &L->oracles.entries[L->oracles.count - 1];
            const LmContext *sel = (const LmContext *)t->ctx_sel;
            memset(&oe->identity, 0, sizeof oe->identity);
            oe->identity.abi_version = CNET_ORACLE_ABI_VERSION;
            oe->identity.struct_size = (uint32_t)sizeof oe->identity;
            oe->identity.artifact_digest = lm_model_fp64;
            oe->identity.contract_digest =
                lm_fnv_port(lm_fnv_port(1469598103934665603ULL, in), goal);
            oe->identity.config_digest = lm_window_fp64;
            oe->identity.retrieval_snapshot_digest = sel ? sel->fp64 : 0;
            oe->identity.toolchain_digest = lm_toolchain_fp64;
            oe->behavior_digest = cnet_oracle_identity_digest(&oe->identity);
            lm_task_count++;
            bound++;
        }
    }
    return bound;
}

static long env_long(const char *name, long dflt) {
    const char *v = getenv(name);
    return (v && v[0]) ? atol(v) : dflt;
}

static double env_double(const char *name, double dflt) {
    const char *v = getenv(name);
    return (v && v[0]) ? atof(v) : dflt;
}

static int stop_requested(const char *stop_path) {
    FILE *f = fopen(stop_path, "r");
    if (!f) return 0;
    fclose(f);
    return 1;
}

int main(int argc, char **argv) {
    GapLane lane;
    GapLaneTickReport r;
    cce_anymodel *am = NULL;
    cce_gguf_qwen2 *model = NULL;
    float *logits = NULL;
    int vocab = 0;
    char inbox_default[560], stop_path[560];
    const char *inbox;
    long interval, token_base;
    double eps;
    unsigned long tick_no = 0;

    if (argc < 3) {
        fprintf(stderr,
                "usage: %s <base.cnb> <ledger.txt> [model.gguf]\n", argv[0]);
        return 2;
    }
    snprintf(inbox_default, sizeof inbox_default, "%s.inbox", argv[1]);
    snprintf(stop_path, sizeof stop_path, "%s.stop", argv[1]);
    inbox = getenv("CNET_GAP_INBOX");
    if (!inbox || !inbox[0]) inbox = inbox_default;
    interval = env_long("CNET_LANE_INTERVAL_SEC", 60);
    if (interval < 1) interval = 1;
    token_base = env_long("CNET_LANE_TOKEN_BASE", 0);
    eps = env_double("CNET_LANE_MARGIN_EPS", 1e-4);

    if (gap_lane_open(&lane, argv[1], argv[2], inbox) != 0) {
        fprintf(stderr, "gap_lane_run: cannot open lane (%s, %s)\n",
                argv[1], argv[2]);
        return 1;
    }
    lane.low_rel_floor = env_double("CNET_LANE_LOW_REL", 0.0);
    lane.low_rel_min_evidence =
        (size_t)env_long("CNET_LANE_LOW_REL_MIN_EV", 32);
    lane.acq.init_hidden = (size_t)env_long("CNET_ACQ_HIDDEN",
                                            (long)lane.acq.init_hidden);
    lane.acq.max_hidden = (size_t)env_long("CNET_ACQ_MAXHIDDEN",
                                           (long)lane.acq.max_hidden);
    lane.acq.max_epochs = (size_t)env_long("CNET_ACQ_EPOCHS",
                                           (long)lane.acq.max_epochs);
    lane.acq.target_loss = env_double("CNET_ACQ_TARGET_LOSS",
                                      lane.acq.target_loss);
    lane.acq.min_improvement = env_double("CNET_ACQ_MIN_IMPROVEMENT",
                                          lane.acq.min_improvement);

    if (argc >= 4) {
        if (cce_anymodel_open(&am, argv[3]) != CCE_OK || !am->transformer) {
            fprintf(stderr, "gap_lane_run: cannot open model %s\n", argv[3]);
            gap_lane_close(&lane);
            return 1;
        }
        model = am->transformer;
        vocab = model->vocab_size;
        logits = (float *)malloc((size_t)vocab * sizeof *logits);
        if (!logits) { gap_lane_close(&lane); return 1; }

        /* corpus-drawn window: one-hot index -> real corpus token id */
        {
            const char *wf = getenv("CNET_WINDOW_FILE");
            if (wf && wf[0]) {
                lm_window_n = gap_lane_load_ids(wf, lm_window, LM_WINDOW_MAX);
                if (lm_window_n <= 0) {
                    fprintf(stderr, "gap_lane_run: bad window file %s\n", wf);
                    gap_lane_close(&lane);
                    return 1;
                }
                /* head restricted to the window ids: bit-identical logits on
                   exactly the ids the teacher reads, at a fraction of the
                   head GEMM (lm_window is static — outlives the model) */
                cce_gguf_qwen2_set_head_window(model, lm_window, lm_window_n);
                printf("gap_lane_run: window %s (%d ids, first %d)\n",
                       wf, lm_window_n, lm_window[0]);
                /* batched-teach buffers: probe lm_probe_chunk window tokens per
                   wide forward and cache the whole window's logits per context */
                lm_probe_chunk = (int)env_long("CNET_LANE_PROBE_BATCH", 32);
                if (lm_probe_chunk < 1) lm_probe_chunk = 1;
                if (lm_probe_chunk > lm_window_n) lm_probe_chunk = lm_window_n;
                lm_probe_buf = (float *)malloc((size_t)lm_probe_chunk *
                                               (size_t)vocab * sizeof *lm_probe_buf);
                lm_cache = (float *)malloc((size_t)lm_window_n *
                                           (size_t)lm_window_n * sizeof *lm_cache);
                if (!lm_probe_buf || !lm_cache) {
                    free(lm_probe_buf); free(lm_cache);
                    lm_probe_buf = NULL; lm_cache = NULL;  /* serial fallback */
                    fprintf(stderr, "gap_lane_run: batched-teach cache "
                            "unavailable (OOM); serial probing\n");
                } else {
                    printf("gap_lane_run: batched teach on (chunk=%d, "
                           "cache=%.1f MiB)\n", lm_probe_chunk,
                           (double)lm_window_n * lm_window_n *
                           sizeof *lm_cache / 1048576.0);
                }
            }
        }
        /* default corpus-drawn teaching context (selected when no named
           context matches the goal tag); pinning happens lazily per gap */
        {
            const char *cf = getenv("CNET_LANE_CONTEXT_FILE");
            if (cf && cf[0]) {
                lm_default_ctx.n = gap_lane_load_ids(cf, lm_default_ctx.ids,
                                                     LM_CTX_MAX);
                if (lm_default_ctx.n <= 0 ||
                    lm_default_ctx.n >= model->max_ctx) {
                    fprintf(stderr,
                            "gap_lane_run: bad context file %s (n=%d, "
                            "max_ctx=%d)\n", cf, lm_default_ctx.n,
                            model->max_ctx);
                    gap_lane_close(&lane);
                    return 1;
                }
                printf("gap_lane_run: default context %s (%d tokens)\n",
                       cf, lm_default_ctx.n);
            }
        }
        /* named contexts: <name>.ids, selected by goal-tag prefix */
        {
            const char *cd = getenv("CNET_LANE_CONTEXT_DIR");
            if (cd && cd[0]) {
                DIR *d = opendir(cd);
                struct dirent *de;
                if (!d) {
                    fprintf(stderr, "gap_lane_run: bad context dir %s\n", cd);
                    gap_lane_close(&lane);
                    return 1;
                }
                while ((de = readdir(d)) != NULL) {
                    size_t nl = strlen(de->d_name);
                    char path[1024];
                    LmContext *c;
                    if (nl <= 4 ||
                        strcmp(de->d_name + nl - 4, ".ids") != 0)
                        continue;
                    if (nl - 4 >= sizeof lm_contexts[0].name ||
                        nl - 4 > PORT_TAG_MAX - 2) {
                        fprintf(stderr,
                                "gap_lane_run: context name cannot fit a "
                                "goal-tag prefix: %s\n", de->d_name);
                        closedir(d);
                        gap_lane_close(&lane);
                        return 1;
                    }
                    if (lm_context_count >= LM_CTX_SLOTS) {
                        fprintf(stderr,
                                "gap_lane_run: too many named contexts in %s "
                                "(max=%d)\n", cd, LM_CTX_SLOTS);
                        closedir(d);
                        gap_lane_close(&lane);
                        return 1;
                    }
                    c = &lm_contexts[lm_context_count];
                    memcpy(c->name, de->d_name, nl - 4);
                    c->name[nl - 4] = '\0';
                    snprintf(path, sizeof path, "%s/%s", cd, de->d_name);
                    c->n = gap_lane_load_ids(path, c->ids, LM_CTX_MAX);
                    if (c->n <= 0 || c->n >= model->max_ctx) {
                        fprintf(stderr,
                                "gap_lane_run: bad context %s (n=%d)\n",
                                path, c->n);
                        closedir(d);
                        gap_lane_close(&lane);
                        return 1;
                    }
                    lm_context_count++;
                }
                closedir(d);
            }
        }
        lm_window_fp64 = lm_window_n ? lm_fnv_ids(lm_window, lm_window_n, 0)
                                     : 0;
        /* artifact identity = the model file's BYTES (streamed FNV-1a; a
           multi-GB GGUF hashes once here at startup). An unhashable model
           refuses to teach: identity may never be guessed. */
        if (gap_lane_digest_file(argv[3], &lm_model_fp64) != 0) {
            fprintf(stderr, "gap_lane_run: cannot digest model bytes %s\n",
                    argv[3]);
            gap_lane_close(&lane);
            return 1;
        }
        lm_toolchain_fp64 = lm_toolchain_identity();
        lm_fingerprint(&lm_default_ctx);
        {
            int ci;
            for (ci = 0; ci < lm_context_count; ci++) {
                lm_fingerprint(&lm_contexts[ci]);
                printf("gap_lane_run: context '%s' (%d tokens, %s)\n",
                       lm_contexts[ci].name, lm_contexts[ci].n,
                       lm_contexts[ci].fp);
            }
        }
        printf("gap_lane_run: teacher %s (vocab %d, base %ld, "
               "artifact %016llx, toolchain %016llx%s%s)\n",
               argv[3], vocab, token_base, lm_model_fp64, lm_toolchain_fp64,
               lm_default_ctx.fp[0] ? ", default provenance " : "",
               lm_default_ctx.fp);
        /* Equivalence gate: the batched cache is only trusted if a wide probe
           is BIT-IDENTICAL to serial probes on the same tokens (the guarantee
           the cache rests on). Cheap (a handful of tokens); disable on mismatch
           rather than teach from divergent logits. */
        if (lm_cache && lm_window_n >= 1) {
            LmTask tt;
            const LmContext *c0 = lm_default_ctx.n ? &lm_default_ctx : NULL;
            int nb = lm_window_n < 4 ? lm_window_n : 4, b, j, ok = 1;
            if (nb > lm_probe_chunk) nb = lm_probe_chunk;  /* fits lm_probe_buf */
            tt.m = model; tt.logits = logits; tt.vocab = vocab;
            tt.ids = lm_window; tt.width = lm_window_n; tt.k = 1; tt.eps = eps;
            tt.ctx_sel = c0;
            lm_pinned = (const LmContext *)-1;      /* force a clean pin */
            if (lm_pin(&tt, c0) != 0 ||
                cce_gguf_qwen2_forward_probes(model, lm_window, nb,
                                              lm_probe_buf, vocab) != CCE_OK)
                ok = 0;
            for (b = 0; ok && b < nb; b++) {
                int tok = lm_window[b];
                if (cce_gguf_qwen2_forward_probes(model, &tok, 1, logits,
                                                  vocab) != CCE_OK) { ok = 0; break; }
                for (j = 0; j < lm_window_n; j++)
                    if (lm_probe_buf[(size_t)b * vocab + lm_window[j]] !=
                        logits[lm_window[j]]) { ok = 0; break; }
            }
            if (!ok) {
                fprintf(stderr, "gap_lane_run: batched-teach equivalence "
                        "FAILED — disabling cache, serial probing\n");
                free(lm_cache); lm_cache = NULL;
            } else {
                printf("gap_lane_run: BATCHED_TEACH_EQUIV_OK (%d tokens x "
                       "%d window ids, bit-exact)\n", nb, lm_window_n);
            }
            lm_pinned = (const LmContext *)-1;      /* real teaching re-pins */
            lm_cache_ctx = (const void *)-1;
            fflush(stdout);
        }
    } else {
        printf("gap_lane_run: maintenance mode (no teacher bound)\n");
    }
    printf("gap_lane_run: base=%s ledger=%s inbox=%s interval=%lds "
           "units=%zu\n",
           argv[1], argv[2], inbox, interval, lane.reg.count);
    fflush(stdout);

    for (;;) {
        long slept;
        if (stop_requested(stop_path)) {
            printf("gap_lane_run: stop file honored\n");
            break;
        }
        tick_no++;
        if (model)
            bind_model_teachers(&lane, model, logits, vocab,
                                (int)token_base, eps);
        if (gap_lane_tick(&lane, &r, 0) != 0) {
            fprintf(stderr, "gap_lane_run: tick failed; retrying\n");
        } else if (r.inbox_ingested || r.health_noted || r.low_rel_noted ||
                   r.healed || r.drain.examined || r.recipe_reopened ||
                   r.checkpointed) {
            printf("gap_lane_run: tick=%lu inbox=%zu health=%zu lowrel=%zu "
                   "healed=%zu reopened=%zu drained=%zu closed=%zu deferred=%zu "
                   "no_oracle=%zu units=%zu%s\n",
                   tick_no, r.inbox_ingested, r.health_noted,
                   r.low_rel_noted, r.healed, r.recipe_reopened,
                   r.drain.examined, r.drain.closed, r.drain.deferred,
                   r.drain.skipped_no_oracle, lane.reg.count,
                   r.checkpointed ? " [checkpoint]" : "");
            fflush(stdout);
        }
        for (slept = 0; slept < interval; slept++) {
            if (stop_requested(stop_path)) break;
            {
                struct timespec ts = {1, 0};
                nanosleep(&ts, NULL);
            }
        }
    }

    gap_lane_checkpoint(&lane);
    gap_lane_close(&lane);
    if (am) cce_anymodel_free(am);
    free(logits);
    free(lm_probe_buf);
    free(lm_cache);
    return 0;
}
