#include "../include/acquire.h"
#include "../include/base.h"
#include "../include/contract/unit.h"
#include "../include/specialist.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _OPENMP
#include <omp.h>
#endif

/* Native-unit admission through the one specialist door: wrap as a
   Specialist(kind=btn) and admit (certify + register + stamp kind). The
   low-level registry_add_certified stays an internal of the admission layer. */
static int admit_native_btn(PrimitiveRegistry *reg, BinaryTransformNetwork *btn,
                            const char *name, const Contract *c) {
    Specialist s;
    if (specialist_wrap_btn(&s, btn, name) != 0) return -1;
    return specialist_admit(reg, &s, c);
}

void acquire_config_defaults(AcquireConfig *cfg) {
    if (!cfg) return;
    memset(cfg, 0, sizeof *cfg);
    cfg->mine_budget = 4096;
    cfg->sample_count = 256;
    cfg->pilot_count = 16;
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

/* ---- Student warm-starting (CNET_ACQ_WARMSTART=1) ------------------------
   Successive units in a mining run share input geometry and output
   vocabulary; training every student from a cold random init re-learns that
   shared structure hundreds of times over. Warm-starting clones the last
   SUCCESSFUL student's weights as the next student's init. Certification
   judges behavior only, so provenance is free — a warm-started student that
   certifies is exactly as certified as a cold one. Serial mining only (the
   snapshot is process-global; flagship processes units sequentially). */
static struct {
    int valid;
    size_t in, out, maxh, h;
    double *input_hidden;
    double *hidden_bias;
    double *hidden_output_weights;
    double *output_bias;
} ws_snap;

static int ws_on(void) {
    const char *e = getenv("CNET_ACQ_WARMSTART");
    return e && e[0] == '1';
}

static void ws_capture(const BinaryTransformNetwork *btn) {
    size_t ih = btn->input_count * btn->max_hidden_count;
    size_t ho = btn->max_hidden_count * btn->output_count;
    if (ws_snap.in != btn->input_count || ws_snap.out != btn->output_count ||
        ws_snap.maxh != btn->max_hidden_count || !ws_snap.input_hidden) {
        free(ws_snap.input_hidden);
        free(ws_snap.hidden_bias);
        free(ws_snap.hidden_output_weights);
        free(ws_snap.output_bias);
        memset(&ws_snap, 0, sizeof ws_snap);
        ws_snap.input_hidden = (double *)malloc(ih * sizeof(double));
        ws_snap.hidden_bias =
            (double *)malloc(btn->max_hidden_count * sizeof(double));
        ws_snap.hidden_output_weights = (double *)malloc(ho * sizeof(double));
        ws_snap.output_bias =
            (double *)malloc(btn->output_count * sizeof(double));
        if (!ws_snap.input_hidden || !ws_snap.hidden_bias ||
            !ws_snap.hidden_output_weights || !ws_snap.output_bias) {
            free(ws_snap.input_hidden);
            free(ws_snap.hidden_bias);
            free(ws_snap.hidden_output_weights);
            free(ws_snap.output_bias);
            memset(&ws_snap, 0, sizeof ws_snap);
            return;   /* OOM: warm-start silently off */
        }
        ws_snap.in = btn->input_count;
        ws_snap.out = btn->output_count;
        ws_snap.maxh = btn->max_hidden_count;
    }
    memcpy(ws_snap.input_hidden, btn->input_hidden, ih * sizeof(double));
    memcpy(ws_snap.hidden_bias, btn->hidden_bias,
           btn->max_hidden_count * sizeof(double));
    memcpy(ws_snap.hidden_output_weights, btn->hidden_output_weights,
           ho * sizeof(double));
    memcpy(ws_snap.output_bias, btn->output_bias,
           btn->output_count * sizeof(double));
    ws_snap.h = btn->hidden_count;
    ws_snap.valid = 1;
}

static void ws_apply(BinaryTransformNetwork *btn) {
    size_t ih, ho;
    if (!ws_snap.valid || ws_snap.in != btn->input_count ||
        ws_snap.out != btn->output_count ||
        ws_snap.maxh != btn->max_hidden_count)
        return;
    ih = btn->input_count * btn->max_hidden_count;
    ho = btn->max_hidden_count * btn->output_count;
    memcpy(btn->input_hidden, ws_snap.input_hidden, ih * sizeof(double));
    memcpy(btn->hidden_bias, ws_snap.hidden_bias,
           btn->max_hidden_count * sizeof(double));
    memcpy(btn->hidden_output_weights, ws_snap.hidden_output_weights,
           ho * sizeof(double));
    memcpy(btn->output_bias, ws_snap.output_bias,
           btn->output_count * sizeof(double));
    btn->hidden_count = ws_snap.h;
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

static uint64_t oracle_fnv_byte(uint64_t h, unsigned char byte) {
    return (h ^ (uint64_t)byte) * UINT64_C(1099511628211);
}

static uint64_t oracle_fnv_u32(uint64_t h, uint32_t value) {
    unsigned i;
    for (i = 0; i < 4; ++i)
        h = oracle_fnv_byte(h, (unsigned char)(value >> (i * 8)));
    return h;
}

static uint64_t oracle_fnv_u64(uint64_t h, uint64_t value) {
    unsigned i;
    for (i = 0; i < 8; ++i)
        h = oracle_fnv_byte(h, (unsigned char)(value >> (i * 8)));
    return h;
}

uint64_t cnet_oracle_identity_digest(const CnetOracleIdentity *identity) {
    uint64_t h = UINT64_C(1469598103934665603);
    /* struct_size must cover the DIGESTED fields (through toolchain_digest),
       NOT the whole struct — the artifact_sha256 tail is an ABI-safe extension
       that is not hashed, so a smaller (pre-sha256) struct_size still validates
       and its digest is unchanged. */
    if (!identity || identity->abi_version != CNET_ORACLE_ABI_VERSION ||
        identity->struct_size < offsetof(CnetOracleIdentity, artifact_sha256) ||
        identity->artifact_digest == 0 || identity->contract_digest == 0) {
        return 0;
    }
    h = oracle_fnv_u32(h, identity->abi_version);
    h = oracle_fnv_u64(h, identity->artifact_digest);
    h = oracle_fnv_u64(h, identity->contract_digest);
    h = oracle_fnv_u64(h, identity->config_digest);
    h = oracle_fnv_u64(h, identity->retrieval_snapshot_digest);
    h = oracle_fnv_u64(h, identity->toolchain_digest);
    return h ? h : UINT64_C(1);
}

static void oracle_result_init(CnetOracleResult *result,
                               CnetOracleStatus status,
                               uint64_t identity_digest) {
    memset(result, 0, sizeof *result);
    result->abi_version = CNET_ORACLE_ABI_VERSION;
    result->struct_size = (uint32_t)sizeof *result;
    result->status = status;
    result->oracle_identity_digest = identity_digest;
}

static void oracle_account(OracleEntry *entry, CnetOracleResult *result,
                           CnetOracleStatus status) {
    result->status = status;
    result->oracle_identity_digest = entry->behavior_digest;
    if ((unsigned)status < CNET_ORACLE_STATUS_COUNT)
        entry->status_counts[status]++;
    if (status == CNET_ORACLE_ABSTAIN_AMBIGUOUS ||
        status == CNET_ORACLE_ABSTAIN_UNDETERMINED) {
        entry->abstains++;
    } else if (status != CNET_ORACLE_ANSWER) {
        entry->rejects++;
    }
    entry->last_result = *result;
}

CnetOracleStatus cnet_oracle_invoke(OracleEntry *entry,
                                    const double *in, size_t in_count,
                                    double *out, size_t out_count,
                                    CnetOracleResult *result_out) {
    CnetOracleResult result;
    CnetOracleStatus status = CNET_ORACLE_FAIL_PERMANENT;
    size_t expected_in, expected_out;

    oracle_result_init(&result, status, entry ? entry->behavior_digest : 0);
    if (!entry || (!entry->fn && !entry->fn_v2)) {
        if (result_out) *result_out = result;
        return status;
    }
    entry->calls++;
    expected_in = port_total(entry->input_port);
    expected_out = port_total(entry->output_port);
    if (!in || !out || in_count != expected_in || out_count != expected_out ||
        !port_validate(entry->input_port, in)) {
        status = CNET_ORACLE_INVALID_INPUT;
        oracle_account(entry, &result, status);
        if (result_out) *result_out = result;
        return status;
    }

    if (entry->fn_v2) {
        int transport_rc;
        memset(&result, 0, sizeof result);
        transport_rc = entry->fn_v2(in, in_count, out, out_count,
                                    &result, entry->ctx);
        if (transport_rc != 0 ||
            result.abi_version != CNET_ORACLE_ABI_VERSION ||
            result.struct_size < sizeof result || result.flags != 0 ||
            (unsigned)result.status >= CNET_ORACLE_STATUS_COUNT) {
            oracle_result_init(&result, CNET_ORACLE_FAIL_PERMANENT,
                               entry->behavior_digest);
            status = CNET_ORACLE_FAIL_PERMANENT;
        } else {
            status = result.status;
            result.oracle_identity_digest = entry->behavior_digest;
        }
    } else {
        int legacy_rc = entry->fn(in, out, entry->ctx);
        status = legacy_rc == 0 ? CNET_ORACLE_ANSWER
               : legacy_rc > 0 ? CNET_ORACLE_ABSTAIN_AMBIGUOUS
                               : CNET_ORACLE_FAIL_PERMANENT;
        oracle_result_init(&result, status, entry->behavior_digest);
        result.confidence = status == CNET_ORACLE_ANSWER ? 1.0 : 0.0;
    }

    if (status == CNET_ORACLE_ANSWER) {
        if (!isfinite(result.confidence) ||
            result.confidence < 0.0 || result.confidence > 1.0 ||
            !port_validate(entry->output_port, out)) {
            status = CNET_ORACLE_INVALID_OUTPUT;
        } else if (entry->validator) {
            CnetOracleValidity validity = entry->validator(
                in, in_count, out, out_count, &result, entry->ctx);
            if (validity == CNET_ORACLE_VALIDITY_UNDETERMINED)
                status = CNET_ORACLE_ABSTAIN_UNDETERMINED;
            else if (validity != CNET_ORACLE_VALID)
                status = CNET_ORACLE_INVALID_OUTPUT;
        }
    }

    oracle_account(entry, &result, status);
    if (result_out) *result_out = result;
    return status;
}

static size_t goal_sample_mix(Port goal) {
    /* Cheap goal-dependent offset so units with same input port but
       different goals get different (still uniform) sample strata.
       Reduces exact cross-unit input table duplication from mining. */
    size_t h = goal.field_width ^ (goal.field_count << 3) ^ (goal.family << 7);
    const char *t = goal.tag;
    for (size_t i = 0; t[i] && i < 48; ++i)
        h = h * 131 + (unsigned char)t[i];
    return h;
}

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

int acquire_oracle_register_v2(OracleRegistry *o, const char *name,
                               Port input_port, Port output_port,
                               CnetOracleFnV2 fn,
                               CnetOracleValidateFn validator,
                               const CnetOracleIdentity *identity,
                               void *ctx) {
    OracleEntry *entry;
    uint64_t digest;
    size_t i;
    if (!o || !fn || !acquire_name_is_atom(name) ||
        o->count >= ACQUIRE_MAX_ORACLES) return -1;
    digest = cnet_oracle_identity_digest(identity);
    if (digest == 0) return -1;
    for (i = 0; i < o->count; ++i)
        if (strcmp(o->entries[i].name, name) == 0) return -1;
    entry = &o->entries[o->count];
    memset(entry, 0, sizeof *entry);
    snprintf(entry->name, ACQUIRE_NAME_MAX, "%s", name);
    entry->input_port = input_port;
    entry->output_port = output_port;
    entry->fn_v2 = fn;
    entry->validator = validator;
    entry->identity = *identity;
    entry->behavior_digest = digest;
    entry->ctx = ctx;
    o->count++;
    return 0;
}

int acquire_oracle_set_batch(OracleRegistry *o, const char *name,
                             CnetOracleBatchFn fn_batch, size_t batch_hint) {
    size_t i;
    if (!o || !fn_batch) return -1;
    for (i = 0; i < o->count; ++i) {
        if (strcmp(o->entries[i].name, name) == 0) {
            o->entries[i].fn_batch = fn_batch;
            o->entries[i].batch_hint = batch_hint ? batch_hint : 16;
            return 0;
        }
    }
    return -1;
}

int acquire_oracle_set_parallel(OracleRegistry *o, const char *name,
                                size_t width) {
    size_t i;
    if (!o || !name) return -1;
    for (i = 0; i < o->count; ++i) {
        if (strcmp(o->entries[i].name, name) == 0) {
            o->entries[i].parallel_width = width;
            return 0;
        }
    }
    return -1;
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
    if (cnet_oracle_invoke(o, cin, in_total, raw, out_total, NULL) !=
        CNET_ORACLE_ANSWER) return -1;
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
        if (reason && strcmp(reason, ACQUIRE_DEFER_WAITING_ORACLE) == 0)
            rep->defer_waiting_oracle++;
        else if (reason && strcmp(reason, "oracle_unfit") == 0)
            rep->defer_oracle_unfit++;
        else if (reason && strcmp(reason, "certify_failed") == 0)
            rep->defer_certify_failed++;
        else
            rep->defer_other++;
    }
}

/* Recipe fingerprint: FNV-1a over the config knobs that decide whether an
   acquisition can succeed. Byte-hashing covers size_t/double/unsigned
   uniformly and is deterministic per host. Deliberately excludes base,
   unit_dir, capture_limit, and the on_close hook — none change a verdict. */
static uint64_t recipe_fnv_bytes(uint64_t h, const void *p, size_t n) {
    const unsigned char *b = (const unsigned char *)p;
    size_t i;
    for (i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ULL; }
    return h;
}

uint64_t acquire_recipe_fingerprint(const AcquireConfig *cfg) {
    uint64_t h = 1469598103934665603ULL;
    if (!cfg) return h ? h : 1;
    h = recipe_fnv_bytes(h, &cfg->mine_budget, sizeof cfg->mine_budget);
    h = recipe_fnv_bytes(h, &cfg->sample_count, sizeof cfg->sample_count);
    h = recipe_fnv_bytes(h, &cfg->pilot_count, sizeof cfg->pilot_count);
    h = recipe_fnv_bytes(h, &cfg->holdout_fraction, sizeof cfg->holdout_fraction);
    h = recipe_fnv_bytes(h, &cfg->evidence_threshold, sizeof cfg->evidence_threshold);
    h = recipe_fnv_bytes(h, &cfg->min_evidence, sizeof cfg->min_evidence);
    h = recipe_fnv_bytes(h, &cfg->min_accuracy_bound, sizeof cfg->min_accuracy_bound);
    h = recipe_fnv_bytes(h, &cfg->wilson_z, sizeof cfg->wilson_z);
    h = recipe_fnv_bytes(h, &cfg->exhaustive_cap, sizeof cfg->exhaustive_cap);
    h = recipe_fnv_bytes(h, &cfg->init_hidden, sizeof cfg->init_hidden);
    h = recipe_fnv_bytes(h, &cfg->max_hidden, sizeof cfg->max_hidden);
    h = recipe_fnv_bytes(h, &cfg->learning_rate, sizeof cfg->learning_rate);
    h = recipe_fnv_bytes(h, &cfg->seed, sizeof cfg->seed);
    h = recipe_fnv_bytes(h, &cfg->max_epochs, sizeof cfg->max_epochs);
    h = recipe_fnv_bytes(h, &cfg->growth_window, sizeof cfg->growth_window);
    h = recipe_fnv_bytes(h, &cfg->target_loss, sizeof cfg->target_loss);
    h = recipe_fnv_bytes(h, &cfg->min_improvement, sizeof cfg->min_improvement);
    h = recipe_fnv_bytes(h, &cfg->momentum, sizeof cfg->momentum);
    return h ? h : 1;
}

/* A defer reason is recipe-dependent when a change to some AcquireConfig knob
   could flip it to success: the student-capacity/training failures and the
   mining/certification-bar failures. Structural failures (unbounded domain,
   base/registry refusals, unknown/multi-port subject) and the deliberate
   incumbent_healthy no-op are NOT — no recipe change rescues them, so they
   are never auto-reopened. */
static int reason_is_recipe_dependent(const char *reason) {
    return strcmp(reason, "certify_failed") == 0 ||
           strcmp(reason, "accuracy_bound") == 0 ||
           strcmp(reason, "insufficient_exemplars") == 0 ||
           strcmp(reason, "oracle_unfit") == 0 ||
           strcmp(reason, "class_imbalance") == 0;
}

/* True for a DEFERRED record whose recipe-dependent deferral predates the
   current recipe fingerprint — a candidate for an automatic retry. */
static int gap_recipe_stale(const GapRecord *g, uint64_t current_fp) {
    return g->status == GAP_DEFERRED &&
           reason_is_recipe_dependent(g->defer_reason) &&
           g->recipe_fp != current_fp;
}

/* A missing oracle is an external dependency, not active work. Park the gap
   after one miss so every daemon tick does not re-examine the same signature.
   The drain wakes it automatically as soon as a matching oracle is bound. */
static int gap_waiting_oracle(const GapRecord *g) {
    return g && g->kind == GAP_NO_PLAN && g->status == GAP_DEFERRED &&
           strcmp(g->defer_reason, ACQUIRE_DEFER_WAITING_ORACLE) == 0;
}

/* Reopen a recipe-stale deferral: back to OPEN with the reason cleared. The
   stamp is left as-is; a re-defer under the current recipe overwrites it,
   which is what stops the retry from repeating every drain (anti-churn). */
static void gap_reopen_recipe(GapRecord *g) {
    g->status = GAP_OPEN;
    g->defer_reason[0] = '\0';
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
   enumerable over budget  -> deterministic stride sample, preceded by a
   PILOT phase (confidence-scheduled acquisition, DSpark-inspired): a small
   domain-spanning pilot is mined first, and a degenerate pilot (all targets
   identical) aborts before the main mine -> -2 (caller defers
   class_imbalance at ~pilot cost instead of full-sample cost);
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
    size_t card, n_points, k, usable = 0, attempts = 0;
    size_t in_total = port_total(in_p), out_total = port_total(goal_p);
    size_t pilot = 0, pilot_idx[64];
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
    if (!*exhaustive_out && cfg->pilot_count > 0 &&
        cfg->pilot_count < n_points && cfg->pilot_count <= 64) {
        pilot = cfg->pilot_count;
    }

    inputs  = malloc((n_points + pilot) * in_total * sizeof *inputs);
    targets = malloc((n_points + pilot) * out_total * sizeof *targets);
    raw     = malloc(out_total * sizeof *raw);
    if (!inputs || !targets || !raw) {
        free(inputs); free(targets); free(raw);
        return -1;
    }

    /* pilot phase: spans the WHOLE domain with an ODD step — a step equal to
       a field width aliases the mixed-radix encoding and samples ONE value of
       the low field (measured: it made a wc-dependent map look constant) */
    for (k = 0; k < pilot; ++k) {
        size_t idx = (k * ((card / pilot) | 1)) % card;
        double *irow = inputs + usable * in_total;
        double *trow = targets + usable * out_total;
        pilot_idx[k] = idx;
        attempts++;
        if (contract_encode_domain_point(&tc, idx, irow) != 0) continue;
        if (cnet_oracle_invoke(o, irow, in_total, raw, out_total, NULL) !=
            CNET_ORACLE_ANSWER) continue;
        if (port_canonicalize(goal_p, raw, trow) != 0) continue;
        usable++;
    }
    if (pilot > 0 && usable >= 8) {
        size_t distinct = 1;
        for (k = 1; k < usable && distinct < 2; ++k)
            if (memcmp(targets, targets + k * out_total,
                       out_total * sizeof *targets) != 0) distinct = 2;
        if (distinct < 2) {
            free(inputs); free(targets); free(raw);
            return -2;   /* degenerate slice: refuse at pilot cost */
        }
    }

    /* main phase (skips indices the pilot already mined). An oracle
       registered PARALLEL (its fn dispatches per-thread state — e.g. one
       model instance per GPU) mines points into per-index slots
       concurrently, then compacts SERIALLY in index order: the exemplar
       table, every counter, and therefore every downstream digest are
       identical to the serial path by construction. */
    {
        size_t width = (o->parallel_width > 1) ? o->parallel_width : 1;
        unsigned char *st = NULL;          /* 0 dup, 1 encode_fail, 2 fn_fail, 3 mined */
        double *slot_in = NULL, *slot_raw = NULL;
#ifndef _OPENMP
        width = 1;
#endif
        /* v2 carries per-call metadata and semantic validation. Until a
           versioned batch result ABI exists it deliberately takes the single
           governed serial path instead of dropping that evidence. */
        if (o->fn_v2) width = 1;
        if (width > 1) {
            st = (unsigned char *)malloc(n_points);
            slot_in = (double *)malloc(n_points * in_total * sizeof *slot_in);
            slot_raw = (double *)malloc(n_points * out_total * sizeof *slot_raw);
            if (!st || !slot_in || !slot_raw) {
                free(st); free(slot_in); free(slot_raw);
                st = NULL; slot_in = NULL; slot_raw = NULL;
                width = 1;                 /* OOM: serial fallback, same result */
            }
        }
        if (width > 1) {
#ifdef _OPENMP
            long kk;
#pragma omp parallel for schedule(dynamic) num_threads((int)width) \
    default(none) \
    shared(n_points, card, pilot, pilot_idx, tc, o, st, slot_in, slot_raw, \
           in_total, out_total, exhaustive_out, goal_p)
            for (kk = 0; kk < (long)n_points; ++kk) {
                size_t base = *exhaustive_out ? (size_t)kk : ((size_t)kk * card) / n_points;
                size_t idx = base;
                if (!*exhaustive_out && card > 0) {
                    idx = (base + (goal_sample_mix(goal_p) % card)) % card;
                }
                size_t p, dup = 0;
                for (p = 0; p < pilot && !dup; ++p) dup = (pilot_idx[p] == idx);
                if (dup) { st[kk] = 0; continue; }
                if (contract_encode_domain_point(
                        &tc, idx, slot_in + (size_t)kk * in_total) != 0) {
                    st[kk] = 1;
                    continue;
                }
                if (o->fn_batch) {
                    st[kk] = 5;   /* encoded; probed by the batch pass below */
                } else {
                    int rc = o->fn(slot_in + (size_t)kk * in_total,
                                   slot_raw + (size_t)kk * out_total, o->ctx);
                    st[kk] = (rc < 0) ? 2 : (rc > 0) ? 4 : 3;  /* 4=abstain */
                }
            }
#endif
            /* Batched probe pass: gather encoded points into contiguous
               staging chunks and let the oracle answer batch_hint points
               per call (GEMV -> GEMM inside the teacher). Chunks fan out
               across lanes exactly like the per-point path; per-point
               verdicts land back in st[] so compaction below is unchanged
               and the mined table stays byte-identical to serial mining. */
            if (o->fn_batch) {
                size_t bh = o->batch_hint ? o->batch_hint : 16;
                size_t *pend = (size_t *)malloc(n_points * sizeof *pend);
                size_t npend = 0, ck;
                double *bin = (double *)malloc(bh * in_total * sizeof *bin);
                double *bout = (double *)malloc(bh * out_total * sizeof *bout);
                int *brcs = (int *)malloc(bh * sizeof *brcs);
                if (pend && bin && bout && brcs) {
                    size_t kk2;
                    for (kk2 = 0; kk2 < n_points; ++kk2)
                        if (st[kk2] == 5) pend[npend++] = kk2;
                    for (ck = 0; ck < npend; ck += bh) {
                        size_t cnt = (npend - ck < bh) ? npend - ck : bh;
                        size_t bi;
                        int brc;
                        for (bi = 0; bi < cnt; ++bi)
                            memcpy(bin + bi * in_total,
                                   slot_in + pend[ck + bi] * in_total,
                                   in_total * sizeof *bin);
                        brc = o->fn_batch(bin, bout, brcs, cnt, o->ctx);
                        for (bi = 0; bi < cnt; ++bi) {
                            size_t sk = pend[ck + bi];
                            if (brc < 0) {
                                int rc = o->fn(slot_in + sk * in_total,
                                               slot_raw + sk * out_total,
                                               o->ctx);
                                st[sk] = (rc < 0) ? 2 : (rc > 0) ? 4 : 3;
                            } else {
                                memcpy(slot_raw + sk * out_total,
                                       bout + bi * out_total,
                                       out_total * sizeof *bout);
                                st[sk] = (brcs[bi] < 0) ? 2
                                         : (brcs[bi] > 0) ? 4 : 3;
                            }
                        }
                    }
                } else {
                    size_t kk2;
                    for (kk2 = 0; kk2 < n_points; ++kk2)
                        if (st[kk2] == 5) {
                            int rc = o->fn(slot_in + kk2 * in_total,
                                           slot_raw + kk2 * out_total,
                                           o->ctx);
                            st[kk2] = (rc < 0) ? 2 : (rc > 0) ? 4 : 3;
                        }
                }
                free(pend); free(bin); free(bout); free(brcs);
            }
            for (k = 0; k < n_points; ++k) {
                if (st[k] == 0) continue;                  /* pilot duplicate */
                attempts++;
                if (st[k] == 1) continue;                  /* encode failed */
                o->calls++;
                if (st[k] == 2) { o->rejects++; continue; }
                if (st[k] == 4) { o->abstains++; continue; }  /* teacher-ambiguous */
                if (!port_validate(goal_p, slot_raw + k * out_total)) {
                    o->rejects++;
                    continue;
                }
                if (port_canonicalize(goal_p, slot_raw + k * out_total,
                                      targets + usable * out_total) != 0)
                    continue;
                memcpy(inputs + usable * in_total, slot_in + k * in_total,
                       in_total * sizeof *inputs);
                usable++;
            }
            free(st); free(slot_in); free(slot_raw);
        } else {
            for (k = 0; k < n_points; ++k) {
                size_t base = *exhaustive_out ? k : (k * card) / n_points; /* stride */
                size_t idx = base;
                if (!*exhaustive_out && card > 0) {
                    idx = (base + (goal_sample_mix(goal_p) % card)) % card;
                }
                double *irow = inputs + usable * in_total;
                double *trow = targets + usable * out_total;
                size_t p, dup = 0;
                for (p = 0; p < pilot && !dup; ++p) dup = (pilot_idx[p] == idx);
                if (dup) continue;
                attempts++;
                if (contract_encode_domain_point(&tc, idx, irow) != 0) continue;
                if (cnet_oracle_invoke(o, irow, in_total, raw, out_total, NULL) !=
                    CNET_ORACLE_ANSWER) continue;
                if (port_canonicalize(goal_p, raw, trow) != 0) continue;
                usable++;
            }
        }
    }
    free(raw);
    *inputs_out = inputs;
    *targets_out = targets;
    *n_out = usable;
    *attempts_out = attempts;
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
    double bound;

    g->attempts++;
    snprintf(g->oracle, ACQUIRE_NAME_MAX, "%s", o->name);

    /* candidate name: acq_<goal tag>, falling back to a counter */
    if (g->goal_port.tag[0])
        snprintf(name, sizeof name, "acq_%s", g->goal_port.tag);
    else
        snprintf(name, sizeof name, "acq_gap%lu",
                 (unsigned long)(g - l->gaps));

    /* 1. mine (pilot-scheduled: -2 = degenerate slice caught at pilot cost) */
    {
        int mrc = mine_from_oracle(o, g->input_port, g->goal_port, cfg,
                                   &inputs, &targets, &usable, &attempts,
                                   &exhaustive);
        if (mrc != 0) {
            gap_defer(g, rep, mrc == -2 ? "class_imbalance"
                                        : "unbounded_domain");
            return -1;
        }
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
    btn_set_momentum(btn, cfg->momentum);   /* 0 = plain SGD (default) */
    {
    int warm_used = 0;
    if (ws_on() && ws_snap.valid) {
        ws_apply(btn);
        warm_used = 1;
    }

train_student:
    /* Adaptive staged training (CNET_ACQ_ADAPTIVE=1): exactness on the
       mined table is the certification bar, so check it BETWEEN training
       stages — stop the instant the student is exact (passing units
       typically need a fraction of the epoch budget) and stop early when
       the exact count plateaus below the bar (the residue is the teacher's
       own coin-flip points; more epochs never close it). The certification
       path below is unchanged — this only decides how much of the budget
       to spend before it runs. */
    {
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
    if (getenv("CNET_ACQ_ADAPTIVE") && getenv("CNET_ACQ_ADAPTIVE")[0] == '1') {
        /* Exactness is checked between stages, so MORE stages = a tighter
           budget granularity: a fast-converging student stops the instant it
           is exact instead of after a coarse 1/8 of max_epochs. The check
           (btn_certify, one forward over the mined table) costs ~one epoch, so
           finer staging is nearly free and only ever stops SOONER — the
           certification bar below is unchanged. CNET_ACQ_STAGES (default 8). */
        size_t stages = 8;
        {
            const char *sv = getenv("CNET_ACQ_STAGES");
            if (sv && sv[0]) {
                long v = atol(sv);
                if (v >= 1 && (size_t)v <= cfg->max_epochs)
                    stages = (size_t)v;
            }
        }
        size_t s, prev_pass = 0, plateau = 0;
        Contract probe;
        int have_probe =
            (contract_init_borrowed(&probe, name, btn, inputs, targets,
                                    usable) == 0);
        /* Give-up window in STAGES scales with granularity so it stays a fixed
           fraction of max_epochs (~1/4) regardless of stage count: finer
           staging must only check exactness sooner, never abandon a
           slow-but-still-improving student earlier. */
        size_t plateau_limit = stages / 4;
        if (plateau_limit < 2) plateau_limit = 2;
        for (s = 0; s < stages; ++s) {
            CertifyReport crep;
            btn_train_dynamic(btn, inputs, targets, n_train,
                              cfg->max_epochs / stages, cfg->growth_window,
                              cfg->target_loss, cfg->min_improvement);
            if (!have_probe) continue;
            if (btn_certify(btn, &probe, &crep) == 0) break;   /* exact */
            if (crep.passed <= prev_pass) {
                if (++plateau >= plateau_limit) break;  /* stuck below the bar */
            } else {
                plateau = 0;
            }
            prev_pass = crep.passed;
        }
        if (have_probe) contract_free(&probe);
    } else {
        btn_train_dynamic(btn, inputs, targets, n_train, cfg->max_epochs,
                          cfg->growth_window, cfg->target_loss,
                          cfg->min_improvement);
    }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        if (rep) {
            double ms = (double)(t1.tv_sec - t0.tv_sec) * 1000.0 +
                        (double)(t1.tv_nsec - t0.tv_nsec) / 1e6;
            rep->train_wall_ms += ms;
        }
    }

    /* Warm-start is a speed bet, never a semantics change: when the
       warm-started student is not exact on the mined table, the previous
       unit's basin hurt more than it helped (measured: one synthetic-gate
       unit fails under unconditional warm-start). Retrain from the cold
       init the pipeline would otherwise have used, so warm-start can only
       ever add speed. */
    if (warm_used) {
        Contract wcheck_c;
        int exact = 0;
        if (contract_init_borrowed(&wcheck_c, name, btn, inputs, targets,
                                   usable) == 0) {
            exact = (btn_certify(btn, &wcheck_c, NULL) == 0);
            contract_free(&wcheck_c);
        }
        if (!exact) {
            btn_free(btn);
            if (btn_init(btn, in_total, out_total, cfg->init_hidden,
                         cfg->max_hidden, cfg->learning_rate,
                         cfg->seed) != 0 ||
                btn_set_ports(btn, g->input_port, g->goal_port) != 0) {
                free(btn); free(inputs); free(targets);
                gap_defer(g, rep, "certify_failed");
                return -1;
            }
            btn_set_momentum(btn, cfg->momentum);
            warm_used = 0;
            goto train_student;
        }
    }
    }

    /* 4. certify: contract over the FULL mined table (holdout rows included:
       they were never trained on, so certification tests them) */
    /* btn_certify_exhaustive returns 0 IFF PROVEN — a successful SAMPLED
       certification returns -1, so judge by the VERDICT, never the rc
       (latent bug caught by the fuzzy-tier gate: every SAMPLED unit was
       being deferred as certify_failed). */
    memset(&ex, 0, sizeof ex);
    if (contract_init_borrowed(&c, name, btn, inputs, targets, usable) != 0) {
        btn_free(btn); free(btn); free(inputs); free(targets);
        gap_defer(g, rep, "certify_failed");
        return -1;
    }
    (void)btn_certify_exhaustive(btn, &c, cfg->exhaustive_cap, &ex);
    /* CNET_CERT_DIAG=1: the near-miss number the tier discards — how many of
       the enumerated exemplars the trained student already reproduces. The
       gap from here to 100% is exactly what stands between a real oracle and
       a certified unit (capacity/training question vs fundamental). */
    if (getenv("CNET_CERT_DIAG") && getenv("CNET_CERT_DIAG")[0] == '1') {
        fprintf(stderr, "CERT_DIAG %s: %zu/%zu exemplars exact, verdict=%d, "
                        "worst_margin=%.4f\n", name,
                ex.certify.passed, ex.certify.passed + ex.certify.failed,
                (int)ex.verdict, ex.min_margin_domain);
    }
    if (ex.verdict == CERT_REFUSED) {
        btn_free(btn); free(btn); free(inputs); free(targets);
        gap_defer(g, rep, "certify_failed");
        return -1;
    }
    bound = 1.0;
    if (ex.verdict != CERT_PROVEN) {
        bound = coverage_accuracy_lower_bound(
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
        if (rep) {
            FILE *sf = fopen(cnu_path, "rb");
            if (sf) {
                long n;
                if (fseek(sf, 0, SEEK_END) == 0 && (n = ftell(sf)) >= 0)
                    rep->student_bytes = (size_t)n;
                fclose(sf);
            }
        }
    }

    /* 6. register (name storage must outlive the registry: ledger-owned) */
    if (ledger_own_btn(l, btn, name) != 0 ||
        admit_native_btn(reg, btn,
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

    if (ws_on()) ws_capture(btn);
    free(inputs); free(targets);   /* contract borrowed them; done with both */
    g->status = GAP_CLOSED;
    snprintf(g->unit, ACQUIRE_NAME_MAX, "%s", name);
    if (cfg->on_close) cfg->on_close((size_t)(g - l->gaps), cfg->on_close_ctx);
    if (rep) {
        rep->closed++;
        rep->last_verdict = ex.verdict;
        rep->last_bound = bound;
        rep->last_min_margin = ex.domain_swept > 0 ? ex.min_margin_domain
                                                   : ex.certify.min_margin;
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
    double bound;

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

    {
        int mrc = mine_from_oracle(o, in_p, goal_p, cfg, &inputs, &targets,
                                   &usable, &attempts, &exhaustive);
        if (mrc != 0) {
            gap_defer(g, rep, mrc == -2 ? "class_imbalance"
                                        : "unbounded_domain");
            return -1;
        }
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
    btn_set_momentum(btn, cfg->momentum);
    btn_train_dynamic(btn, inputs, targets, usable, cfg->max_epochs,
                      cfg->growth_window, cfg->target_loss,
                      cfg->min_improvement);
    /* judge by VERDICT, not rc (rc==0 iff PROVEN; SAMPLED returns -1) */
    memset(&ex, 0, sizeof ex);
    if (contract_init_borrowed(&mined, name, btn, inputs, targets,
                               usable) != 0) {
        btn_free(btn); free(btn); free(inputs); free(targets);
        gap_defer(g, rep, "certify_failed"); return -1;
    }
    (void)btn_certify_exhaustive(btn, &mined, cfg->exhaustive_cap, &ex);
    if (ex.verdict == CERT_REFUSED) {
        btn_free(btn); free(btn); free(inputs); free(targets);
        gap_defer(g, rep, "certify_failed"); return -1;
    }
    bound = 1.0;
    if (ex.verdict != CERT_PROVEN) {
        bound = coverage_accuracy_lower_bound(ex.certify.passed,
                                              ex.certify.exemplars,
                                              cfg->wilson_z);
        if (bound < cfg->min_accuracy_bound) {
            btn_free(btn); free(btn); free(inputs); free(targets);
            gap_defer(g, rep, "accuracy_bound"); return -1;
        }
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
        admit_native_btn(reg, btn,
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
    snprintf(g->unit, ACQUIRE_NAME_MAX, "%s", name);
    if (cfg->on_close) cfg->on_close((size_t)(g - l->gaps), cfg->on_close_ctx);
    if (rep) {
        rep->closed++;
        rep->last_verdict = ex.verdict;
        rep->last_bound = bound;
        rep->last_min_margin = ex.domain_swept > 0 ? ex.min_margin_domain
                                                   : ex.certify.min_margin;
        snprintf(rep->last_unit_name, ACQUIRE_NAME_MAX, "%s", name);
    }
    return 0;
}

int acquire_drain(PrimitiveRegistry *reg, AcquireLedger *l,
                  OracleRegistry *oracles, const AcquireConfig *cfg,
                  AcquireReport *report) {
    size_t i;
    uint64_t fp;
    if (!reg || !l || !cfg) return -1;
    fp = acquire_recipe_fingerprint(cfg);
    /* Oracle-arrival retry: a parked NO_PLAN gap becomes active exactly when
       its matching teacher is present. Unsupported gaps otherwise stay quiet. */
    for (i = 0; i < l->count; ++i) {
        GapRecord *g = &l->gaps[i];
        if (gap_waiting_oracle(g) &&
            find_oracle(oracles, g->input_port, g->goal_port)) {
            g->status = GAP_OPEN;
            g->defer_reason[0] = '\0';
        }
    }
    /* Recipe-change retry: reopen recipe-dependent deferrals stamped under an
       older recipe, so a student/certification improvement re-attempts them
       (the no-churn re-note policy would otherwise strand them forever). A
       re-defer below re-stamps the current fp, so this fires once per change. */
    for (i = 0; i < l->count; ++i) {
        if (gap_recipe_stale(&l->gaps[i], fp)) {
            gap_reopen_recipe(&l->gaps[i]);
            if (report) report->recipe_reopened++;
        }
    }
    {
        size_t closed_this_drain = 0;
        for (i = 0; i < l->count; ++i) {
            GapRecord *g = &l->gaps[i];
            OracleEntry *o;
            if (g->status != GAP_OPEN) continue;
            /* Budgeted self-improve: stop after max successful closes. */
            if (cfg->max_closures_per_drain &&
                closed_this_drain >= cfg->max_closures_per_drain)
                break;
            if (report) report->examined++;
            if (g->kind == GAP_NO_PLAN) {
                o = find_oracle(oracles, g->input_port, g->goal_port);
                if (!o) {
                    if (report) report->skipped_no_oracle++;
                    gap_defer(g, report, ACQUIRE_DEFER_WAITING_ORACLE);
                    continue;
                }
                attempt_no_plan(reg, l, o, g, cfg, report);
            } else {
                attempt_rebuild(reg, l, oracles, g, cfg, report);
            }
            if (g->status == GAP_CLOSED) closed_this_drain++;
            /* stamp the recipe a fresh deferral happened under, so it will not
               reopen again until the recipe changes */
            if (g->status == GAP_DEFERRED) g->recipe_fp = fp;
        }
    }
    /* Aggregate oracle economics after the drain (best-effort). */
    if (report && oracles) {
        size_t oi;
        for (oi = 0; oi < oracles->count; ++oi) {
            report->total_oracle_calls += oracles->entries[oi].calls;
            report->total_oracle_rejects += oracles->entries[oi].rejects;
            report->total_oracle_abstains += oracles->entries[oi].abstains;
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
    {
        uint64_t fp = acquire_recipe_fingerprint(cfg);
        int rc;
        /* recipe-change retry for this one gap, same rule as the drain */
        if (gap_recipe_stale(&l->gaps[idx], fp)) {
            gap_reopen_recipe(&l->gaps[idx]);
            if (report) report->recipe_reopened++;
        }
        if (l->gaps[idx].status != GAP_OPEN) /* already CLOSED by an earlier run */
            return l->gaps[idx].status == GAP_CLOSED ? 0 : -1;
        if (report) report->examined++;
        o = find_oracle(oracles, input_port, goal_port);
        if (!o) {
            if (report) report->skipped_no_oracle++;
            gap_defer(&l->gaps[idx], report, ACQUIRE_DEFER_WAITING_ORACLE);
            l->gaps[idx].recipe_fp = fp;
            return -1;
        }
        rc = attempt_no_plan(reg, l, o, &l->gaps[idx], cfg, report);
        if (l->gaps[idx].status == GAP_DEFERRED) l->gaps[idx].recipe_fp = fp;
        return rc;
    }
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
    fprintf(f, "CNET_GAPS 4\n%lu\n", (unsigned long)l->count);
    for (i = 0; i < l->count; ++i) {
        const GapRecord *g = &l->gaps[i];
        fprintf(f, "%d %d %lu %lu %d %lu %lu %s %d %lu %lu %s %s %s %s %s %d %llu\n",
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
                str_or_dash(g->defer_reason),
                str_or_dash(g->unit),
                g->provenance_done ? 1 : 0,
                (unsigned long long)g->recipe_fp);
    }
    fclose(f);
    return 0;
}

int acquire_ledger_load(AcquireLedger *l, const char *path) {
    FILE *f;
    unsigned long count, i;
    int ver;
    AcquireLedger fresh;   /* parse into a temp; swap only on full success */
    if (!l || !path) return -1;
    f = fopen(path, "r");
    if (!f) return -1;
    {
        char magic[16];
        if (fscanf(f, "%15s %d\n", magic, &ver) != 2 ||
            strcmp(magic, "CNET_GAPS") != 0 ||
            ver < 1 || ver > 4) { fclose(f); return -1; }
    }
    if (fscanf(f, "%lu\n", &count) != 1) { fclose(f); return -1; }
    acquire_ledger_init(&fresh);
    for (i = 0; i < count; ++i) {
        int kind, status, in_fam, goal_fam;
        unsigned long hit, att, in_w, in_c, goal_w, goal_c;
        char in_tag[PORT_TAG_MAX], goal_tag[PORT_TAG_MAX];
        char subject[ACQUIRE_NAME_MAX], oracle[ACQUIRE_NAME_MAX];
        char reason[ACQUIRE_REASON_MAX];
        char unit[ACQUIRE_NAME_MAX];   /* v2 column; v1 rows load as "" */
        int done = 0;                  /* v3 column; older rows load as 0 */
        unsigned long long recipe_fp = 0; /* v4 column; older rows load as 0 */
        GapRecord *g;
        snprintf(unit, sizeof unit, "-");
        if (fscanf(f, "%d %d %lu %lu %d %lu %lu %31s %d %lu %lu %31s %63s %63s %63s",
                   &kind, &status, &hit, &att,
                   &in_fam, &in_w, &in_c, in_tag,
                   &goal_fam, &goal_w, &goal_c, goal_tag,
                   subject, oracle, reason) != 15 ||
            kind < 0 || kind > 2 || status < 0 || status > 2 ||
            (ver >= 2 && fscanf(f, " %63s", unit) != 1) ||
            (ver >= 3 && (fscanf(f, " %d", &done) != 1 ||
                          done < 0 || done > 1)) ||
            (ver >= 4 && fscanf(f, " %llu", &recipe_fp) != 1)) {
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
        dash_to_str(g->unit, ACQUIRE_NAME_MAX, unit);
        g->provenance_done = done;
        g->recipe_fp = (uint64_t)recipe_fp;
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
