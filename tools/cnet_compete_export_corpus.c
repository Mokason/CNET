#include "cnet_compete_intent.h"

#include <stdio.h>

int main(int argc, char **argv) {
    size_t prompts = 0;
    if (argc != 2) {
        fprintf(stderr, "usage: %s OUTPUT.tsv\n", argv[0]);
        return 2;
    }
    if (cnet_compete_intent_export_development_corpus(argv[1], &prompts) != 0) {
        fprintf(stderr, "failed to export development corpus\n");
        return 1;
    }
    printf("CNET_7B_DEVELOPMENT_CORPUS_PASS prompts=%zu\n", prompts);
    return 0;
}
