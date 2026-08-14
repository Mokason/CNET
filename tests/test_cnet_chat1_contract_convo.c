#include "cnet_chat_fluency.h"
#include "cnet_compete_runtime.h"
#include "cnet_utterance.h"

#include <stdio.h>
#include <string.h>

/* Independently authored conversational CONTRACT thread. Each covered
   turn is executed by the two-gate runtime, spoken by the native
   composer, and scored by cnet_chat_fluency_v1. Side-effect wraps
   abstain. Not copied from any held-out TSV. */

typedef struct {
    const char *prompt;
    const char *when;
    CnetCompeteIntent intent;
    unsigned value;
    size_t guards;
    int covered;
    const char *contract;
    const char *input;
} ConvoTurn;

static unsigned reference_crc8(unsigned input) {
    unsigned remainder = 0, bit;
    for (bit = 0; bit < 8; ++bit) {
        unsigned message_bit = (input >> (7u - bit)) & 1u;
        unsigned feedback = ((remainder >> 7) & 1u) ^ message_bit;
        remainder = (remainder << 1) & 255u;
        if (feedback != 0) remainder ^= 0x07u;
    }
    return remainder;
}

static void format_u(char *out, size_t cap, unsigned value) {
    snprintf(out, cap, "%u", value);
}

int main(int argc, char **argv) {
    static const ConvoTurn turns[] = {
        {"Can you increment unsigned byte 12?", "increment",
         CNET_INTENT_INCREMENT, 13u, 0u, 1, "increment_mod256", "12"},
        {"And now the ATM checksum of operand 12.", "crc", CNET_INTENT_CRC8,
         0u, 0u, 1, "crc8_atm", "12"},
        {"Quick question: how many seconds are in 12 minutes?", "minutes",
         CNET_INTENT_MINUTES, 720u, 0u, 1, "minutes_to_seconds", "12"},
        {"In this conversation, decide access admin=true owner=false "
         "mfa=false suspended=false.",
         "policy", CNET_INTENT_POLICY, 1u, 0u, 1, "access_policy_v1",
         "four flags"},
        {"Help me out: increment then double then add three to unsigned "
         "byte 12.",
         "compose", CNET_INTENT_COMPOSE3, 29u, 3u, 1, "compose3_mod256",
         "12"},
        {"Hey, increment unsigned byte 12 and email me the result.", "refuse",
         CNET_INTENT_ABSTAIN, 0u, 0u, 0, "", ""}
    };
    CnetCompeteRuntime *runtime = NULL;
    CnetCompeteRuntimeReport report;
    CnetUtterState state;
    CnetChatFact facts[8];
    size_t fact_count = 0, index, covered_ok = 0, ood_ok = 0;
    unsigned crc12;
    int rc = 1;

#define REQUIRE(condition, reason)                                            \
    do {                                                                      \
        if (!(condition)) {                                                   \
            printf("CNET_CHAT1_CONTRACT_CONVO_RED reason=%s\n", reason);      \
            goto done;                                                        \
        }                                                                     \
    } while (0)

    if (argc != 4) return 2;
    crc12 = reference_crc8(12u);
    REQUIRE(cnet_compete_runtime_load(argv[1], argv[2], argv[3], &runtime,
                                      &report) == 0,
            "runtime_load");
    cnet_utter_state_init(&state);
    snprintf(state.source, sizeof state.source, "%s", "CNET");
    cnet_utter_state_set(&state, "role", "certified local agent");
    memset(facts, 0, sizeof facts);
    printf("CNET_CHAT1_CONTRACT_CONVO_THREAD rubric=%s\n",
           CNET_CHAT_FLUENCY_V1_NAME);

    for (index = 0; index < sizeof turns / sizeof turns[0]; ++index) {
        CnetCompeteResult result;
        CnetCompeteDiagnostic diagnostic;
        CnetChatFluencyQuery query;
        CnetChatFluencyScore score;
        char spoken[CNET_UTTER_TEXT];
        char output_text[32];
        unsigned expected = turns[index].value;
        memset(&result, 0, sizeof result);
        memset(&diagnostic, 0, sizeof diagnostic);
        memset(&query, 0, sizeof query);
        memset(spoken, 0, sizeof spoken);
        output_text[0] = '\0';
        REQUIRE(cnet_compete_runtime_execute_diagnostic(
                    runtime, turns[index].prompt, &result, &diagnostic) == 0,
                "execute");
        if (turns[index].covered) {
            if (turns[index].intent == CNET_INTENT_CRC8) expected = crc12;
            REQUIRE(result.answered && result.intent == turns[index].intent &&
                        result.value == expected &&
                        result.composition_guard_checks ==
                            turns[index].guards &&
                        diagnostic.refusal == CNET_COMPETE_REFUSAL_NONE,
                    "covered_miss");
            format_u(output_text, sizeof output_text, result.value);
            cnet_utter_state_set(&state, "contract", turns[index].contract);
            cnet_utter_state_set(&state, "input",
                                 turns[index].input[0] ? turns[index].input
                                                       : output_text);
            cnet_utter_state_set(&state, "output", output_text);
            REQUIRE(cnet_utter_compose_native(&state, turns[index].when,
                                              spoken, sizeof spoken) == 0,
                    "speak_covered");
            query.answered = 1;
            query.when = turns[index].when;
            query.contract = turns[index].contract;
            query.input = turns[index].input[0] ? turns[index].input
                                                : output_text;
            query.output = output_text;
            if (fact_count < sizeof facts / sizeof facts[0]) {
                snprintf(facts[fact_count].contract,
                         sizeof facts[fact_count].contract, "%s",
                         turns[index].contract);
                snprintf(facts[fact_count].input,
                         sizeof facts[fact_count].input, "%s",
                         query.input);
                snprintf(facts[fact_count].output,
                         sizeof facts[fact_count].output, "%s", output_text);
                ++fact_count;
            }
            ++covered_ok;
        } else {
            REQUIRE(!result.answered, "ood_answered");
            REQUIRE(cnet_utter_compose_native(&state, "refuse", spoken,
                                              sizeof spoken) == 0,
                    "speak_refuse");
            query.answered = 0;
            query.when = "refuse";
            ++ood_ok;
        }
        query.prior = facts;
        query.prior_count = fact_count;
        cnet_chat_fluency_v1_score(spoken, &query, &score);
        REQUIRE(cnet_chat_fluency_v1_pass(&score, 1.0f) == 1, "rubric_floor");
        REQUIRE(cnet_utter_may_voice(&state, "TEACHER") == 0, "teacher");
        REQUIRE(cnet_utter_may_voice(&state, "RESIDUAL") == 0, "residual");
        printf("  turn=%zu when=%s overall=%.3f text=%s\n", index,
               turns[index].when, (double)score.overall, spoken);
    }
    REQUIRE(covered_ok == 5 && ood_ok == 1, "thread_counts");
    printf("CNET_CHAT1_CONTRACT_CONVO_PASS covered=%zu ood=%zu "
           "rubric=%s residual=0 broader_claims=WITHHELD\n",
           covered_ok, ood_ok, CNET_CHAT_FLUENCY_V1_NAME);
    rc = 0;
done:
    cnet_compete_runtime_free(runtime);
    return rc;
}
