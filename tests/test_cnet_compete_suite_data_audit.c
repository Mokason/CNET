#include "cnet_compete_suite_data_audit.h"

#include <stdio.h>
#include <string.h>

static const char valid_header[] =
    "#ifndef CNET_COMPETE_SUITE_DATA_V4_H\n"
    "#define CNET_COMPETE_SUITE_DATA_V4_H\n"
    "\n"
    "#define CNET_COMPETE_SUITE_ID \"CNET-ASI-5-v4\"\n"
    "#define CNET_COMPETE_CANDIDATE_FREEZE_COMMIT \"0123456789abcdef0123456789abcdef01234567\"\n"
    "#define CNET_COMPETE_FIXTURE_PATH \"benchmarks/cnet_asi5_v4/heldout.tsv\"\n"
    "#define CNET_COMPETE_SYSTEM_PATH \"benchmarks/cnet_asi5_v4/baseline_system.txt\"\n"
    "#define CNET_COMPETE_GENERATOR_PATH \"tools/cnet_compete_fixture_v4.c\"\n"
    "#define CNET_COMPETE_FIXTURE_PROVENANCE \"verified_spec_v4\"\n"
    "#define CNET_COMPETE_FIXTURE_SHA256 \"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\"\n"
    "#define CNET_COMPETE_SYSTEM_SHA256 \"123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0\"\n"
    "#define CNET_COMPETE_GENERATOR_SHA256 \"23456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef01\"\n"
    "#define CNET_COMPETE_TOTAL_ROWS 448u\n"
    "#define CNET_COMPETE_COVERED_ROWS 320u\n"
    "#define CNET_COMPETE_OOD_ROWS 128u\n"
    "#define CNET_COMPETE_STATE_ROOT \"/home/marble/.local/state/cnet/cnet_asi5_v4\"\n"
    "\n"
    "#endif\n";

int main(void) {
    char commit[CNET_COMPETE_FREEZE_COMMIT_HEX + 1u];
    char changed[sizeof valid_header + 32u];
    size_t invalid_refused = 0;

#define REQUIRE(condition, reason)                                             \
    do {                                                                       \
        if (!(condition)) {                                                    \
            printf("CNET_7B_V4_SUITE_DATA_AUDIT_RED reason=%s\n", reason);  \
            return 1;                                                          \
        }                                                                      \
    } while (0)

    REQUIRE(cnet_compete_suite_data_v4_validate_buffer(
                valid_header, sizeof valid_header - 1u, commit) == 0 &&
                strcmp(commit,
                       "0123456789abcdef0123456789abcdef01234567") == 0,
            "valid_header_refused");

    memcpy(changed, valid_header, sizeof valid_header);
    strstr(changed, "CNET-ASI-5-v4")[12] = '3';
    invalid_refused += cnet_compete_suite_data_v4_validate_buffer(
                           changed, sizeof valid_header - 1u, commit) != 0;

    memcpy(changed, valid_header, sizeof valid_header);
    strstr(changed, "0123456789abcdef")[1] = 'A';
    invalid_refused += cnet_compete_suite_data_v4_validate_buffer(
                           changed, sizeof valid_header - 1u, commit) != 0;

    memcpy(changed, valid_header, sizeof valid_header);
    strstr(changed, "CNET_COMPETE_TOTAL_ROWS 448u")[25] = '9';
    invalid_refused += cnet_compete_suite_data_v4_validate_buffer(
                           changed, sizeof valid_header - 1u, commit) != 0;

    memcpy(changed, valid_header, sizeof valid_header);
    changed[10] = '\0';
    invalid_refused += cnet_compete_suite_data_v4_validate_buffer(
                           changed, sizeof valid_header - 1u, commit) != 0;

    invalid_refused += cnet_compete_suite_data_v4_validate_buffer(
                           valid_header, sizeof valid_header - 2u, commit) != 0;

    memcpy(changed, valid_header, sizeof valid_header - 1u);
    memcpy(changed + sizeof valid_header - 1u, "#include \"evil.h\"\n", 18u);
    invalid_refused += cnet_compete_suite_data_v4_validate_buffer(
                           changed, sizeof valid_header - 1u + 18u, commit) != 0;

    REQUIRE(invalid_refused == 6u, "invalid_header_accepted");
    printf("CNET_7B_V4_SUITE_DATA_AUDIT_PASS invalid_refused=%zu\n",
           invalid_refused);
    return 0;
}
