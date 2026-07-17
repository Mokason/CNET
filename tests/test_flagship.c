/* Flagship harness gate — synthetic deterministic "model", no CCE, no GPU.
   Proves: sweep -> acquire -> seal-into-base; crash-resume off the base;
   deferral tally; stop-file interruption; ledger persistence. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/flagship.h"

static int checks_run = 0;

static void check(int cond, const char *what) {
    ++checks_run;
    if (!cond) {
        printf("FAIL: %s\n", what);
        exit(1);
    }
    printf("  ok: %s\n", what);
}

#define V 16u   /* == acquire min_evidence default */

/* Synthetic model: next(w | t) = (3*w + t) mod V — a distinct permutation of
   the vocab per conditioning token (3 is coprime to 16). */
typedef struct { int t; unsigned v; int broken; } SynCtx;

static int syn_oracle(const double *in, double *out, void *ctx) {
    SynCtx *s = (SynCtx *)ctx;
    unsigned w = 0, i, nxt;
    if (s->broken) {   /* garbage: fails port_validate -> oracle_unfit */
        for (i = 0; i < s->v; ++i) out[i] = 0.5;
        return 0;
    }
    for (i = 0; i < s->v; ++i)
        if (in[i] > 0.5) w = i;
    nxt = (3u * w + (unsigned)s->t) % s->v;
    for (i = 0; i < s->v; ++i) out[i] = (i == nxt) ? 1.0 : 0.0;
    return 0;
}

typedef struct { SynCtx slot; int broken_token; } SynMaker;

static int syn_maker(void *maker_ctx, size_t k, int token_id,
                     FlagshipOracle *out) {
    SynMaker *m = (SynMaker *)maker_ctx;
    (void)k;
    m->slot.t = token_id;
    m->slot.v = V;
    m->slot.broken = (token_id == m->broken_token);
    out->fn = syn_oracle;
    out->ctx = &m->slot;
    return 0;
}

/* PAIR fixture: next(w_prev, w_cur | t) = (5*w_cur + t) mod V.
   Deliberately w_cur-only: the gate proves the SAMPLED-tier MECHANICS
   (stride mining, Wilson floor, conformal probe on unseen phases), not hard
   training — probed empirically, a full (wp,wc)-entangled lookup trains to
   ~94% exact and exactness-on-sample is the admission bar, so an entangled
   fixture just measures the trainer, which is the REAL run's job. */
typedef struct { int t; unsigned v; } PairCtx;

static int pair_oracle(const double *in, double *out, void *ctx) {
    PairCtx *s = (PairCtx *)ctx;
    unsigned wc = 0, i, nxt;
    for (i = 0; i < s->v; ++i)
        if (in[s->v + i] > 0.5) wc = i;
    nxt = (5u * wc + (unsigned)s->t) % s->v;
    for (i = 0; i < s->v; ++i) out[i] = (i == nxt) ? 1.0 : 0.0;
    return 0;
}

static int pair_maker(void *maker_ctx, size_t k, int token_id,
                      FlagshipOracle *out) {
    PairCtx *s = (PairCtx *)maker_ctx;
    (void)k;
    s->t = token_id;
    s->v = V;
    out->fn = pair_oracle;
    out->ctx = s;
    return 0;
}

/* TOPK fixture: three distinct deterministic ranks per (w, t). */
typedef struct { int t; unsigned v; } TopkCtx;

static void topk_ranks(unsigned w, unsigned t, unsigned v,
                       unsigned *r1, unsigned *r2, unsigned *r3) {
    *r1 = (w + t) % v;
    *r2 = (w + 2u * t + 1u) % v;
    while (*r2 == *r1) *r2 = (*r2 + 1u) % v;
    *r3 = (w + 3u * t + 2u) % v;
    while (*r3 == *r1 || *r3 == *r2) *r3 = (*r3 + 1u) % v;
}

static int topk_oracle(const double *in, double *out, void *ctx) {
    TopkCtx *s = (TopkCtx *)ctx;
    unsigned w = 0, i, r1, r2, r3;
    for (i = 0; i < s->v; ++i)
        if (in[i] > 0.5) w = i;
    topk_ranks(w, (unsigned)s->t, s->v, &r1, &r2, &r3);
    for (i = 0; i < 3u * s->v; ++i) out[i] = 0.0;
    out[r1] = 1.0;
    out[s->v + r2] = 1.0;
    out[2u * s->v + r3] = 1.0;
    return 0;
}

static int topk_maker(void *maker_ctx, size_t k, int token_id,
                      FlagshipOracle *out) {
    TopkCtx *s = (TopkCtx *)maker_ctx;
    (void)k;
    s->t = token_id;
    s->v = V;
    out->fn = topk_oracle;
    out->ctx = s;
    return 0;
}

/* Constant-slice fixture (the degenerate case measured on the real model):
   every context maps to the same token. Counts oracle calls so the gate can
   assert the pilot refused BEFORE the main mine spent the budget. */
typedef struct { unsigned v; size_t calls; } ConstCtx;

static int const_pair_oracle(const double *in, double *out, void *ctx) {
    ConstCtx *s = (ConstCtx *)ctx;
    unsigned i;
    (void)in;
    s->calls++;
    for (i = 0; i < s->v; ++i) out[i] = (i == 3u) ? 1.0 : 0.0;
    return 0;
}

static int const_maker(void *maker_ctx, size_t k, int token_id,
                       FlagshipOracle *out) {
    ConstCtx *s = (ConstCtx *)maker_ctx;
    (void)k; (void)token_id;
    s->v = V;
    out->fn = const_pair_oracle;
    out->ctx = s;
    return 0;
}

int main(void) {
    int vocab[V];
    unsigned i;
    FlagshipConfig cfg;
    FlagshipReport rep;
    SynMaker maker;
    const char *base_path = "flagship_test.cnb";
    const char *ledger_path = "flagship_test_gaps.txt";

    printf("== flagship: thermal-governed compounding harness ==\n");

    for (i = 0; i < V; ++i) vocab[i] = 100 + (int)i * 10;   /* spaced ids */
    remove(base_path);
    remove(ledger_path);

    printf("[1] first run: sweep, acquire, one deferral\n");
    {
        flagship_config_defaults(&cfg);
        cfg.vocab_tokens = vocab;
        cfg.vocab_size = V;
        cfg.max_units = 6;
        cfg.base_path = base_path;
        cfg.ledger_path = ledger_path;
        cfg.checkpoint_every = 2;
        cfg.gpu_temp_limit_c = 0;    /* no nvidia-smi in the gate */
        cfg.duty_fraction = 1.0;     /* no sleeping in the gate */

        memset(&maker, 0, sizeof maker);
        maker.broken_token = vocab[3];

        memset(&rep, 0, sizeof rep);
        check(flagship_run(&cfg, syn_maker, &maker, &rep) == 0, "run 1 runs");
        check(rep.attempted == 6 && rep.acquired == 5 && rep.deferred == 1,
              "5 acquired, 1 deferred out of 6");
        check(rep.reason_kinds == 1 &&
              strcmp(rep.reasons[0], "oracle_unfit") == 0 &&
              rep.reason_counts[0] == 1,
              "deferral tallied as oracle_unfit");
        check(rep.stopped == 0, "run 1 swept to completion");
        flagship_print_report(&rep, stdout);
    }

    printf("[2] resume: base is the checkpoint\n");
    {
        memset(&rep, 0, sizeof rep);
        check(flagship_run(&cfg, syn_maker, &maker, &rep) == 0, "run 2 runs");
        check(rep.skipped_resume == 5,
              "5 prior units skipped without retraining");
        check(rep.attempted == 1 && rep.deferred == 1,
              "only the DEFERRED gap is retried (reopened by re-note)");
        check(rep.registry_skipped == 0,
              "all base units re-certified on load (trust replayed)");
    }

    printf("[3] healed oracle: the deferred token acquires on retry\n");
    {
        maker.broken_token = -1;   /* healed */
        memset(&rep, 0, sizeof rep);
        check(flagship_run(&cfg, syn_maker, &maker, &rep) == 0, "run 3 runs");
        check(rep.attempted == 1 && rep.acquired == 1 && rep.deferred == 0,
              "previously deferred token now acquired");
    }

    printf("[4] the acquired knowledge is real: verify against the model\n");
    {
        CnetBase b;
        PrimitiveRegistry reg;
        size_t skipped = 99;
        RoutePlan plan;
        Port in_p, goal_p;
        char goal_tag[PORT_TAG_MAX];
        double in[V], out[V];
        unsigned w;

        cnb_init(&b);
        check(cnb_load(&b, base_path) == 0 && b.unit_count == 6,
              "base holds all 6 units");
        registry_init(&reg);
        check(cnb_load_registry(&b, &reg, &skipped) == 0 && skipped == 0,
              "all 6 certify on load");

        memset(&in_p, 0, sizeof in_p);
        in_p.family = PORT_ONEHOT;
        in_p.field_width = V;
        in_p.field_count = 1;
        check(port_set_tag(&in_p, "w_cur") == 0, "input port");
        goal_p = in_p;
        snprintf(goal_tag, sizeof goal_tag, "wa%dq%d", vocab[2], vocab[2]);
        check(port_set_tag(&goal_p, goal_tag) == 0, "goal port");

        reg.require_certified = 1;
        check(route_plan(&reg, in_p, goal_p, &plan) == 0 && plan.length == 1,
              "router plans the certified slice");
        plan.strict = 1;
        for (w = 0; w < V; ++w) {
            unsigned got, want = (3u * w + (unsigned)vocab[2]) % V;
            unsigned j;
            memset(in, 0, sizeof in);
            in[w] = 1.0;
            check(route_execute(&plan, in, V, out, V) == 0, "slice executes");
            got = 0;
            for (j = 0; j < V; ++j)
                if (out[j] > 0.5) got = j;
            check(got == want, "slice matches the model exactly");
        }
        registry_free(&reg);
        cnb_free(&b);
    }

    printf("[5] stop file: clean user interruption\n");
    {
        char stop_path[256];
        FILE *f;
        snprintf(stop_path, sizeof stop_path, "%s.stop", base_path);
        f = fopen(stop_path, "w");
        fclose(f);
        cfg.max_units = 0;   /* would sweep all 16 */
        memset(&rep, 0, sizeof rep);
        check(flagship_run(&cfg, syn_maker, &maker, &rep) == 0, "run 4 runs");
        check(rep.stopped == 2 && rep.attempted == 0,
              "stop file halts before any work");
        remove(stop_path);
    }

    remove(base_path);
    remove(ledger_path);

    printf("[6] PAIR task: the SAMPLED tier runs for real\n");
    printf("[7] conformal wrapper (report-only)\n");
    {
        FlagshipConfig fc;
        FlagshipReport rp;
        PairCtx pm;
        const char *bp = "flagship_pair_test.cnb";
        const char *lp = "flagship_pair_gaps.txt";
        remove(bp);
        remove(lp);

        flagship_config_defaults(&fc);
        fc.task = FLAGSHIP_TASK_PAIR;
        fc.vocab_tokens = vocab;
        fc.vocab_size = V;
        fc.max_units = 2;
        fc.base_path = bp;
        fc.ledger_path = lp;
        fc.gpu_temp_limit_c = 0;
        fc.duty_fraction = 1.0;
        fc.acq.mine_budget = 32;       /* < V^2 = 256 -> SAMPLED path */
        fc.acq.sample_count = 64;      /* Wilson(64 all-pass) ~ .9433 */
        fc.acq.min_accuracy_bound = 0.90; /* gate tests mechanics, not the
                                             production bar (which needs n>=73) */
        fc.acq.holdout_fraction = 0.0; /* gate determinism: train == contract */
        fc.acq.init_hidden = 16;
        fc.acq.max_hidden = 64;
        fc.acq.max_epochs = 8000;
        fc.conformal_alpha = 0.05;
        fc.conformal_n = 64;

        memset(&pm, 0, sizeof pm);
        memset(&rp, 0, sizeof rp);
        check(flagship_run(&fc, pair_maker, &pm, &rp) == 0, "pair run runs");
        flagship_print_report(&rp, stdout);
        check(rp.attempted == 2 && rp.acquired == 2 && rp.deferred == 0,
              "both pair units acquired");
        check(rp.proof_count == 0 && rp.sampled_count == 2,
              "V^2 domain under a small budget lands in the SAMPLED tier");
        check(rp.bound_count == 2 && rp.bounds[0] > 0.9 && rp.bounds[0] <= 1.0,
              "wilson floor recorded in (0.9, 1]");
        check(rp.conf_units == 2, "conformal probe ran per SAMPLED unit");
        check(rp.conf_answered + rp.conf_abstained > 0,
              "conformal test queries measured");
        check(rp.conf_wrong <= rp.conf_answered, "risk numerator sane");
        remove(bp);
        remove(lp);
    }

    printf("[8] TOPK ranked preference (the soul, PROOF path)\n");
    {
        FlagshipConfig fc;
        FlagshipReport rp;
        TopkCtx tm;
        const char *bp = "flagship_topk_test.cnb";
        const char *lp = "flagship_topk_gaps.txt";
        remove(bp);
        remove(lp);

        flagship_config_defaults(&fc);
        fc.task = FLAGSHIP_TASK_TOPK;
        fc.topk = 3;
        fc.vocab_tokens = vocab;
        fc.vocab_size = V;
        fc.max_units = 2;
        fc.base_path = bp;
        fc.ledger_path = lp;
        fc.gpu_temp_limit_c = 0;
        fc.duty_fraction = 1.0;

        memset(&tm, 0, sizeof tm);
        memset(&rp, 0, sizeof rp);
        check(flagship_run(&fc, topk_maker, &tm, &rp) == 0, "topk run runs");
        flagship_print_report(&rp, stdout);
        check(rp.attempted == 2 && rp.acquired == 2 && rp.proof_count == 2,
              "ranked-preference units PROOF-certified");
        check(rp.margin_count == 2 && rp.margins[0] > 0.0,
              "rank margins recorded");

        /* the extracted soul matches the synthetic model's ranking exactly */
        {
            CnetBase b;
            PrimitiveRegistry reg;
            size_t skipped = 99;
            RoutePlan plan;
            Port in_p, goal_p;
            char goal_tag[PORT_TAG_MAX];
            double in[V], out[3 * V];
            unsigned w = 5, r1, r2, r3, j, g1 = 0, g2 = 0, g3 = 0;

            cnb_init(&b);
            check(cnb_load(&b, bp) == 0 && b.unit_count == 2,
                  "topk base holds both units");
            registry_init(&reg);
            check(cnb_load_registry(&b, &reg, &skipped) == 0 && skipped == 0,
                  "both certify on load");
            memset(&in_p, 0, sizeof in_p);
            in_p.family = PORT_ONEHOT;
            in_p.field_width = V;
            in_p.field_count = 1;
            check(port_set_tag(&in_p, "w_cur") == 0, "input port");
            goal_p = in_p;
            goal_p.field_count = 3;
            snprintf(goal_tag, sizeof goal_tag, "tk%dq%d", vocab[0], vocab[0]);
            check(port_set_tag(&goal_p, goal_tag) == 0, "goal port");
            reg.require_certified = 1;
            check(route_plan(&reg, in_p, goal_p, &plan) == 0 &&
                  plan.length == 1,
                  "router plans the soul unit");
            plan.strict = 1;
            memset(in, 0, sizeof in);
            in[w] = 1.0;
            check(route_execute(&plan, in, V, out, 3 * V) == 0,
                  "soul unit executes");
            topk_ranks(w, (unsigned)vocab[0], V, &r1, &r2, &r3);
            for (j = 0; j < V; ++j) {
                if (out[j] > 0.5) g1 = j;
                if (out[V + j] > 0.5) g2 = j;
                if (out[2 * V + j] > 0.5) g3 = j;
            }
            check(g1 == r1 && g2 == r2 && g3 == r3,
                  "extracted ranking matches the model's preference order");
            registry_free(&reg);
            cnb_free(&b);
        }
        remove(bp);
        remove(lp);
    }

    printf("[9] TOPK capacity-starved: the honest refusal\n");
    {
        FlagshipConfig fc;
        FlagshipReport rp;
        TopkCtx tm;
        CnetBase b;
        const char *bp = "flagship_starve_test.cnb";
        const char *lp = "flagship_starve_gaps.txt";
        remove(bp);
        remove(lp);

        flagship_config_defaults(&fc);
        fc.task = FLAGSHIP_TASK_TOPK;
        fc.topk = 3;
        fc.vocab_tokens = vocab;
        fc.vocab_size = V;
        fc.max_units = 1;
        fc.base_path = bp;
        fc.ledger_path = lp;
        fc.gpu_temp_limit_c = 0;
        fc.duty_fraction = 1.0;
        fc.acq.init_hidden = 2;      /* never 1 (saturation trap), but far */
        fc.acq.max_hidden = 2;       /* below what a 3-rank map needs */
        fc.acq.max_epochs = 400;

        memset(&tm, 0, sizeof tm);
        memset(&rp, 0, sizeof rp);
        check(flagship_run(&fc, topk_maker, &tm, &rp) == 0, "starved run runs");
        check(rp.acquired == 0 && rp.deferred == 1 &&
              rp.reason_kinds == 1 &&
              strcmp(rp.reasons[0], "certify_failed") == 0,
              "capacity starvation refused honestly (certify_failed)");
        cnb_init(&b);
        check(cnb_load(&b, bp) == 0 && b.unit_count == 0 && b.tag_count == 0,
              "nothing sealed by the refusal (DEFER total)");
        cnb_free(&b);
        remove(bp);
        remove(lp);
    }

    printf("[10] pilot-scheduled acquisition (confidence-scheduled refusal)\n");
    {
        FlagshipConfig fc;
        FlagshipReport rp;
        ConstCtx cm;
        const char *bp = "flagship_pilot_test.cnb";
        const char *lp = "flagship_pilot_gaps.txt";
        remove(bp);
        remove(lp);

        flagship_config_defaults(&fc);
        fc.task = FLAGSHIP_TASK_PAIR;
        fc.vocab_tokens = vocab;
        fc.vocab_size = V;
        fc.max_units = 1;
        fc.base_path = bp;
        fc.ledger_path = lp;
        fc.gpu_temp_limit_c = 0;
        fc.duty_fraction = 1.0;
        fc.acq.mine_budget = 32;     /* < V^2 -> sampled -> pilot active */
        fc.acq.sample_count = 128;
        fc.conformal_alpha = 0.0;

        memset(&cm, 0, sizeof cm);
        memset(&rp, 0, sizeof rp);
        check(flagship_run(&fc, const_maker, &cm, &rp) == 0, "pilot run runs");
        check(rp.deferred == 1 && rp.reason_kinds == 1 &&
              strcmp(rp.reasons[0], "class_imbalance") == 0,
              "constant slice refused as class_imbalance");
        check(cm.calls <= fc.acq.pilot_count + 4,
              "refused at PILOT cost, not the full sample (16ish calls, not 128)");
        printf("  [pilot] oracle calls for the refusal: %lu (budget was %lu)\n",
               (unsigned long)cm.calls, (unsigned long)fc.acq.sample_count);
        remove(bp);
        remove(lp);
    }

    printf("[11] unit shard [start,end) — no train outside the range\n");
    {
        FlagshipConfig fc;
        FlagshipReport rp;
        SynMaker mk;
        const char *bp = "flagship_shard_test.cnb";
        const char *lp = "flagship_shard_gaps.txt";
        remove(bp);
        remove(lp);
        flagship_config_defaults(&fc);
        fc.vocab_tokens = vocab;
        fc.vocab_size = V;
        fc.max_units = V;
        fc.unit_start = 2;
        fc.unit_end = 5;            /* indices 2,3,4 only */
        fc.base_path = bp;
        fc.ledger_path = lp;
        fc.gpu_temp_limit_c = 0;
        fc.duty_fraction = 1.0;
        memset(&mk, 0, sizeof mk);
        mk.broken_token = -1;
        memset(&rp, 0, sizeof rp);
        check(flagship_run(&fc, syn_maker, &mk, &rp) == 0, "shard run runs");
        check(rp.attempted == 3 && rp.acquired == 3,
              "only 3 units in [2,5) attempted");
        check(rp.skipped_screen == V - 3,
              "remaining indices counted as screen-skips");
        remove(bp);
        remove(lp);
    }

    printf("[12] allowlist screen — only listed token ids are trained\n");
    {
        FlagshipConfig fc;
        FlagshipReport rp;
        SynMaker mk;
        int allow[2];
        const char *bp = "flagship_allow_test.cnb";
        const char *lp = "flagship_allow_gaps.txt";
        remove(bp);
        remove(lp);
        allow[0] = vocab[0];
        allow[1] = vocab[5];
        flagship_config_defaults(&fc);
        fc.vocab_tokens = vocab;
        fc.vocab_size = V;
        fc.max_units = V;
        fc.allowlist_tokens = allow;
        fc.allowlist_count = 2;
        fc.base_path = bp;
        fc.ledger_path = lp;
        fc.gpu_temp_limit_c = 0;
        fc.duty_fraction = 1.0;
        memset(&mk, 0, sizeof mk);
        mk.broken_token = -1;
        memset(&rp, 0, sizeof rp);
        check(flagship_run(&fc, syn_maker, &mk, &rp) == 0, "allowlist run runs");
        check(rp.attempted == 2 && rp.acquired == 2,
              "only 2 allowlisted tokens attempted");
        check(rp.skipped_screen == V - 2,
              "non-allowlisted tokens are screen-skips (no train)");
        remove(bp);
        remove(lp);
    }

    printf("checks run: %d\n", checks_run);
    printf("ALL FLAGSHIP TESTS PASSED\n");
    return 0;
}
