#include "../include/contract/narrative_coherence.h"
#include "../include/cce/cce_router.h"

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
                       cce_specialist_type_t type,
                       float x,
                       float y,
                       float goodness) {
    memset(br, 0, sizeof(*br));
    memset(cas, 0, sizeof(*cas));
    cas->goodness = goodness;
    snprintf(cas->name, sizeof(cas->name), "%s", name);
    br->cascade = cas;
    br->specialist_type = type;
    br->centroid_dim = 2;
    br->centroid[0] = x;
    br->centroid[1] = y;
    snprintf(br->name, sizeof(br->name), "%s", name);
}

static void test_rubric_scores_texture_above_flatness(void) {
    const char* textured =
        "In the shadowed halls, an oath-bearer guarded an ancient pact. "
        "The oath demanded silence, but betrayal could save the village. "
        "Long after the cost was paid, its consequence became a forgotten legend "
        "that would echo across generations.";
    const char* flat =
        "A person solved the problem quickly. Everyone agreed it was fine. The end.";
    NarrativeCoherenceConfig config = NARRATIVE_DEFAULT_CONFIG;
    NarrativeCoherenceScore rich = cnet_narrative_evaluate(textured, &config);
    NarrativeCoherenceScore thin = cnet_narrative_evaluate(flat, &config);

    CHECK(rich.is_valid && thin.is_valid,
          "both narrative samples score successfully");
    CHECK(rich.voice_consistency > 0.65,
          "rubric rewards prompt-grounded voice consistency");
    CHECK(rich.moral_ambiguity > 0.65,
          "rubric rewards moral ambiguity");
    CHECK(rich.delayed_consequence > 0.65,
          "rubric rewards delayed consequence");
    CHECK(rich.folklore_texture > 0.65,
          "rubric rewards folklore texture");
    CHECK(rich.overall_score > thin.overall_score + 0.25,
          "textured continuation separates from flat continuation");
    CHECK(cnet_narrative_passes(&rich, &config),
          "textured continuation passes the default contract");
    CHECK(!cnet_narrative_passes(&thin, &config),
          "flat continuation fails the default contract");
}

static void test_router_prefers_narrative_specialist_for_creative_tasks(void) {
    cce_cascade cascades[2];
    cce_branch branches[2];
    cce_forest forest;
    cce_router router;
    float input[2] = {0.0f, 0.0f};
    int branch = -1;
    float score = 0.0f;

    memset(&forest, 0, sizeof(forest));
    forest.branches = branches;
    forest.num_branches = 2;
    forest.max_branches = 2;

    set_branch(&branches[0], &cascades[0], "general_math", CCE_SPECIALIST_GENERAL,
               0.0f, 0.0f, 0.60f);
    set_branch(&branches[1], &cascades[1], "memory_witness_story", CCE_SPECIALIST_NARRATIVE,
               1.0f, 0.0f, 0.60f);

    CHECK(cce_router_init(&router, 1.0f, 1) == CCE_OK,
          "router initializes for narrative preference test");
    CHECK(cce_router_route(&router, &forest, input, 2, &branch, &score) == CCE_OK,
          "baseline route succeeds");
    CHECK(branch == 0,
          "baseline route keeps nearest general specialist");
    CHECK(cce_router_is_creative_task("continue this moral folklore story") != 0,
          "creative task detector recognizes story prompts");
    CHECK(cce_router_route_for_task(&router, &forest, input, 2,
                                    "continue this moral folklore story",
                                    &branch, &score) == CCE_OK,
          "task-aware route succeeds");
    CHECK(branch == 1,
          "creative task route prefers the narrative specialist");
}

int main(void) {
    printf("narrative coherence tests:\n");
    test_rubric_scores_texture_above_flatness();
    test_router_prefers_narrative_specialist_for_creative_tasks();

    if (failures == 0) {
        printf("\nAll narrative coherence tests passed.\n");
        return 0;
    }
    printf("\n%d narrative coherence test(s) FAILED.\n", failures);
    return 1;
}
