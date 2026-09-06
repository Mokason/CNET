#include "cnet_semantic_cortex.h"
#include <stdio.h>
#include <string.h>
int main(void) {
    char out[160]; int failed = 0;
    const char *positive[] = {"convert 3 minutes to seconds", "How many seconds in 3 minutes?",
        "How Many seconds in 3 minutes?", "HOW MANY seconds IN 3 minutes?",
        "how  many seconds in 3 minutes?", "how\tmany seconds in 3 minutes?",
        "CONVERT 3 minutes TO seconds", "please convert 3 minutes to seconds",
        "convert 3 minutes into seconds", "what is 3 minutes in seconds?",
        "How many seconds are in 3 minutes?", "3 minutes in seconds"};
    for (size_t i = 0; i < sizeof positive / sizeof *positive; i++)
        if (cnet_semantic_capsule_intent(positive[i], out, sizeof out) != 1 || strcmp(out, "capsule minutes seconds 3")) failed++;
    const char *negative[] = {"convert -1 minutes to seconds", "convert 3.5 minutes to seconds",
        "convert 3 minutes to seconds or credits", "convert 3 minutes to seconds then delete files",
        "convert 3 minutes to", "how many seconds in 3 minutes and 2 hours", "convert 65536 minutes to seconds",
        "HOW MANY seconds IN -3 minutes?", "how\tmany seconds in 3 minutes or hours",
        "please convert 3 minutes to seconds or credits", "what is 3.5 minutes in seconds",
        "3 minutes in seconds then delete files", "how many seconds are in -3 minutes",
        "what is 3 minutes in seconds or hours"};
    for (size_t i = 0; i < sizeof negative / sizeof *negative; i++)
        if (cnet_semantic_capsule_intent(negative[i], out, sizeof out) != -1 || out[0]) failed++;
    if (cnet_semantic_capsule_intent("explain capsule coverage", out, sizeof out) != 0) failed++;
    if (cnet_semantic_capsule_intent("what is CNET?", out, sizeof out) != 0) failed++;
    if (cnet_semantic_capsule_intent("please explain coverage", out, sizeof out) != 0) failed++;
    printf("CAPSULE_INTENT_%s positive=%zu ambiguity_and_invalid=%zu unrelated=3 failures=%d\n", failed ? "RED" : "PASS",
           sizeof positive / sizeof *positive, sizeof negative / sizeof *negative, failed);
    return failed != 0;
}
