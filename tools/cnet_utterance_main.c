/* cnet_utterance CLI — compose C-native speakable lines (never Teacher).
 *   ./bin/cnet_utterance --test
 *   ./bin/cnet_utterance --source LOCAL --skill soul_who --answer "I am Marble..." 
 *   ./bin/cnet_utterance --when status --hit 0.86 --da 0.5 --ht 0.5 --ado 0.4
 */
#include "../include/cnet_utterance.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;

#define CHECK(c, m)                                                            \
    do {                                                                       \
        if (!(c)) {                                                            \
            fprintf(stderr, "FAIL %s\n", m);                                   \
            fails++;                                                           \
        }                                                                      \
    } while (0)

static int selftest(void) {
    CnetUtterBank B;
    CnetUtterState S;
    char out[CNET_UTTER_TEXT];
    cnet_utter_bank_init_default(&B);
    cnet_utter_state_init(&S);
    snprintf(S.source, sizeof S.source, "LOCAL");
    snprintf(S.skill, sizeof S.skill, "soul_who");
    snprintf(S.base_answer, sizeof S.base_answer, "%s",
             "I am Marble — continuous Hermes self on this machine.");
    snprintf(S.pattern, sizeof S.pattern, "who are you");
    CHECK(cnet_utter_compose(&B, &S, "identity", out, sizeof out) == 0, "compose id");
    CHECK(strstr(out, "Marble") != NULL, "has Marble");
    CHECK(cnet_utter_may_voice(&S, "LOCAL") == 1, "voice local");
    CHECK(cnet_utter_may_voice(&S, "LLM") == 0, "no voice llm default");
    S.never_voice_llm = 0;
    CHECK(cnet_utter_may_voice(&S, "LLM") == 1, "voice llm override");
    S.never_voice_llm = 1;
    S.local_hit = 0.857;
    S.dopamine = 0.47;
    S.serotonin = 0.53;
    S.adenosine = 0.51;
    S.miss_n = 2;
    CHECK(cnet_utter_compose(&B, &S, "status", out, sizeof out) == 0, "status");
    CHECK(strstr(out, "Local hit") != NULL || strstr(out, "percent") != NULL, "hit in status");
    cnet_utter_chain_brief("PARSE | SPLIT | ACT | SHOW", out, sizeof out);
    CHECK(strstr(out, "PARSE") != NULL, "chain");
    snprintf(S.chain_brief, sizeof S.chain_brief, "%s", out);
    snprintf(S.base_answer, sizeof S.base_answer, "%s", "done");
    CHECK(cnet_utter_compose(&B, &S, "chain", out, sizeof out) == 0, "chain compose");
    if (fails) {
        printf("CNET_UTTERANCE_FAIL fails=%d\n", fails);
        return 1;
    }
    printf("CNET_UTTERANCE_PASS\n");
    return 0;
}

int main(int argc, char **argv) {
    CnetUtterBank B;
    CnetUtterState S;
    char out[CNET_UTTER_TEXT];
    const char *when = NULL;
    int i;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--test")) return selftest();
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            fprintf(stderr,
                    "usage: cnet_utterance --test | [--when identity|status|miss|chain|generic]\n"
                    "  --source LOCAL --skill ID --answer TEXT --pattern P\n"
                    "  --hit 0.86 --da 0.5 --ht 0.5 --ado 0.4 --miss 2\n"
                    "  --chain \"A|B|C\" --allow-voice-llm\n");
            return 0;
        }
    }

    cnet_utter_bank_init_default(&B);
    cnet_utter_bank_load_tsv(&B, "config/utterance_phrases.tsv");
    cnet_utter_state_init(&S);
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--when") && i + 1 < argc) when = argv[++i];
        else if (!strcmp(argv[i], "--source") && i + 1 < argc)
            snprintf(S.source, sizeof S.source, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--skill") && i + 1 < argc)
            snprintf(S.skill, sizeof S.skill, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--answer") && i + 1 < argc)
            snprintf(S.base_answer, sizeof S.base_answer, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--pattern") && i + 1 < argc)
            snprintf(S.pattern, sizeof S.pattern, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--name") && i + 1 < argc)
            snprintf(S.name, sizeof S.name, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--hit") && i + 1 < argc)
            S.local_hit = atof(argv[++i]);
        else if (!strcmp(argv[i], "--da") && i + 1 < argc)
            S.dopamine = atof(argv[++i]);
        else if (!strcmp(argv[i], "--ht") && i + 1 < argc)
            S.serotonin = atof(argv[++i]);
        else if (!strcmp(argv[i], "--ado") && i + 1 < argc)
            S.adenosine = atof(argv[++i]);
        else if (!strcmp(argv[i], "--miss") && i + 1 < argc)
            S.miss_n = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--chain") && i + 1 < argc) {
            cnet_utter_chain_brief(argv[++i], S.chain_brief, sizeof S.chain_brief);
        } else if (!strcmp(argv[i], "--allow-voice-llm"))
            S.never_voice_llm = 0;
        else if (!strcmp(argv[i], "--domain") && i + 1 < argc)
            snprintf(S.domain, sizeof S.domain, "%s", argv[++i]);
    }
    if (!S.source[0]) snprintf(S.source, sizeof S.source, "LOCAL");
    if (cnet_utter_compose(&B, &S, when, out, sizeof out) != 0) return 1;
    printf("%s\n", out);
    printf("may_voice=%d never_voice_llm=%d source=%s\n",
           cnet_utter_may_voice(&S, S.source), S.never_voice_llm, S.source);
    return 0;
}
