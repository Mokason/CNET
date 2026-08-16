#include "cnet_compete_independence.h"

#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    const char *fixtures[8];
    const char *sources[8];
    static const char *const base_sources[] = {
        "src/cnet_compete_intent.c",
        "src/cnet_compete_runtime.c",
        "tests/test_cnet_compete_intent.c",
        "tests/test_cnet_compete_runtime.c"
    };
    const char *output_path;
    size_t fixture_count, source_count, index;
    char error[256];
    size_t count = 0;
    for (index = 0u;
         index < sizeof base_sources / sizeof base_sources[0]; ++index)
        sources[index] = base_sources[index];
    source_count = sizeof base_sources / sizeof base_sources[0];
    if (argc != 3 && argc != 4 && argc != 6) {
        fprintf(stderr,
                "usage: %s OUTPUT.tsv INTENT_DEVELOPMENT.tsv "
                "[SEMANTIC_DEVELOPMENT.tsv]\n"
                "       %s --pre-v2 OUTPUT.tsv "
                "INTENT_DEVELOPMENT.tsv\n"
                "       %s --v5 OUTPUT.tsv INTENT_DEVELOPMENT.tsv "
                "V4_SEMANTIC.tsv V5_SEMANTIC.tsv\n",
                argv[0],
                argv[0],
                argv[0]);
        return 2;
    }
    if (argc == 6 && strcmp(argv[1], "--v5") == 0) {
        output_path = argv[2];
        fixtures[0] = argv[3];
        fixtures[1] = argv[4];
        fixtures[2] = argv[5];
        fixtures[3] = "benchmarks/cnet_asi5_v1/heldout.tsv";
        fixtures[4] = "benchmarks/cnet_asi5_v2/heldout.tsv";
        fixtures[5] = "benchmarks/cnet_asi5_v3/heldout.tsv";
        fixtures[6] = "benchmarks/cnet_asi5_v4/heldout.tsv";
        fixture_count = 7u;
        sources[source_count++] = "src/cnet_compete_v5_semantics.c";
        sources[source_count++] = "tests/test_cnet_compete_v5_semantics.c";
        sources[source_count++] = "tests/test_cnet_compete_intent_v5.c";
    } else if (argc == 4 && strcmp(argv[1], "--pre-v2") == 0) {
        output_path = argv[2];
        fixtures[0] = argv[3];
        fixtures[1] = "benchmarks/cnet_asi5_v1/heldout.tsv";
        fixture_count = 2u;
        source_count = 1u;
    } else {
        output_path = argv[1];
        fixtures[0] = argv[2];
        fixtures[1] = "benchmarks/cnet_asi5_v1/heldout.tsv";
        fixtures[2] = "benchmarks/cnet_asi5_v2/heldout.tsv";
        fixture_count = 3u;
    }
    if (argc == 4 && strcmp(argv[1], "--pre-v2") != 0) {
        fixtures[1] = argv[3];
        fixtures[2] = "benchmarks/cnet_asi5_v1/heldout.tsv";
        fixtures[3] = "benchmarks/cnet_asi5_v2/heldout.tsv";
        fixtures[4] = "benchmarks/cnet_asi5_v3/heldout.tsv";
        fixture_count = 5u;
    }
    if (cnet_compete_independence_export_exclusions(
            output_path, fixtures, fixture_count,
            sources, source_count, &count,
            error, sizeof error) != CNET_INDEPENDENCE_OK) {
        fprintf(stderr, "exclusion export failed: %s\n", error);
        return 1;
    }
    printf("CNET_7B_EXCLUSIONS_PASS prompts=%zu\n", count);
    return 0;
}
