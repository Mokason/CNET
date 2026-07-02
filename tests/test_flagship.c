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

    printf("checks run: %d\n", checks_run);
    printf("ALL FLAGSHIP TESTS PASSED\n");
    return 0;
}
