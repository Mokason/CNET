#include "../include/cce/cce_uncertainty.h"
#include "../include/cce/cce_compression.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, desc) do { \
    if (cond) { printf("  ok   %s\n", (desc)); } \
    else { printf("  FAIL %s\n", (desc)); ++failures; } \
} while (0)

static void test_activation_entropy_uncertainty(void) {
    float peaked[4] = {8.0f, 0.0f, -1.0f, -2.0f};
    float flat[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    float u_peaked = 1.0f;
    float u_flat = 0.0f;

    CHECK(cce_specialist_get_uncertainty(peaked, 4, &u_peaked) == CCE_OK,
          "peaked activation uncertainty computes");
    CHECK(cce_specialist_get_uncertainty(flat, 4, &u_flat) == CCE_OK,
          "flat activation uncertainty computes");
    CHECK(u_peaked < 0.05f,
          "peaked activations produce low uncertainty");
    CHECK(u_flat > 0.99f,
          "flat activations produce high uncertainty");
}

static void test_counterfactual_route_entropy(void) {
    float activations[3] = {2.0f, 1.0f, 0.0f};
    CounterfactualRoute confident[3];
    CounterfactualRoute ambiguous[3];
    cce_specialist_uncertainty_report rc;
    cce_specialist_uncertainty_report ra;

    memset(confident, 0, sizeof(confident));
    memset(ambiguous, 0, sizeof(ambiguous));
    confident[0].route_score = 0.90f;
    confident[1].route_score = 0.05f;
    confident[2].route_score = 0.05f;
    ambiguous[0].route_score = 0.40f;
    ambiguous[1].route_score = 0.35f;
    ambiguous[2].route_score = 0.25f;

    CHECK(cce_specialist_get_uncertainty_ex(activations, 3, confident, 3, &rc) == CCE_OK,
          "confident route uncertainty computes");
    CHECK(cce_specialist_get_uncertainty_ex(activations, 3, ambiguous, 3, &ra) == CCE_OK,
          "ambiguous route uncertainty computes");
    CHECK(ra.route_entropy > rc.route_entropy,
          "near-tie counterfactual routes raise route entropy");
    CHECK(ra.combined_uncertainty > rc.combined_uncertainty,
          "route entropy contributes to combined uncertainty");
}

static void test_compression_gradient_accumulator(void) {
    cce_compression_grads grads;
    float identity[3] = {1.0f, 2.0f, 3.0f};
    float residual[3] = {0.5f, 0.5f, 0.5f};
    memset(&grads, 0, sizeof(grads));

    CHECK(cce_compression_grads_init(&grads, 3) == CCE_OK,
          "compression Grads[] buffer initializes");
    CHECK(grads.preserve_identity_path && grads.preserve_residual_path,
          "compression gradients preserve identity and residual paths by default");
    CHECK(cce_compression_grads_accumulate(&grads, identity, residual, 3, 1.0f, 0.5f) == CCE_OK,
          "identity and residual gradients accumulate");
    CHECK(fabsf(grads.values[0] - 1.25f) < 0.0001f &&
          fabsf(grads.values[1] - 2.25f) < 0.0001f &&
          fabsf(grads.values[2] - 3.25f) < 0.0001f,
          "Grads[] stores identity plus scaled residual path");
    CHECK(cce_compression_grads_l2_norm(&grads) > 4.0f,
          "gradient norm reports accumulated signal");
    cce_compression_grads_zero(&grads);
    CHECK(cce_compression_grads_l2_norm(&grads) == 0.0f,
          "gradient buffer zeroes cleanly");
    cce_compression_grads_free(&grads);
}

int main(void) {
    printf("phase 4 uncertainty/compression tests:\n");
    test_activation_entropy_uncertainty();
    test_counterfactual_route_entropy();
    test_compression_gradient_accumulator();

    if (failures == 0) {
        printf("\nAll phase 4 uncertainty/compression tests passed.\n");
        return 0;
    }
    printf("\n%d phase 4 test(s) FAILED.\n", failures);
    return 1;
}
