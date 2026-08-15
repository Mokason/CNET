#define _POSIX_C_SOURCE 200809L

#include "cnet_chat_lookup.h"

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
    CnetLookupReport unbound;
    unsigned residual_calls = 0;
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

    /* file:// fixtures use the test-only flag. Production/cnetd denies them. */
    snprintf(turn, sizeof turn, "In this conversation, look up file://%s",
             bound_path);
    memset(&hop, 0, sizeof hop);
    REQUIRE(cnet_chat_lookup_turn_flags(turn, CNET_LOOKUP_F_ALLOW_FILE, &hop) ==
                0,
            "url_offered");
    residual_calls += hop.residual_calls;
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
    REQUIRE(cnet_lookup_speak(&hop.report, spoken, sizeof spoken) == 0 &&
                strcmp(spoken, hop.spoken) == 0,
            "same_mouth");
    REQUIRE(hop.residual_calls == 0, "bound_no_residual");

    /* Production path (the cnetd hop) refuses file://. */
    memset(&hop, 0, sizeof hop);
    REQUIRE(cnet_chat_lookup_cnetd_hop(turn, &hop) == 1, "cnetd_no_file");
    residual_calls += hop.residual_calls;
    REQUIRE(hop.offered == 1, "cnetd_file_offered");
    REQUIRE(hop.answered == 0, "cnetd_file_unanswered");
    REQUIRE(hop.spoken[0] == '\0', "cnetd_file_silent");
    REQUIRE(strcmp(hop.report.refusal, "scheme") == 0, "cnetd_file_scheme");
    REQUIRE(hop.residual_calls == 0, "cnetd_file_no_residual");

    memset(&hop, 0, sizeof hop);
    REQUIRE(cnet_chat_lookup_cnetd_hop("What is the capital of France?",
                                       &hop) == 1,
            "no_url");
    residual_calls += hop.residual_calls;
    REQUIRE(hop.offered == 0, "no_url_offered");
    REQUIRE(hop.answered == 0, "no_url_unanswered");
    REQUIRE(hop.spoken[0] == '\0', "no_url_silent");
    REQUIRE(strcmp(hop.refusal, "empty_url") == 0, "no_url_reason");
    REQUIRE(cnet_lookup_speak(&hop.report, spoken, sizeof spoken) != 0,
            "no_url_cannot_speak");

    memset(&hop, 0, sizeof hop);
    REQUIRE(cnet_chat_lookup_cnetd_hop("Please fetch javascript:alert(1)",
                                       &hop) == 1,
            "bad_scheme");
    residual_calls += hop.residual_calls;
    REQUIRE(hop.offered == 1, "js_offered");
    REQUIRE(hop.answered == 0, "js_unanswered");
    REQUIRE(hop.spoken[0] == '\0', "js_silent");
    REQUIRE(strcmp(hop.report.refusal, "scheme") == 0, "js_reason");

    memset(&hop, 0, sizeof hop);
    REQUIRE(cnet_chat_lookup_cnetd_hop("Read mailto:x@example.com as a line",
                                       &hop) == 1,
            "mailto");
    residual_calls += hop.residual_calls;
    REQUIRE(hop.bind == CNET_LOOKUP_BIND_LINE, "mailto_line");
    REQUIRE(hop.answered == 0, "mailto_unanswered");
    REQUIRE(hop.spoken[0] == '\0', "mailto_silent");
    REQUIRE(strcmp(hop.report.refusal, "scheme") == 0, "mailto_reason");

    memset(&hop, 0, sizeof hop);
    REQUIRE(cnet_chat_lookup_cnetd_hop("fetch ftp://example.com/x", &hop) == 1,
            "ftp");
    residual_calls += hop.residual_calls;
    REQUIRE(hop.answered == 0, "ftp_unanswered");
    REQUIRE(hop.spoken[0] == '\0', "ftp_silent");
    REQUIRE(strcmp(hop.report.refusal, "scheme") == 0, "ftp_reason");

    memset(&hop, 0, sizeof hop);
    REQUIRE(cnet_chat_lookup_cnetd_hop("fetch data:text/plain,13", &hop) == 1,
            "data_scheme");
    residual_calls += hop.residual_calls;
    REQUIRE(hop.answered == 0, "data_unanswered");
    REQUIRE(hop.spoken[0] == '\0', "data_silent");
    REQUIRE(strcmp(hop.report.refusal, "scheme") == 0, "data_reason");

    memset(&hop, 0, sizeof hop);
    REQUIRE(cnet_chat_lookup_cnetd_hop(
                "look up https://no-such-host-cnet-lookup.invalid/x", &hop) == 1,
            "fetch_fail");
    residual_calls += hop.residual_calls;
    REQUIRE(hop.offered == 1, "fetch_offered");
    REQUIRE(hop.answered == 0, "fetch_unanswered");
    REQUIRE(hop.spoken[0] == '\0', "fetch_silent");
    REQUIRE(strcmp(hop.report.refusal, "fetch") == 0, "fetch_reason");

    snprintf(turn, sizeof turn, "Bind the integer at file://%s", words_path);
    memset(&hop, 0, sizeof hop);
    REQUIRE(cnet_chat_lookup_turn_flags(turn, CNET_LOOKUP_F_ALLOW_FILE, &hop) ==
                1,
            "unbindable");
    residual_calls += hop.residual_calls;
    REQUIRE(hop.offered == 1, "words_offered");
    REQUIRE(hop.answered == 0, "words_unanswered");
    REQUIRE(hop.spoken[0] == '\0', "words_silent");
    REQUIRE(strcmp(hop.report.refusal, "unbindable") == 0, "words_reason");

    snprintf(turn, sizeof turn, "What token is in file://%s", words_path);
    memset(&hop, 0, sizeof hop);
    REQUIRE(cnet_chat_lookup_turn_flags(turn, CNET_LOOKUP_F_ALLOW_FILE, &hop) ==
                0,
            "token_bind");
    residual_calls += hop.residual_calls;
    REQUIRE(hop.bind == CNET_LOOKUP_BIND_TOKEN, "token_kind");
    REQUIRE(hop.answered == 1, "token_answered");
    REQUIRE(strcmp(hop.report.value, "hello") == 0, "token_value");
    REQUIRE(hop.spoken[0] != '\0', "token_spoken");
    REQUIRE(strstr(hop.spoken, "hello") != NULL, "token_speak_value");
    REQUIRE(hop.residual_calls == 0, "token_no_residual");

    /* SSRF: cnetd hop (production) denies loopback / link-local / metadata. */
    memset(&hop, 0, sizeof hop);
    REQUIRE(cnet_chat_lookup_cnetd_hop("look up http://127.0.0.1/x", &hop) == 1,
            "loopback");
    residual_calls += hop.residual_calls;
    REQUIRE(hop.spoken[0] == '\0', "loopback_silent");
    REQUIRE(strcmp(hop.report.refusal, "host") == 0, "loopback_reason");

    memset(&hop, 0, sizeof hop);
    REQUIRE(cnet_chat_lookup_cnetd_hop("look up http://[::1]/", &hop) == 1,
            "loopback6");
    residual_calls += hop.residual_calls;
    REQUIRE(strcmp(hop.report.refusal, "host") == 0, "loopback6_reason");

    memset(&hop, 0, sizeof hop);
    REQUIRE(cnet_chat_lookup_cnetd_hop("look up http://169.254.169.254/latest",
                                       &hop) == 1,
            "metadata_ip");
    residual_calls += hop.residual_calls;
    REQUIRE(strcmp(hop.report.refusal, "host") == 0, "metadata_ip_reason");

    memset(&hop, 0, sizeof hop);
    REQUIRE(cnet_chat_lookup_cnetd_hop(
                "look up http://metadata.google.internal/", &hop) == 1,
            "metadata_name");
    residual_calls += hop.residual_calls;
    REQUIRE(strcmp(hop.report.refusal, "host") == 0, "metadata_name_reason");

    memset(&hop, 0, sizeof hop);
    REQUIRE(cnet_chat_lookup_cnetd_hop("look up http://[fe80::1]/", &hop) == 1,
            "link_local6");
    residual_calls += hop.residual_calls;
    REQUIRE(strcmp(hop.report.refusal, "host") == 0, "link_local6_reason");

    memset(&unbound, 0, sizeof unbound);
    REQUIRE(cnet_lookup_speak(&unbound, spoken, sizeof spoken) != 0,
            "empty_report_silent");
    REQUIRE(residual_calls == 0, "residual_counter");

    printf("CNET_CHAT_LOOKUP_PASS contract=%s cnetd_hop=1 url_offered=1 "
           "no_url=1 bad_scheme=1 ftp=1 data=1 fetch_fail=1 mailto_silent=1 "
           "token_spoken=1 unbound_silent=1 residual=%u "
           "broader_claims=WITHHELD\n",
           CNET_LOOKUP_CONTRACT, residual_calls);
    rc = 0;
done:
    if (have_bound) unlink(bound_path);
    if (have_words) unlink(words_path);
    return rc;
}
