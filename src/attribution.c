/* Core attribution layer (v1) — see include/attribution.h. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/attribution.h"
#include "../include/acquire.h"   /* ACQUIRE_NAME_MAX, for the size assert only */

/* ATTRIB_NAME_MAX and ACQUIRE_NAME_MAX are declared in separate headers to
   avoid a cycle; they MUST stay equal. Fails the build if they diverge. */
typedef char attrib_name_max_matches_acquire[
    (ATTRIB_NAME_MAX == ACQUIRE_NAME_MAX) ? 1 : -1];

/* The complete defer-atom set, verified by enumerating every gap_defer call
   site in src/acquire.c plus the two indirect producers: base_precheck
   (tag_collision, register_refused) and the class_imbalance/unbounded_domain
   ternary in attempt_no_plan. ACQUIRE_DEFER_WAITING_ORACLE is "waiting_oracle".

   The rule: the oracle-evidence gate (evidence_threshold, min_evidence) runs
   BEFORE any training step. Dying at or before it means the labels were
   unusable => proposer fault. Dying after it means the labels were fine and
   the student could not fit => system fault.

   tag_collision is SYSTEM_FAULT in v1 because the creative hemisphere does
   not mint tags (spec section 9) — the tags come from the gap's ports, so
   charging the proposer for a tag it did not choose would corrupt the trust
   number. Revisit if tag minting is ever delegated. */
static const struct { const char *atom; AttribVerdict verdict; } ATOMS[] = {
    { "waiting_oracle",         ATTRIB_BLAMELESS },
    { "incumbent_healthy",      ATTRIB_BLAMELESS },
    { "unknown_subject",        ATTRIB_BLAMELESS },
    { "multi_port_unsupported", ATTRIB_BLAMELESS },
    { "unbounded_domain",       ATTRIB_BLAMELESS },
    { "oracle_unfit",           ATTRIB_PROPOSER_FAULT },
    { "insufficient_exemplars", ATTRIB_PROPOSER_FAULT },
    { "class_imbalance",        ATTRIB_PROPOSER_FAULT },
    { "certify_failed",         ATTRIB_SYSTEM_FAULT },
    { "accuracy_bound",         ATTRIB_SYSTEM_FAULT },
    { "seal_failed",            ATTRIB_SYSTEM_FAULT },
    { "register_refused",       ATTRIB_SYSTEM_FAULT },
    { "replan_failed",          ATTRIB_SYSTEM_FAULT },
    { "tag_collision",          ATTRIB_SYSTEM_FAULT }
};
#define ATOM_COUNT (sizeof ATOMS / sizeof ATOMS[0])

void attrib_ledger_init(AttribLedger *L) {
    if (!L) return;
    memset(L, 0, sizeof *L);
}

AttribVerdict attrib_classify(const char *reason_atom, int admitted) {
    size_t i;
    if (admitted) return ATTRIB_ADMITTED;
    if (!reason_atom || !reason_atom[0]) return ATTRIB_UNCLASSIFIED;
    for (i = 0; i < ATOM_COUNT; ++i)
        if (strcmp(reason_atom, ATOMS[i].atom) == 0) return ATOMS[i].verdict;
    return ATTRIB_UNCLASSIFIED;
}

/* Exact signature equality: family, field_width, field_count AND tag —
   the same rule acquire_port_eq_public applies. */
static int port_sig_eq(Port a, Port b) {
    return a.family == b.family &&
           a.field_width == b.field_width &&
           a.field_count == b.field_count &&
           strcmp(a.tag, b.tag) == 0;
}

static AttribRecord *find_mut(AttribLedger *L, const char *proposer, Port goal) {
    size_t i;
    for (i = 0; i < L->count; ++i)
        if (strcmp(L->keys[i].proposer, proposer) == 0 &&
            port_sig_eq(L->keys[i].goal, goal))
            return &L->keys[i];
    return NULL;
}

const AttribRecord *attrib_find(const AttribLedger *L, const char *proposer,
                                Port goal) {
    if (!L || !proposer) return NULL;
    return find_mut((AttribLedger *)L, proposer, goal);
}

static void note_recipe_fp(AttribRecord *r, uint64_t fp) {
    size_t i;
    if (fp == 0) return;
    for (i = 0; i < r->recipe_fp_count; ++i)
        if (r->recipe_fps[i] == fp) return;
    if (r->recipe_fp_count >= ATTRIB_MAX_RECIPE_FPS) return;
    r->recipe_fps[r->recipe_fp_count++] = fp;
}

int attrib_record(AttribLedger *L, const struct AttributionEvent *ev) {
    AttribRecord *r;
    const char *who;
    AttribVerdict v;
    if (!L || !ev) { if (L) L->dropped_events++; return -1; }
    who = ev->proposer ? ev->proposer : "";
    r = find_mut(L, who, ev->goal_port);
    if (!r) {
        /* Bounded by refusing NEW keys, never by evicting existing ones —
           eviction would silently corrupt a posterior. */
        if (L->count >= ATTRIB_MAX_KEYS) { L->dropped_events++; return -1; }
        r = &L->keys[L->count++];
        memset(r, 0, sizeof *r);
        snprintf(r->proposer, ATTRIB_NAME_MAX, "%s", who);
        r->goal = ev->goal_port;
        r->admitted_margin_min = 1.0;
    }
    v = attrib_classify(ev->reason, ev->admitted);
    switch (v) {
    case ATTRIB_ADMITTED:
        r->admitted++;
        r->admitted_card_sum += ev->domain_cardinality;
        if (ev->min_margin < r->admitted_margin_min)
            r->admitted_margin_min = ev->min_margin;
        if (ev->cert_verdict == (int)CERT_PROVEN) r->admitted_proof++;
        else if (ev->cert_verdict >= 0) r->admitted_sampled++;
        break;
    case ATTRIB_PROPOSER_FAULT: r->proposer_fault++; break;
    case ATTRIB_SYSTEM_FAULT:
        r->system_fault++;
        note_recipe_fp(r, ev->recipe_fp);
        break;
    case ATTRIB_BLAMELESS:      r->blameless++; break;
    case ATTRIB_UNCLASSIFIED:
    case ATTRIB_VERDICT_COUNT:
    default:                    r->unclassified++; break;
    }
    return 0;
}

double attrib_proposer_trust(const AttribRecord *r) {
    double good, total;
    if (!r) return 0.5;
    good = (double)(r->admitted + r->system_fault) + 1.0;
    total = (double)(r->admitted + r->system_fault + r->proposer_fault) + 2.0;
    return good / total;
}

double attrib_signature_yield(const AttribRecord *r) {
    double good, total;
    if (!r) return 0.5;
    good = (double)r->admitted + 1.0;
    total = (double)(r->admitted + r->system_fault) + 2.0;
    return good / total;
}

void attrib_sink(const struct AttributionEvent *ev, void *ctx) {
    (void)attrib_record((AttribLedger *)ctx, ev);
}
