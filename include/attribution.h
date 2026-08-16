#ifndef ATTRIBUTION_H
#define ATTRIBUTION_H

/* Core attribution layer (v1): per-(proposer, goal signature) evidence that
   separates "the proposer supplied unusable labels" from "the student could
   not learn this signature". REPORT-ONLY: acquire never reads this back, so
   attaching the sink is provably a no-op (see the byte-identical drain gate
   in tests/test_attribution.c section 11).

   The blame boundary is not a new judgment — it is the STAGE at which an
   acquisition attempt died. The oracle-evidence gate (evidence_threshold,
   min_evidence) runs before a single training step, so dying at or before it
   means the labels were unusable, and dying after it means the labels were
   fine and the student could not fit.

   Spec: docs/superpowers/specs/2026-08-16-core-attribution-layer-design.md */

#include <stddef.h>
#include <stdint.h>

#include "nn.h"                 /* Port, PortFamily, PORT_TAG_MAX */
#include "contract/coverage.h"  /* CertVerdict */

/* Must equal ACQUIRE_NAME_MAX. Declared separately so acquire.h can
   forward-declare struct AttributionEvent without a header cycle; the
   equality is asserted at compile time in src/attribution.c. */
#define ATTRIB_NAME_MAX 64

#define ATTRIB_MAX_KEYS 256
#define ATTRIB_MAX_RECIPE_FPS 8
#define ATTRIB_MAX_CALIB 512

typedef enum {
    ATTRIB_ADMITTED = 0,
    ATTRIB_BLAMELESS = 1,        /* excluded from every denominator */
    ATTRIB_PROPOSER_FAULT = 2,   /* died at or before the evidence gate */
    ATTRIB_SYSTEM_FAULT = 3,     /* cleared the gate; recipe or domain */
    ATTRIB_UNCLASSIFIED = 4,     /* unknown atom: counted, never guessed */
    ATTRIB_VERDICT_COUNT = 5
} AttribVerdict;

/* One acquisition attempt, emitted by acquire_drain / acquire_now.
   Pointer fields are BORROWED for the duration of the call only. */
struct AttributionEvent {
    const char *proposer;       /* matched oracle name; "" when none */
    Port input_port;
    Port goal_port;             /* the key's signature */
    const char *reason;         /* defer atom; "" when admitted */
    int admitted;               /* 1 = reached GAP_CLOSED */
    uint64_t recipe_fp;
    size_t domain_cardinality;  /* certified domain size; 0 = unknown */
    double min_margin;          /* worst certified output margin */
    int cert_verdict;           /* CertVerdict as int; -1 = not applicable */
};

typedef struct {
    char proposer[ATTRIB_NAME_MAX];
    Port goal;
    size_t admitted;
    size_t proposer_fault;
    size_t system_fault;
    size_t blameless;
    size_t unclassified;
    /* anti-triviality, recorded on ADMITTED only: a perfect admission rate
       over constant or tiny domains must not read as excellence */
    size_t admitted_card_sum;
    double admitted_margin_min;  /* 1.0 when admitted == 0 */
    size_t admitted_proof;
    size_t admitted_sampled;
    /* distinct recipe fingerprints under which SYSTEM_FAULT was seen:
       spread across several => domain-fault; repeated under one => recipe */
    uint64_t recipe_fps[ATTRIB_MAX_RECIPE_FPS];
    size_t recipe_fp_count;
} AttribRecord;

typedef struct {
    AttribRecord keys[ATTRIB_MAX_KEYS];
    size_t count;
    size_t dropped_events;  /* table full, or malformed event */
} AttribLedger;

/* Zeroes the ledger. */
void attrib_ledger_init(AttribLedger *L);

/* Verdict for one terminated attempt. admitted != 0 => ATTRIB_ADMITTED
   regardless of reason. An unrecognised atom => ATTRIB_UNCLASSIFIED. */
AttribVerdict attrib_classify(const char *reason_atom, int admitted);

/* Fold one event into the ledger. Returns 0, or -1 when the event was
   dropped (bad args, or table full) with dropped_events bumped. NEVER
   returns an error the caller is expected to act on. */
int attrib_record(AttribLedger *L, const struct AttributionEvent *ev);

/* Exact-signature lookup. NULL when absent. */
const AttribRecord *attrib_find(const AttribLedger *L, const char *proposer,
                                Port goal);

/* (admitted + system_fault + 1) / (admitted + system_fault + proposer_fault + 2)
   "When this proposer labels this signature, are the labels usable?"
   SYSTEM_FAULT sits in the numerator: it is explicitly not the proposer's
   fault. Zero evidence reads 0.5 (neutral, not blind). */
double attrib_proposer_trust(const AttribRecord *r);

/* (admitted + 1) / (admitted + system_fault + 2)
   "Given usable labels, does this signature certify at all?" Isolates
   recipe/domain difficulty from proposer quality. */
double attrib_signature_yield(const AttribRecord *r);

/* AcquireConfig.on_attempt adapter. ctx must be an AttribLedger*. */
void attrib_sink(const struct AttributionEvent *ev, void *ctx);

/* Sidecar persistence ("CNET_ATTRIB 1"), CNET_STATS rules: never inside a
   weight file; load REPLACES on success; missing or malformed file returns
   -1 with *L untouched (parse into a temp, swap only on full success).
   Write AFTER the base and the gap ledger in any checkpoint, so a persisted
   count never claims work the ledger does not hold. */
int attrib_ledger_save(const AttribLedger *L, const char *path);
int attrib_ledger_load(AttribLedger *L, const char *path);

/* Append one event to the raw event log ("CNET_ATTRIB_LOG 1" header, written
   once when the file is created). NEVER loaded on the hot path — this exists
   so posteriors can be recomputed under a revised blame taxonomy without
   re-running a mining campaign. Returns 0, or -1 on bad args / IO failure;
   a failure here must never propagate into acquisition. */
int attrib_log_append(const char *path, const struct AttributionEvent *ev);

/* ---- split-conformal calibration of proposer confidence ----------------
   The oracle is the ground-truth source, so it cannot be calibrated against
   itself. Independent labels, in order of availability: a port_validate
   failure (definitively wrong, always available, free), or a registered
   CnetOracleValidateFn verdict. With neither, the honest answer is
   "uncalibratable" — never a fabricated threshold. Spec section 4. */

typedef struct {
    double valid_scores[ATTRIB_MAX_CALIB];   /* 1 - confidence, VALID points */
    size_t n_valid;
    double invalid_conf[ATTRIB_MAX_CALIB];   /* confidence, INVALID points */
    size_t n_invalid;
    size_t dropped;
} AttribCalib;

typedef struct {
    int calibratable;      /* 0 => q is meaningless */
    double q;              /* conformal threshold; accept iff 1-conf <= q */
    double alpha;          /* target miscoverage */
    size_t n_valid;
    size_t n_invalid;
    size_t caught_invalid; /* invalid answers this threshold would reject */
} AttribCalibReport;

void attrib_calib_init(AttribCalib *c);

/* truth_valid: 1 = independently confirmed valid, 0 = independently
   confirmed invalid. Do NOT call for UNDETERMINED points. Returns 0, or
   -1 when the point was dropped (buffer full / confidence out of range). */
int attrib_calib_add(AttribCalib *c, double confidence, int truth_valid);

/* Fills *out. Returns 0 when calibratable, or -1 when n_valid < min_n (out
   is still filled with the counts, and out->calibratable is 0). */
int attrib_calib_report(const AttribCalib *c, double alpha, size_t min_n,
                        AttribCalibReport *out);

#endif /* ATTRIBUTION_H */
