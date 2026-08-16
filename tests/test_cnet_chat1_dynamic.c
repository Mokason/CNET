#include "cnet_chat_fluency.h"
#include "cnet_compete_runtime.h"
#include "cnet_utterance.h"

#include <stdio.h>
#include <string.h>

/* Unique, independently authored chat questions with varying operands.
   End goal slice: a new number/phrasing still answers through two-gate
   + native speech. Not copied from held-out TSVs. */

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

static unsigned policy_of(int admin, int owner, int mfa, int suspended) {
    return ((admin || (owner && mfa)) && !suspended) ? 1u : 0u;
}

static int run_covered(CnetCompeteRuntime *runtime, CnetUtterState *state,
                       const char *prompt, const char *when,
                       const char *contract, const char *input,
                       unsigned expected, size_t guards) {
    CnetCompeteResult result;
    CnetCompeteDiagnostic diagnostic;
    CnetChatFluencyQuery query;
    CnetChatFluencyScore score;
    char spoken[CNET_UTTER_TEXT];
    char output_text[32];
    memset(&result, 0, sizeof result);
    memset(&diagnostic, 0, sizeof diagnostic);
    memset(&query, 0, sizeof query);
    if (cnet_compete_runtime_execute_diagnostic(runtime, prompt, &result,
                                                &diagnostic) != 0 ||
        !result.answered || result.value != expected ||
        result.composition_guard_checks != guards ||
        diagnostic.refusal != CNET_COMPETE_REFUSAL_NONE) {
        printf("CNET_CHAT1_DYNAMIC_RED reason=covered prompt=%s answered=%d "
               "value=%u expected=%u refusal=%d\n",
               prompt, result.answered, result.value, expected,
               (int)diagnostic.refusal);
        return 0;
    }
    snprintf(output_text, sizeof output_text, "%u", result.value);
    cnet_utter_state_set(state, "contract", contract);
    cnet_utter_state_set(state, "input", input);
    cnet_utter_state_set(state, "output", output_text);
    if (cnet_utter_compose_native(state, when, spoken, sizeof spoken) != 0) {
        printf("CNET_CHAT1_DYNAMIC_RED reason=speak prompt=%s\n", prompt);
        return 0;
    }
    query.answered = 1;
    query.when = when;
    query.contract = contract;
    query.input = input;
    query.output = output_text;
    cnet_chat_fluency_v1_score(spoken, &query, &score);
    if (!cnet_chat_fluency_v1_pass(&score, 1.0f)) {
        printf("CNET_CHAT1_DYNAMIC_RED reason=rubric prompt=%s spoken=%s "
               "overall=%.3f\n",
               prompt, spoken, (double)score.overall);
        return 0;
    }
    printf("  ok when=%s in=%s out=%s text=%s\n", when, input, output_text,
           spoken);
    return 1;
}

int main(int argc, char **argv) {
    static const unsigned inc_n[] = {0u, 7u, 41u, 200u, 255u};
    static const unsigned min_n[] = {3u, 15u, 90u, 154u};
    static const unsigned crc_n[] = {0u, 12u, 99u, 201u};
    static const unsigned cmp_n[] = {5u, 12u, 40u, 250u};
    static const struct {
        int admin, owner, mfa, suspended;
    } policies[] = {{1, 0, 0, 0}, {0, 1, 1, 0}, {0, 1, 0, 0}, {1, 1, 1, 1}};
    CnetCompeteRuntime *runtime = NULL;
    CnetCompeteRuntimeReport report;
    CnetUtterState state;
    size_t i, pass = 0, total = 0;
    int rc = 1;
    char prompt[256], input[16];

#define REQUIRE(c, r)                                                         \
    do {                                                                      \
        if (!(c)) {                                                           \
            printf("CNET_CHAT1_DYNAMIC_RED reason=%s\n", r);                  \
            goto done;                                                        \
        }                                                                     \
    } while (0)

    if (argc != 4) return 2;
    REQUIRE(cnet_compete_runtime_load(argv[1], argv[2], argv[3], &runtime,
                                      &report) == 0,
            "runtime_load");
    cnet_utter_state_init(&state);
    snprintf(state.source, sizeof state.source, "%s", "CNET");
    printf("CNET_CHAT1_DYNAMIC_RUN rubric=%s\n", CNET_CHAT_FLUENCY_V1_NAME);

    for (i = 0; i < sizeof inc_n / sizeof inc_n[0]; ++i) {
        snprintf(prompt, sizeof prompt, "Can you increment unsigned byte %u?",
                 inc_n[i]);
        snprintf(input, sizeof input, "%u", inc_n[i]);
        ++total;
        pass += (size_t)run_covered(runtime, &state, prompt, "increment",
                                    "increment_mod256", input,
                                    (inc_n[i] + 1u) & 255u, 0u);
        snprintf(prompt, sizeof prompt,
                 "Thanks, add one to octet %u modulo 256.", inc_n[i]);
        ++total;
        pass += (size_t)run_covered(runtime, &state, prompt, "increment",
                                    "increment_mod256", input,
                                    (inc_n[i] + 1u) & 255u, 0u);
    }
    for (i = 0; i < sizeof min_n / sizeof min_n[0]; ++i) {
        snprintf(prompt, sizeof prompt,
                 "Quick question: how many seconds are in %u minutes?",
                 min_n[i]);
        snprintf(input, sizeof input, "%u", min_n[i]);
        ++total;
        pass += (size_t)run_covered(runtime, &state, prompt, "minutes",
                                    "minutes_to_seconds", input, min_n[i] * 60u,
                                    0u);
    }
    for (i = 0; i < sizeof crc_n / sizeof crc_n[0]; ++i) {
        snprintf(prompt, sizeof prompt,
                 "Could you evaluate the ATM checksum of operand %u?", crc_n[i]);
        snprintf(input, sizeof input, "%u", crc_n[i]);
        ++total;
        pass += (size_t)run_covered(runtime, &state, prompt, "crc", "crc8_atm",
                                    input, reference_crc8(crc_n[i]), 0u);
    }
    for (i = 0; i < sizeof cmp_n / sizeof cmp_n[0]; ++i) {
        snprintf(prompt, sizeof prompt,
                 "Help me out: increment then double then add three to "
                 "unsigned byte %u.",
                 cmp_n[i]);
        snprintf(input, sizeof input, "%u", cmp_n[i]);
        ++total;
        pass += (size_t)run_covered(runtime, &state, prompt, "compose",
                                    "compose3_mod256", input,
                                    (((cmp_n[i] + 1u) * 2u) + 3u) & 255u, 3u);
    }
    for (i = 0; i < sizeof policies / sizeof policies[0]; ++i) {
        snprintf(prompt, sizeof prompt,
                 "In this conversation, decide access admin=%s owner=%s "
                 "mfa=%s suspended=%s.",
                 policies[i].admin ? "true" : "false",
                 policies[i].owner ? "true" : "false",
                 policies[i].mfa ? "true" : "false",
                 policies[i].suspended ? "true" : "false");
        ++total;
        pass += (size_t)run_covered(
            runtime, &state, prompt, "policy", "access_policy_v1", "four flags",
            policy_of(policies[i].admin, policies[i].owner, policies[i].mfa,
                      policies[i].suspended),
            0u);
    }
    REQUIRE(pass == total, "incomplete");
    printf("CNET_CHAT1_DYNAMIC_PASS answered=%zu unique=1 residual=0 "
           "broader_claims=WITHHELD\n",
           pass);
    rc = 0;
done:
    cnet_compete_runtime_free(runtime);
    return rc;
}
