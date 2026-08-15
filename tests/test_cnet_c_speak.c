/* test_cnet_c_speak — draft mouth wrap only.
 *
 * DRAFT MOUTH — WRAP ONLY.
 * C (cce_wordlm_predict) drafts glue. A asserts slots.
 * Teacher / residual is never the mouth. Not open chat. Not F11.
 */
#include "../include/cnet_c_speak.h"
#include "../include/cnet_utterance.h"

#include <stdio.h>
#include <string.h>

static int failures;
static int checks;

static void check(int cond, const char *msg) {
    checks++;
    if (cond) {
        printf("  ok   %s\n", msg);
    } else {
        printf("  FAIL %s\n", msg);
        failures++;
    }
}

static void fill_hop(CnetChatLookupTurn *hop, const char *value,
                     const char *host) {
    memset(hop, 0, sizeof *hop);
    hop->answered = 1;
    hop->report.bound = 1;
    hop->residual_calls = 0;
    snprintf(hop->report.value, sizeof hop->report.value, "%s", value);
    snprintf(hop->report.host, sizeof hop->report.host, "%s", host);
    snprintf(hop->report.sha256, sizeof hop->report.sha256,
             "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    snprintf(hop->spoken, sizeof hop->spoken,
             "Marble reports %s from %s (%s, sha256=%s).", value, host,
             CNET_LOOKUP_CONTRACT, hop->report.sha256);
}

int main(void) {
    CnetCSpeakSlots slots;
    CnetCSpeakResult wrap;
    CnetChatLookupTurn hop;
    CnetUtterState U;
    const char *teacher = "The capital of France is Paris and the year is 1799.";

    printf("cnet_c_speak — DRAFT MOUTH / WRAP ONLY\n");

    check(cnet_c_speak_init() == 0, "leaf_init");
    check(strcmp(cnet_c_speak_leaf(), "cce_wordlm_predict") == 0, "leaf_symbol");

    memset(&slots, 0, sizeof slots);
    snprintf(slots.value, sizeof slots.value, "13");
    snprintf(slots.host, sizeof slots.host, "local-file");
    snprintf(slots.contract, sizeof slots.contract, "%s", CNET_LOOKUP_CONTRACT);
    slots.bound = 1;
    check(cnet_c_speak_wrap(&slots, &wrap) == 0, "bound_wrap_rc");
    check(wrap.wrapped == 1, "bound_wrapped");
    check(wrap.claimed_cert == 1, "bound_claims_cert");
    check(strstr(wrap.spoken, "13") != NULL, "bound_has_A_value");
    check(wrap.leaf_calls > 0, "bound_called_leaf");
    check(wrap.residual_calls == 0, "bound_residual_zero");
    check(strstr(wrap.spoken, teacher) == NULL, "bound_not_teacher_text");
    check(strcmp(wrap.spoken, teacher) != 0, "bound_spoken_ne_teacher");
    check(wrap.spoken[0] >= 'A' && wrap.spoken[0] <= 'Z', "bound_sentence_case");
    check(wrap.spoken[strlen(wrap.spoken) - 1u] == '.', "bound_sentence_end");

    memset(&slots, 0, sizeof slots);
    snprintf(slots.value, sizeof slots.value, "hello");
    snprintf(slots.host, sizeof slots.host, "example.com");
    slots.bound = 1;
    check(cnet_c_speak_wrap(&slots, &wrap) == 0, "second_value_rc");
    check(strstr(wrap.spoken, "hello") != NULL, "second_has_A_value");
    check(strstr(wrap.spoken, "13") == NULL, "second_not_stale_value");

    memset(&slots, 0, sizeof slots);
    check(cnet_c_speak_wrap(&slots, &wrap) == 1, "nobind_refuse");
    check(wrap.wrapped == 0, "nobind_not_wrapped");
    check(wrap.claimed_cert == 0, "nobind_no_cert_claim");
    check(wrap.spoken[0] == '\0', "nobind_silent");
    check(strcmp(wrap.refusal, "no_bind") == 0, "nobind_reason");

    fill_hop(&hop, "13", "local-file");
    check(cnet_c_speak_after_lookup(&hop, &wrap) == 0, "lookup_wrap_rc");
    check(strstr(wrap.spoken, "13") != NULL, "lookup_has_A_value");
    check(wrap.residual_calls == 0, "lookup_residual_zero");
    check(wrap.claimed_cert == 1, "lookup_claims_bound");
    check(strstr(wrap.spoken, teacher) == NULL, "lookup_not_teacher");

    memset(&hop, 0, sizeof hop);
    hop.answered = 0;
    hop.residual_calls = 0;
    snprintf(hop.refusal, sizeof hop.refusal, "host");
    check(cnet_c_speak_after_lookup(&hop, &wrap) == 1, "ssrf_unanswered_refuse");
    check(wrap.wrapped == 0, "ssrf_not_wrapped");
    check(wrap.claimed_cert == 0, "ssrf_no_cert");
    check(wrap.spoken[0] == '\0', "ssrf_silent");
    check(wrap.residual_calls == 0, "ssrf_residual_zero");

    hop.answered = 1;
    hop.report.bound = 1;
    hop.residual_calls = 1;
    snprintf(hop.report.value, sizeof hop.report.value, "13");
    check(cnet_c_speak_after_lookup(&hop, &wrap) == 1, "residual_mouth_refuse");
    check(wrap.claimed_cert == 0, "residual_no_cert");

    check(cnet_c_speak_after_capsule("42", "increment_mod256", &wrap) == 0,
          "capsule_wrap_rc");
    check(strstr(wrap.spoken, "42") != NULL, "capsule_has_A_value");
    check(wrap.residual_calls == 0, "capsule_residual_zero");
    check(cnet_c_speak_after_capsule("", "increment_mod256", &wrap) == 1,
          "capsule_empty_refuse");
    check(wrap.claimed_cert == 0, "capsule_empty_no_cert");

    memset(&slots, 0, sizeof slots);
    snprintf(slots.value, sizeof slots.value, "13");
    slots.bound = 1;
    check(cnet_c_speak_cd_ask_step(&slots, teacher, "LLM", 1, &wrap) == 0,
          "cd_ask_bound_over_teacher");
    check(strstr(wrap.spoken, "13") != NULL, "cd_ask_bound_value");
    check(strstr(wrap.spoken, teacher) == NULL, "cd_ask_teacher_not_spoken");
    check(wrap.claimed_cert == 1, "cd_ask_bound_cert");
    check(wrap.residual_calls == 0, "cd_ask_bound_residual_zero");

    check(cnet_c_speak_cd_ask_step(NULL, teacher, "LLM", 1, &wrap) == 1,
          "cd_ask_llm_nobind_refuse");
    check(wrap.spoken[0] == '\0', "cd_ask_llm_silent");
    check(strcmp(wrap.spoken, teacher) != 0, "cd_ask_llm_not_teacher_line");
    check(wrap.claimed_cert == 0, "cd_ask_llm_no_cert");
    check(wrap.may_voice == 0, "cd_ask_never_voice_llm");

    check(cnet_c_speak_may_voice("LLM", 1) == 0, "may_voice_blocks_llm");
    check(cnet_c_speak_may_voice("TEACHER", 1) == 0, "may_voice_blocks_teacher");
    check(cnet_c_speak_may_voice("RESIDUAL", 1) == 0, "may_voice_blocks_residual");
    check(cnet_c_speak_may_voice("CNET", 1) == 1, "may_voice_allows_cnet");
    check(cnet_c_speak_may_voice("LOCAL", 1) == 1, "may_voice_allows_local");

    cnet_utter_state_init(&U);
    check(U.never_voice_llm == 1, "utter_never_voice_default");
    check(cnet_utter_may_voice(&U, "LLM") == 0, "utter_still_blocks_llm");
    U.never_voice_llm = 0;
    check(cnet_utter_may_voice(&U, "LLM") == 1, "utter_override_still_works");

    check(cnet_c_speak_slot_like("13") == 1, "slot_like_scalar");
    check(cnet_c_speak_slot_like("hello") == 1, "slot_like_token");
    check(cnet_c_speak_slot_like("Marble is the certified local agent.") == 0,
          "slot_like_rejects_prose");

    cnet_c_speak_shutdown();

    if (failures) {
        printf("CNET_C_SPEAK_RED failures=%d checks=%d\n", failures, checks);
        return 1;
    }
    printf("CNET_C_SPEAK_PASS\n");
    printf("checks=%d leaf=%s residual=0 broader_claims=WITHHELD\n", checks,
           cnet_c_speak_leaf());
    return 0;
}
