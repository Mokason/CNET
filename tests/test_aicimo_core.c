/*
 * test_aicimo_core.c — Hermetic focused test for AICIMO core routing.
 *
 * Tests the four behavioral contracts:
 *   1. Identity / residual preservation (identity-init adapter yields output == input)
 *   2. Deterministic role routing selection (same role -> same adapter index)
 *   3. Uncertainty based on actual route state (varies with router config, not a
 *      fixed constant formula)
 *   4. Invalid arguments rejected (NULL pointers, dimension mismatches, bad roles)
 *
 * This test is hermetic: no network, no GPU, no external files. C11 only.
 */
#include "../include/cce/cce_aicimo.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- helpers ---- */

static int approx_eq(float a, float b, float tol) {
    return fabsf(a - b) < tol;
}

static int vec_eq(const float *a, const float *b, size_t n, float tol) {
    for (size_t i = 0; i < n; ++i)
        if (!approx_eq(a[i], b[i], tol)) return 0;
    return 1;
}

static int test_fail_count = 0;
static int test_pass_count = 0;

#define CHECK(cond, msg) do {                                  \
    if (cond) { test_pass_count++; }                           \
    else { test_fail_count++; fprintf(stderr, "FAIL: %s\n", msg); } \
} while (0)

/* ---- tests ---- */

/* 1. Identity / residual preservation */
static void test_identity_preservation(void) {
    const size_t dim = 64;
    const size_t num_ops = 4;
    cce_aicimo_router router;

    CHECK(cce_aicimo_router_init(&router, num_ops, dim) == CCE_OK,
          "identity: router_init");

    float input[64], output[64];
    for (size_t i = 0; i < dim; ++i)
        input[i] = (float)((i % 11) * 0.1 + (i % 3) * 0.05);

    size_t used = 999;
    CHECK(cce_aicimo_route(&router, input, dim, output, dim, &used) == CCE_OK,
          "identity: route");
    CHECK(used == 1, "identity: used_ops == 1");

    /* Identity init means output == input (residual preserved) */
    CHECK(vec_eq(input, output, dim, 1e-5f),
          "identity: output == input (residual preserved)");

    cce_aicimo_router_free(&router);
    fprintf(stderr, "  test_identity_preservation: done\n");
}

/* 2. Deterministic role routing selection */
static void test_deterministic_role_routing(void) {
    const size_t dim = 32;
    const size_t num_ops = 4;
    cce_aicimo_router router;

    CHECK(cce_aicimo_router_init(&router, num_ops, dim) == CCE_OK,
          "role: router_init");

    /* Set non-uniform strengths so routing has a deterministic best */
    cce_aicimo_set_route_strength(&router, 2, 2, 0.5f);
    cce_aicimo_set_route_strength(&router, 0, 1, 0.3f);

    float input[32], output_a[32], output_b[32];
    for (size_t i = 0; i < dim; ++i)
        input[i] = (float)(i * 0.02);

    size_t sel_a = 999, sel_b = 999;
    CHECK(cce_aicimo_route_for_role(&router, input, dim, output_a, dim,
                                     "narrative", &sel_a) == CCE_OK,
          "role: route_for_role (a)");
    CHECK(cce_aicimo_route_for_role(&router, input, dim, output_b, dim,
                                     "narrative", &sel_b) == CCE_OK,
          "role: route_for_role (b)");

    /* Same role + same router state => same selection */
    CHECK(sel_a == sel_b, "role: deterministic selection (sel_a == sel_b)");
    CHECK(sel_a < num_ops, "role: selection in range");

    /* Different role with different strength => possibly different selection */
    cce_aicimo_set_route_strength(&router, 3, 3, 0.8f);
    size_t sel_c = 999;
    float output_c[32];
    CHECK(cce_aicimo_route_for_role(&router, input, dim, output_c, dim,
                                     "analytical", &sel_c) == CCE_OK,
          "role: route_for_role (c)");
    /* The analytical role should pick op 3 (highest strength) */
    CHECK(sel_c == 3, "role: different strength => different selection");

    cce_aicimo_router_free(&router);
    fprintf(stderr, "  test_deterministic_role_routing: done\n");
}

/* 3. Uncertainty based on actual route state */
static void test_uncertainty_from_route_state(void) {
    const size_t dim = 16;
    const size_t num_ops = 4;

    /* Router A: uniform strengths (high route entropy = high uncertainty) */
    cce_aicimo_router router_a;
    CHECK(cce_aicimo_router_init(&router_a, num_ops, dim) == CCE_OK,
          "unc: router_a init");

    float input[16], output[16];
    for (size_t i = 0; i < dim; ++i)
        input[i] = (float)(i * 0.1);

    float unc_a = -1.0f;
    CHECK(cce_aicimo_route_with_uncertainty(&router_a, input, dim, output, dim,
                                             &unc_a) == CCE_OK,
          "unc: route_with_uncertainty (uniform)");
    CHECK(unc_a >= 0.0f && unc_a <= 1.0f, "unc: unc_a in [0,1]");

    /* Router B: peaked strengths (low route entropy = lower uncertainty) */
    cce_aicimo_router router_b;
    CHECK(cce_aicimo_router_init(&router_b, num_ops, dim) == CCE_OK,
          "unc: router_b init");
    cce_aicimo_set_route_strength(&router_b, 0, 0, 1.0f);
    cce_aicimo_set_route_strength(&router_b, 1, 1, 0.001f);
    cce_aicimo_set_route_strength(&router_b, 2, 2, 0.001f);
    cce_aicimo_set_route_strength(&router_b, 3, 3, 0.001f);

    float unc_b = -1.0f;
    CHECK(cce_aicimo_route_with_uncertainty(&router_b, input, dim, output, dim,
                                             &unc_b) == CCE_OK,
          "unc: route_with_uncertainty (peaked)");
    CHECK(unc_b >= 0.0f && unc_b <= 1.0f, "unc: unc_b in [0,1]");

    /* Uncertainty from uniform routing must be strictly higher than peaked */
    CHECK(unc_a > unc_b,
          "unc: uniform route entropy > peaked route entropy");

    /* Sanity: uncertainty must not be a fixed constant regardless of state.
       The two values must differ by a meaningful margin. */
    CHECK(fabsf(unc_a - unc_b) > 0.01f,
          "unc: uncertainty varies with route state (not a fixed formula)");

    cce_aicimo_router_free(&router_a);
    cce_aicimo_router_free(&router_b);
    fprintf(stderr, "  test_uncertainty_from_route_state: done\n");
}

/* 3b. Role hash must distribute over at least two adapter rows under equal base
 *     strengths. This encodes the mandatory prerequisite: with equal strengths
 *     the deterministic role bias is the only signal — if bias_row is always
 *     zero, no role selects row != 0 and adapter routing collapses. */
static void test_role_distribution_under_equal_strengths(void) {
    const size_t dim = 32;
    const size_t num_ops = 4;
    cce_aicimo_router router;

    CHECK(cce_aicimo_router_init(&router, num_ops, dim) == CCE_OK,
          "role_dist: router_init");

    /* Do NOT change strengths. All rows have identical base sum (0.01). Only the
     * role bias can decide the winner in this configuration. */
    static const char *roles[] = {
        "narrative", "analytical", "planner", "critic",
        "coder",     "summarizer", "planner_meta", "explorer",
        "verifier",  "empathic",   "adversary",    "arbiter",
        "story",     "diagnostic", "trickster",    "synthesizer",
    };
    const size_t nroles = sizeof(roles) / sizeof(roles[0]);

    float input[32];
    for (size_t i = 0; i < dim; ++i) input[i] = (float)(i * 0.03);

    int hits[8] = {0};
    for (size_t r = 0; r < nroles; ++r) {
        float output[32];
        size_t sel = 999;
        CHECK(cce_aicimo_route_for_role(&router, input, dim, output, dim,
                                         roles[r], &sel) == CCE_OK,
              "role_dist: route_for_role");
        CHECK(sel < num_ops, "role_dist: sel in range");
        if (sel < num_ops) hits[sel]++;
    }

    int distinct = 0;
    for (size_t i = 0; i < num_ops; ++i) if (hits[i] > 0) distinct++;
    CHECK(distinct >= 2,
          "role_dist: at least two adapters chosen across stable roles "
          "(RED without fix — bias always resolves to row 0)");

    cce_aicimo_router_free(&router);
    fprintf(stderr, "  test_role_distribution_under_equal_strengths: done "
                    "(distinct=%d, hits={%d,%d,%d,%d})\n",
            distinct, hits[0], hits[1], hits[2], hits[3]);
}

/* 3c. The additive shared-decision API returns adapter AND uncertainty from the
 *     same role-biased routing state. Bridge callers must not need two separate
 *     routes. */
static void test_role_decision_shared_uncertainty(void) {
    const size_t dim = 16;
    const size_t num_ops = 4;
    cce_aicimo_router router;

    CHECK(cce_aicimo_router_init(&router, num_ops, dim) == CCE_OK,
          "role_dec: router_init");

    float input[16], output[16];
    for (size_t i = 0; i < dim; ++i) input[i] = (float)(i * 0.05);

    size_t sel = 999;
    float unc = -1.0f;
    CHECK(cce_aicimo_route_decision(&router, input, dim, output, dim,
                                     "planner", &sel, &unc) == CCE_OK,
          "role_dec: route_decision");
    CHECK(sel < num_ops, "role_dec: sel in range");
    CHECK(unc >= 0.0f && unc <= 1.0f, "role_dec: unc in [0,1]");

    /* Same role -> same selection AND same uncertainty (determinism). */
    size_t sel2 = 999;
    float unc2 = -1.0f;
    CHECK(cce_aicimo_route_decision(&router, input, dim, output, dim,
                                     "planner", &sel2, &unc2) == CCE_OK,
          "role_dec: route_decision (repeat)");
    CHECK(sel == sel2, "role_dec: deterministic selection");
    CHECK(approx_eq(unc, unc2, 1e-6f), "role_dec: deterministic uncertainty");

    /* Invalid-arg surface. */
    CHECK(cce_aicimo_route_decision(NULL, input, dim, output, dim,
                                     "x", &sel, &unc) != CCE_OK,
          "role_dec: NULL router rejected");
    CHECK(cce_aicimo_route_decision(&router, input, dim, output, dim,
                                     NULL, &sel, &unc) != CCE_OK,
          "role_dec: NULL role rejected");
    CHECK(cce_aicimo_route_decision(&router, input, dim, output, dim,
                                     "x", NULL, &unc) != CCE_OK,
          "role_dec: NULL sel rejected");
    CHECK(cce_aicimo_route_decision(&router, input, dim, output, dim,
                                     "x", &sel, NULL) != CCE_OK,
          "role_dec: NULL unc rejected");

    cce_aicimo_router_free(&router);
    fprintf(stderr, "  test_role_decision_shared_uncertainty: done\n");
}

/* 4. Invalid arguments rejected */
static void test_invalid_args(void) {
    const size_t dim = 8;
    cce_aicimo_router router;
    CHECK(cce_aicimo_router_init(&router, 4, dim) == CCE_OK,
          "invalid: router_init");

    float input[8], output[8];
    for (size_t i = 0; i < dim; ++i) input[i] = 0.5f;

    size_t used;

    /* NULL router */
    CHECK(cce_aicimo_route(NULL, input, dim, output, dim, &used) != CCE_OK,
          "invalid: NULL router rejected");

    /* NULL input */
    CHECK(cce_aicimo_route(&router, NULL, dim, output, dim, &used) != CCE_OK,
          "invalid: NULL input rejected");

    /* NULL output */
    CHECK(cce_aicimo_route(&router, input, dim, NULL, dim, &used) != CCE_OK,
          "invalid: NULL output rejected");

    /* NULL used_ops */
    CHECK(cce_aicimo_route(&router, input, dim, output, dim, NULL) != CCE_OK,
          "invalid: NULL used_ops rejected");

    /* dimension mismatch: in_len != current_context */
    CHECK(cce_aicimo_route(&router, input, dim + 1, output, dim, &used) != CCE_OK,
          "invalid: in_len mismatch rejected");

    /* out_cap < in_len */
    CHECK(cce_aicimo_route(&router, input, dim, output, dim - 1, &used) != CCE_OK,
          "invalid: out_cap < in_len rejected");

    /* router_init with zero ops */
    cce_aicimo_router bad_router;
    CHECK(cce_aicimo_router_init(&bad_router, 0, dim) != CCE_OK,
          "invalid: zero ops rejected");

    /* router_init with zero dim */
    CHECK(cce_aicimo_router_init(&bad_router, 4, 0) != CCE_OK,
          "invalid: zero dim rejected");

    /* NULL router for init */
    CHECK(cce_aicimo_router_init(NULL, 4, dim) != CCE_OK,
          "invalid: NULL router_init rejected");

    /* NULL role for route_for_role */
    size_t sel;
    CHECK(cce_aicimo_route_for_role(&router, input, dim, output, dim, NULL, &sel) != CCE_OK,
          "invalid: NULL role rejected");

    /* NULL uncertainty pointer */
    CHECK(cce_aicimo_route_with_uncertainty(&router, input, dim, output, dim, NULL) != CCE_OK,
          "invalid: NULL uncertainty rejected");

    /* set_route_strength with NULL router */
    CHECK(cce_aicimo_set_route_strength(NULL, 0, 0, 1.0f) != CCE_OK,
          "invalid: NULL set_route_strength rejected");

    /* set_route_strength out of bounds */
    CHECK(cce_aicimo_set_route_strength(&router, 10, 0, 1.0f) != CCE_OK,
          "invalid: OOB row rejected");
    CHECK(cce_aicimo_set_route_strength(&router, 0, 10, 1.0f) != CCE_OK,
          "invalid: OOB col rejected");

    cce_aicimo_router_free(&router);
    cce_aicimo_router_free(NULL);  /* must be a no-op, not crash */
    fprintf(stderr, "  test_invalid_args: done\n");
}

/* ---- main ---- */

int main(void) {
    printf("=== AICIMO Core Routing Test ===\n");

    test_identity_preservation();
    test_deterministic_role_routing();
    test_role_distribution_under_equal_strengths();
    test_role_decision_shared_uncertainty();
    test_uncertainty_from_route_state();
    test_invalid_args();

    printf("\nResults: %d passed, %d failed\n", test_pass_count, test_fail_count);
    if (test_fail_count > 0) {
        printf("AICIMO_CORE_TEST_FAIL\n");
        return 1;
    }
    printf("AICIMO_CORE_TEST_PASS\n");
    return 0;
}