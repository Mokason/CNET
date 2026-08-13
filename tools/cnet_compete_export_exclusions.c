#include "cnet_compete_independence.h"

#include <stdio.h>

int main(int argc, char **argv) {
    const char *fixtures[3];
    static const char *const sources[] = {
        "src/cnet_compete_intent.c",
        "src/cnet_compete_runtime.c",
        "tests/test_cnet_compete_intent.c",
        "tests/test_cnet_compete_runtime.c"
    };
    char error[256];
    size_t count = 0;
    if (argc != 3) {
        fprintf(stderr, "usage: %s OUTPUT.tsv DEVELOPMENT.tsv\n", argv[0]);
        return 2;
    }
    fixtures[0] = argv[2];
    fixtures[1] = "benchmarks/cnet_asi5_v1/heldout.tsv";
    fixtures[2] = "benchmarks/cnet_asi5_v2/heldout.tsv";
    if (cnet_compete_independence_export_exclusions(
            argv[1], fixtures, sizeof fixtures / sizeof fixtures[0],
            sources, sizeof sources / sizeof sources[0], &count,
            error, sizeof error) != CNET_INDEPENDENCE_OK) {
        fprintf(stderr, "exclusion export failed: %s\n", error);
        return 1;
    }
    printf("CNET_7B_EXCLUSIONS_PASS prompts=%zu\n", count);
    return 0;
}
