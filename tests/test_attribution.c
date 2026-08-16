/* Core attribution layer — report-only observer gate.
   Sections are numbered; first failure exits non-zero. */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/attribution.h"

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

    free(L);
    printf("checks run: %d\n", checks_run);
    printf("ALL ATTRIBUTION TESTS PASSED\n");
    return 0;
}
