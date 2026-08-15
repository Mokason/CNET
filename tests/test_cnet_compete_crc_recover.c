#include "cnet_compete_crc_recover.h"
#include "cnet_compete_independence.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *const other_intents[] = {
    "Advance unsigned byte 19 by one with wraparound.",
    "Express 19 whole minutes as seconds.",
    "Adjudicate access with admin false owner true mfa true suspended false."
};

int main(int argc, char **argv) {
    char export_path[] = "/tmp/cnet-crc-dev-XXXXXX";
    char error[512];
    CnetCompeteIndependenceReport audit;
    size_t index, proposed = 0, refused = 0, prompt_count = 0;
    int fd;
    int rc = 1;

#define REQUIRE(condition, reason)                                            \
    do {                                                                      \
        if (!(condition)) {                                                   \
            printf("CNET_7B_CRC_RECOVER_RED reason=%s\n", reason);          \
            goto done;                                                        \
        }                                                                     \
    } while (0)

    REQUIRE(argc == 2 && argv[1] != NULL && argv[1][0] != '\0',
            "exclusion_argument");
    REQUIRE(strstr(argv[1], "heldout") == NULL &&
                strstr(argv[1], "cases.tsv") == NULL,
            "heldout_path_refused");
    {
        CnetCompeteIntent intent = CNET_INTENT_INCREMENT;
        unsigned octet = 7u;
        REQUIRE(cnet_compete_crc_recover_propose(NULL, &intent, &octet) == -1 &&
                    intent == CNET_INTENT_ABSTAIN && octet == 0u,
                "null_prompt");
        REQUIRE(cnet_compete_crc_recover_propose(
                    "Report ATM check code of operand 19.", NULL, &octet) == -1,
                "null_intent");
    }
    for (index = 0; index < CNET_COMPETE_CRC_DEV_TOTAL; ++index) {
        CnetCompeteCrcDevCase dev_case;
        CnetCompeteIntent intent = CNET_INTENT_ABSTAIN;
        unsigned octet = 0;
        int recovered;
        REQUIRE(cnet_compete_crc_dev_case(index, &dev_case) == 0,
                "dev_case");
        recovered = cnet_compete_crc_recover_propose(dev_case.prompt, &intent,
                                                     &octet);
        if (dev_case.covered) {
            REQUIRE(recovered == 0 && intent == CNET_INTENT_CRC8 &&
                        octet == dev_case.octet,
                    "covered_not_proposed");
            ++proposed;
        } else {
            REQUIRE(recovered == 1 && intent == CNET_INTENT_ABSTAIN &&
                        octet == 0u,
                    "ood_not_refused");
            ++refused;
        }
    }
    for (index = 0; index < sizeof other_intents / sizeof other_intents[0];
         ++index) {
        CnetCompeteIntent intent = CNET_INTENT_CRC8;
        unsigned octet = 99u;
        REQUIRE(cnet_compete_crc_recover_propose(other_intents[index], &intent,
                                                 &octet) == 1 &&
                    intent == CNET_INTENT_ABSTAIN && octet == 0u,
                "other_intent_recovered");
    }
    fd = mkstemp(export_path);
    REQUIRE(fd >= 0, "export_path");
    REQUIRE(close(fd) == 0, "export_close");
    REQUIRE(cnet_compete_crc_dev_export(export_path, &prompt_count) == 0 &&
                prompt_count == CNET_COMPETE_CRC_DEV_TOTAL,
            "dev_export");
    memset(&audit, 0, sizeof audit);
    error[0] = '\0';
    {
        const char *priors[] = {argv[1]};
        REQUIRE(cnet_compete_independence_audit(export_path, priors, 1u, NULL,
                                                0u, &audit, error,
                                                sizeof error) ==
                    CNET_INDEPENDENCE_OK,
                "exclusion_overlap");
        REQUIRE(audit.candidate_prompts == CNET_COMPETE_CRC_DEV_TOTAL &&
                    audit.canonical_matches == 0u &&
                    audit.near_matches == 0u &&
                    audit.duplicate_prompts == 0u,
                "exclusion_counts");
    }
    printf("CNET_7B_CRC_RECOVER_PASS proposed=%zu/%u refused=%zu/%u "
           "exclusion_overlap=0 excluded_prompts=%zu "
           "heldout_unread=1 wordlm_untrained=1 f11_scores_unchanged=1\n",
           proposed, CNET_COMPETE_CRC_DEV_COVERED, refused,
           CNET_COMPETE_CRC_DEV_OOD, audit.excluded_prompts);
    rc = 0;
done:
    (void)unlink(export_path);
#undef REQUIRE
    return rc;
}
