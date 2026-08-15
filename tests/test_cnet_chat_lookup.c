#define _POSIX_C_SOURCE 200809L

#include "cnet_chat_lookup.h"
#include "cnet_utterance.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int write_temp(char *path, size_t cap, const char *body, size_t n) {
    int fd;
    snprintf(path, cap, "/tmp/cnet-chat-lookup-XXXXXX");
    fd = mkstemp(path);
    if (fd < 0) return -1;
    if (body != NULL && n > 0 && write(fd, body, n) != (ssize_t)n) {
        close(fd);
        unlink(path);
        return -1;
    }
    close(fd);
    return 0;
}

int main(void) {
    char bound_path[128];
    char words_path[128];
    char turn[768];
    char spoken[512];
    CnetChatLookupTurn hop;
    CnetUtterState state;
    CnetLookupReport unbound;
    int have_bound = 0, have_words = 0;
    int rc = 1;

#define REQUIRE(condition, reason)                                            \
    do {                                                                      \
        if (!(condition)) {                                                   \
            printf("CNET_CHAT_LOOKUP_RED reason=%s\n", reason);               \
            goto done;                                                        \
        }                                                                     \
    } while (0)

    REQUIRE(write_temp(bound_path, sizeof bound_path, "answer: 13\n", 11) == 0,
            "temp_bound");
    have_bound = 1;
    REQUIRE(write_temp(words_path, sizeof words_path, "hello world\n", 12) == 0,
            "temp_words");
    have_words = 1;

    cnet_utter_state_init(&state);
    snprintf(state.source, sizeof state.source, "%s", "CNET");
    REQUIRE(state.never_voice_llm == 1, "never_voice_llm");
    REQUIRE(cnet_utter_may_voice(&state, "RESIDUAL") == 0, "residual_mouth");
    REQUIRE(cnet_utter_may_voice(&state, "TEACHER") == 0, "teacher_mouth");
    REQUIRE(cnet_utter_may_voice(&state, "LLM") == 0, "llm_mouth");

    snprintf(turn, sizeof turn, "In this conversation, look up file://%s",
             bound_path);
    memset(&hop, 0, sizeof hop);
    REQUIRE(cnet_chat_lookup_turn(turn, &hop) == 0, "url_offered");
    REQUIRE(hop.offered == 1, "offered");
    REQUIRE(hop.answered == 1, "answered");
    REQUIRE(hop.bind == CNET_LOOKUP_BIND_INTEGER, "default_integer");
    REQUIRE(hop.report.bound == 1, "bound");
    REQUIRE(strcmp(hop.report.value, "13") == 0, "value");
    REQUIRE(strstr(hop.spoken, "13") != NULL, "speak_value");
    REQUIRE(strstr(hop.spoken, "local-file") != NULL, "speak_host");
    REQUIRE(strstr(hop.spoken, CNET_LOOKUP_CONTRACT) != NULL, "speak_contract");
    REQUIRE(strstr(hop.spoken, hop.report.sha256) != NULL, "speak_hash");
    REQUIRE(strstr(hop.spoken, "Marble reports") != NULL, "native_mouth");
    REQUIRE(cnet_utter_may_voice(&state, "RESIDUAL") == 0, "residual_after");
    REQUIRE(cnet_lookup_speak(&hop.report, spoken, sizeof spoken) == 0 &&
                strcmp(spoken, hop.spoken) == 0,
            "same_mouth");

    memset(&hop, 0, sizeof hop);
    REQUIRE(cnet_chat_lookup_turn("What is the capital of France?", &hop) == 1,
            "no_url");
    REQUIRE(hop.offered == 0, "no_url_offered");
    REQUIRE(hop.answered == 0, "no_url_unanswered");
    REQUIRE(hop.spoken[0] == '\0', "no_url_silent");
    REQUIRE(strcmp(hop.refusal, "empty_url") == 0, "no_url_reason");
    REQUIRE(cnet_lookup_speak(&hop.report, spoken, sizeof spoken) != 0,
            "no_url_cannot_speak");

    memset(&hop, 0, sizeof hop);
    REQUIRE(cnet_chat_lookup_turn("Please fetch javascript:alert(1)", &hop) ==
                1,
            "bad_scheme");
    REQUIRE(hop.offered == 1, "js_offered");
    REQUIRE(hop.answered == 0, "js_unanswered");
    REQUIRE(hop.spoken[0] == '\0', "js_silent");
    REQUIRE(strcmp(hop.report.refusal, "scheme") == 0, "js_reason");
    REQUIRE(cnet_lookup_speak(&hop.report, spoken, sizeof spoken) != 0,
            "js_cannot_speak");

    memset(&hop, 0, sizeof hop);
    REQUIRE(cnet_chat_lookup_turn("Read mailto:x@example.com as a line",
                                 &hop) == 1,
            "mailto");
    REQUIRE(hop.bind == CNET_LOOKUP_BIND_LINE, "mailto_line");
    REQUIRE(hop.answered == 0, "mailto_unanswered");

    snprintf(turn, sizeof turn, "Bind the integer at file://%s", words_path);
    memset(&hop, 0, sizeof hop);
    REQUIRE(cnet_chat_lookup_turn(turn, &hop) == 1, "unbindable");
    REQUIRE(hop.offered == 1, "words_offered");
    REQUIRE(hop.answered == 0, "words_unanswered");
    REQUIRE(hop.spoken[0] == '\0', "words_silent");
    REQUIRE(strcmp(hop.report.refusal, "unbindable") == 0, "words_reason");
    REQUIRE(cnet_lookup_speak(&hop.report, spoken, sizeof spoken) != 0,
            "unbound_cannot_speak");

    snprintf(turn, sizeof turn, "What token is in file://%s", words_path);
    memset(&hop, 0, sizeof hop);
    REQUIRE(cnet_chat_lookup_turn(turn, &hop) == 0, "token_bind");
    REQUIRE(hop.bind == CNET_LOOKUP_BIND_TOKEN, "token_kind");
    REQUIRE(strcmp(hop.report.value, "hello") == 0, "token_value");

    memset(&unbound, 0, sizeof unbound);
    REQUIRE(cnet_lookup_speak(&unbound, spoken, sizeof spoken) != 0,
            "empty_report_silent");
    REQUIRE(cnet_utter_may_voice(&state, "RESIDUAL") == 0, "residual_final");

    printf("CNET_CHAT_LOOKUP_PASS contract=%s url_offered=1 no_url=1 "
           "bad_scheme=1 unbound_silent=1 residual=0 broader_claims=WITHHELD\n",
           CNET_LOOKUP_CONTRACT);
    rc = 0;
done:
    if (have_bound) unlink(bound_path);
    if (have_words) unlink(words_path);
    return rc;
}
