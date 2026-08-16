#include "cnet_compete_runtime.h"

#include <stdio.h>
#include <string.h>

#define REQUIRE(condition, reason)                                            \
    do {                                                                      \
        if (!(condition)) {                                                   \
            printf("CNET_7B_V5_DIAGNOSTIC_RED reason=%s\n", reason);       \
            goto done;                                                        \
        }                                                                     \
    } while (0)

int main(int argc, char **argv) {
    CnetCompeteRuntime *runtime = NULL;
    CnetCompeteRuntimeReport report;
    CnetCompeteResult result, serving_result;
    CnetCompeteDiagnostic diagnostic;
    int rc = 1;
    if (argc != 4) return 2;
    memset(&report, 0, sizeof report);
    REQUIRE(cnet_compete_runtime_load(argv[1], argv[2], argv[3], &runtime,
                                      &report) == 0,
            "runtime_load");
    memset(&result, 0, sizeof result);
    memset(&diagnostic, 0, sizeof diagnostic);
    REQUIRE(cnet_compete_runtime_execute_diagnostic(
                runtime,
                "Take unsigned byte 12 forward by one with wraparound.",
                &result, &diagnostic) == 0 && result.answered &&
                result.intent == CNET_INTENT_INCREMENT &&
                diagnostic.refusal == CNET_COMPETE_REFUSAL_NONE &&
                diagnostic.proposed_intent == CNET_INTENT_INCREMENT &&
                diagnostic.semantic_intent == CNET_INTENT_INCREMENT &&
                diagnostic.confidence == result.confidence,
            "answered_stage");
    memset(&serving_result, 0, sizeof serving_result);
    REQUIRE(cnet_compete_runtime_execute(
                runtime,
                "Take unsigned byte 12 forward by one with wraparound.",
                &serving_result) == 0 &&
                serving_result.answered == result.answered &&
                serving_result.intent == result.intent &&
                serving_result.value == result.value &&
                serving_result.confidence == result.confidence &&
                serving_result.composition_guard_checks ==
                    result.composition_guard_checks,
            "serving_path_drift");
    memset(&result, 0, sizeof result);
    memset(&diagnostic, 0, sizeof diagnostic);
    REQUIRE(cnet_compete_runtime_execute_diagnostic(
                runtime, "Tell me a joke about databases.", &result,
                &diagnostic) == 0 && !result.answered &&
                diagnostic.refusal ==
                    CNET_COMPETE_REFUSAL_INTENT_PROPOSAL &&
                diagnostic.proposed_intent == CNET_INTENT_ABSTAIN &&
                diagnostic.semantic_intent == CNET_INTENT_ABSTAIN,
            "intent_refusal_stage");
    memset(&result, 0, sizeof result);
    memset(&diagnostic, 0, sizeof diagnostic);
    REQUIRE(cnet_compete_runtime_execute_diagnostic(
                runtime, "Increment signed byte 12 with wraparound.", &result,
                &diagnostic) == 0 && !result.answered &&
                diagnostic.refusal == CNET_COMPETE_REFUSAL_SEMANTIC_FRAME &&
                diagnostic.proposed_intent == CNET_INTENT_INCREMENT,
            "semantic_refusal_stage");
    memset(&result, 0, sizeof result);
    memset(&diagnostic, 0, sizeof diagnostic);
    REQUIRE(cnet_compete_runtime_execute_diagnostic(
                runtime, "Increment unsigned byte with modulo 256 wraparound.",
                &result,
                &diagnostic) == 0 && !result.answered &&
                diagnostic.refusal == CNET_COMPETE_REFUSAL_ARGUMENT &&
                diagnostic.proposed_intent == CNET_INTENT_INCREMENT &&
                diagnostic.semantic_intent == CNET_INTENT_INCREMENT,
            "argument_refusal_stage");
    REQUIRE(cnet_compete_runtime_execute_diagnostic(
                NULL, "byte 1", &result, &diagnostic) != 0 &&
                cnet_compete_runtime_execute_diagnostic(
                    runtime, NULL, &result, &diagnostic) != 0 &&
                cnet_compete_runtime_execute_diagnostic(
                    runtime, "byte 1", NULL, &diagnostic) != 0 &&
                cnet_compete_runtime_execute_diagnostic(
                    runtime, "byte 1", &result, NULL) != 0,
            "invalid_arguments");
    printf("CNET_7B_V5_DIAGNOSTIC_PASS stages=4 serving_path_identity=1 "
           "answer_values_exposed=0\n");
    rc = 0;
done:
    cnet_compete_runtime_free(runtime);
#undef REQUIRE
    return rc;
}
