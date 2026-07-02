/* contract_coverage.c -- the "proof vs sample" layer for contract certification.
 * See include/contract_coverage.h for the rationale. Everything here is built
 * on the existing port helpers, btn_certify, btn_forward and the registry; no
 * core machinery is modified.
 */

#include "../../include/contract/coverage.h"
#include "../../include/nn.h"
#include "../../include/contract/contract.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <limits.h>

#define COVERAGE_DEFAULT_CAP ((size_t)1 << 16)   /* 65536 canonical inputs */

/* ---- geometry helpers --------------------------------------------------- */

static size_t port_total(Port p) {
    return p.field_width * p.field_count;
}

static size_t ports_sum_total(const Port *ports, size_t n) {
    size_t s = 0, i;
    for (i = 0; i < n; ++i) s += port_total(ports[i]);
    return s;
}

int port_value_count(Port p, size_t *out) {
    size_t per_field, total, i;

    switch (p.family) {
        case PORT_ONEHOT:
            per_field = p.field_width;          /* W categories per field */
            break;
        case PORT_BINARY_MSB:
        case PORT_BINARY_LSB:
            if (p.field_width == 0 || p.field_width >= sizeof(size_t) * CHAR_BIT)
                return 0;                        /* 2^fw overflows */
            per_field = ((size_t)1) << p.field_width;
            break;
        default:
            return 0;                            /* RAW / EVIDENCE / CONCEPT */
    }
    if (per_field == 0 || p.field_count == 0) return 0;

    total = 1;
    for (i = 0; i < p.field_count; ++i) {        /* per_field ^ field_count */
        if (total > ((size_t)-1) / per_field) return 0;  /* overflow */
        total *= per_field;
    }
    if (out) *out = total;
    return 1;
}

int contract_domain_cardinality(const Contract *c, size_t *out) {
    size_t total = 1, i;
    if (c == NULL || c->input_port_count == 0) return 0;

    for (i = 0; i < c->input_port_count; ++i) {
        size_t vc;
        if (!port_value_count(c->input_ports[i], &vc)) return 0;  /* unbounded */
        if (vc == 0 || total > ((size_t)-1) / vc) return 0;       /* overflow  */
        total *= vc;
    }
    if (out) *out = total;
    return 1;
}

/* Encode one canonical value `local` (in [0, value_count)) of a single port
   into its slice. The slice is zeroed first. */
static void encode_port_point(Port p, size_t local, double *slice) {
    size_t total = port_total(p), i;
    for (i = 0; i < total; ++i) slice[i] = 0.0;

    if (p.family == PORT_ONEHOT) {
        /* field_count one-hot fields, field 0 most significant (base W). */
        size_t f;
        for (f = 0; f < p.field_count; ++f) {
            size_t place = p.field_count - 1 - f;
            size_t denom = 1, k;
            for (k = 0; k < place; ++k) denom *= p.field_width;
            size_t digit = (local / denom) % p.field_width;
            slice[f * p.field_width + digit] = 1.0;
        }
    } else {  /* BINARY_MSB / BINARY_LSB: `total` bits, big- or little-endian */
        size_t b;
        for (b = 0; b < total; ++b) {
            size_t bit = (p.family == PORT_BINARY_MSB)
                             ? ((local >> (total - 1 - b)) & 1u)
                             : ((local >> b) & 1u);
            slice[b] = (double)bit;
        }
    }
}

int contract_encode_domain_point(const Contract *c, size_t index, double *buf) {
    size_t card, off = 0, pi;
    if (c == NULL || buf == NULL) return -1;
    if (!contract_domain_cardinality(c, &card) || index >= card) return -1;

    /* Port 0 varies fastest (least significant); a clean bijection over
       [0, card). The exact ordering is irrelevant -- only injectivity is. */
    for (pi = 0; pi < c->input_port_count; ++pi) {
        Port p = c->input_ports[pi];
        size_t vc = 0, local;
        if (!port_value_count(p, &vc) || vc == 0) return -1;  /* enumerable: never */
        local = index % vc;
        index /= vc;
        encode_port_point(p, local, buf + off);
        off += port_total(p);
    }
    return 0;
}

/* ---- Fix 2: coverage ---------------------------------------------------- */

/* Distinct canonical input rows in the exemplar table (O(n^2 * in_total), but
   exemplar tables are small -- a byte domain is 256 rows). */
static size_t count_distinct_inputs(const Contract *c, size_t in_total) {
    size_t distinct = 0, a, b;
    if (in_total == 0) return 0;
    for (a = 0; a < c->exemplar_count; ++a) {
        const double *ra = c->inputs + a * in_total;
        int dup = 0;
        for (b = 0; b < a; ++b) {
            const double *rb = c->inputs + b * in_total;
            if (memcmp(ra, rb, in_total * sizeof(double)) == 0) { dup = 1; break; }
        }
        if (!dup) ++distinct;
    }
    return distinct;
}

int contract_coverage(const Contract *c, CoverageReport *out) {
    CoverageReport r;
    size_t card, in_total;
    memset(&r, 0, sizeof r);
    if (c == NULL) { if (out) *out = r; return 0; }

    in_total = ports_sum_total(c->input_ports, c->input_port_count);
    r.distinct_exemplars = count_distinct_inputs(c, in_total);

    if (!contract_domain_cardinality(c, &card)) {
        r.kind = COVERAGE_UNBOUNDED;            /* non-enumerable / overflow */
    } else {
        r.domain_cardinality = card;
        if (r.distinct_exemplars >= card) {
            r.kind = COVERAGE_EXHAUSTIVE;        /* pigeonhole: covers the domain */
            r.uncovered = 0;
        } else {
            r.kind = COVERAGE_SAMPLED;
            r.uncovered = card - r.distinct_exemplars;
        }
    }
    if (out) *out = r;
    return 0;
}

/* ---- Fix 1: exhaustive certification ------------------------------------ */

/* Sweep every canonical input through the net; count ill-formed outputs and
   track the worst output margin. Returns 0 on success, -1 if not enumerable /
   over cap / on allocation failure. */
static int domain_wellformed_sweep(BinaryTransformNetwork *btn, const Contract *c,
                                   size_t cap, size_t *swept, size_t *illformed,
                                   double *worst_margin) {
    size_t card, in_total, out_total, idx;
    double *buf;
    double worst = 1.0;

    *swept = 0; *illformed = 0; *worst_margin = 0.0;
    if (!contract_domain_cardinality(c, &card)) return -1;
    if (cap == 0) cap = COVERAGE_DEFAULT_CAP;
    if (card > cap) return -1;

    in_total  = ports_sum_total(c->input_ports,  c->input_port_count);
    out_total = ports_sum_total(c->output_ports, c->output_port_count);
    if (in_total != btn->input_count || out_total != btn->output_count) return -1;

    buf = (double *)malloc(in_total * sizeof(double));
    if (buf == NULL) return -1;

    for (idx = 0; idx < card; ++idx) {
        const double *raw;
        size_t off = 0, p;
        int ill = 0;
        contract_encode_domain_point(c, idx, buf);
        raw = btn_forward(btn, buf);
        if (raw == NULL) { ++(*illformed); continue; }
        for (p = 0; p < c->output_port_count; ++p) {
            size_t tot = port_total(c->output_ports[p]);
            double mm;
            if (port_margin(c->output_ports[p], raw + off, &mm) == 0 && mm < worst)
                worst = mm;
            if (!port_validate(c->output_ports[p], raw + off)) ill = 1;
            off += tot;
        }
        ++(*swept);
        if (ill) ++(*illformed);
    }
    *worst_margin = worst;
    free(buf);
    return 0;
}

int btn_certify_exhaustive(BinaryTransformNetwork *btn, const Contract *c,
                           size_t cap, ExhaustiveReport *out) {
    ExhaustiveReport r;
    int cert_ok;
    memset(&r, 0, sizeof r);
    r.min_margin_domain = 1.0;
    if (btn == NULL || c == NULL) { if (out) *out = r; return -1; }

    cert_ok = (btn_certify(btn, c, &r.certify) == 0);
    contract_coverage(c, &r.coverage);

    if (!cert_ok) {
        r.verdict = CERT_REFUSED;
    } else if (r.coverage.kind == COVERAGE_EXHAUSTIVE) {
        r.verdict = CERT_PROVEN;
    } else if (r.coverage.kind == COVERAGE_SAMPLED) {
        r.verdict = CERT_SAMPLED;
    } else {
        r.verdict = CERT_UNBOUNDED;
    }

    /* Best-effort full-domain sweep for the abstention-region signal. */
    if (r.coverage.kind != COVERAGE_UNBOUNDED) {
        size_t swept, ill; double worst;
        if (domain_wellformed_sweep(btn, c, cap, &swept, &ill, &worst) == 0) {
            r.domain_swept = swept;
            r.domain_illformed = ill;
            r.min_margin_domain = worst;
        }
    }

    if (out) *out = r;
    return r.verdict == CERT_PROVEN ? 0 : -1;
}

/* ---- Fix 3: tiered admission + propagation ------------------------------ */

int registry_add_proven(PrimitiveRegistry *reg, BinaryTransformNetwork *btn,
                        const char *name, const Contract *c, size_t cap) {
    ExhaustiveReport r;
    if (btn_certify_exhaustive(btn, c, cap, &r) != 0) return -1;  /* not PROVEN */
    return registry_add_certified(reg, btn, name, c);
}

CertVerdict plan_weakest_verdict(BinaryTransformNetwork *const *btns,
                                 const Contract *const *contracts,
                                 size_t n, size_t cap) {
    int any_refused = 0, any_sampled = 0, any_unbounded = 0;
    size_t i;
    if (btns == NULL || contracts == NULL || n == 0) return CERT_REFUSED;

    for (i = 0; i < n; ++i) {
        ExhaustiveReport r;
        btn_certify_exhaustive(btns[i], contracts[i], cap, &r);
        switch (r.verdict) {
            case CERT_REFUSED:   any_refused = 1; break;
            case CERT_SAMPLED:   any_sampled = 1; break;
            case CERT_UNBOUNDED: any_unbounded = 1; break;
            case CERT_PROVEN:    break;
        }
    }
    if (any_refused)   return CERT_REFUSED;
    if (any_sampled)   return CERT_SAMPLED;
    if (any_unbounded) return CERT_UNBOUNDED;
    return CERT_PROVEN;
}

/* ---- Fix 4: statistical bound + abstention ------------------------------ */

double coverage_accuracy_lower_bound(size_t passed, size_t n, double z) {
    double phat, denom, centre, half;
    if (n == 0) return 0.0;
    if (passed > n) passed = n;
    phat   = (double)passed / (double)n;
    denom  = 1.0 + (z * z) / (double)n;
    centre = phat + (z * z) / (2.0 * (double)n);
    half   = z * sqrt((phat * (1.0 - phat) + (z * z) / (4.0 * (double)n)) / (double)n);
    double lo = (centre - half) / denom;
    if (lo < 0.0) lo = 0.0;
    if (lo > 1.0) lo = 1.0;
    return lo;
}

int coverage_input_is_certified(const Contract *c, const double *input) {
    size_t in_total, s;
    if (c == NULL || input == NULL) return 0;
    in_total = ports_sum_total(c->input_ports, c->input_port_count);
    if (in_total == 0) return 0;
    for (s = 0; s < c->exemplar_count; ++s) {
        const double *row = c->inputs + s * in_total;
        size_t i; int match = 1;
        for (i = 0; i < in_total; ++i) {
            double d = row[i] - input[i];
            if (d < 0) d = -d;
            if (d > 1e-9) { match = 0; break; }
        }
        if (match) return 1;
    }
    return 0;
}

/* ---- Fix 5: domain-spanning totality ------------------------------------ */

int btn_check_totality(BinaryTransformNetwork *btn, const Contract *c,
                       size_t cap, TotalityReport *out) {
    TotalityReport r;
    size_t swept, ill; double worst;
    int rc;
    memset(&r, 0, sizeof r);
    if (btn == NULL || c == NULL) { if (out) *out = r; return -1; }

    rc = domain_wellformed_sweep(btn, c, cap, &swept, &ill, &worst);
    if (rc != 0) { if (out) *out = r; return -1; }  /* not enumerable / over cap */

    r.domain = swept;
    r.well_formed = swept - ill;
    r.ill_formed = ill;
    r.spans_domain = 1;
    if (out) *out = r;
    return (ill == 0) ? 0 : -1;
}



