/*
 * test_residue.c -- the mod-k residue domain's correctness gate.
 * delta(r,d)=(r*b+d) mod k trains and certifies EXACTLY on its k*b table
 * (so the whole scan is bounded), and a short scan matches ground truth.
 */
#include "residue_common.h"

#include <stdio.h>

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", (msg)); ++failures; } \
} while (0)

static void test_delta_certifies(void) {
    const int b = 4, k = 7;
    double inputs[RES_MAX_K * RES_MAX_B * (RES_MAX_K + RES_MAX_B)];
    double targets[RES_MAX_K * RES_MAX_B * RES_MAX_K];
    BinaryTransformNetwork delta;
    CertifyReport rep;
    size_t samples;
    int rc;

    memset(&delta, 0, sizeof delta);
    samples = build_residue_step_data(b, k, inputs, targets);
    CHECK(samples == (size_t)(b * k), "delta table has k*b rows");
    train_residue_step(&delta, b, k, inputs, targets, samples, 12345u);
    rc = certify_residue_step(&delta, k, inputs, targets, samples, 0.0, &rep);
    CHECK(rc == 0, "delta certifies (exact replay on all k*b pairs)");
    CHECK(rep.passed == samples && rep.failed == 0, "all k*b exemplars pass");
    printf("  [delta b=%d k=%d: %lu/%lu certified, min margin %.4f]\n",
           b, k, (unsigned long)rep.passed, (unsigned long)samples, rep.min_margin);
    btn_free(&delta);
}

/* A cheap deterministic LCG so the sample is reproducible without rand() state. */
static unsigned int res_rng_state = 2463534242u;
static unsigned int res_rng(void) {
    res_rng_state ^= res_rng_state << 13;
    res_rng_state ^= res_rng_state >> 17;
    res_rng_state ^= res_rng_state << 5;
    return res_rng_state;
}

static void test_scan_matches_ground_truth(void) {
    const int b = 4, k = 7;
    const size_t N = 8, TRIALS = 2000;
    double inputs[RES_MAX_K * RES_MAX_B * (RES_MAX_K + RES_MAX_B)];
    double targets[RES_MAX_K * RES_MAX_B * RES_MAX_K];
    BinaryTransformNetwork delta;
    CertifyReport rep;
    size_t samples, t, mism = 0;

    memset(&delta, 0, sizeof delta);
    samples = build_residue_step_data(b, k, inputs, targets);
    train_residue_step(&delta, b, k, inputs, targets, samples, 12345u);
    if (certify_residue_step(&delta, k, inputs, targets, samples, 0.0, &rep) != 0) {
        printf("FAIL: delta did not certify (scan test)\n"); ++failures;
        btn_free(&delta); return;
    }

    res_rng_state = 99991u;
    for (t = 0; t < TRIALS; ++t) {
        int digits[RES_MAX_N];
        size_t i;
        int got, want;
        for (i = 0; i < N; ++i) digits[i] = (int)(res_rng() % (unsigned)b);
        got = run_scan(&delta, b, k, digits, N);
        want = string_residue(digits, N, b, k);
        if (got != want) ++mism;
    }
    CHECK(mism == 0, "scan == ground truth on 2000 random length-8 strings");
    printf("  [scan b=%d k=%d N=%lu: %lu/%lu exact]\n",
           b, k, (unsigned long)N, (unsigned long)(TRIALS - mism),
           (unsigned long)TRIALS);
    btn_free(&delta);
}

int run_test_residue(void) {
    test_delta_certifies();
    test_scan_matches_ground_truth();
    if (failures == 0) { printf("RESIDUE PASS\n"); return 0; }
    printf("RESIDUE FAIL: %d checks failed.\n", failures);
    return 1;
}
