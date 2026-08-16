#include "cnet_utterance.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* CHAT-1.3: native clause assembly must produce coherent speech that is
   CNET's own (slots + glue), not a prewritten bank paragraph and not a
   residual teacher draft. */

static int looks_bank_blob(const char *spoken) {
    return spoken != NULL &&
           (strstr(spoken, "Continuous self on this host") != NULL ||
            strstr(spoken, "continuous Hermes self") != NULL ||
            strstr(spoken, "I am Marble —") != NULL ||
            strstr(spoken, "Speech capsule ready") != NULL);
}

int main(void) {
    static const char *const turns[] = {
        "identity", "status", "increment", "miss", "refuse"
    };
    CnetUtterBank bank;
    CnetUtterState state;
    char spoken[CNET_UTTER_TEXT];
    char previous[CNET_UTTER_TEXT];
    size_t index, pass = 0;
    int rc = 1;

#define REQUIRE(condition, reason)                                            \
    do {                                                                      \
        if (!(condition)) {                                                   \
            printf("CNET_CHAT1_FLUENCY_RED reason=%s spoken=%s\n", reason,    \
                   spoken);                                                   \
            goto done;                                                        \
        }                                                                     \
    } while (0)

    cnet_utter_bank_init_default(&bank);
    cnet_utter_state_init(&state);
    snprintf(state.source, sizeof state.source, "%s", "CNET");
    snprintf(state.domain, sizeof state.domain, "%s", "chat");
    cnet_utter_state_set(&state, "role", "certified local agent");
    cnet_utter_state_set(&state, "contract", "increment_mod256");
    cnet_utter_state_set(&state, "input", "12");
    cnet_utter_state_set(&state, "output", "13");
    state.local_hit = 0.86;
    state.miss_n = 1;
    previous[0] = '\0';

    printf("CNET_CHAT1_FLUENCY_THREAD\n");
    for (index = 0; index < sizeof turns / sizeof turns[0]; ++index) {
        memset(spoken, 0, sizeof spoken);
        REQUIRE(cnet_utter_compose_native(&state, turns[index], spoken,
                                          sizeof spoken) == 0,
                "compose_native");
        REQUIRE(spoken[0] != '\0', "empty");
        REQUIRE(isupper((unsigned char)spoken[0]), "capital");
        REQUIRE(spoken[strlen(spoken) - 1u] == '.', "period");
        REQUIRE(strstr(spoken, "  ") == NULL, "double_space");
        REQUIRE(strchr(spoken, '{') == NULL, "unfilled_slot");
        REQUIRE(!looks_bank_blob(spoken), "bank_blob");
        REQUIRE(cnet_utter_fluency_check(&bank, &state, turns[index], spoken) ==
                    1,
                "fluency_check");
        REQUIRE(cnet_utter_may_voice(&state, "TEACHER") == 0, "teacher");
        REQUIRE(cnet_utter_may_voice(&state, "RESIDUAL") == 0, "residual");
        REQUIRE(strstr(spoken, "Marble") != NULL, "speaker");
        REQUIRE(previous[0] == '\0' || strcmp(previous, spoken) != 0,
                "same_blob");
        if (strcmp(turns[index], "increment") == 0) {
            REQUIRE(strstr(spoken, "12") != NULL && strstr(spoken, "13") != NULL,
                    "contract_slots");
            REQUIRE(strstr(spoken, "increment") != NULL, "contract_name");
            REQUIRE(strstr(spoken, "successor") != NULL ||
                        strstr(spoken, "computes") != NULL,
                    "increment_predicate");
        }
        if (strcmp(turns[index], "refuse") == 0 ||
            strcmp(turns[index], "identity") == 0)
            REQUIRE(strstr(spoken, "never self-cert") != NULL, "law");
        printf("  turn=%s text=%s\n", turns[index], spoken);
        snprintf(previous, sizeof previous, "%s", spoken);
        ++pass;
    }
    REQUIRE(pass == sizeof turns / sizeof turns[0], "thread_short");
    printf("CNET_CHAT1_FLUENCY_PASS turns=%zu own_text=1 bank_blob=0 "
           "residual=0 broader_claims=WITHHELD\n",
           pass);
    rc = 0;
done:
    return rc;
}
