#include "cnet_compete_independence.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int write_text(char path[64], const char *text) {
    FILE *file;
    int descriptor;
    strcpy(path, "/tmp/cnet-independence-XXXXXX");
    descriptor = mkstemp(path);
    if (descriptor < 0) return -1;
    file = fdopen(descriptor, "wb");
    if (file == NULL) {
        close(descriptor);
        unlink(path);
        return -1;
    }
    if (fputs(text, file) == EOF || fclose(file) != 0) {
        unlink(path);
        return -1;
    }
    return 0;
}

static int audit(const char *candidate, const char *prior, const char *source,
                 CnetCompeteIndependenceReport *report, char error[256]) {
    const char *priors[] = {prior};
    const char *sources[] = {source};
    return cnet_compete_independence_audit(candidate, priors, 1, sources, 1,
                                           report, error, 256);
}

int main(void) {
    static const char header[] =
        "#suite=test\n"
        "id\tsplit\tintent\tvalue_kind\texpected_value\tprompt\tprovenance\n";
    char candidate[64] = "", prior[64] = "", source[64] = "";
    char exclusions[64] = "";
    char error[256], text[2048];
    CnetCompeteIndependenceReport report;
    int rc = 1;

#define REQUIRE(condition, reason)                                           \
    do {                                                                      \
        if (!(condition)) {                                                   \
            printf("CNET_7B_INDEPENDENCE_RED reason=%s detail=%s\n",        \
                   reason, error);                                            \
            goto done;                                                        \
        }                                                                     \
    } while (0)

    REQUIRE(write_text(prior,
                       "#suite=old\n"
                       "id\tsplit\tintent\tvalue_kind\texpected_value\tprompt\tprovenance\n"
                       "old\tcovered\tincrement_mod256\tinteger\t13\tAdvance byte 12 by one with overflow wrap.\tdev\n") == 0,
            "prior_setup");
    REQUIRE(write_text(source,
                       "static const char *prompt = \"please compute the ATM checksum for one byte input 17\";\n"
                       "static const char *progress = \"audit is 100% complete\";\n") == 0,
            "source_setup");
    {
        const char *fixtures[] = {prior};
        const char *sources[] = {source};
        size_t prompt_count = 0;
        REQUIRE(write_text(exclusions, "placeholder\n") == 0,
                "exclusion_path_setup");
        error[0] = '\0';
        REQUIRE(cnet_compete_independence_export_exclusions(
                    exclusions, fixtures, 1, sources, 1, &prompt_count,
                    error, sizeof error) == CNET_INDEPENDENCE_OK &&
                    prompt_count == 3,
                "exclusion_export");
    }

    snprintf(text, sizeof text,
             "%s"
             "a\tcovered\tincrement_mod256\tinteger\t95\tAdvance byte 94 by one, with overflow wrap!\tnew\n",
             header);
    REQUIRE(write_text(candidate, text) == 0, "canonical_setup");
    memset(&report, 0, sizeof report);
    error[0] = '\0';
    REQUIRE(audit(candidate, prior, source, &report, error) ==
                CNET_INDEPENDENCE_ERR_OVERLAP &&
                report.canonical_matches == 1,
            "canonical_overlap_admitted");
    unlink(candidate);
    candidate[0] = '\0';

    snprintf(text, sizeof text,
             "%s"
             "a\tcovered\tcrc8_atm\tinteger\t1\tNow compute the ATM checksum for one byte input 91 please.\tnew\n",
             header);
    REQUIRE(write_text(candidate, text) == 0, "near_setup");
    memset(&report, 0, sizeof report);
    error[0] = '\0';
    REQUIRE(audit(candidate, prior, source, &report, error) ==
                CNET_INDEPENDENCE_ERR_OVERLAP && report.near_matches == 1,
            "near_overlap_admitted");
    unlink(candidate);
    candidate[0] = '\0';

    snprintf(text, sizeof text,
             "%s"
             "a\tcovered\tincrement_mod256\tinteger\t74\tInput record carries octet 73; emit its cyclic successor under unsigned-byte wrap.\tnew\n"
             "b\tcovered\tincrement_mod256\tinteger\t74\tInput record carries octet 73; emit its cyclic successor under unsigned-byte wrap.\tnew\n",
             header);
    REQUIRE(write_text(candidate, text) == 0, "duplicate_setup");
    memset(&report, 0, sizeof report);
    error[0] = '\0';
    REQUIRE(audit(candidate, prior, source, &report, error) ==
                CNET_INDEPENDENCE_ERR_DUPLICATE &&
                report.duplicate_prompts == 1,
            "duplicate_admitted");
    unlink(candidate);
    candidate[0] = '\0';

    snprintf(text, sizeof text,
             "%s"
             "a\tcovered\tincrement_mod256\tinteger\t74\tInput record carries octet 73; emit its cyclic successor under unsigned-byte wrap.\tnew\n",
             header);
    REQUIRE(write_text(candidate, text) == 0, "clean_setup");
    memset(&report, 0, sizeof report);
    error[0] = '\0';
    REQUIRE(audit(candidate, prior, source, &report, error) ==
                CNET_INDEPENDENCE_OK && report.candidate_prompts == 1 &&
                report.excluded_prompts >= 2,
            "clean_fixture_refused");
    REQUIRE(cnet_compete_independence_audit(NULL, NULL, 0, NULL, 0, NULL,
                                            NULL, 0) ==
                CNET_INDEPENDENCE_ERR_ARGUMENT,
            "null_arguments_admitted");
    printf("CNET_7B_INDEPENDENCE_PASS candidates=%zu exclusions=%zu\n",
           report.candidate_prompts, report.excluded_prompts);
    rc = 0;
done:
    if (candidate[0] != '\0') unlink(candidate);
    if (prior[0] != '\0') unlink(prior);
    if (source[0] != '\0') unlink(source);
    if (exclusions[0] != '\0') unlink(exclusions);
#undef REQUIRE
    return rc;
}
