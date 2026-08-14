#include "cnet_compete_runtime.h"

#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    CnetCompeteRuntime *runtime = NULL;
    CnetCompeteRuntimeReport report;
    CnetCompeteResult result;
    CnetCompeteDiagnostic diagnostic;
    int execution;
    if (argc != 4) return 2;
    memset(&report, 0, sizeof report);
    if (cnet_compete_runtime_load(argv[1], argv[2], argv[3], &runtime,
                                  &report) != 0)
        return 2;
    memset(&result, 0, sizeof result);
    memset(&diagnostic, 0, sizeof diagnostic);
    execution = cnet_compete_runtime_execute_diagnostic(
        runtime,
        "Take unsigned byte 12 forward by one with wraparound.",
        &result, &diagnostic);
    cnet_compete_runtime_free(runtime);
    if (execution != 0) {
        printf("CNET_7B_V5_DIAGNOSTIC_RED reason=api_unimplemented\n");
        return 1;
    }
    printf("CNET_7B_V5_DIAGNOSTIC_GREEN\n");
    return 0;
}
