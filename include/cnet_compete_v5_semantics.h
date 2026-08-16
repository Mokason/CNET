#ifndef CNET_COMPETE_V5_SEMANTICS_H
#define CNET_COMPETE_V5_SEMANTICS_H

#include "cnet_compete_runtime.h"

#include <stddef.h>

#define CNET_COMPETE_V5_CASES_PER_INTENT 32u
#define CNET_COMPETE_V5_COVERED_CASES \
    (CNET_COMPETE_V5_CASES_PER_INTENT * 5u)
#define CNET_COMPETE_V5_OOD_CASES CNET_COMPETE_V5_COVERED_CASES
#define CNET_COMPETE_V5_TOTAL_CASES \
    (CNET_COMPETE_V5_COVERED_CASES + CNET_COMPETE_V5_OOD_CASES)
#define CNET_COMPETE_V5_PROMPT_MAX 512u

typedef struct {
    char prompt[CNET_COMPETE_V5_PROMPT_MAX];
    CnetCompeteIntent intent;
    unsigned value;
    size_t guard_checks;
    int covered;
} CnetCompeteV5SemanticCase;

int cnet_compete_v5_semantic_case(
    size_t index, CnetCompeteV5SemanticCase *semantic_case);

#endif
