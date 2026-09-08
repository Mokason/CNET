#include "cnet_query_alias.h"
#include <stdio.h>
#include <string.h>

int main(void) {
    static const char *const unrelated[] = {
        "Can you explain what a language model is?",
        "What are you able to calculate?",
        "What are you doing tomorrow?",
        "Explain how your chatbot works",
        "Are you using a language model to translate this?",
        "Tell me your name and calculate 17 times 3",
        "Who made you laugh?",
        "The article asks what are you",
        "Are you Marble or can you convert bytes to bits?"
    };
    CnetQueryAliasTable table;
    CnetQueryPrepareMeta meta;
    char out[CNET_QA_OUT], norm[CNET_QA_OUT];
    unsigned i, failures = 0;
    cnet_query_alias_init(&table);
    for (i = 0; i < sizeof unrelated / sizeof unrelated[0]; i++) {
        cnet_query_normalize(unrelated[i], norm, sizeof norm);
        cnet_query_prepare(&table, unrelated[i], out, sizeof out, &meta);
        if (meta.alias_hit || strcmp(out, norm) || cnet_query_identity_bot(norm)) {
            printf("IDENTITY_ALIAS_RED case=%u\n", i);
            failures++;
        }
    }
    if (failures) return 1;
    puts("IDENTITY_ALIAS_PASS unrelated=9");
    return 0;
}
