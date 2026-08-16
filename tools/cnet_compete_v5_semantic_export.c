#include "cnet_compete_v5_semantics.h"

#include <stdio.h>
#include <string.h>

static const char *intent_name(CnetCompeteIntent intent) {
    static const char *const names[CNET_INTENT_COUNT] = {
        "increment_mod256", "minutes_to_seconds", "crc8_atm",
        "access_policy_v1", "compose3_mod256", "none"
    };
    return intent >= CNET_INTENT_INCREMENT && intent < CNET_INTENT_COUNT
               ? names[intent]
               : NULL;
}

int main(int argc, char **argv) {
    FILE *output = NULL;
    size_t index, covered = 0u, ood = 0u;
    int result = 2;
    if (argc != 2) return 2;
    output = fopen(argv[1], "wb");
    if (output == NULL) return 2;
    if (fprintf(output, "#suite=CNET-ASI-5-semantic-development-v5\n") < 0 ||
        fprintf(output,
                "id\tsplit\tintent\tvalue_kind\texpected_value\tprompt\t"
                "provenance\n") < 0)
        goto done;
    for (index = 0u; index < CNET_COMPETE_V5_TOTAL_CASES; ++index) {
        CnetCompeteV5SemanticCase semantic_case;
        const char *name;
        if (cnet_compete_v5_semantic_case(index, &semantic_case) != 0 ||
            semantic_case.prompt[0] == '\0' ||
            strchr(semantic_case.prompt, '\t') != NULL ||
            strchr(semantic_case.prompt, '\n') != NULL ||
            strchr(semantic_case.prompt, '\r') != NULL)
            goto done;
        name = intent_name(semantic_case.intent);
        if (name == NULL ||
            fprintf(output,
                    "v5-semantic-%04zu\tdevelopment\t%s\tnone\t\t%s\t"
                    "semantic_boundary_development_v5\n",
                    index, name, semantic_case.prompt) < 0)
            goto done;
        if (semantic_case.covered) ++covered;
        else ++ood;
    }
    if (covered != CNET_COMPETE_V5_COVERED_CASES ||
        ood != CNET_COMPETE_V5_OOD_CASES || fflush(output) != 0)
        goto done;
    result = 0;
done:
    if (fclose(output) != 0) result = 2;
    if (result == 0)
        printf("CNET_7B_V5_SEMANTIC_EXPORT_PASS covered=%zu ood=%zu "
               "answer_values=0\n",
               covered, ood);
    return result;
}
