#include "../../include/cce/cce_router.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, desc) do { \
    if (cond) { printf("  ok   %s\n", (desc)); } \
    else { printf("  FAIL %s\n", (desc)); ++failures; } \
} while (0)

static void set_branch(cce_branch* br,
                       cce_cascade* cas,
                       const char* name,
                       float x,
                       float y,
                       float goodness) {
    memset(br, 0, sizeof(*br));
    memset(cas, 0, sizeof(*cas));
    cas->goodness = goodness;
    snprintf(cas->name, sizeof(cas->name), "%s", name);
    br->cascade = cas;
    br->centroid_dim = 2;
    br->centroid[0] = x;
    br->centroid[1] = y;
    snprintf(br->name, sizeof(br->name), "%s", name);
}

static void test_counterfactual_sampler(void) {
    cce_cascade cascades[4];
    cce_branch branches[4];
    cce_forest forest;
    cce_router router;
    float input[2] = {0.0f, 0.0f};
    int top_branch = -1;
    float top_score = 0.0f;
    CounterfactualRoute routes[CCE_ROUTER_MAX_COUNTERFACTUALS];
    int count = 0;

    memset(&forest, 0, sizeof(forest));
    forest.branches = branches;
    forest.num_branches = 4;
    forest.max_branches = 4;

    set_branch(&branches[0], &cascades[0], "primary_truth", 0.0f, 0.0f, 0.95f);
    set_branch(&branches[1], &cascades[1], "near_counterfactual", 0.8f, 0.0f, 0.90f);
    set_branch(&branches[2], &cascades[2], "distant_memory", 2.0f, 0.0f, 0.80f);
    set_branch(&branches[3], &cascades[3], "off_topic", 4.0f, 4.0f, 0.50f);

    CHECK(cce_router_init(&router, 1.0f, 2) == CCE_OK,
          "router initializes");
    CHECK(cce_router_route(&router, &forest, input, 2, &top_branch, &top_score) == CCE_OK,
          "primary route resolves");
    CHECK(top_branch == 0, "primary branch wins by centroid + goodness");

    CHECK(cce_router_sample_counterfactuals(&router, &forest, input, 2,
                                            top_branch, routes, 3, &count) == CCE_OK,
          "counterfactual sampling succeeds");
    CHECK(count == 3, "sampler returns three alternatives when available");
    CHECK(routes[0].branch_index == 1, "nearest non-primary route ranks first");
    CHECK(routes[0].branch_index != top_branch &&
          routes[1].branch_index != top_branch &&
          routes[2].branch_index != top_branch,
          "primary route is excluded from counterfactuals");
    CHECK(strcmp(routes[0].branch_name, "near_counterfactual") == 0,
          "counterfactual route carries branch name");
    CHECK(routes[0].route_score >= routes[1].route_score &&
          routes[1].route_score >= routes[2].route_score,
          "counterfactuals are ranked by route score");
    CHECK(routes[0].consistency_score >= 0.0f &&
          routes[0].consistency_score <= 1.0f,
          "consistency score is normalized");
    CHECK(routes[0].consistency_score < routes[2].consistency_score,
          "closer counterfactual applies stronger consistency pressure");
}

static float verify_synthetic_claim(const char* claim,
                                    float primary_score,
                                    const float* cf_scores,
                                    int cf_count) {
    CounterfactualRoute primary;
    CounterfactualRoute cfs[CCE_ROUTER_MAX_COUNTERFACTUALS];
    float consistency = 0.0f;
    float uncertainty = 0.0f;

    memset(&primary, 0, sizeof(primary));
    primary.branch_index = 0;
    primary.route_score = primary_score;
    snprintf(primary.branch_name, sizeof(primary.branch_name), "primary");

    for (int i = 0; i < cf_count; ++i) {
        memset(&cfs[i], 0, sizeof(cfs[i]));
        cfs[i].branch_index = i + 1;
        cfs[i].route_score = cf_scores[i];
        snprintf(cfs[i].branch_name, sizeof(cfs[i].branch_name), "cf_%d", i + 1);
    }

    CHECK(cce_router_verify_claim(&primary, claim, cfs, cf_count,
                                  &consistency, &uncertainty) == CCE_OK,
          "claim verifier accepts synthetic route evidence");
    CHECK(fabsf((1.0f - consistency) - uncertainty) < 0.0001f,
          "uncertainty complements consistency");
    return consistency;
}

static void test_truthfulqa_style_synthetic_dataset(void) {
    const float confident_cfs[3] = {0.06f, 0.04f, 0.02f};
    const float ambiguous_cfs[3] = {0.42f, 0.08f, 0.05f};
    float confident;
    float ambiguous;

    printf("TruthfulQA-style synthetic counterfactual claims:\n");
    confident = verify_synthetic_claim(
        "The Eiffel Tower is located in Paris.",
        0.86f,
        confident_cfs,
        3);
    ambiguous = verify_synthetic_claim(
        "A widely repeated folk cure is guaranteed to work.",
        0.45f,
        ambiguous_cfs,
        3);

    CHECK(confident > 0.85f,
          "well-separated route evidence yields high consistency");
    CHECK(ambiguous < 0.55f,
          "near-tie counterfactual route flags a claim for review");
    CHECK(confident > ambiguous + 0.30f,
          "synthetic truthful claim separates from ambiguous claim");
}

int main(void) {
    printf("counterfactual routing tests:\n");
    test_counterfactual_sampler();
    test_truthfulqa_style_synthetic_dataset();

    if (failures == 0) {
        printf("\nAll counterfactual routing tests passed.\n");
        return 0;
    }
    printf("\n%d counterfactual routing test(s) FAILED.\n", failures);
    return 1;
}
