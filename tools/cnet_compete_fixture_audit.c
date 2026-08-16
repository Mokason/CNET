#include "cnet_compete_independence.h"

#include <stdio.h>

int main(int argc, char **argv) {
    CnetCompeteIndependenceReport report;
    const char *exclusions[1];
    char error[512];
    int result;
    if (argc != 3) {
        fprintf(stderr, "usage: %s CANDIDATE.tsv EXCLUDED.tsv\n", argv[0]);
        return 2;
    }
    exclusions[0] = argv[2];
    result = cnet_compete_independence_audit(
        argv[1], exclusions, 1, NULL, 0, &report, error, sizeof error);
    if (result != CNET_INDEPENDENCE_OK) {
        printf("CNET_7B_INDEPENDENCE_FAIL rc=%d candidates=%zu "
               "exclusions=%zu duplicates=%zu canonical=%zu near=%zu "
               "reason=%s\n",
               result, report.candidate_prompts, report.excluded_prompts,
               report.duplicate_prompts, report.canonical_matches,
               report.near_matches, error);
        return 1;
    }
    printf("CNET_7B_INDEPENDENCE_PASS candidates=%zu exclusions=%zu "
           "duplicates=0 canonical=0 near=0\n",
           report.candidate_prompts, report.excluded_prompts);
    return 0;
}
