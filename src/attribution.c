/* Core attribution layer (v1) — see include/attribution.h. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/attribution.h"
#include "../include/acquire.h"   /* ACQUIRE_NAME_MAX, for the size assert only */
#include "../include/contract/conformal.h"

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

/* ---- sidecar persistence ------------------------------------------------
   Empty strings are written as "-" so the field count stays fixed — the same
   convention acquire_ledger_save uses. */

static const char *sod(const char *s) { return s[0] ? s : "-"; }

static void dts(char *dst, size_t cap, const char *src) {
    if (strcmp(src, "-") == 0) { dst[0] = '\0'; return; }
    snprintf(dst, cap, "%s", src);
}

int attrib_ledger_save(const AttribLedger *L, const char *path) {
    FILE *f;
    size_t i, j;
    if (!L || !path) return -1;
    f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "CNET_ATTRIB 1\n%lu %lu\n",
            (unsigned long)L->count, (unsigned long)L->dropped_events);
    for (i = 0; i < L->count; ++i) {
        const AttribRecord *r = &L->keys[i];
        fprintf(f, "%s %d %lu %lu %s %lu %lu %lu %lu %lu %lu %.17g %lu %lu %lu",
                sod(r->proposer),
                (int)r->goal.family,
                (unsigned long)r->goal.field_width,
                (unsigned long)r->goal.field_count,
                sod(r->goal.tag),
                (unsigned long)r->admitted,
                (unsigned long)r->proposer_fault,
                (unsigned long)r->system_fault,
                (unsigned long)r->blameless,
                (unsigned long)r->unclassified,
                (unsigned long)r->admitted_card_sum,
                r->admitted_margin_min,
                (unsigned long)r->admitted_proof,
                (unsigned long)r->admitted_sampled,
                (unsigned long)r->recipe_fp_count);
        for (j = 0; j < r->recipe_fp_count; ++j)
            fprintf(f, " %llu", (unsigned long long)r->recipe_fps[j]);
        fprintf(f, "\n");
    }
    fclose(f);
    return 0;
}

int attrib_ledger_load(AttribLedger *L, const char *path) {
    FILE *f;
    unsigned long count, dropped, i, j;
    int ver;
    AttribLedger *fresh;   /* parse into a temp; swap only on full success */
    if (!L || !path) return -1;
    f = fopen(path, "r");
    if (!f) return -1;
    {
        char magic[16];
        if (fscanf(f, "%15s %d\n", magic, &ver) != 2 ||
            strcmp(magic, "CNET_ATTRIB") != 0 || ver != 1) {
            fclose(f); return -1;
        }
    }
    if (fscanf(f, "%lu %lu\n", &count, &dropped) != 2 ||
        count > ATTRIB_MAX_KEYS) { fclose(f); return -1; }
    fresh = (AttribLedger *)malloc(sizeof *fresh);
    if (!fresh) { fclose(f); return -1; }
    attrib_ledger_init(fresh);
    for (i = 0; i < count; ++i) {
        AttribRecord *r = &fresh->keys[i];
        char name[ATTRIB_NAME_MAX], tag[PORT_TAG_MAX];
        char decoded[PORT_TAG_MAX];
        unsigned long w, c, adm, pf, sf, bl, un, cs, ap, as, nfp;
        int fam;
        memset(r, 0, sizeof *r);
        if (fscanf(f, "%63s %d %lu %lu %31s %lu %lu %lu %lu %lu %lu %lf %lu %lu %lu",
                   name, &fam, &w, &c, tag, &adm, &pf, &sf, &bl, &un, &cs,
                   &r->admitted_margin_min, &ap, &as, &nfp) != 15 ||
            nfp > ATTRIB_MAX_RECIPE_FPS) {
            free(fresh); fclose(f); return -1;
        }
        dts(r->proposer, ATTRIB_NAME_MAX, name);
        r->goal.family = (PortFamily)fam;
        r->goal.field_width = (size_t)w;
        r->goal.field_count = (size_t)c;
        /* port_set_tag validates the atom, so a corrupt tag is refused rather
           than copied in blind. */
        dts(decoded, PORT_TAG_MAX, tag);
        if (decoded[0] && port_set_tag(&r->goal, decoded) != 0) {
            free(fresh); fclose(f); return -1;
        }
        r->admitted = (size_t)adm;
        r->proposer_fault = (size_t)pf;
        r->system_fault = (size_t)sf;
        r->blameless = (size_t)bl;
        r->unclassified = (size_t)un;
        r->admitted_card_sum = (size_t)cs;
        r->admitted_proof = (size_t)ap;
        r->admitted_sampled = (size_t)as;
        r->recipe_fp_count = (size_t)nfp;
        for (j = 0; j < nfp; ++j) {
            unsigned long long fp;
            if (fscanf(f, " %llu", &fp) != 1) {
                free(fresh); fclose(f); return -1;
            }
            r->recipe_fps[j] = (uint64_t)fp;
        }
    }
    fresh->count = (size_t)count;
    fresh->dropped_events = (size_t)dropped;
    fclose(f);
    *L = *fresh;          /* swap only now: parse fully succeeded */
    free(fresh);
    return 0;
}

/* ---- split-conformal calibration of proposer confidence ----------------
   The oracle is the ground-truth source, so it cannot be calibrated against
   itself. Calibration points are those with an INDEPENDENT verdict: a
   port_validate failure (always available, free) or a registered
   CnetOracleValidateFn result. Score = 1 - confidence over VALID points; the
   quantile at alpha is the threshold below which the proposer should abstain.
   INVALID points do not set the threshold — they measure it, by counting how
   many the threshold would have caught. That count is the direct measurement
   of whether the ABSTAIN valve still works, which is what a future ternary
   (quantization-degraded) proposer would silently break. */

void attrib_calib_init(AttribCalib *c) {
    if (!c) return;
    memset(c, 0, sizeof *c);
}

int attrib_calib_add(AttribCalib *c, double confidence, int truth_valid) {
    if (!c) return -1;
    /* A non-finite or out-of-range confidence is a proposer ABI violation,
       not a calibration point. The !(x >= a) form also rejects NaN. */
    if (!(confidence >= 0.0) || !(confidence <= 1.0)) { c->dropped++; return -1; }
    if (truth_valid) {
        if (c->n_valid >= ATTRIB_MAX_CALIB) { c->dropped++; return -1; }
        c->valid_scores[c->n_valid++] = 1.0 - confidence;
    } else {
        if (c->n_invalid >= ATTRIB_MAX_CALIB) { c->dropped++; return -1; }
        c->invalid_conf[c->n_invalid++] = confidence;
    }
    return 0;
}

int attrib_calib_report(const AttribCalib *c, double alpha, size_t min_n,
                        AttribCalibReport *out) {
    size_t i;
    double *scratch;
    if (!c || !out) return -1;
    memset(out, 0, sizeof *out);
    out->alpha = alpha;
    out->n_valid = c->n_valid;
    out->n_invalid = c->n_invalid;
    /* Short evidence => uncalibratable. Never fabricate a threshold. */
    if (c->n_valid < min_n || c->n_valid == 0) return -1;
    /* conformal_quantile does not modify its input, but it takes a non-const
       pointer; copy rather than casting the const away. */
    scratch = (double *)malloc(c->n_valid * sizeof *scratch);
    if (!scratch) return -1;
    memcpy(scratch, c->valid_scores, c->n_valid * sizeof *scratch);
    out->q = conformal_quantile(scratch, c->n_valid, alpha);
    free(scratch);
    if (out->q < 0.0) return -1;   /* bad alpha; still not calibratable */
    /* An invalid answer is caught when its nonconformity exceeds the
       threshold, i.e. 1 - confidence > q. */
    for (i = 0; i < c->n_invalid; ++i)
        if ((1.0 - c->invalid_conf[i]) > out->q) out->caught_invalid++;
    out->calibratable = 1;
    return 0;
}
