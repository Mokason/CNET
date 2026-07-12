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
static unsigned long long lm_model_fp64;   /* model path: the artifact (v1) */
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

static int lm_teach(const double *in, double *out, void *ctx) {
    LmTask *t = (LmTask *)ctx;
    int i, hot = 0, tok, picked[LM_MAX_K];
    if (!t || !t->m) return -1;
    for (i = 1; i < t->width; i++) if (in[i] > in[hot]) hot = i;
    tok = t->ids[hot];
    /* pin this gap's context if another occupies the KV (one cache, one
       prefix; the drain works gap-by-gap so this is one forward per gap
       switch, not per point) */
    {
        const LmContext *want = (const LmContext *)t->ctx_sel;
        if (lm_pinned != want) {
            /* Invalidate before touching the shared KV. A failed prefix
               forward may have partially mutated it; the same context must
               retry rather than being mistaken for a valid pinned prefix. */
            lm_pinned = NULL;
            t->m->cur_pos = 0;
            if (want && want->n > 0 &&
                cce_gguf_qwen2_forward(t->m, want->ids, want->n, t->logits,
                                       t->vocab) != CCE_OK)
                return -1;
            lm_pinned = want;
        }
    }
    /* independent probe of prefix+token: the pinned prefix stays put
       (probes never modify the KV); with a bare context, cur_pos is 0 */
    if (cce_gguf_qwen2_forward_probes(t->m, &tok, 1, t->logits,
                                      t->vocab) != CCE_OK)
        return -1;
    /* ordered top-k within the window; margin-aware abstention on the
       teacher's own near-tie at the k/k+1 boundary */
    {
        int used[LM_MAX_K];
        float kth = 0.0f, next_best = -1e30f;
        int r, j;
        for (r = 0; r < t->k; r++) {
            int best = -1;
            for (j = 0; j < t->width; j++) {
                int taken = 0, u;
                for (u = 0; u < r; u++) if (used[u] == j) taken = 1;
                if (taken) continue;
                if (best < 0 ||
                    t->logits[t->ids[j]] > t->logits[t->ids[best]])
                    best = j;
            }
            used[r] = best;
            picked[r] = best;
            kth = t->logits[t->ids[best]];
        }
        for (j = 0; j < t->width; j++) {
            int taken = 0, u;
            for (u = 0; u < t->k; u++) if (used[u] == j) taken = 1;
            if (taken) continue;
            if (t->logits[t->ids[j]] > next_best)
                next_best = t->logits[t->ids[j]];
        }
        if (t->width > t->k && (double)(kth - next_best) < t->eps)
            return 1;  /* abstain: the model itself is undecided here */
    }
    memset(out, 0, (size_t)t->width * (size_t)t->k * sizeof *out);
    for (i = 0; i < t->k; i++) out[(size_t)i * t->width + picked[i]] = 1.0;
    return 0;
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
        {
            const char *mp = argv[3];
            unsigned long long h = 1469598103934665603ULL;
            while (*mp) { h ^= (unsigned char)*mp++; h *= 1099511628211ULL; }
            lm_model_fp64 = h;   /* path identity (v1) — honest, labeled */
        }
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
        printf("gap_lane_run: teacher %s (vocab %d, base %ld%s%s)\n",
               argv[3], vocab, token_base,
               lm_default_ctx.fp[0] ? ", default provenance " : "",
               lm_default_ctx.fp);
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
                   r.healed || r.drain.examined || r.checkpointed) {
            printf("gap_lane_run: tick=%lu inbox=%zu health=%zu lowrel=%zu "
                   "healed=%zu drained=%zu closed=%zu deferred=%zu "
                   "no_oracle=%zu units=%zu%s\n",
                   tick_no, r.inbox_ingested, r.health_noted,
                   r.low_rel_noted, r.healed, r.drain.examined,
                   r.drain.closed, r.drain.deferred,
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
    return 0;
}
