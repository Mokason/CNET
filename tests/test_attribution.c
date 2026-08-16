/* Core attribution layer — report-only observer gate.
   Sections are numbered; first failure exits non-zero. */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/attribution.h"
#include "../include/acquire.h"
#include "../include/router.h"

static int checks_run = 0;

static void check(int cond, const char *what) {
    ++checks_run;
    if (!cond) { printf("FAIL: %s\n", what); exit(1); }
    printf("  ok: %s\n", what);
}

static Port make_port(PortFamily fam, size_t w, size_t c, const char *tag) {
    Port p;
    memset(&p, 0, sizeof p);
    p.family = fam;
    p.field_width = w;
    p.field_count = c;
    if (port_set_tag(&p, tag) != 0) { printf("FAIL: bad tag %s\n", tag); exit(1); }
    return p;
}

/* Local mirror of the exact-signature rule, so the round-trip test asserts
   the port survived rather than trusting the lookup that found it. */
static int port_sig_eq_test(Port a, Port b) {
    return a.family == b.family && a.field_width == b.field_width &&
           a.field_count == b.field_count && strcmp(a.tag, b.tag) == 0;
}

static struct AttributionEvent ev_of(const char *proposer, Port goal,
                                     const char *reason, int admitted) {
    struct AttributionEvent e;
    memset(&e, 0, sizeof e);
    e.proposer = proposer;
    e.goal_port = goal;
    e.input_port = goal;
    e.reason = reason;
    e.admitted = admitted;
    e.cert_verdict = -1;
    e.min_margin = 1.0;
    return e;
}

/* ---- acquisition fixture (mirrors tests/test_acquire.c) ---------------- */

static void nibble_bits(unsigned v, double *out) {
    out[0] = (v >> 3) & 1u; out[1] = (v >> 2) & 1u;
    out[2] = (v >> 1) & 1u; out[3] = v & 1u;
}

static unsigned bits_nibble(const double *in) {
    return (unsigned)(((in[0] > 0.5) << 3) | ((in[1] > 0.5) << 2) |
                      ((in[2] > 0.5) << 1) | (in[3] > 0.5));
}

static int oracle_increment(const double *in, double *out, void *ctx) {
    (void)ctx;
    nibble_bits((bits_nibble(in) + 1u) & 0xFu, out);
    return 0;
}

/* Runs one complete acquisition against the 4-bit increment fixture.
   hook/ctx are installed on the config; pass NULL/NULL for the control run.
   Everything is rebuilt from scratch each call so the two runs are
   independent. Deliberately does NOT require the gap to close: the no-op
   guarantee is about the two runs being IDENTICAL, not about them
   succeeding, so this gate holds on any box regardless of whether the
   student certifies. */
static void run_fixture_drain(void (*hook)(const struct AttributionEvent *, void *),
                              void *ctx, AcquireReport *report) {
    PrimitiveRegistry reg;
    AcquireLedger led;
    OracleRegistry orc;
    AcquireConfig cfg;
    Port nib, nibn;

    registry_init(&reg);
    acquire_ledger_init(&led);
    memset(&orc, 0, sizeof orc);
    acquire_config_defaults(&cfg);
    cfg.unit_dir = NULL;          /* no sealing: keeps the gate hermetic */
    cfg.on_attempt = hook;
    cfg.on_attempt_ctx = ctx;

    nib  = make_port(PORT_BINARY_MSB, 4, 1, "nibble");
    nibn = make_port(PORT_BINARY_MSB, 4, 1, "nibble_next");

    acquire_oracle_register(&orc, "increment_ref", nib, nibn,
                            oracle_increment, NULL);
    acquire_note_no_plan(&led, nib, nibn);
    acquire_drain(&reg, &led, &orc, &cfg, report);

    acquire_ledger_free(&led);
    registry_free(&reg);
}

int main(void) {
    AttribLedger *L = (AttribLedger *)malloc(sizeof *L);
    Port g;
    if (!L) { printf("FAIL: alloc\n"); return 1; }
    g = make_port(PORT_BINARY_MSB, 4, 1, "nibble_next");

    printf("== attribution: report-only core ==\n");

    printf("[1] verdict classification\n");
    check(attrib_classify("certify_failed", 1) == ATTRIB_ADMITTED,
          "admitted wins over any reason");
    check(attrib_classify("waiting_oracle", 0) == ATTRIB_BLAMELESS,
          "waiting_oracle is blameless");
    check(attrib_classify("incumbent_healthy", 0) == ATTRIB_BLAMELESS,
          "incumbent_healthy is blameless");
    check(attrib_classify("unbounded_domain", 0) == ATTRIB_BLAMELESS,
          "unbounded_domain is blameless");
    check(attrib_classify("unknown_subject", 0) == ATTRIB_BLAMELESS,
          "unknown_subject is blameless");
    check(attrib_classify("multi_port_unsupported", 0) == ATTRIB_BLAMELESS,
          "multi_port_unsupported is blameless");
    check(attrib_classify("oracle_unfit", 0) == ATTRIB_PROPOSER_FAULT,
          "oracle_unfit is proposer fault");
    check(attrib_classify("class_imbalance", 0) == ATTRIB_PROPOSER_FAULT,
          "class_imbalance is proposer fault");
    check(attrib_classify("insufficient_exemplars", 0) == ATTRIB_PROPOSER_FAULT,
          "insufficient_exemplars is proposer fault");
    check(attrib_classify("certify_failed", 0) == ATTRIB_SYSTEM_FAULT,
          "certify_failed is system fault");
    check(attrib_classify("accuracy_bound", 0) == ATTRIB_SYSTEM_FAULT,
          "accuracy_bound is system fault");
    check(attrib_classify("seal_failed", 0) == ATTRIB_SYSTEM_FAULT,
          "seal_failed is system fault");
    check(attrib_classify("register_refused", 0) == ATTRIB_SYSTEM_FAULT,
          "register_refused is system fault");
    check(attrib_classify("replan_failed", 0) == ATTRIB_SYSTEM_FAULT,
          "replan_failed is system fault");
    check(attrib_classify("tag_collision", 0) == ATTRIB_SYSTEM_FAULT,
          "tag_collision is system fault in v1 (proposer does not mint tags)");
    check(attrib_classify("a_brand_new_atom", 0) == ATTRIB_UNCLASSIFIED,
          "unknown atom is UNCLASSIFIED, never guessed");
    check(attrib_classify(NULL, 0) == ATTRIB_UNCLASSIFIED,
          "NULL reason is UNCLASSIFIED");

    printf("[2] posteriors\n");
    attrib_ledger_init(L);
    {
        const AttribRecord *r;
        struct AttributionEvent e = ev_of("gemma", g, "", 1);
        check(attrib_record(L, &e) == 0, "record an admission");
        r = attrib_find(L, "gemma", g);
        check(r != NULL && r->admitted == 1, "key created and counted");
        check(fabs(attrib_proposer_trust(r) - 2.0 / 3.0) < 1e-12,
              "trust after 1 admission = 2/3");
        check(fabs(attrib_signature_yield(r) - 2.0 / 3.0) < 1e-12,
              "yield after 1 admission = 2/3");
    }

    printf("[3] cold key is neutral, not blind\n");
    {
        AttribRecord empty;
        memset(&empty, 0, sizeof empty);
        check(fabs(attrib_proposer_trust(&empty) - 0.5) < 1e-12,
              "zero evidence trust reads 0.5");
        check(fabs(attrib_signature_yield(&empty) - 0.5) < 1e-12,
              "zero evidence yield reads 0.5");
    }

    printf("[4] system fault does not blame the proposer\n");
    attrib_ledger_init(L);
    {
        const AttribRecord *r;
        int i;
        for (i = 0; i < 5; ++i) {
            struct AttributionEvent e = ev_of("gemma", g, "certify_failed", 0);
            e.recipe_fp = 1234u;
            check(attrib_record(L, &e) == 0, "record a system fault");
        }
        r = attrib_find(L, "gemma", g);
        check(r->system_fault == 5 && r->proposer_fault == 0,
              "5 system faults, 0 proposer faults");
        check(attrib_proposer_trust(r) > 0.5,
              "trust stays above 0.5 under pure system fault");
        check(attrib_signature_yield(r) < 0.5,
              "yield falls below 0.5 under pure system fault");
        check(r->recipe_fp_count == 1,
              "one distinct recipe fingerprint recorded (recipe-suspect)");
    }

    printf("[5] blameless is in no denominator\n");
    attrib_ledger_init(L);
    {
        const AttribRecord *r;
        struct AttributionEvent a = ev_of("gemma", g, "", 1);
        struct AttributionEvent b = ev_of("gemma", g, "waiting_oracle", 0);
        attrib_record(L, &a);
        attrib_record(L, &b);
        attrib_record(L, &b);
        r = attrib_find(L, "gemma", g);
        check(r->blameless == 2, "blameless counted separately");
        check(fabs(attrib_proposer_trust(r) - 2.0 / 3.0) < 1e-12,
              "trust unchanged by blameless events");
        check(fabs(attrib_signature_yield(r) - 2.0 / 3.0) < 1e-12,
              "yield unchanged by blameless events");
    }

    printf("[6] key identity is the exact goal signature\n");
    attrib_ledger_init(L);
    {
        Port other = make_port(PORT_BINARY_MSB, 4, 1, "nibble");
        Port wider = make_port(PORT_BINARY_MSB, 8, 1, "nibble_next");
        struct AttributionEvent e1 = ev_of("gemma", g, "", 1);
        struct AttributionEvent e2 = ev_of("gemma", other, "", 1);
        struct AttributionEvent e3 = ev_of("gemma", wider, "", 1);
        struct AttributionEvent e4 = ev_of("other_llm", g, "", 1);
        attrib_record(L, &e1);
        attrib_record(L, &e2);
        attrib_record(L, &e3);
        attrib_record(L, &e4);
        check(L->count == 4, "tag, width and proposer each split the key");
        check(attrib_find(L, "gemma", g)->admitted == 1, "lookup is exact");
        check(attrib_find(L, "nobody", g) == NULL, "absent key returns NULL");
    }

    printf("[7] bounded, never allocating, never failing upward\n");
    attrib_ledger_init(L);
    {
        size_t i;
        for (i = 0; i < ATTRIB_MAX_KEYS + 10; ++i) {
            char tag[PORT_TAG_MAX];
            Port p;
            struct AttributionEvent e;
            snprintf(tag, sizeof tag, "t%lu", (unsigned long)i);
            p = make_port(PORT_BINARY_MSB, 4, 1, tag);
            e = ev_of("gemma", p, "", 1);
            attrib_record(L, &e);
        }
        check(L->count == ATTRIB_MAX_KEYS, "key table saturates at the cap");
        check(L->dropped_events == 10, "overflow counted as dropped, not evicted");
    }
    check(attrib_record(NULL, NULL) == -1, "NULL args refused without crashing");

    printf("[8] anti-triviality columns\n");
    attrib_ledger_init(L);
    {
        const AttribRecord *r;
        struct AttributionEvent e = ev_of("gemma", g, "", 1);
        e.domain_cardinality = 16;
        e.min_margin = 0.25;
        e.cert_verdict = (int)CERT_PROVEN;
        attrib_record(L, &e);
        e.domain_cardinality = 4;
        e.min_margin = 0.10;
        attrib_record(L, &e);
        r = attrib_find(L, "gemma", g);
        check(r->admitted_card_sum == 20, "cardinality accumulates");
        check(fabs(r->admitted_margin_min - 0.10) < 1e-12,
              "worst margin retained, not averaged");
        check(r->admitted_proof == 2, "PROVEN admissions counted");
    }

    printf("[9] sidecar round-trip\n");
    attrib_ledger_init(L);
    {
        AttribLedger *M = (AttribLedger *)malloc(sizeof *M);
        struct AttributionEvent a = ev_of("gemma", g, "", 1);
        struct AttributionEvent b = ev_of("gemma", g, "certify_failed", 0);
        struct AttributionEvent c = ev_of("gemma", g, "oracle_unfit", 0);
        if (!M) { printf("FAIL: alloc\n"); return 1; }
        a.domain_cardinality = 16;
        a.min_margin = 0.25;
        a.cert_verdict = (int)CERT_PROVEN;
        b.recipe_fp = 99u;
        attrib_record(L, &a);
        attrib_record(L, &b);
        attrib_record(L, &c);

        check(attrib_ledger_save(L, "logs/attrib_rt.stats") == 0, "save ok");
        attrib_ledger_init(M);
        check(attrib_ledger_load(M, "logs/attrib_rt.stats") == 0, "load ok");
        check(M->count == L->count, "key count round-trips");
        {
            const AttribRecord *r = attrib_find(M, "gemma", g);
            check(r != NULL, "key found after load");
            check(r->admitted == 1 && r->system_fault == 1 &&
                  r->proposer_fault == 1, "counters round-trip");
            check(r->admitted_card_sum == 16, "cardinality round-trips");
            check(fabs(r->admitted_margin_min - 0.25) < 1e-9,
                  "margin round-trips");
            check(r->admitted_proof == 1, "PROVEN count round-trips");
            check(r->recipe_fp_count == 1 && r->recipe_fps[0] == 99u,
                  "recipe fingerprints round-trip");
            check(port_sig_eq_test(r->goal, g), "goal signature round-trips");
            check(fabs(attrib_proposer_trust(r) -
                       attrib_proposer_trust(attrib_find(L, "gemma", g))) < 1e-12,
                  "posterior identical after round-trip");
        }

        printf("[10] malformed load leaves state untouched\n");
        {
            FILE *bad = fopen("logs/attrib_bad.stats", "w");
            size_t before;
            if (!bad) { printf("FAIL: fopen\n"); return 1; }
            fprintf(bad, "CNET_ATTRIB 1\n3\ngarbage not a record\n");
            fclose(bad);
            before = M->count;
            check(attrib_ledger_load(M, "logs/attrib_bad.stats") == -1,
                  "malformed file refused");
            check(M->count == before, "ledger untouched after refusal");
        }
        {
            FILE *bad = fopen("logs/attrib_magic.stats", "w");
            if (!bad) { printf("FAIL: fopen\n"); return 1; }
            fprintf(bad, "CNET_GAPS 1\n0 0\n");
            fclose(bad);
            check(attrib_ledger_load(M, "logs/attrib_magic.stats") == -1,
                  "wrong magic refused");
        }
        check(attrib_ledger_load(M, "logs/does_not_exist.stats") == -1,
              "missing file refused");
        check(M->count > 0, "ledger still untouched after missing file");
        free(M);
    }

    printf("[11] sidecar fuzz: byte flips + truncations\n");
    /* Mirrors the sweep registered in tests/test_mutate.c, run here as well
       because the mutate target links the whole CCE layer and is not
       buildable on every box. An unsealed stats file need not refuse every
       mutation — but a REFUSAL must leave the ledger untouched, which is the
       stronger property: a parser that half-applied a corrupt file would
       still pass a crash-only sweep. */
    {
        AttribLedger *F = (AttribLedger *)malloc(sizeof *F);
        unsigned char *bytes = NULL;
        size_t len = 0, off, accepted = 0, tried = 0;
        FILE *f;
        if (!F) { printf("FAIL: alloc\n"); return 1; }

        f = fopen("logs/attrib_rt.stats", "rb");
        check(f != NULL, "fuzz seed sidecar readable");
        fseek(f, 0, SEEK_END);
        len = (size_t)ftell(f);
        fseek(f, 0, SEEK_SET);
        bytes = (unsigned char *)malloc(len ? len : 1);
        check(bytes != NULL && fread(bytes, 1, len, f) == len, "seed slurped");
        fclose(f);

        for (off = 0; off < len; ++off) {
            unsigned char save = bytes[off];
            bytes[off] = (unsigned char)(save ^ 0xFF);
            f = fopen("logs/attrib_fuzz.stats", "wb");
            if (!f) { printf("FAIL: fuzz write\n"); return 1; }
            fwrite(bytes, 1, len, f);
            fclose(f);
            bytes[off] = save;

            ++tried;
            attrib_ledger_init(F);
            F->dropped_events = 0xA5A5u;   /* sentinel */
            if (attrib_ledger_load(F, "logs/attrib_fuzz.stats") == 0) {
                ++accepted;
            } else if (F->count != 0 || F->dropped_events != 0xA5A5u) {
                printf("FAIL: refused load mutated the ledger at byte %lu\n",
                       (unsigned long)off);
                return 1;
            }
        }
        check(tried == len, "every byte position flipped");
        printf("  info: %lu/%lu flips accepted, rest refused with state "
               "untouched, 0 crashes\n",
               (unsigned long)accepted, (unsigned long)tried);

        for (off = 0; off <= len; ++off) {
            f = fopen("logs/attrib_fuzz.stats", "wb");
            if (!f) { printf("FAIL: fuzz write\n"); return 1; }
            fwrite(bytes, 1, off, f);
            fclose(f);
            attrib_ledger_init(F);
            F->dropped_events = 0xA5A5u;
            if (attrib_ledger_load(F, "logs/attrib_fuzz.stats") != 0 &&
                (F->count != 0 || F->dropped_events != 0xA5A5u)) {
                printf("FAIL: refused truncation mutated the ledger at %lu\n",
                       (unsigned long)off);
                return 1;
            }
        }
        check(1, "every truncation length refused cleanly or loaded validly");
        free(bytes);
        free(F);
    }

    printf("[12] attaching the sink is a no-op on acquisition\n");
    {
        /* Run the SAME acquisition twice: once with no sink, once with the
           sink attached. Every acquire-visible outcome must be identical.
           This IS the report-only guarantee — without it, "report-only" is a
           claim rather than a property. */
        AcquireReport r_off, r_on;
        AttribLedger *sink = (AttribLedger *)malloc(sizeof *sink);
        if (!sink) { printf("FAIL: alloc\n"); return 1; }
        attrib_ledger_init(sink);

        memset(&r_off, 0, sizeof r_off);
        memset(&r_on, 0, sizeof r_on);
        run_fixture_drain(NULL, NULL, &r_off);
        run_fixture_drain(attrib_sink, sink, &r_on);

        check(r_off.examined == r_on.examined, "examined identical");
        check(r_off.closed == r_on.closed, "closed identical");
        check(r_off.deferred == r_on.deferred, "deferred identical");
        check(r_off.skipped_no_oracle == r_on.skipped_no_oracle,
              "skipped_no_oracle identical");
        check(r_off.last_verdict == r_on.last_verdict, "verdict identical");
        check(fabs(r_off.last_bound - r_on.last_bound) < 1e-15,
              "accuracy bound identical");
        check(fabs(r_off.last_min_margin - r_on.last_min_margin) < 1e-15,
              "min margin identical");
        check(strcmp(r_off.last_unit_name, r_on.last_unit_name) == 0,
              "minted unit name identical");
        check(strcmp(r_off.last_defer_reason, r_on.last_defer_reason) == 0,
              "defer reason identical");
        check(r_off.total_oracle_calls == r_on.total_oracle_calls,
              "oracle call count identical");
        check(r_off.total_oracle_rejects == r_on.total_oracle_rejects,
              "oracle reject count identical");
        check(r_off.total_oracle_abstains == r_on.total_oracle_abstains,
              "oracle abstain count identical");
        check(r_off.defer_certify_failed == r_on.defer_certify_failed,
              "defer histogram identical");

        check(sink->count == 1, "the sink observed exactly one attempt");
        check(sink->dropped_events == 0, "nothing dropped");
        {
            const AttribRecord *r = &sink->keys[0];
            printf("  info: observed proposer='%s' admitted=%lu "
                   "proposer_fault=%lu system_fault=%lu trust=%.3f yield=%.3f\n",
                   r->proposer, (unsigned long)r->admitted,
                   (unsigned long)r->proposer_fault,
                   (unsigned long)r->system_fault,
                   attrib_proposer_trust(r), attrib_signature_yield(r));
            check(strcmp(r->proposer, "increment_ref") == 0,
                  "event carries the matched proposer name");
            check(strcmp(r->goal.tag, "nibble_next") == 0,
                  "event carries the goal signature");
            check(r->admitted + r->proposer_fault + r->system_fault +
                  r->blameless + r->unclassified == 1,
                  "exactly one verdict recorded for the attempt");
            check(r->unclassified == 0,
                  "the real drain produced no unclassified atom");
        }
        free(sink);
    }

    printf("[13] conformal calibration of proposer confidence\n");
    {
        AttribCalib cal;
        AttribCalibReport rep;
        int i;
        attrib_calib_init(&cal);

        check(attrib_calib_report(&cal, 0.1, 16, &rep) == -1,
              "empty calibration set is uncalibratable");
        check(rep.calibratable == 0, "report flags uncalibratable");

        /* 40 well-calibrated valid answers (high confidence) ... */
        for (i = 0; i < 40; ++i) attrib_calib_add(&cal, 0.90 + 0.001 * i, 1);
        /* ... and 10 invalid answers the proposer was overconfident about */
        for (i = 0; i < 10; ++i) attrib_calib_add(&cal, 0.60 + 0.001 * i, 0);

        check(attrib_calib_report(&cal, 0.1, 16, &rep) == 0,
              "calibratable once n_valid >= min_n");
        check(rep.calibratable == 1, "report flags calibratable");
        check(rep.n_valid == 40 && rep.n_invalid == 10,
              "valid and invalid counts tracked separately");
        check(rep.q >= 0.0 && rep.q <= 1.0, "threshold in range");
        printf("  info: q=%.4f -> abstain below confidence %.4f; "
               "catches %lu/%lu invalid answers\n",
               rep.q, 1.0 - rep.q, (unsigned long)rep.caught_invalid,
               (unsigned long)rep.n_invalid);
        check(rep.caught_invalid == 10,
              "threshold catches all clearly-overconfident invalid answers");

        printf("[14] min_n is honoured, not silently relaxed\n");
        {
            AttribCalib small;
            AttribCalibReport srep;
            attrib_calib_init(&small);
            for (i = 0; i < 5; ++i) attrib_calib_add(&small, 0.9, 1);
            check(attrib_calib_report(&small, 0.1, 16, &srep) == -1,
                  "5 points under min_n 16 is uncalibratable");
            check(srep.calibratable == 0, "flag stays 0");
            check(srep.n_valid == 5, "count still reported when uncalibratable");
        }

        printf("[15] calibration buffer is bounded, bad input refused\n");
        {
            AttribCalib big;
            size_t k;
            attrib_calib_init(&big);
            for (k = 0; k < ATTRIB_MAX_CALIB + 50; ++k)
                attrib_calib_add(&big, 0.9, 1);
            check(big.n_valid == ATTRIB_MAX_CALIB, "valid buffer saturates");
            check(big.dropped == 50, "overflow counted");
            attrib_calib_init(&big);
            check(attrib_calib_add(&big, 1.5, 1) == -1,
                  "confidence above 1 refused as an ABI violation");
            check(attrib_calib_add(&big, -0.1, 1) == -1,
                  "negative confidence refused");
            check(big.n_valid == 0 && big.dropped == 2,
                  "refused points never enter the calibration set");
        }
    }

    printf("[16] append-only event log\n");
    {
        struct AttributionEvent e = ev_of("gemma", g, "certify_failed", 0);
        FILE *f;
        int lines = 0, ch, prev = 0;
        remove("logs/attrib_events.log");
        e.recipe_fp = 7u;
        check(attrib_log_append("logs/attrib_events.log", &e) == 0,
              "first append ok");
        check(attrib_log_append("logs/attrib_events.log", &e) == 0,
              "second append ok");
        f = fopen("logs/attrib_events.log", "r");
        check(f != NULL, "log file exists");
        while ((ch = fgetc(f)) != EOF) { if (ch == '\n') lines++; prev = ch; }
        fclose(f);
        check(prev == '\n', "log ends on a newline");
        check(lines == 3, "header written once, plus two appended events");
        check(attrib_log_append(NULL, &e) == -1, "NULL path refused");
        check(attrib_log_append("logs/attrib_events.log", NULL) == -1,
              "NULL event refused");
    }

    free(L);
    printf("checks run: %d\n", checks_run);
    printf("ALL ATTRIBUTION TESTS PASSED\n");
    return 0;
}
