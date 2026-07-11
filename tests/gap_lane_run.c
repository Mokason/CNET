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
 *   CNET_LANE_TOKEN_BASE      window base token id (default 0)
 *   CNET_LANE_MARGIN_EPS      teacher abstention margin (default 1e-4)
 *   CNET_ACQ_HIDDEN / CNET_ACQ_MAXHIDDEN / CNET_ACQ_EPOCHS
 *                             student structure budget (dynamic growth)
 * Stop file: <base>.stop (same convention as the flagship harness).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
    int base;      /* window base token id */
    int width;     /* W */
    int k;         /* ordered top-k fields */
    double eps;    /* abstention margin */
} LmTask;

static LmTask lm_tasks[ACQUIRE_MAX_ORACLES];
static size_t lm_task_count;

static int lm_teach(const double *in, double *out, void *ctx) {
    LmTask *t = (LmTask *)ctx;
    int i, hot = 0, tok, picked[LM_MAX_K];
    if (!t || !t->m) return -1;
    for (i = 1; i < t->width; i++) if (in[i] > in[hot]) hot = i;
    tok = t->base + hot;
    if (cce_gguf_qwen2_forward(t->m, &tok, 1, t->logits, t->vocab) != CCE_OK)
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
                    t->logits[t->base + j] > t->logits[t->base + best])
                    best = j;
            }
            used[r] = best;
            picked[r] = best;
            kth = t->logits[t->base + best];
        }
        for (j = 0; j < t->width; j++) {
            int taken = 0, u;
            for (u = 0; u < t->k; u++) if (used[u] == j) taken = 1;
            if (taken) continue;
            if (t->logits[t->base + j] > next_best)
                next_best = t->logits[t->base + j];
        }
        if (t->width > t->k && (double)(kth - next_best) < t->eps)
            return 1;  /* abstain: the model itself is undecided here */
    }
    memset(out, 0, (size_t)t->width * (size_t)t->k * sizeof *out);
    for (i = 0; i < t->k; i++) out[(size_t)i * t->width + picked[i]] = 1.0;
    return 0;
}

static int lm_shape_ok(Port in, Port goal, int vocab, int base) {
    return in.family == PORT_ONEHOT && in.field_count == 1 &&
           goal.family == PORT_ONEHOT &&
           goal.field_width == in.field_width &&
           goal.field_count >= 1 && goal.field_count <= LM_MAX_K &&
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
        snprintf(name, sizeof name, "lm_%.56s",
                 goal.tag[0] ? goal.tag : "untagged");
        t = &lm_tasks[lm_task_count];
        t->m = m; t->logits = logits; t->vocab = vocab; t->base = base;
        t->width = (int)in.field_width;
        t->k = (int)goal.field_count;
        t->eps = eps;
        if (acquire_oracle_register(&L->oracles, name, in, goal,
                                    lm_teach, t) == 0) {
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
        printf("gap_lane_run: teacher %s (vocab %d, base %ld)\n",
               argv[3], vocab, token_base);
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
