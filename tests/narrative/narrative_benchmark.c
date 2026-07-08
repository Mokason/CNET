#include <stdio.h>
#include <stdlib.h>
#include "contract/narrative_coherence.h"

static void run_test(const char* filename, const char* label) {
    FILE* f = fopen(filename, "r");
    if (!f) {
        printf("%s: Could not open file.\n", label);
        return;
    }

    char buffer[2048];
    size_t len = fread(buffer, 1, sizeof(buffer) - 1, f);
    buffer[len] = '\0';
    fclose(f);

    NarrativeCoherenceConfig config = NARRATIVE_DEFAULT_CONFIG;
    NarrativeCoherenceScore score = cnet_narrative_evaluate(buffer, &config);

    printf("%s -> Overall: %.2f | Passes: %s\n", label, score.overall_score, 
           cnet_narrative_passes(&score, &config) ? "true" : "false");
}

int main(void) {
    run_test("tests/narrative/test_story.txt", "Rich narrative");
    run_test("tests/narrative/test_story_generic.txt", "Generic story");
    return 0;
}