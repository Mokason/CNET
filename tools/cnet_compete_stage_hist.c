#include "cnet_compete_eval.h"
#include "cnet_compete_runtime.h"

#include <stdio.h>
#include <string.h>

/* Aggregate admission-stage histogram. Prints no prompt and no answer. */

static const char *lane_name(CnetCompeteLane lane) {
    switch (lane) {
        case CNET_COMPETE_LANE_INCREMENT: return "increment";
        case CNET_COMPETE_LANE_MINUTES: return "minutes";
        case CNET_COMPETE_LANE_CRC8: return "crc8";
        case CNET_COMPETE_LANE_POLICY: return "policy";
        case CNET_COMPETE_LANE_COMPOSE3: return "compose3";
        case CNET_COMPETE_LANE_OOD: return "ood";
        default: return "unknown";
    }
}

static const char *refusal_name(CnetCompeteRefusal refusal) {
    switch (refusal) {
        case CNET_COMPETE_REFUSAL_NONE: return "none";
        case CNET_COMPETE_REFUSAL_INTENT_PROPOSAL: return "proposal";
        case CNET_COMPETE_REFUSAL_SEMANTIC_FRAME: return "frame";
        case CNET_COMPETE_REFUSAL_INTENT_DISAGREEMENT: return "disagree";
        case CNET_COMPETE_REFUSAL_ARGUMENT: return "argument";
        case CNET_COMPETE_REFUSAL_COVERAGE: return "coverage";
        case CNET_COMPETE_REFUSAL_EXECUTION: return "execution";
        default: return "other";
    }
}

int main(int argc, char **argv) {
    CnetCompeteEvalFixture fixture;
    CnetCompeteRuntime *runtime = NULL;
    CnetCompeteRuntimeReport report;
    size_t counts[CNET_COMPETE_LANE_COUNT][8];
    size_t answered[CNET_COMPETE_LANE_COUNT];
    size_t ood_intent[CNET_INTENT_ABSTAIN + 1u];
    size_t index, lane, refusal;
    char error[256];

    if (argc != 5) {
        fprintf(stderr, "usage: %s MODEL META CAPSULES FIXTURE\n", argv[0]);
        return 2;
    }
    memset(counts, 0, sizeof counts);
    memset(answered, 0, sizeof answered);
    memset(ood_intent, 0, sizeof ood_intent);
    memset(&fixture, 0, sizeof fixture);
    if (cnet_compete_eval_load_fixture(argv[4], &fixture, error,
                                       sizeof error) != 0 ||
        cnet_compete_runtime_load(argv[1], argv[2], argv[3], &runtime,
                                  &report) != 0) {
        fprintf(stderr, "load failed\n");
        return 1;
    }
    for (index = 0; index < fixture.count; ++index) {
        CnetCompeteResult result;
        CnetCompeteDiagnostic diagnostic;
        memset(&result, 0, sizeof result);
        memset(&diagnostic, 0, sizeof diagnostic);
        if (cnet_compete_runtime_execute_diagnostic(
                runtime, fixture.rows[index].prompt, &result,
                &diagnostic) != 0)
            diagnostic.refusal = CNET_COMPETE_REFUSAL_EXECUTION;
        lane = (size_t)fixture.rows[index].lane;
        refusal = (size_t)diagnostic.refusal;
        if (lane >= CNET_COMPETE_LANE_COUNT) lane = 0;
        if (refusal >= 8u) refusal = 7u;
        counts[lane][refusal] += 1u;
        if (result.answered) {
            answered[lane] += 1u;
            if (lane == (size_t)CNET_COMPETE_LANE_OOD &&
                result.intent <= CNET_INTENT_ABSTAIN)
                ood_intent[result.intent] += 1u;
        }
    }
    printf("CNET_7B_STAGE_HIST rows=%zu\n", fixture.count);
    for (lane = 0; lane < CNET_COMPETE_LANE_COUNT; ++lane) {
        printf("lane=%s answered=%zu", lane_name((CnetCompeteLane)lane),
               answered[lane]);
        for (refusal = 0; refusal < 7u; ++refusal)
            printf(" %s=%zu", refusal_name((CnetCompeteRefusal)refusal),
                   counts[lane][refusal]);
        printf("\n");
    }
    printf("ood_answered_as increment=%zu minutes=%zu crc=%zu policy=%zu "
           "compose=%zu\n",
           ood_intent[CNET_INTENT_INCREMENT],
           ood_intent[CNET_INTENT_MINUTES], ood_intent[CNET_INTENT_CRC8],
           ood_intent[CNET_INTENT_POLICY], ood_intent[CNET_INTENT_COMPOSE3]);
    cnet_compete_eval_free_fixture(&fixture);
    cnet_compete_runtime_free(runtime);
    return 0;
}
