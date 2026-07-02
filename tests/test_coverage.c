/*
 * tests/test_coverage.c -- evidence for the proof-vs-sample contract layer.
 *
 * Each of the 5 fixes gets deterministic CHECKs, plus a benchmark section that
 * times exhaustive certification at the byte scale (the realistic ceiling for
 * an enumerable CNET domain). Run standalone via `make coverage`, or as part of
 * `make test` (wired into test_all).
 */
#include "../include/nn.h"
#include "../include/router.h"
#include "../include/contract/contract.h"
#include "../include/contract/coverage.h"
#include "../include/property.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

static int failures = 0;

#define CHECK(cond, desc) do {                       \
    if (cond) { printf("  ok   %s\n", (desc)); }     \
    else { printf("  FAIL %s\n", (desc)); ++failures; } \
} while (0)

static Port PT(PortFamily family, size_t field_width, size_t field_count,
               const char *tag) {
    Port p;
    p.family = family;
    p.field_width = field_width;
    p.field_count = field_count;
    p.tag[0] = '\0';
    if (tag != NULL) port_set_tag(&p, tag);
    return p;
}

/* Train an N-way one-hot identity net (i -> i). Canonical tables in/tg are
   borrowed by every contract built from them, so the CALLER must keep them
   alive (and free them) for the lifetime of those contracts. */
static int make_identity(BinaryTransformNetwork *btn, size_t N,
                         double *in, double *tg) {
    size_t i, j;
    for (i = 0; i < N * N; ++i) { in[i] = 0.0; tg[i] = 0.0; }
    for (i = 0; i < N; ++i) { in[i * N + i] = 1.0; tg[i * N + i] = 1.0; }
    if (btn_init(btn, N, N, N + 4, (N + 4) * 2, 0.5, 1234u) != 0) return -1;
    Port p = PT(PORT_ONEHOT, N, 1, "sym");
    if (btn_set_ports(btn, p, p) != 0) return -1;
    btn_train_dynamic(btn, in, tg, N, 60000, 800, 0.0003, 0.0008);
    btn_train(btn, in, tg, N, 8000);
    (void)j;
    return 0;
}

int run_test_coverage(void);
int run_test_coverage(void) {
    failures = 0;
    printf("== contract coverage: proof vs sample ==\n");

    enum { N = 4 };
    static double in4[N * N], tg4[N * N];     /* outlive every contract below */
    BinaryTransformNetwork id;
    if (make_identity(&id, N, in4, tg4) != 0) {
        printf("  FAIL identity net did not initialize\n");
        return ++failures;
    }

    /* ---- Fix 2: cardinality + coverage are computed from the signature ---- */
    {
        size_t card = 0, vc = 0;
        CHECK(port_value_count(PT(PORT_ONEHOT, 16, 1, ""), &vc) && vc == 16,
              "ONEHOT16 has 16 canonical values");
        CHECK(port_value_count(PT(PORT_ONEHOT, 4, 2, ""), &vc) && vc == 16,
              "ONEHOT4 x2 fields = 4^2 = 16 values");
        CHECK(port_value_count(PT(PORT_BINARY_MSB, 8, 1, ""), &vc) && vc == 256,
              "BINARY8 has 2^8 = 256 canonical values");
        CHECK(!port_value_count(PT(PORT_EVIDENCE, 4, 1, ""), &vc),
              "EVIDENCE is not enumerable");

        Contract c4;
        CHECK(contract_init_borrowed(&c4, "id4", &id, in4, tg4, N) == 0,
              "exhaustive contract built (4 exemplars)");
        CHECK(contract_domain_cardinality(&c4, &card) && card == 4,
              "id4 input is ONEHOT4 -> domain cardinality = 4");
        contract_free(&c4);
    }

    /* The identity contract's input is ONEHOT4 (one field) -> domain 4. */
    Contract c_full;       /* 4 exemplars -> EXHAUSTIVE */
    Contract c_samp;       /* 3 exemplars -> SAMPLED    */
    CHECK(contract_init_borrowed(&c_full, "id_full", &id, in4, tg4, 4) == 0,
          "full contract: 4/4 exemplars");
    CHECK(contract_init_borrowed(&c_samp, "id_samp", &id, in4, tg4, 3) == 0,
          "sampled contract: 3/4 exemplars (same net)");

    {
        size_t card = 0;
        CHECK(contract_domain_cardinality(&c_full, &card) && card == 4,
              "ONEHOT4 input domain cardinality = 4");

        CoverageReport cov;
        contract_coverage(&c_full, &cov);
        CHECK(cov.kind == COVERAGE_EXHAUSTIVE && cov.uncovered == 0,
              "4/4 exemplars -> EXHAUSTIVE (proof-ready)");
        contract_coverage(&c_samp, &cov);
        CHECK(cov.kind == COVERAGE_SAMPLED && cov.uncovered == 1 &&
              cov.distinct_exemplars == 3,
              "3/4 exemplars -> SAMPLED, 1 input uncovered");
    }

    /* ---- Fix 1: same net, two verdicts -- exhaustive certify --------------- */
    {
        ExhaustiveReport e_full, e_samp;
        int rc_full = btn_certify_exhaustive(&id, &c_full, 0, &e_full);
        int rc_samp = btn_certify_exhaustive(&id, &c_samp, 0, &e_samp);

        CHECK(rc_full == 0 && e_full.verdict == CERT_PROVEN,
              "exhaustive contract -> PROVEN (certify == proof over the domain)");
        CHECK(rc_samp != 0 && e_samp.verdict == CERT_SAMPLED && e_samp.certify.failed == 0,
              "sampled contract -> SAMPLED: net passes its exemplars but is NOT proven");
        CHECK(e_full.domain_swept == 4 && e_full.domain_illformed == 0,
              "full-domain sweep: all 4 inputs produce well-formed outputs");
        CHECK(e_samp.domain_swept == 4,
              "sweep still covers the WHOLE domain (4), even for a 3-exemplar contract");
    }

    /* ---- Fix 2 (unbounded): a non-enumerable input is honestly flagged ----- */
    {
        Contract cu;
        memset(&cu, 0, sizeof cu);
        strcpy(cu.name, "evid_in");
        cu.input_ports[0] = PT(PORT_EVIDENCE, 4, 1, "dist");
        cu.input_port_count = 1;
        cu.output_ports[0] = PT(PORT_ONEHOT, 2, 1, "y");
        cu.output_port_count = 1;
        static double cu_in[2 * 4] = {1,0,0,0, 0,1,0,0};
        static double cu_out[2 * 2] = {1,0, 0,1};
        cu.inputs = cu_in; cu.outputs = cu_out; cu.exemplar_count = 2; cu.owns_data = 0;

        size_t card = 0;
        CHECK(!contract_domain_cardinality(&cu, &card),
              "EVIDENCE input -> domain NOT enumerable");
        CoverageReport cov; contract_coverage(&cu, &cov);
        CHECK(cov.kind == COVERAGE_UNBOUNDED,
              "EVIDENCE input -> COVERAGE_UNBOUNDED (cannot claim a proof)");
    }

    /* ---- Fix 3: tiered admission + weakest-link propagation ---------------- */
    {
        PrimitiveRegistry reg; registry_init(&reg);
        CHECK(registry_add_proven(&reg, &id, "id_proven", &c_full, 0) == 0,
              "registry_add_proven admits a PROVEN primitive");
        CHECK(registry_add_proven(&reg, &id, "id_only_sampled", &c_samp, 0) == -1,
              "registry_add_proven REJECTS a merely-sampled primitive");
        registry_free(&reg);

        BinaryTransformNetwork *btns2[2] = { &id, &id };
        const Contract *cs_proven[2] = { &c_full, &c_full };
        const Contract *cs_mixed[2]  = { &c_full, &c_samp };
        CHECK(plan_weakest_verdict(btns2, cs_proven, 2, 0) == CERT_PROVEN,
              "all-proven plan -> PROVEN end-to-end");
        CHECK(plan_weakest_verdict(btns2, cs_mixed, 2, 0) == CERT_SAMPLED,
              "one sampled link drags the whole plan to SAMPLED (weakest link)");
    }

    /* ---- Fix 4: statistical floor + input-side abstention ------------------ */
    {
        double b_lo = coverage_accuracy_lower_bound(95, 100, 1.96);
        double b_hi = coverage_accuracy_lower_bound(256, 256, 1.96);
        double b_few = coverage_accuracy_lower_bound(9, 10, 1.96);
        CHECK(b_lo > 0.0 && b_lo < 0.95,
              "95/100 -> 95% lower bound is BELOW the point estimate (honest discount)");
        CHECK(b_hi > b_lo, "256/256 gives a tighter floor than 95/100");
        CHECK(b_few < b_lo, "9/10 (tiny n) gives a looser floor than 95/100");

        /* The 4th one-hot is the uncovered point of the SAMPLED contract. */
        double pt0[N] = {1,0,0,0};
        double pt3[N] = {0,0,0,1};
        CHECK(coverage_input_is_certified(&c_samp, pt0) == 1,
              "abstention gate: a proven exemplar input is TRUSTED");
        CHECK(coverage_input_is_certified(&c_samp, pt3) == 0,
              "abstention gate: the uncovered input ABSTAINS (the 7-seg fix in miniature)");
        CHECK(coverage_input_is_certified(&c_full, pt3) == 1,
              "an EXHAUSTIVE contract trusts every canonical input (never abstains)");
    }

    /* ---- Fix 5: domain-spanning laws (totality + an equational property) --- */
    {
        TotalityReport tot;
        int rc = btn_check_totality(&id, &c_full, 0, &tot);
        CHECK(rc == 0 && tot.spans_domain == 1 && tot.ill_formed == 0 &&
              tot.domain == 4 && tot.well_formed == 4,
              "totality law: EVERY one of the 4 canonical inputs -> a well-formed output");

        /* The existing property machinery, over the FULL enumerated domain. */
        PrimitiveRegistry reg; registry_init(&reg);
        registry_add(&reg, &id, "idp");
        Property law;
        memset(&law, 0, sizeof law);
        strcpy(law.name, "id_is_identity");
        law.sources[0] = PT(PORT_ONEHOT, N, 1, "sym");
        law.source_count = 1;
        strcpy(law.lhs[0], "idp"); law.lhs_len = 1;
        law.rhs_len = 0;                       /* RHS empty = identity on sources */
        PropertyReport prep;
        int prc = property_check(&law, &reg, 1024, &prep);
        CHECK(prc == 0 && prep.violated == 0 && prep.inputs == 4 && prep.held == 4,
              "equational law holds over all 4 inputs (domain-spanning, not sampled)");
        registry_free(&reg);
    }

    /* ---- Benchmark: exhaustive certification at the byte scale ------------- */
    {
        /* A BINARY8 -> ONEHOT2 net: 256-point domain (the realistic ceiling). */
        BinaryTransformNetwork b8;
        btn_init(&b8, 8, 2, 12, 24, 0.5, 99u);
        Port bin = PT(PORT_BINARY_MSB, 8, 1, "byte");
        Port bout = PT(PORT_ONEHOT, 2, 1, "bit");
        btn_set_ports(&b8, bin, bout);
        static double b8_in[8] = {0,0,0,0,0,0,0,0};
        static double b8_tg[2] = {1,0};
        Contract c8;
        contract_init_borrowed(&c8, "byte_dom", &b8, b8_in, b8_tg, 1);

        const int iters = 2000;
        clock_t t0 = clock();
        TotalityReport tot; size_t total_points = 0;
        for (int it = 0; it < iters; ++it) {
            btn_check_totality(&b8, &c8, 0, &tot);  /* sweeps all 256 each call */
            total_points += tot.domain;
        }
        clock_t t1 = clock();
        double secs = (double)(t1 - t0) / CLOCKS_PER_SEC;
        double per_call_us = secs / iters * 1e6;
        double pts_per_sec = (secs > 0) ? (double)total_points / secs : 0.0;

        printf("  [bench] exhaustive 256-point sweep: %.2f us/call, %.0f domain-points/sec (%d calls)\n",
               per_call_us, pts_per_sec, iters);
        CHECK(tot.domain == 256, "byte domain enumerated to exactly 256 points");
        CHECK(per_call_us < 5000.0,
              "a full 256-point exhaustive sweep is sub-5ms (affordable at admission)");

        /* Overhead of exhaustive certify vs plain certify, same contract. */
        clock_t p0 = clock();
        for (int it = 0; it < iters; ++it) { CertifyReport cr; btn_certify(&id, &c_full, &cr); }
        clock_t p1 = clock();
        clock_t e0 = clock();
        for (int it = 0; it < iters; ++it) { ExhaustiveReport er; btn_certify_exhaustive(&id, &c_full, 0, &er); }
        clock_t e1 = clock();
        double plain_us = (double)(p1 - p0) / CLOCKS_PER_SEC / iters * 1e6;
        double exh_us   = (double)(e1 - e0) / CLOCKS_PER_SEC / iters * 1e6;
        printf("  [bench] id4 certify: plain %.2f us vs exhaustive %.2f us/call\n",
               plain_us, exh_us);

        contract_free(&c8);
        btn_free(&b8);
    }

    contract_free(&c_full);
    contract_free(&c_samp);
    btn_free(&id);

    printf("== coverage tests done: %d failure(s) ==\n", failures);
    return failures;
}

#ifndef TEST_ALL
int main(void) { return run_test_coverage() == 0 ? 0 : 1; }
#endif

