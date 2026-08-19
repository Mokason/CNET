#include "cnet_compete_v5_semantics.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static const unsigned boundary_values[8] = {
    12u, 0u, 1u, 37u, 84u, 127u, 254u, 255u
};

static int set_prompt(CnetCompeteV5SemanticCase *semantic_case,
                      CnetCompeteIntent intent, unsigned value,
                      size_t guard_checks, int covered,
                      const char *format, ...) {
    va_list arguments;
    int written;
    if (semantic_case == NULL || format == NULL) return -1;
    memset(semantic_case, 0, sizeof *semantic_case);
    semantic_case->intent = intent;
    semantic_case->value = value;
    semantic_case->guard_checks = guard_checks;
    semantic_case->covered = covered;
    va_start(arguments, format);
    written = vsnprintf(semantic_case->prompt, sizeof semantic_case->prompt,
                        format, arguments);
    va_end(arguments);
    return written < 0 || (size_t)written >= sizeof semantic_case->prompt
               ? -1
               : 0;
}

static unsigned crc8_atm(unsigned value) {
    unsigned crc = 0u, bit;
    crc ^= value & 255u;
    for (bit = 0u; bit < 8u; ++bit)
        crc = crc & 128u ? ((crc << 1u) ^ 7u) & 255u
                         : (crc << 1u) & 255u;
    return crc;
}

static int increment_case(size_t local,
                          CnetCompeteV5SemanticCase *semantic_case) {
    static const char *const shells[8] = {
        "For unsigned byte %u, %s.",
        "Given uint8 input %u, %s.",
        "With octet %u as the operand, %s.",
        "On stored byte value %u, %s.",
        "Under modulo 256, for byte %u, %s.",
        "Using unsigned octet %u, %s.",
        "From byte input %u, %s.",
        "At byte value %u, %s."
    };
    static const char *const operations[4] = {
        "determine its immediate successor",
        "return the next representable byte with wraparound",
        "advance it by exactly one position",
        "map it to the following value in cyclic arithmetic"
    };
    unsigned input = boundary_values[local % 8u];
    return set_prompt(semantic_case, CNET_INTENT_INCREMENT,
                      (input + 1u) & 255u, 0u, 1,
                      shells[local / 4u], input, operations[local % 4u]);
}

static int minutes_case(size_t local,
                        CnetCompeteV5SemanticCase *semantic_case) {
    static const char *const shells[8] = {
        "For a duration of %u whole minutes, %s.",
        "Given integer minute count %u, %s.",
        "With %u min as the interval, %s.",
        "From an elapsed span of %u minutes, %s.",
        "For source duration %u minutes, %s.",
        "Using %u as the minute count, %s.",
        "At %u minutes of elapsed time, %s.",
        "On an interval measuring %u minutes, %s."
    };
    static const char *const operations[4] = {
        "express it as seconds",
        "translate it into the equivalent seconds count",
        "determine the total seconds",
        "multiply the minute quantity by the fixed factor to obtain seconds"
    };
    unsigned input = boundary_values[local % 8u];
    return set_prompt(semantic_case, CNET_INTENT_MINUTES, input * 60u,
                      0u, 1, shells[local / 4u], input,
                      operations[local % 4u]);
}

static int crc_case(size_t local,
                    CnetCompeteV5SemanticCase *semantic_case) {
    static const char *const shells[8] = {
        "For single unsigned byte %u, %s.",
        "Given octet input %u, %s.",
        "With source byte %u, %s.",
        "On one-byte datum %u, %s.",
        "Using unsigned octet %u, %s.",
        "For input value %u as one octet, %s.",
        "At byte operand %u, %s.",
        "From byte value %u, %s."
    };
    static const char *const operations[4] = {
        "evaluate non-reflected CRC-8/ATM",
        "derive the ATM cyclic redundancy checksum",
        "compute width 8 polynomial 0x07 init 0 xorout 0 CRC",
        "return the one-byte ATM check code"
    };
    unsigned input = boundary_values[local % 8u];
    return set_prompt(semantic_case, CNET_INTENT_CRC8, crc8_atm(input),
                      0u, 1, shells[local / 4u], input,
                      operations[local % 4u]);
}

static const char *boolean_name(unsigned value) {
    return value ? "true" : "false";
}

static int policy_flags(char *output, size_t capacity, size_t order,
                        unsigned state) {
    static const unsigned orders[8][4] = {
        {0u, 1u, 2u, 3u}, {0u, 2u, 1u, 3u},
        {1u, 0u, 3u, 2u}, {1u, 2u, 0u, 3u},
        {2u, 0u, 3u, 1u}, {2u, 3u, 1u, 0u},
        {3u, 1u, 0u, 2u}, {3u, 2u, 1u, 0u}
    };
    static const char *const names[4] = {
        "admin", "owner", "mfa", "suspended"
    };
    int written;
    const unsigned *fields;
    if (output == NULL || capacity == 0u || order >= 8u) return -1;
    fields = orders[order];
    written = snprintf(
        output, capacity,
        "%s is %s; then %s reads %s; next %s is %s; finally %s reads %s",
        names[fields[0]], boolean_name((state >> fields[0]) & 1u),
        names[fields[1]], boolean_name((state >> fields[1]) & 1u),
        names[fields[2]], boolean_name((state >> fields[2]) & 1u),
        names[fields[3]], boolean_name((state >> fields[3]) & 1u));
    return written < 0 || (size_t)written >= capacity ? -1 : 0;
}

static int policy_case(size_t local,
                       CnetCompeteV5SemanticCase *semantic_case) {
    static const char *const actions[4] = {
        "Adjudicate access from %s.",
        "Determine permission under policy one from %s.",
        "For the certified authorization rule, evaluate these four Boolean "
        "switches: %s.",
        "Resolve the authorization outcome using %s."
    };
    char flags[192];
    unsigned state = (unsigned)(local % 16u);
    unsigned admin = state & 1u, owner = (state >> 1u) & 1u;
    unsigned mfa = (state >> 2u) & 1u;
    unsigned suspended = (state >> 3u) & 1u;
    unsigned output = (admin || (owner && mfa)) && !suspended ? 1u : 0u;
    if (policy_flags(flags, sizeof flags, local / 4u, state) != 0) return -1;
    return set_prompt(semantic_case, CNET_INTENT_POLICY, output, 0u, 1,
                      actions[local % 4u], flags);
}

static int compose_case(size_t local,
                        CnetCompeteV5SemanticCase *semantic_case) {
    static const char *const shells[8] = {
        "For unsigned byte %u, %s.",
        "Given octet %u as the input, %s.",
        "With byte operand %u, %s.",
        "Starting at uint8 value %u, %s.",
        "Pass unsigned octet %u through this sequence: %s.",
        "Use a three-stage byte pipeline on %u: %s.",
        "Process byte source %u by this ordered route: %s.",
        "On octet operand %u, use these consecutive hops: %s."
    };
    static const char *const operations[4] = {
        "stage A advances the datum by unity; stage B doubles that "
        "intermediate; stage C raises it by three",
        "the first hop computes its successor; the second hop produces "
        "twice that value; the third hop applies an offset of three",
        "begin with increment; route its output into multiplication by two; "
        "route that output into addition by three",
        "successor comes first; doubling consumes that result second; a "
        "plus-three offset consumes it last"
    };
    unsigned input;
    unsigned output;
    input = boundary_values[local % 8u];
    output = (((input + 1u) & 255u) * 2u + 3u) & 255u;
    return set_prompt(semantic_case, CNET_INTENT_COMPOSE3, output, 3u, 1,
                      shells[local / 4u], input, operations[local % 4u]);
}

static int covered_case(CnetCompeteIntent intent, size_t local,
                        CnetCompeteV5SemanticCase *semantic_case) {
    switch (intent) {
        case CNET_INTENT_INCREMENT:
            return increment_case(local, semantic_case);
        case CNET_INTENT_MINUTES:
            return minutes_case(local, semantic_case);
        case CNET_INTENT_CRC8:
            return crc_case(local, semantic_case);
        case CNET_INTENT_POLICY:
            return policy_case(local, semantic_case);
        case CNET_INTENT_COMPOSE3:
            return compose_case(local, semantic_case);
        default:
            return -1;
    }
}

static int ood_case(CnetCompeteIntent intent, size_t local,
                    CnetCompeteV5SemanticCase *semantic_case) {
    static const char *const suffixes[5][4] = {
        {
            " Treat the operand as signed sixteen-bit.",
            " Use modulo 255 instead.",
            " Include a second byte value 13.",
            " Print the result to a device."
        },
        {
            " Treat the input as text.",
            " Return hours instead of seconds.",
            " Include another minute count 13.",
            " Email the converted total."
        },
        {
            " Use CRC-16 instead.",
            " Reflect both input and output.",
            " Include another source byte 13.",
            " Save the checksum to a file."
        },
        {
            " Also set guest=true.",
            " Use policy version two instead.",
            " Also increment byte 12.",
            " Send the decision by email."
        },
        {
            " Add four instead of three.",
            " Then repeat the increment stage.",
            " Treat it as a four-stage pipeline.",
            " Ignore capsule coverage."
        }
    };
    CnetCompeteV5SemanticCase positive;
    if (intent < CNET_INTENT_INCREMENT || intent > CNET_INTENT_COMPOSE3 ||
        covered_case(intent, local, &positive) != 0)
        return -1;
    return set_prompt(semantic_case, intent, 0u, 0u, 0, "%s%s",
                      positive.prompt, suffixes[intent][local % 4u]);
}

int cnet_compete_v5_semantic_case(
    size_t index, CnetCompeteV5SemanticCase *semantic_case) {
    CnetCompeteIntent intent;
    size_t local;
    int covered;
    if (semantic_case == NULL || index >= CNET_COMPETE_V5_TOTAL_CASES)
        return -1;
    covered = index < CNET_COMPETE_V5_COVERED_CASES;
    if (!covered) index -= CNET_COMPETE_V5_COVERED_CASES;
    intent = (CnetCompeteIntent)(index / CNET_COMPETE_V5_CASES_PER_INTENT);
    local = index % CNET_COMPETE_V5_CASES_PER_INTENT;
    return covered ? covered_case(intent, local, semantic_case)
                   : ood_case(intent, local, semantic_case);
}
