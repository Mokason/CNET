#include "../include/acquire.h"
#include "../include/base.h"
#include "../include/contract/unit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void acquire_config_defaults(AcquireConfig *cfg) {
    if (!cfg) return;
    memset(cfg, 0, sizeof *cfg);
    cfg->mine_budget = 4096;
    cfg->sample_count = 256;
    cfg->holdout_fraction = 0.25;
    cfg->evidence_threshold = 0.9;   /* mirrors library_gate_config_defaults */
    cfg->min_evidence = 16;          /* mirrors library_gate_config_defaults */
    cfg->min_accuracy_bound = 0.95;
    cfg->wilson_z = 1.96;
    cfg->exhaustive_cap = 0;
    cfg->unit_dir = NULL;
    cfg->capture_limit = 256;
    cfg->init_hidden = 8;
    cfg->max_hidden = 64;
    cfg->learning_rate = 0.5;
    cfg->seed = 42;
    cfg->max_epochs = 4000;
    cfg->growth_window = 200;
    cfg->target_loss = 1e-4;
    cfg->min_improvement = 1e-6;
}

void acquire_ledger_init(AcquireLedger *l) {
    if (!l) return;
    memset(l, 0, sizeof *l);
}

void acquire_ledger_free(AcquireLedger *l) {
    size_t i;
    if (!l) return;
    for (i = 0; i < l->count; ++i) {
        free(l->gaps[i].cap_inputs);
        free(l->gaps[i].cap_targets);
    }
    free(l->gaps);
    for (i = 0; i < l->acquired_count; ++i) {
        btn_free(l->acquired[i]);
        free(l->acquired[i]);
    }
    free(l->acquired);
    free(l->acquired_names);
    memset(l, 0, sizeof *l);
}

/* ---- small helpers ------------------------------------------------------ */

static int acquire_name_is_atom(const char *s) {
    size_t i;
    if (!s || !s[0]) return 0;
    for (i = 0; s[i]; ++i) {
        char c = s[i];
        int ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                 (c >= '0' && c <= '9') || c == '_';
        if (!ok) return 0;
    }
    return i < ACQUIRE_NAME_MAX;
}

static int acquire_port_eq(Port a, Port b) {
    return a.family == b.family && a.field_width == b.field_width &&
           a.field_count == b.field_count && strcmp(a.tag, b.tag) == 0;
}

int acquire_port_eq_public(Port a, Port b) { return acquire_port_eq(a, b); }

static size_t port_total(Port p) { return p.field_width * p.field_count; }

static GapRecord *ledger_push(AcquireLedger *l) {
    if (l->count == l->capacity) {
        size_t ncap = l->capacity ? l->capacity * 2 : 8;
        GapRecord *ng = realloc(l->gaps, ncap * sizeof *ng);
        if (!ng) return NULL;
        l->gaps = ng;
        l->capacity = ncap;
    }
    memset(&l->gaps[l->count], 0, sizeof l->gaps[l->count]);
    return &l->gaps[l->count++];
}

/* Reopen a DEFERRED record when it is hit again (a later drain retries). */
static void gap_rehit(GapRecord *g) {
    g->times_hit++;
    if (g->status == GAP_DEFERRED) {
        g->status = GAP_OPEN;
        g->defer_reason[0] = '\0';
    }
}

int acquire_oracle_register(OracleRegistry *o, const char *name,
                            Port input_port, Port output_port,
                            CnetOracleFn fn, void *ctx) {
    size_t i;
    if (!o || !fn || !acquire_name_is_atom(name)) return -1;
    if (o->count >= ACQUIRE_MAX_ORACLES) return -1;
    for (i = 0; i < o->count; ++i)
        if (strcmp(o->entries[i].name, name) == 0) return -1;
    memset(&o->entries[o->count], 0, sizeof o->entries[o->count]);
    snprintf(o->entries[o->count].name, ACQUIRE_NAME_MAX, "%s", name);
    o->entries[o->count].input_port = input_port;
    o->entries[o->count].output_port = output_port;
    o->entries[o->count].fn = fn;
    o->entries[o->count].ctx = ctx;
    o->count++;
    return 0;
}

int acquire_note_no_plan(AcquireLedger *l, Port input_port, Port goal_port) {
    size_t i;
    GapRecord *g;
    if (!l) return -1;
    for (i = 0; i < l->count; ++i) {
        g = &l->gaps[i];
        if (g->kind == GAP_NO_PLAN &&
            acquire_port_eq(g->input_port, input_port) &&
            acquire_port_eq(g->goal_port, goal_port)) {
            gap_rehit(g);
            return (int)i;
        }
    }
    g = ledger_push(l);
    if (!g) return -1;
    g->kind = GAP_NO_PLAN;
    g->status = GAP_OPEN;
    g->input_port = input_port;
    g->goal_port = goal_port;
    g->times_hit = 1;
    return (int)(l->count - 1);
}

/* LOW_RELIABILITY and HEALTH both mean "rebuild this unit": they coalesce on
   subject (first kind wins; the distinction is attribution, not action). */
static int note_rebuild(AcquireLedger *l, GapKind kind, const char *subject) {
    size_t i;
    GapRecord *g;
    if (!l || !acquire_name_is_atom(subject)) return -1;
    for (i = 0; i < l->count; ++i) {
        g = &l->gaps[i];
        if ((g->kind == GAP_LOW_RELIABILITY || g->kind == GAP_HEALTH) &&
            strcmp(g->subject, subject) == 0) {
            gap_rehit(g);
            return (int)i;
        }
    }
    g = ledger_push(l);
    if (!g) return -1;
    g->kind = kind;
    g->status = GAP_OPEN;
    snprintf(g->subject, ACQUIRE_NAME_MAX, "%s", subject);
    g->times_hit = 1;
    return (int)(l->count - 1);
}

int acquire_note_low_reliability(AcquireLedger *l, const char *subject,
                                 double score, double floor_used) {
    (void)score; (void)floor_used; /* recorded demand; thresholds live at the caller */
    return note_rebuild(l, GAP_LOW_RELIABILITY, subject);
}

int acquire_note_health(AcquireLedger *l, const char *subject,
                        const char *reason_atom) {
    (void)reason_atom;
    return note_rebuild(l, GAP_HEALTH, subject);
}

static OracleEntry *find_oracle(OracleRegistry *o, Port in_p, Port goal_p) {
    size_t i;
    if (!o) return NULL;
    for (i = 0; i < o->count; ++i)
        if (acquire_port_eq(o->entries[i].input_port, in_p) &&
            acquire_port_eq(o->entries[i].output_port, goal_p))
            return &o->entries[i];
    return NULL;
}

/* Append one canonical (input, target) pair to a gap's capture buffer. */
static int gap_capture(GapRecord *g, const AcquireConfig *cfg,
                       const double *cin, const double *ctgt,
                       size_t in_total, size_t out_total) {
    if (g->cap_count >= cfg->capture_limit) return 0;   /* bounded: drop */
    if (g->cap_count == g->cap_limit) {
        size_t ncap = g->cap_limit ? g->cap_limit * 2 : 16;
        double *ni, *nt;
        if (ncap > cfg->capture_limit) ncap = cfg->capture_limit;
        ni = realloc(g->cap_inputs, ncap * in_total * sizeof *ni);
        if (!ni) return -1;
        g->cap_inputs = ni;
        nt = realloc(g->cap_targets, ncap * out_total * sizeof *nt);
        if (!nt) return -1;
        g->cap_targets = nt;
        g->cap_limit = ncap;
    }
    memcpy(g->cap_inputs + g->cap_count * in_total, cin,
           in_total * sizeof *cin);
    memcpy(g->cap_targets + g->cap_count * out_total, ctgt,
           out_total * sizeof *ctgt);
    g->cap_count++;
    return 0;
}

int acquire_execute_or_fallback(PrimitiveRegistry *reg, AcquireLedger *l,
                                OracleRegistry *oracles,
                                const AcquireConfig *cfg,
                                Port input_port, Port goal_port,
                                const double *input, size_t in_len,
                                double *output, size_t out_cap) {
    RoutePlan plan;
    size_t in_total = port_total(input_port);
    size_t out_total = port_total(goal_port);
    double cin[64], raw[64], ctgt[64];
    OracleEntry *o;
    int gap_idx;

    if (!reg || !l || !cfg || !input || !output) return -1;
    if (in_len != in_total || out_cap < out_total) return -1;
    if (in_total > 64 || out_total > 64) return -1;  /* v1 stack bound */

    if (route_plan(reg, input_port, goal_port, &plan) == 0) {
        plan.strict = 1;
        return route_execute(&plan, input, in_len, output, out_cap);
    }

    gap_idx = acquire_note_no_plan(l, input_port, goal_port);
    if (gap_idx < 0) return -1;

    o = find_oracle(oracles, input_port, goal_port);
    if (!o) return -1;

    /* validate-then-canonicalize on BOTH sides of the oracle boundary */
    if (!port_validate(input_port, input)) return -1;
    if (port_canonicalize(input_port, input, cin) != 0) return -1;
    o->calls++;
    if (o->fn(cin, raw, o->ctx) != 0) { o->rejects++; return -1; }
    if (!port_validate(goal_port, raw)) { o->rejects++; return -1; }
    if (port_canonicalize(goal_port, raw, ctgt) != 0) return -1;
    memcpy(output, ctgt, out_total * sizeof *ctgt);

    snprintf(l->gaps[gap_idx].oracle, ACQUIRE_NAME_MAX, "%s", o->name);
    gap_capture(&l->gaps[gap_idx], cfg, cin, ctgt, in_total, out_total);
    return 0;
}

static void gap_defer(GapRecord *g, AcquireReport *rep, const char *reason) {
    g->status = GAP_DEFERRED;
    snprintf(g->defer_reason, ACQUIRE_REASON_MAX, "%s", reason);
    if (rep) {
        rep->deferred++;
        snprintf(rep->last_defer_reason, ACQUIRE_REASON_MAX, "%s", reason);
    }
}

/* Track a minted BTN (and its name) as ledger-owned. */
static int ledger_own_btn(AcquireLedger *l, BinaryTransformNetwork *btn,
                          const char *name) {
    if (l->acquired_count == l->acquired_capacity) {
        size_t ncap = l->acquired_capacity ? l->acquired_capacity * 2 : 4;
        BinaryTransformNetwork **nb =
            realloc(l->acquired, ncap * sizeof *nb);
        char (*nn)[ACQUIRE_NAME_MAX];
        if (!nb) return -1;
        l->acquired = nb;
        nn = realloc(l->acquired_names, ncap * sizeof *nn);
        if (!nn) return -1;
        l->acquired_names = nn;
        l->acquired_capacity = ncap;
    }
    l->acquired[l->acquired_count] = btn;
    snprintf(l->acquired_names[l->acquired_count], ACQUIRE_NAME_MAX, "%s", name);
    l->acquired_count++;
    return 0;
}

/* Mine the (in -> goal) exemplar table from the oracle.
   Enumerable within budget -> full enumeration (exhaustive=1);
   enumerable over budget  -> deterministic stride sample;
   non-enumerable          -> -1 ("unbounded_domain").
   Rows failing the oracle or port_validate are skipped and counted on the
   oracle entry. Returns usable row count via *n_out (tables are malloc'd,
   caller frees), attempts via *attempts_out. */
static int mine_from_oracle(OracleEntry *o, Port in_p, Port goal_p,
                            const AcquireConfig *cfg,
                            double **inputs_out, double **targets_out,
                            size_t *n_out, size_t *attempts_out,
                            int *exhaustive_out) {
    Contract tc;                 /* temp: ports only, for domain enumeration */
    size_t card, n_points, k, usable = 0;
    size_t in_total = port_total(in_p), out_total = port_total(goal_p);
    double *inputs, *targets;
    double *raw;                 /* heap: out_total is caller-sized (a fixed
                                    stack buffer here overflowed at V=256) */

    memset(&tc, 0, sizeof tc);
    snprintf(tc.name, CONTRACT_NAME_MAX, "acq_tmp");
    tc.input_ports[0] = in_p;  tc.input_port_count = 1;
    tc.output_ports[0] = goal_p; tc.output_port_count = 1;

    if (!contract_domain_cardinality(&tc, &card)) return -1; /* unbounded */

    *exhaustive_out = (card <= cfg->mine_budget);
    n_points = *exhaustive_out ? card
             : (cfg->sample_count < card ? cfg->sample_count : card);

    inputs  = malloc(n_points * in_total * sizeof *inputs);
    targets = malloc(n_points * out_total * sizeof *targets);
    raw     = malloc(out_total * sizeof *raw);
    if (!inputs || !targets || !raw) {
        free(inputs); free(targets); free(raw);
        return -1;
    }

    for (k = 0; k < n_points; ++k) {
        size_t idx = *exhaustive_out ? k : (k * card) / n_points; /* stride */
        double *irow = inputs + usable * in_total;
        double *trow = targets + usable * out_total;
        if (contract_encode_domain_point(&tc, idx, irow) != 0) continue;
        o->calls++;
        if (o->fn(irow, raw, o->ctx) != 0) { o->rejects++; continue; }
        if (!port_validate(goal_p, raw))  { o->rejects++; continue; }
        if (port_canonicalize(goal_p, raw, trow) != 0) continue;
        usable++;
    }
    free(raw);
    *inputs_out = inputs;
    *targets_out = targets;
    *n_out = usable;
    *attempts_out = n_points;
    return 0;
}

/* Base-governance precheck: tags that would near-miss the base's registry,
   or a name the base already holds, refuse the acquisition BEFORE any state
   changes (DEFER stays total). Returns NULL when clear, else the defer atom. */
static const char *base_precheck(struct CnetBase *base, const char *name,
                                 Port in_p, Port goal_p) {
    if (!base) return NULL;
    if (in_p.tag[0] && cnb_tag_lookup(base, in_p.tag) < 0 &&
        cnb_tag_near_miss(base, in_p.tag, NULL, 0)) return "tag_collision";
    if (goal_p.tag[0] && cnb_tag_lookup(base, goal_p.tag) < 0 &&
        cnb_tag_near_miss(base, goal_p.tag, NULL, 0)) return "tag_collision";
    if (cnb_has_unit(base, name)) return "register_refused";
    return NULL;
}

/* One acquisition attempt for one OPEN gap with a matched oracle.
   All behavioral checks run BEFORE any state change; the only
   post-registration step is a read-only replan (rollback on failure =
   registry_remove_last + remove the sealed file). */
static int attempt_no_plan(PrimitiveRegistry *reg, AcquireLedger *l,
                           OracleEntry *o, GapRecord *g,
                           const AcquireConfig *cfg, AcquireReport *rep) {
    size_t in_total = port_total(g->input_port);
    size_t out_total = port_total(g->goal_port);
    double *inputs = NULL, *targets = NULL;
    size_t usable = 0, attempts = 0, n_train;
    int exhaustive = 0;
    BinaryTransformNetwork *btn = NULL;
    Contract c;
    ExhaustiveReport ex;
    char name[ACQUIRE_NAME_MAX];
    char cnu_path[512];
    int sealed = 0;

    g->attempts++;
    snprintf(g->oracle, ACQUIRE_NAME_MAX, "%s", o->name);

    /* candidate name: acq_<goal tag>, falling back to a counter */
    if (g->goal_port.tag[0])
        snprintf(name, sizeof name, "acq_%s", g->goal_port.tag);
    else
        snprintf(name, sizeof name, "acq_gap%lu",
                 (unsigned long)(g - l->gaps));

    /* 1. mine */
    if (mine_from_oracle(o, g->input_port, g->goal_port, cfg,
                         &inputs, &targets, &usable, &attempts,
                         &exhaustive) != 0) {
        gap_defer(g, rep, "unbounded_domain");
        return -1;
    }

    /* merge captured pairs (sampled mode only; exhaustive already covers them) */
    if (!exhaustive && g->cap_count > 0) {
        size_t i, j;
        double *ni = realloc(inputs, (usable + g->cap_count) * in_total * sizeof *ni);
        double *nt = ni ? realloc(targets, (usable + g->cap_count) * out_total * sizeof *nt) : NULL;
        if (ni) inputs = ni;
        if (nt) targets = nt;
        if (ni && nt) {
            for (i = 0; i < g->cap_count; ++i) {
                int dup = 0;
                const double *cin = g->cap_inputs + i * in_total;
                for (j = 0; j < usable && !dup; ++j)
                    dup = (memcmp(inputs + j * in_total, cin,
                                  in_total * sizeof *cin) == 0);
                if (dup) continue;
                memcpy(inputs + usable * in_total, cin, in_total * sizeof *cin);
                memcpy(targets + usable * out_total,
                       g->cap_targets + i * out_total,
                       out_total * sizeof *targets);
                usable++;
            }
        }
    }

    /* 2. oracle-evidence gate (the acquisition analog of EVIDENCE_CLEAR) */
    if (usable == 0 && attempts > 0) {
        free(inputs); free(targets);
        gap_defer(g, rep, "oracle_unfit");
        return -1;
    }
    if (usable < cfg->min_evidence) {
        free(inputs); free(targets);
        gap_defer(g, rep, "insufficient_exemplars");
        return -1;
    }
    if ((double)usable / (double)(attempts ? attempts : 1) <
        cfg->evidence_threshold) {
        free(inputs); free(targets);
        gap_defer(g, rep, "oracle_unfit");
        return -1;
    }

    /* class balance (sampled mode heuristic): >= 2 distinct target rows */
    if (!exhaustive) {
        size_t i, distinct = 1;
        for (i = 1; i < usable && distinct < 2; ++i)
            if (memcmp(targets, targets + i * out_total,
                       out_total * sizeof *targets) != 0) distinct = 2;
        if (distinct < 2) {
            free(inputs); free(targets);
            gap_defer(g, rep, "class_imbalance");
            return -1;
        }
    }

    /* 3. train candidate (heap: the ledger will own it on success) */
    n_train = exhaustive ? usable
            : usable - (size_t)((double)usable * cfg->holdout_fraction);
    if (n_train == 0) n_train = usable;
    btn = calloc(1, sizeof *btn);
    if (!btn ||
        btn_init(btn, in_total, out_total, cfg->init_hidden, cfg->max_hidden,
                 cfg->learning_rate, cfg->seed) != 0 ||
        btn_set_ports(btn, g->input_port, g->goal_port) != 0) {
        free(btn); free(inputs); free(targets);
        gap_defer(g, rep, "certify_failed");
        return -1;
    }
    btn_train_dynamic(btn, inputs, targets, n_train, cfg->max_epochs,
                      cfg->growth_window, cfg->target_loss,
                      cfg->min_improvement);

    /* 4. certify: contract over the FULL mined table (holdout rows included:
       they were never trained on, so certification tests them) */
    if (contract_init_borrowed(&c, name, btn, inputs, targets, usable) != 0 ||
        btn_certify_exhaustive(btn, &c, cfg->exhaustive_cap, &ex) < 0 ||
        ex.verdict == CERT_REFUSED) {
        btn_free(btn); free(btn); free(inputs); free(targets);
        gap_defer(g, rep, "certify_failed");
        return -1;
    }
    if (ex.verdict != CERT_PROVEN) {
        double bound = coverage_accuracy_lower_bound(
            ex.certify.passed, ex.certify.exemplars, cfg->wilson_z);
        if (bound < cfg->min_accuracy_bound) {
            btn_free(btn); free(btn); free(inputs); free(targets);
            gap_defer(g, rep, "accuracy_bound");
            return -1;
        }
    }

    /* 5. seal target precheck + loose-file seal (base commit happens LAST,
       after registration + replan, so DEFER stays total for the base too) */
    {
        const char *why = base_precheck(cfg->base, name,
                                        g->input_port, g->goal_port);
        if (why) {
            btn_free(btn); free(btn); free(inputs); free(targets);
            gap_defer(g, rep, why);
            return -1;
        }
    }
    if (!cfg->base && cfg->unit_dir) {
        snprintf(cnu_path, sizeof cnu_path, "%s/%s.cnu", cfg->unit_dir, name);
        if (unit_save(btn, &c, cnu_path) != 0) {
            btn_free(btn); free(btn); free(inputs); free(targets);
            gap_defer(g, rep, "seal_failed");
            return -1;
        }
        sealed = 1;
    }

    /* 6. register (name storage must outlive the registry: ledger-owned) */
    if (ledger_own_btn(l, btn, name) != 0 ||
        registry_add_certified(reg, btn,
                               l->acquired_names[l->acquired_count - 1],
                               &c) != 0) {
        if (sealed) remove(cnu_path);
        if (l->acquired_count > 0 &&
            l->acquired[l->acquired_count - 1] == btn) {
            l->acquired_count--;
        }
        btn_free(btn); free(btn); free(inputs); free(targets);
        gap_defer(g, rep, "register_refused");
        return -1;
    }

    /* 7. read-only replan check; rollback on the (unexpected) miss */
    {
        RoutePlan plan;
        if (route_plan(reg, g->input_port, g->goal_port, &plan) != 0) {
            registry_remove_last(reg);
            if (sealed) remove(cnu_path);
            l->acquired_count--;
            btn_free(btn); free(btn); free(inputs); free(targets);
            gap_defer(g, rep, "replan_failed");
            return -1;
        }
    }

    /* 8. commit into the unified base (precheck passed; failure = rare OOM /
       digest collision -> full rollback) */
    if (cfg->base && cnb_add_unit(cfg->base, btn, &c, NULL) != 0) {
        registry_remove_last(reg);
        l->acquired_count--;
        btn_free(btn); free(btn); free(inputs); free(targets);
        gap_defer(g, rep, "register_refused");
        return -1;
    }

    free(inputs); free(targets);   /* contract borrowed them; done with both */
    g->status = GAP_CLOSED;
    if (rep) {
        rep->closed++;
        rep->last_verdict = ex.verdict;
        snprintf(rep->last_unit_name, ACQUIRE_NAME_MAX, "%s", name);
    }
    return 0;
}

static RegistryEntry *find_entry(PrimitiveRegistry *reg, const char *name) {
    size_t i;
    for (i = 0; i < reg->count; ++i)
        if (strcmp(reg->entries[i].name, name) == 0) return &reg->entries[i];
    return NULL;
}

/* Rebuild a suspect unit (LOW_RELIABILITY / HEALTH): re-mine the truth from
   the oracle, certify the INCUMBENT against it first (a healthy unit is never
   churned), and only on incumbent failure mint a fresh-named replacement and
   demote the incumbent to PRIM_RESET (never a same-name replace). */
static int attempt_rebuild(PrimitiveRegistry *reg, AcquireLedger *l,
                           OracleRegistry *oracles, GapRecord *g,
                           const AcquireConfig *cfg, AcquireReport *rep) {
    RegistryEntry *e = find_entry(reg, g->subject);
    OracleEntry *o;
    Port in_p, goal_p;
    double *inputs = NULL, *targets = NULL;
    size_t usable = 0, attempts = 0;
    int exhaustive = 0;
    Contract mined;
    char name[ACQUIRE_NAME_MAX];
    char cnu_path[512];
    int sealed = 0;
    BinaryTransformNetwork *btn = NULL;
    ExhaustiveReport ex;

    g->attempts++;
    if (!e || !e->btn) { gap_defer(g, rep, "unknown_subject"); return -1; }
    if (e->btn->input_port_count != 1 || e->btn->output_port_count != 1) {
        gap_defer(g, rep, "multi_port_unsupported"); return -1;
    }
    in_p = e->btn->input_ports[0];
    goal_p = e->btn->output_ports[0];

    o = find_oracle(oracles, in_p, goal_p);
    if (!o) { if (rep) rep->skipped_no_oracle++; return -1; } /* stays OPEN */
    snprintf(g->oracle, ACQUIRE_NAME_MAX, "%s", o->name);

    if (mine_from_oracle(o, in_p, goal_p, cfg, &inputs, &targets,
                         &usable, &attempts, &exhaustive) != 0) {
        gap_defer(g, rep, "unbounded_domain"); return -1;
    }
    if (usable == 0 && attempts > 0) {
        free(inputs); free(targets);
        gap_defer(g, rep, "oracle_unfit"); return -1;
    }
    if (usable < cfg->min_evidence) {
        free(inputs); free(targets);
        gap_defer(g, rep, "insufficient_exemplars"); return -1;
    }
    if ((double)usable / (double)attempts < cfg->evidence_threshold) {
        free(inputs); free(targets);
        gap_defer(g, rep, "oracle_unfit"); return -1;
    }

    /* subject bounded to 48 chars so "_r<n>" always fits in NAME_MAX */
    snprintf(name, sizeof name, "%.48s_r%lu", g->subject,
             (unsigned long)g->attempts);

    /* incumbent health check against the freshly mined truth */
    if (contract_init_borrowed(&mined, name, e->btn,
                               inputs, targets, usable) == 0 &&
        btn_certify(e->btn, &mined, NULL) == 0) {
        free(inputs); free(targets);
        gap_defer(g, rep, "incumbent_healthy");
        return -1;
    }

    /* train + certify the replacement (same recipe as attempt_no_plan) */
    btn = calloc(1, sizeof *btn);
    if (!btn ||
        btn_init(btn, port_total(in_p), port_total(goal_p),
                 cfg->init_hidden, cfg->max_hidden,
                 cfg->learning_rate, cfg->seed) != 0 ||
        btn_set_ports(btn, in_p, goal_p) != 0) {
        free(btn); free(inputs); free(targets);
        gap_defer(g, rep, "certify_failed"); return -1;
    }
    btn_train_dynamic(btn, inputs, targets, usable, cfg->max_epochs,
                      cfg->growth_window, cfg->target_loss,
                      cfg->min_improvement);
    if (contract_init_borrowed(&mined, name, btn, inputs, targets,
                               usable) != 0 ||
        btn_certify_exhaustive(btn, &mined, cfg->exhaustive_cap, &ex) < 0 ||
        ex.verdict == CERT_REFUSED) {
        btn_free(btn); free(btn); free(inputs); free(targets);
        gap_defer(g, rep, "certify_failed"); return -1;
    }
    if (ex.verdict != CERT_PROVEN &&
        coverage_accuracy_lower_bound(ex.certify.passed,
                                      ex.certify.exemplars,
                                      cfg->wilson_z) < cfg->min_accuracy_bound) {
        btn_free(btn); free(btn); free(inputs); free(targets);
        gap_defer(g, rep, "accuracy_bound"); return -1;
    }
    {
        const char *why = base_precheck(cfg->base, name, in_p, goal_p);
        if (why) {
            btn_free(btn); free(btn); free(inputs); free(targets);
            gap_defer(g, rep, why); return -1;
        }
    }
    if (!cfg->base && cfg->unit_dir) {
        snprintf(cnu_path, sizeof cnu_path, "%s/%s.cnu", cfg->unit_dir, name);
        if (unit_save(btn, &mined, cnu_path) != 0) {
            btn_free(btn); free(btn); free(inputs); free(targets);
            gap_defer(g, rep, "seal_failed"); return -1;
        }
        sealed = 1;
    }
    if (ledger_own_btn(l, btn, name) != 0 ||
        registry_add_certified(reg, btn,
                               l->acquired_names[l->acquired_count - 1],
                               &mined) != 0) {
        if (sealed) remove(cnu_path);
        if (l->acquired_count > 0 &&
            l->acquired[l->acquired_count - 1] == btn)
            l->acquired_count--;
        btn_free(btn); free(btn); free(inputs); free(targets);
        gap_defer(g, rep, "register_refused"); return -1;
    }

    /* demote the incumbent + invalidate its stale evidence */
    registry_set_state(reg, g->subject, PRIM_RESET);
    if (cfg->unit_dir) {
        char stats_path[512];
        snprintf(stats_path, sizeof stats_path, "%s/%s.stats",
                 cfg->unit_dir, g->subject);
        remove(stats_path);   /* absent file is fine */
    }

    {
        RoutePlan plan;
        if (route_plan(reg, in_p, goal_p, &plan) != 0) {
            /* roll back everything, restore the incumbent */
            registry_remove_last(reg);
            registry_set_state(reg, g->subject, PRIM_FROZEN);
            if (sealed) remove(cnu_path);
            l->acquired_count--;
            btn_free(btn); free(btn); free(inputs); free(targets);
            gap_defer(g, rep, "replan_failed"); return -1;
        }
    }

    /* commit the replacement into the unified base */
    if (cfg->base && cnb_add_unit(cfg->base, btn, &mined, NULL) != 0) {
        registry_remove_last(reg);
        registry_set_state(reg, g->subject, PRIM_FROZEN);
        l->acquired_count--;
        btn_free(btn); free(btn); free(inputs); free(targets);
        gap_defer(g, rep, "register_refused"); return -1;
    }

    free(inputs); free(targets);
    g->status = GAP_CLOSED;
    if (rep) {
        rep->closed++;
        rep->last_verdict = ex.verdict;
        snprintf(rep->last_unit_name, ACQUIRE_NAME_MAX, "%s", name);
    }
    return 0;
}

int acquire_drain(PrimitiveRegistry *reg, AcquireLedger *l,
                  OracleRegistry *oracles, const AcquireConfig *cfg,
                  AcquireReport *report) {
    size_t i;
    if (!reg || !l || !cfg) return -1;
    for (i = 0; i < l->count; ++i) {
        GapRecord *g = &l->gaps[i];
        OracleEntry *o;
        if (g->status != GAP_OPEN) continue;
        if (report) report->examined++;
        if (g->kind == GAP_NO_PLAN) {
            o = find_oracle(oracles, g->input_port, g->goal_port);
            if (!o) { if (report) report->skipped_no_oracle++; continue; }
            attempt_no_plan(reg, l, o, g, cfg, report);
        } else {
            attempt_rebuild(reg, l, oracles, g, cfg, report);
        }
    }
    return 0;
}

int acquire_now(PrimitiveRegistry *reg, AcquireLedger *l,
                OracleRegistry *oracles, const AcquireConfig *cfg,
                Port input_port, Port goal_port, AcquireReport *report) {
    int idx;
    OracleEntry *o;
    if (!reg || !l || !cfg) return -1;
    idx = acquire_note_no_plan(l, input_port, goal_port);
    if (idx < 0) return -1;
    if (l->gaps[idx].status != GAP_OPEN) /* already CLOSED by an earlier run */
        return l->gaps[idx].status == GAP_CLOSED ? 0 : -1;
    if (report) report->examined++;
    o = find_oracle(oracles, input_port, goal_port);
    if (!o) { if (report) report->skipped_no_oracle++; return -1; }
    return attempt_no_plan(reg, l, o, &l->gaps[idx], cfg, report);
}

static const char *str_or_dash(const char *s) { return s[0] ? s : "-"; }

static void dash_to_str(char *dst, size_t cap, const char *src) {
    if (strcmp(src, "-") == 0) { dst[0] = '\0'; return; }
    snprintf(dst, cap, "%s", src);
}

int acquire_ledger_save(const AcquireLedger *l, const char *path) {
    FILE *f;
    size_t i;
    if (!l || !path) return -1;
    f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "CNET_GAPS 1\n%lu\n", (unsigned long)l->count);
    for (i = 0; i < l->count; ++i) {
        const GapRecord *g = &l->gaps[i];
        fprintf(f, "%d %d %lu %lu %d %lu %lu %s %d %lu %lu %s %s %s %s\n",
                (int)g->kind, (int)g->status,
                (unsigned long)g->times_hit, (unsigned long)g->attempts,
                (int)g->input_port.family,
                (unsigned long)g->input_port.field_width,
                (unsigned long)g->input_port.field_count,
                str_or_dash(g->input_port.tag),
                (int)g->goal_port.family,
                (unsigned long)g->goal_port.field_width,
                (unsigned long)g->goal_port.field_count,
                str_or_dash(g->goal_port.tag),
                str_or_dash(g->subject),
                str_or_dash(g->oracle),
                str_or_dash(g->defer_reason));
    }
    fclose(f);
    return 0;
}

int acquire_ledger_load(AcquireLedger *l, const char *path) {
    FILE *f;
    unsigned long count, i;
    AcquireLedger fresh;   /* parse into a temp; swap only on full success */
    if (!l || !path) return -1;
    f = fopen(path, "r");
    if (!f) return -1;
    {
        char magic[16]; int ver;
        if (fscanf(f, "%15s %d\n", magic, &ver) != 2 ||
            strcmp(magic, "CNET_GAPS") != 0 || ver != 1) { fclose(f); return -1; }
    }
    if (fscanf(f, "%lu\n", &count) != 1) { fclose(f); return -1; }
    acquire_ledger_init(&fresh);
    for (i = 0; i < count; ++i) {
        int kind, status, in_fam, goal_fam;
        unsigned long hit, att, in_w, in_c, goal_w, goal_c;
        char in_tag[PORT_TAG_MAX], goal_tag[PORT_TAG_MAX];
        char subject[ACQUIRE_NAME_MAX], oracle[ACQUIRE_NAME_MAX];
        char reason[ACQUIRE_REASON_MAX];
        GapRecord *g;
        if (fscanf(f, "%d %d %lu %lu %d %lu %lu %31s %d %lu %lu %31s %63s %63s %63s\n",
                   &kind, &status, &hit, &att,
                   &in_fam, &in_w, &in_c, in_tag,
                   &goal_fam, &goal_w, &goal_c, goal_tag,
                   subject, oracle, reason) != 15 ||
            kind < 0 || kind > 2 || status < 0 || status > 2) {
            acquire_ledger_free(&fresh); fclose(f); return -1;
        }
        g = ledger_push(&fresh);
        if (!g) { acquire_ledger_free(&fresh); fclose(f); return -1; }
        g->kind = (GapKind)kind;
        g->status = (GapStatus)status;
        g->times_hit = hit;
        g->attempts = att;
        g->input_port.family = (PortFamily)in_fam;
        g->input_port.field_width = in_w;
        g->input_port.field_count = in_c;
        dash_to_str(g->input_port.tag, PORT_TAG_MAX, in_tag);
        g->goal_port.family = (PortFamily)goal_fam;
        g->goal_port.field_width = goal_w;
        g->goal_port.field_count = goal_c;
        dash_to_str(g->goal_port.tag, PORT_TAG_MAX, goal_tag);
        dash_to_str(g->subject, ACQUIRE_NAME_MAX, subject);
        dash_to_str(g->oracle, ACQUIRE_NAME_MAX, oracle);
        dash_to_str(g->defer_reason, ACQUIRE_REASON_MAX, reason);
    }
    fclose(f);
    /* success: replace gap records; acquired-BTN ownership is NOT touched */
    {
        size_t j;
        for (j = 0; j < l->count; ++j) {
            free(l->gaps[j].cap_inputs);
            free(l->gaps[j].cap_targets);
        }
        free(l->gaps);
        l->gaps = fresh.gaps;
        l->count = fresh.count;
        l->capacity = fresh.capacity;
    }
    return 0;
}
