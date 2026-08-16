#define _POSIX_C_SOURCE 200809L

#include "cnet_lookup.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    char path[] = "/tmp/cnet-lookup-XXXXXX";
    char empty[] = "/tmp/cnet-lookup-empty-XXXXXX";
    char words[] = "/tmp/cnet-lookup-words-XXXXXX";
    char lined[] = "/tmp/cnet-lookup-line-XXXXXX";
    char years[] = "/tmp/cnet-lookup-year-XXXXXX";
    char url[640];
    char spoken[512];
    int fd;
    CnetLookupReport report;
    int have_path = 0, have_empty = 0, have_words = 0, have_lined = 0,
        have_years = 0;
    int rc = 1;

#define REQUIRE(condition, reason)                                            \
    do {                                                                      \
        if (!(condition)) {                                                   \
            printf("CNET_LOOKUP_CAPSULE_RED reason=%s\n", reason);            \
            goto done;                                                        \
        }                                                                     \
    } while (0)

    fd = mkstemp(path);
    REQUIRE(fd >= 0, "temp");
    have_path = 1;
    REQUIRE(write(fd, "answer: 13\n", 11) == 11, "write");
    close(fd);
    snprintf(url, sizeof url, "file://%s", path);

    memset(&report, 0, sizeof report);
    REQUIRE(cnet_lookup_execute_flags(url, CNET_LOOKUP_BIND_INTEGER,
                                      CNET_LOOKUP_F_ALLOW_FILE, &report) == 0,
            "bind_integer");
    REQUIRE(report.bound == 1, "bound");
    REQUIRE(strcmp(report.value, "13") == 0, "value");
    REQUIRE(strlen(report.sha256) == 64, "sha256");
    REQUIRE(report.bytes == 11, "bytes");
    REQUIRE(cnet_lookup_speak(&report, spoken, sizeof spoken) == 0, "speak");
    REQUIRE(strstr(spoken, "13") != NULL, "speak_value");
    REQUIRE(strstr(spoken, report.sha256) != NULL, "speak_hash");
    REQUIRE(strstr(spoken, CNET_LOOKUP_CONTRACT) != NULL, "speak_contract");

    memset(&report, 0, sizeof report);
    REQUIRE(cnet_lookup_execute(NULL, CNET_LOOKUP_BIND_INTEGER, &report) == 1,
            "empty_url");
    REQUIRE(strcmp(report.refusal, "empty_url") == 0, "empty_url_reason");

    memset(&report, 0, sizeof report);
    REQUIRE(cnet_lookup_execute("javascript:alert(1)", CNET_LOOKUP_BIND_INTEGER,
                                &report) == 1,
            "js_scheme");
    REQUIRE(report.bound == 0, "js_unbound");
    REQUIRE(strcmp(report.refusal, "scheme") == 0, "js_reason");
    REQUIRE(cnet_lookup_speak(&report, spoken, sizeof spoken) != 0,
            "js_no_speak");

    memset(&report, 0, sizeof report);
    REQUIRE(cnet_lookup_execute("mailto:x@example.com", CNET_LOOKUP_BIND_LINE,
                                &report) == 1,
            "mailto");
    REQUIRE(cnet_lookup_execute("ftp://example.com/x", CNET_LOOKUP_BIND_LINE,
                                &report) == 1,
            "ftp");
    REQUIRE(cnet_lookup_execute("data:text/plain,13", CNET_LOOKUP_BIND_INTEGER,
                                &report) == 1,
            "data_scheme");

    fd = mkstemp(empty);
    REQUIRE(fd >= 0, "empty_temp");
    have_empty = 1;
    close(fd);
    snprintf(url, sizeof url, "file://%s", empty);
    memset(&report, 0, sizeof report);
    REQUIRE(cnet_lookup_execute_flags(url, CNET_LOOKUP_BIND_INTEGER,
                                      CNET_LOOKUP_F_ALLOW_FILE, &report) == 1,
            "empty_abstain");
    REQUIRE(report.bound == 0, "empty_unbound");

    fd = mkstemp(words);
    REQUIRE(fd >= 0 && write(fd, "hello world\n", 12) == 12, "words");
    have_words = 1;
    close(fd);
    snprintf(url, sizeof url, "file://%s", words);
    memset(&report, 0, sizeof report);
    REQUIRE(cnet_lookup_execute_flags(url, CNET_LOOKUP_BIND_INTEGER,
                                      CNET_LOOKUP_F_ALLOW_FILE, &report) == 1,
            "no_integer");
    memset(&report, 0, sizeof report);
    REQUIRE(cnet_lookup_execute_flags(url, CNET_LOOKUP_BIND_TOKEN,
                                      CNET_LOOKUP_F_ALLOW_FILE, &report) == 0 &&
                strcmp(report.value, "hello") == 0,
            "token");

    fd = mkstemp(lined);
    REQUIRE(fd >= 0 && write(fd, "  first line\nsecond\n", 20) == 20, "line");
    have_lined = 1;
    close(fd);
    snprintf(url, sizeof url, "file://%s", lined);
    memset(&report, 0, sizeof report);
    REQUIRE(cnet_lookup_execute_flags(url, CNET_LOOKUP_BIND_LINE,
                                      CNET_LOOKUP_F_ALLOW_FILE, &report) == 0 &&
                strcmp(report.value, "first line") == 0,
            "line");

    fd = mkstemp(years);
    REQUIRE(fd >= 0 &&
                write(fd, "charset=utf-8 founded in 2002 by spacex\n", 40) == 40,
            "year_write");
    have_years = 1;
    close(fd);
    snprintf(url, sizeof url, "file://%s", years);
    memset(&report, 0, sizeof report);
    REQUIRE(cnet_lookup_execute_flags(url, CNET_LOOKUP_BIND_INTEGER,
                                      CNET_LOOKUP_F_ALLOW_FILE, &report) == 0 &&
                strcmp(report.value, "8") == 0,
            "year_first_int");
    memset(&report, 0, sizeof report);
    REQUIRE(cnet_lookup_execute_flags(url, CNET_LOOKUP_BIND_YEAR,
                                      CNET_LOOKUP_F_ALLOW_FILE, &report) == 0 &&
                strcmp(report.value, "2002") == 0,
            "year");

    /* Decoy template year first; cue in the body still wins. Not a fact table. */
    {
        char decoy[] = "/tmp/cnet-lookup-decoy-XXXXXX";
        int dfd = mkstemp(decoy);
        const char *body =
            "Use mdy dates from 2026. founded = {{Start date and age|1976|04|01}}\n";
        REQUIRE(dfd >= 0 &&
                    write(dfd, body, (size_t)strlen(body)) == (ssize_t)strlen(body),
                "decoy_write");
        close(dfd);
        snprintf(url, sizeof url, "file://%s", decoy);
        memset(&report, 0, sizeof report);
        REQUIRE(cnet_lookup_execute_flags(url, CNET_LOOKUP_BIND_YEAR,
                                          CNET_LOOKUP_F_ALLOW_FILE, &report) ==
                    0 &&
                    strcmp(report.value, "1976") == 0,
                "year_cue_not_first");
        unlink(decoy);
    }


    /* Production path: file:// closed. Loopback / link-local / metadata denied.
       Redirects cannot follow file:// (REDIR_PROTOCOLS=http,https). */
    snprintf(url, sizeof url, "file://%s", path);
    memset(&report, 0, sizeof report);
    REQUIRE(cnet_lookup_execute(url, CNET_LOOKUP_BIND_INTEGER, &report) == 1,
            "prod_no_file");
    REQUIRE(strcmp(report.refusal, "scheme") == 0, "prod_file_scheme");
    REQUIRE(cnet_lookup_url_allowed(url, 0) == 0, "prod_file_url");
    REQUIRE(cnet_lookup_url_allowed(url, CNET_LOOKUP_F_ALLOW_FILE) == 1,
            "test_file_url");

    REQUIRE(cnet_lookup_host_allowed("127.0.0.1") == 0, "deny_127");
    REQUIRE(cnet_lookup_host_allowed("127.0.0.5") == 0, "deny_127_5");
    REQUIRE(cnet_lookup_host_allowed("localhost") == 0, "deny_localhost");
    REQUIRE(cnet_lookup_host_allowed("::1") == 0, "deny_v6_loop");
    REQUIRE(cnet_lookup_host_allowed("169.254.1.1") == 0, "deny_link_local");
    REQUIRE(cnet_lookup_host_allowed("169.254.169.254") == 0, "deny_metadata_ip");
    REQUIRE(cnet_lookup_host_allowed("metadata.google.internal") == 0,
            "deny_metadata_name");
    REQUIRE(cnet_lookup_host_allowed("METADATA.GOOGLE.INTERNAL") == 0,
            "deny_metadata_case");
    REQUIRE(cnet_lookup_host_allowed("fe80::1") == 0, "deny_fe80");
    REQUIRE(cnet_lookup_host_allowed("example.com") == 1, "allow_example");
    REQUIRE(cnet_lookup_url_allowed("http://127.0.0.1/x", 0) == 0,
            "url_loopback");
    REQUIRE(cnet_lookup_url_allowed("http://[::1]/", 0) == 0, "url_loopback6");
    REQUIRE(cnet_lookup_url_allowed("http://169.254.169.254/", 0) == 0,
            "url_metadata");
    REQUIRE(cnet_lookup_url_allowed("file:///etc/passwd", 0) == 0,
            "redir_no_file");
    REQUIRE(cnet_lookup_url_allowed("ftp://example.com/x", 0) == 0, "url_ftp");
    REQUIRE(cnet_lookup_url_allowed("data:text/plain,13", 0) == 0, "url_data");
    REQUIRE(cnet_lookup_url_allowed("https://example.com/a", 0) == 1,
            "url_https");

    memset(&report, 0, sizeof report);
    REQUIRE(cnet_lookup_execute("http://127.0.0.1/x", CNET_LOOKUP_BIND_INTEGER,
                                &report) == 1,
            "exec_loopback");
    REQUIRE(strcmp(report.refusal, "host") == 0, "exec_loopback_reason");
    memset(&report, 0, sizeof report);
    REQUIRE(cnet_lookup_execute("http://metadata.google.internal/",
                                CNET_LOOKUP_BIND_INTEGER, &report) == 1,
            "exec_metadata");
    REQUIRE(strcmp(report.refusal, "host") == 0, "exec_metadata_reason");

    printf("CNET_LOOKUP_CAPSULE_PASS contract=%s bound=1 abstain=1 "
           "residual=0 broader_claims=WITHHELD\n",
           CNET_LOOKUP_CONTRACT);
    rc = 0;
done:
    if (have_path) unlink(path);
    if (have_empty) unlink(empty);
    if (have_words) unlink(words);
    if (have_lined) unlink(lined);
    if (have_years) unlink(years);
    return rc;
}
