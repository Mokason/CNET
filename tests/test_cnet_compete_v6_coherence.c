#include "cnet_compete_runtime.h"

#include <stdio.h>
#include <string.h>

/* Independently authored request English. These prompts are not copied from
   any held-out TSV. They restate the public contracts the way a coherent
   8B-class reader would: polite requests, "how many", "plus one", and
   ordered hop verbs. */

typedef struct {
    const char *prompt;
    CnetCompeteIntent intent;
    unsigned value;
    size_t guard_checks;
} CoherenceCase;

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

int main(int argc, char **argv) {
    static const CoherenceCase covered[] = {
        {"Please increment unsigned byte 12.",
         CNET_INTENT_INCREMENT, 13u, 0u},
        {"What is the next unsigned byte after 12 with wraparound?",
         CNET_INTENT_INCREMENT, 13u, 0u},
        {"Add one to octet 12 modulo 256.",
         CNET_INTENT_INCREMENT, 13u, 0u},
        {"Return the cyclic successor of unsigned byte 12.",
         CNET_INTENT_INCREMENT, 13u, 0u},
        {"Compute uint8 12 plus one.",
         CNET_INTENT_INCREMENT, 13u, 0u},
        {"How many seconds are in 12 minutes?",
         CNET_INTENT_MINUTES, 720u, 0u},
        {"Please convert 12 minutes into seconds.",
         CNET_INTENT_MINUTES, 720u, 0u},
        {"12 minutes equals how many seconds?",
         CNET_INTENT_MINUTES, 720u, 0u},
        {"Please compute CRC-8/ATM of unsigned byte 12.",
         CNET_INTENT_CRC8, 0u, 0u},
        {"What is the ATM checksum of octet 12?",
         CNET_INTENT_CRC8, 0u, 0u},
        {"Please decide access: admin is true, owner is false, mfa is false, "
         "suspended is false.",
         CNET_INTENT_POLICY, 1u, 0u},
        {"Is permission granted when admin is true, owner is false, mfa is "
         "false, and suspended is false?",
         CNET_INTENT_POLICY, 1u, 0u},
        {"Increment, then double, then add three to unsigned byte 12.",
         CNET_INTENT_COMPOSE3, 29u, 3u},
        {"Apply successor then doubling then plus three to octet 12.",
         CNET_INTENT_COMPOSE3, 29u, 3u},
        {"Increment 12 by one modulo 256.",
         CNET_INTENT_INCREMENT, 13u, 0u},
        {"Increment 12 with wraparound.",
         CNET_INTENT_INCREMENT, 13u, 0u},
        {"Add one to 12 modulo 256.",
         CNET_INTENT_INCREMENT, 13u, 0u},
        {"Increment then double then add three to 12 modulo 256.",
         CNET_INTENT_COMPOSE3, 29u, 3u},
        {"Decide access admin=true owner=false mfa=false suspended=false.",
         CNET_INTENT_POLICY, 1u, 0u}
    };
    static const char *const ood[] = {
        "Please increment signed byte 12.",
        "Email the increment of unsigned byte 12.",
        "Increment unsigned byte 12 and also convert 12 minutes.",
        "Please compute CRC-16 of unsigned byte 12.",
        "Decide access and then increment unsigned byte 12."
    };
    CnetCompeteRuntime *runtime = NULL;
    CnetCompeteRuntimeReport report;
    size_t index, covered_pass = 0, ood_pass = 0;
    unsigned crc12;
    int rc = 1;

#define REQUIRE(condition, reason)                                            \
    do {                                                                      \
        if (!(condition)) {                                                   \
            printf("CNET_7B_V6_COHERENCE_RED reason=%s\n", reason);         \
            goto done;                                                        \
        }                                                                     \
    } while (0)

    if (argc != 4) return 2;
    crc12 = reference_crc8(12u);
    REQUIRE(cnet_compete_runtime_load(argv[1], argv[2], argv[3], &runtime,
                                      &report) == 0,
            "runtime_load");
    for (index = 0; index < sizeof covered / sizeof covered[0]; ++index) {
        CnetCompeteResult result;
        CnetCompeteDiagnostic diagnostic;
        unsigned expected = covered[index].value;
        if (covered[index].intent == CNET_INTENT_CRC8) expected = crc12;
        memset(&result, 0, sizeof result);
        memset(&diagnostic, 0, sizeof diagnostic);
        if (cnet_compete_runtime_execute_diagnostic(
                runtime, covered[index].prompt, &result, &diagnostic) != 0 ||
            !result.answered || result.intent != covered[index].intent ||
            result.value != expected ||
            result.composition_guard_checks !=
                covered[index].guard_checks ||
            diagnostic.refusal != CNET_COMPETE_REFUSAL_NONE) {
            printf("CNET_7B_V6_COHERENCE_RED reason=covered_miss index=%zu "
                   "answered=%d intent=%d value=%u guards=%zu refusal=%d "
                   "proposed=%d semantic=%d\n",
                   index, result.answered, (int)result.intent, result.value,
                   result.composition_guard_checks, (int)diagnostic.refusal,
                   (int)diagnostic.proposed_intent,
                   (int)diagnostic.semantic_intent);
        } else {
            ++covered_pass;
        }
    }
    for (index = 0; index < sizeof ood / sizeof ood[0]; ++index) {
        CnetCompeteResult result;
        memset(&result, 0, sizeof result);
        REQUIRE(cnet_compete_runtime_execute(runtime, ood[index], &result) ==
                        0 &&
                    !result.answered,
                "ood_answered");
        ++ood_pass;
    }
    REQUIRE(covered_pass == sizeof covered / sizeof covered[0],
            "covered_incomplete");
    printf("CNET_7B_V6_COHERENCE_PASS covered=%zu ood=%zu "
           "unsafe=0 broader_claims=WITHHELD\n",
           covered_pass, ood_pass);
    rc = 0;
done:
    cnet_compete_runtime_free(runtime);
    return rc;
}
