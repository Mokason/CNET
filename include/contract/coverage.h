#ifndef CONTRACT_COVERAGE_H
#define CONTRACT_COVERAGE_H

#include <stddef.h>

#include "../nn.h"
#include "contract.h"

/* ===========================================================================
 * The "proof vs sample" layer for the hybrid contract-type model.
 *
 * btn_certify guarantees behaviour on a contract's EXEMPLAR TABLE. When the
 * input domain is enumerable and the exemplars cover it, that is a PROOF; when
 * they are a subset it is only evidence. Nothing in the certificate told the
 * two apart -- so "certified" meant different strengths in different places.
 * This module makes the distinction first-class and mechanical:
 *
 *   Fix 1  btn_certify_exhaustive  -- certify + sweep the whole computed domain
 *   Fix 2  CoverageReport          -- proof / sample / unbounded, from the type
 *   Fix 3  registry_add_proven +   -- admit only proven primitives, and
 *          plan_weakest_verdict       propagate the weakest link end-to-end
 *   Fix 4  accuracy_lower_bound +  -- a statistical floor for the sampled case
 *          input_is_certified         and an input-side abstention region
 *   Fix 5  btn_check_totality      -- a domain-spanning law (every input -> a
 *                                      well-formed output), which point
 *                                      exemplars cannot assert
 *
 * The domain cardinality is DERIVED from the port signature you already have:
 *   ONEHOT   field: field_width values         -> field_width ^ field_count
 *   BINARY_* field: 2^field_width values        -> 2 ^ (field_width*field_count)
 *   RAW / EVIDENCE / CONCEPT: not enumerable.
 * ===========================================================================
 */

/* ---- Fix 2: coverage descriptor ---------------------------------------- */

typedef enum {
    COVERAGE_UNBOUNDED = 0, /* a non-enumerable input port, or a domain that
                               overflows size_t -- "exhaustive" is undefined */
    COVERAGE_SAMPLED,       /* enumerable, but exemplars < domain cardinality */
    COVERAGE_EXHAUSTIVE     /* every canonical input is an exemplar -> proof */
} CoverageKind;

/* Canonical value count of a single port. Returns 1 and writes *out on an
   enumerable family within size_t; returns 0 (out untouched) otherwise. */
int port_value_count(Port p, size_t *out);

/* Cardinality of a contract's canonical INPUT domain (product over input
   ports). Returns 1 and writes *out when enumerable within size_t; returns 0
   when any input port is non-enumerable or the product overflows. */
int contract_domain_cardinality(const Contract *c, size_t *out);

typedef struct {
    CoverageKind kind;
    size_t domain_cardinality;  /* 0 when unbounded */
    size_t distinct_exemplars;  /* distinct canonical input rows in the table */
    size_t uncovered;           /* cardinality - distinct (0 if exhaustive/unbounded) */
} CoverageReport;

/* Classify how a contract's exemplar table covers its own input domain.
   Returns 0 (always succeeds; an unbounded domain yields COVERAGE_UNBOUNDED). */
int contract_coverage(const Contract *c, CoverageReport *out);

/* Encode the index-th canonical input of the domain into buf (input total
   doubles). A bijection over [0, cardinality). Returns 0, or -1 if the domain
   is non-enumerable or index is out of range. */
int contract_encode_domain_point(const Contract *c, size_t index, double *buf);

/* ---- Fix 1: exhaustive certification ----------------------------------- */

typedef enum {
    CERT_REFUSED = 0, /* btn_certify failed: not even sample-correct */
    CERT_PROVEN,      /* certified AND exhaustive over an enumerable domain */
    CERT_SAMPLED,     /* certified, but the exemplars do not cover the domain */
    CERT_UNBOUNDED    /* certified, but the domain is not enumerable */
} CertVerdict;

typedef struct {
    CertVerdict  verdict;
    CoverageReport coverage;
    CertifyReport  certify;        /* the underlying btn_certify report */
    /* Full-domain well-formedness sweep (only when enumerable and <= cap): */
    size_t domain_swept;           /* canonical inputs run through the net */
    size_t domain_illformed;       /* inputs whose RAW output was OOD / ambiguous */
    double min_margin_domain;      /* worst output margin over the whole domain */
} ExhaustiveReport;

/* Certify and assign a verdict. When the domain is enumerable and <= cap the
   ENTIRE canonical domain is also swept through the net, counting inputs whose
   output is ill-formed (abstention-region candidates) and the worst margin.
   cap == 0 uses a built-in default. Returns 0 iff verdict == CERT_PROVEN. */
int btn_certify_exhaustive(BinaryTransformNetwork *btn, const Contract *c,
                           size_t cap, ExhaustiveReport *out);

/* ---- Fix 3: tiered admission + weakest-link propagation ----------------- */

/* Admit to the registry ONLY when the primitive is PROVEN (exhaustive certify
   succeeds). Mirrors registry_add_certified but rejects a merely-sampled or
   unbounded primitive. Returns 0 on admission, -1 on refusal. */
int registry_add_proven(PrimitiveRegistry *reg, BinaryTransformNetwork *btn,
                        const char *name, const Contract *c, size_t cap);

/* The end-to-end guarantee of a plan is its weakest primitive. Returns the
   weakest verdict over n (btn, contract) pairs:
   REFUSED if any refuses; else PROVEN if all prove; else SAMPLED if any is
   sampled; else UNBOUNDED. */
CertVerdict plan_weakest_verdict(BinaryTransformNetwork *const *btns,
                                 const Contract *const *contracts,
                                 size_t n, size_t cap);

/* ---- Fix 4: statistical bound + abstention region ---------------------- */

/* Wilson score lower bound on the true pass-rate given `passed` of `n`, at
   confidence multiplier z (1.96 = 95%, 2.576 = 99%). For the SAMPLED regime:
   an exhaustive domain is a proof and needs no bound. Result in [0, 1]. */
double coverage_accuracy_lower_bound(size_t passed, size_t n, double z);

/* Abstention gate: is `input` inside the contract's PROVEN region? Returns 1
   when it matches a canonical exemplar (a proven point), 0 otherwise (the
   caller should abstain rather than trust an off-sample extrapolation). For an
   exhaustive contract every canonical input is proven, so it never abstains. */
int coverage_input_is_certified(const Contract *c, const double *input);

/* ---- Fix 5: domain-spanning totality law ------------------------------- */

typedef struct {
    size_t domain;       /* canonical inputs in the enumerated domain */
    size_t well_formed;  /* outputs that validate + canonicalize cleanly */
    size_t ill_formed;   /* the rest */
    int    spans_domain; /* 1 iff the check ran over the FULL domain */
} TotalityReport;

/* Totality law: the net yields a well-formed canonical output for EVERY
   canonical input -- a guarantee point exemplars cannot make. Enumerates the
   whole domain (<= cap; cap == 0 -> default). Returns 0 iff it spanned the
   domain with zero ill-formed outputs, -1 otherwise. */
int btn_check_totality(BinaryTransformNetwork *btn, const Contract *c,
                       size_t cap, TotalityReport *out);

#endif /* CONTRACT_COVERAGE_H */





