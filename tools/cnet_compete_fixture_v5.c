#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define V5_SUITE "CNET-ASI-5-v5"
#define V5_SEED UINT64_C(0xd25b3cb4906ea3dc)

typedef struct {
    FILE *fixture;
    FILE *cases;
    size_t rows;
} Output;

static uint64_t splitmix64(uint64_t *state) {
    uint64_t value = (*state += UINT64_C(0x9e3779b97f4a7c15));
    value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31);
}

static void byte_permutation(unsigned values[256], uint64_t salt) {
    uint64_t state = V5_SEED ^ salt;
    size_t index;
    for (index = 0; index < 256; ++index) values[index] = (unsigned)index;
    for (index = 255; index > 0; --index) {
        size_t swap = (size_t)(splitmix64(&state) % (index + 1u));
        unsigned temporary = values[index];
        values[index] = values[swap];
        values[swap] = temporary;
    }
}

static unsigned generator_crc8(unsigned input) {
    unsigned accumulator = input & 255u;
    unsigned remaining = 8;
    while (remaining-- != 0)
        accumulator = (accumulator & 128u) != 0
            ? ((accumulator * 2u) ^ 7u) & 255u
            : (accumulator * 2u) & 255u;
    return accumulator;
}

static int covered(Output *output, const char *lane, size_t row,
                   const char *value_kind, const char *expected,
                   const char *prompt, const char *input_a,
                   const char *state) {
    if (fprintf(output->fixture,
                "%s-%03zu\tcovered\t%s\t%s\t%s\t%s\tverified_spec_v5\n",
                lane, row, lane, value_kind, expected, prompt) < 0 ||
        fprintf(output->cases,
                "%s-%03zu\tcovered\t%s\tnone\texact_contract\t%s\t-\t-\t%s\t%s\t%s\n",
                lane, row, lane, input_a, state, value_kind, expected) < 0)
        return -1;
    ++output->rows;
    return 0;
}

static int ood(Output *output, const char *group, size_t row,
               const char *mutation, const char *semantic_case,
               const char *input_a,
               const char *input_b, const char *input_c, const char *state,
               const char *prompt) {
    if (fprintf(output->fixture,
                "OOD-%s-%03zu\tood\tnone\tnone\t\t%s\tverified_spec_v5\n",
                group, row, prompt) < 0 ||
        fprintf(output->cases,
                "OOD-%s-%03zu\tood\tnone\t%s\t%s\t%s\t%s\t%s\t%s\tnone\t-\n",
                group, row, mutation, semantic_case, input_a, input_b, input_c,
                state) < 0)
        return -1;
    ++output->rows;
    return 0;
}

static int numeric_lanes(Output *output) {
    static const char *const increment_forms[4] = {
        "Find the successor of unsigned operand %u.",
        "What increment value follows input %u?",
        "Map operand %u through increment with wrap.",
        "Give the increment output of unsigned value %u."
    };
    static const char *const minute_forms[4] = {
        "Find how many seconds %u minutes contain.",
        "How many seconds does this duration give for %u minutes?",
        "Map the %u minute quantity onto seconds.",
        "Translate the %u minute duration into a seconds count."
    };
    static const char *const crc_forms[4] = {
        "Evaluate the ATM checksum of operand %u.",
        "Compute non-reflected ATM checksum of datum %u.",
        "Return the ATM checksum of input %u.",
        "Evaluate ATM checksum on single byte %u."
    };
    static const char *const compose_forms[4] = {
        "Map operand %u by increment, then double, then plus three.",
        "Starting at byte %u, increment, then double, then plus three.",
        "For unsigned octet %u, increment, then double, then plus three.",
        "Take operand %u through increment, double, plus three."
    };
    unsigned permutations[4][256];
    char prompt[320], expected[32], input[32];
    size_t row;
    byte_permutation(permutations[0], UINT64_C(0xa1));
    byte_permutation(permutations[1], UINT64_C(0xb2));
    byte_permutation(permutations[2], UINT64_C(0xc3));
    byte_permutation(permutations[3], UINT64_C(0xd4));
    for (row = 0; row < 64; ++row) {
        unsigned value = permutations[0][row];
        snprintf(prompt, sizeof prompt, increment_forms[row / 16u], value);
        snprintf(expected, sizeof expected, "%u", (value + 1u) & 255u);
        snprintf(input, sizeof input, "%u", value);
        if (covered(output, "increment_mod256", row, "integer", expected,
                    prompt, input, "-") != 0)
            return -1;

        value = permutations[1][row];
        snprintf(prompt, sizeof prompt, minute_forms[row / 16u], value);
        snprintf(expected, sizeof expected, "%u", value * 60u);
        snprintf(input, sizeof input, "%u", value);
        if (covered(output, "minutes_to_seconds", row, "integer", expected,
                    prompt, input, "-") != 0)
            return -1;

        value = permutations[2][row];
        snprintf(prompt, sizeof prompt, crc_forms[row / 16u], value);
        snprintf(expected, sizeof expected, "%u", generator_crc8(value));
        snprintf(input, sizeof input, "%u", value);
        if (covered(output, "crc8_atm", row, "integer", expected, prompt,
                    input, "-") != 0)
            return -1;

        value = permutations[3][row];
        snprintf(prompt, sizeof prompt, compose_forms[row / 16u], value);
        snprintf(expected, sizeof expected, "%u",
                 ((((value + 1u) & 255u) * 2u) + 3u) & 255u);
        snprintf(input, sizeof input, "%u", value);
        if (covered(output, "compose3_mod256", row, "integer", expected,
                    prompt, input, "-") != 0)
            return -1;
    }
    return 0;
}

static const char *boolean_word(unsigned value) {
    return value != 0 ? "true" : "false";
}

static int policy_lane(Output *output) {
    static const char *const forms[4] = {
        "Return policy one. Suspended reads %s. Next mfa reads %s. Next admin reads %s. Last owner reads %s.",
        "Determine policy one permission. Owner reads %s. Next suspended reads %s. Next admin reads %s. Last mfa reads %s.",
        "Evaluate policy one. Mfa reads %s. Next admin reads %s. Next suspended reads %s. Last owner reads %s.",
        "Resolve policy one. Suspended reads %s. Next owner reads %s. Next mfa reads %s. Last admin reads %s."
    };
    size_t form, state, row = 0;
    char prompt[384], state_text[32];
    for (form = 0; form < 4; ++form) {
        for (state = 0; state < 16; ++state, ++row) {
            unsigned admin = (unsigned)state & 1u;
            unsigned owner = ((unsigned)state >> 1) & 1u;
            unsigned mfa = ((unsigned)state >> 2) & 1u;
            unsigned suspended = ((unsigned)state >> 3) & 1u;
            const char *decision =
                (admin || (owner && mfa)) && !suspended ? "allow" : "deny";
            if (form == 0)
                snprintf(prompt, sizeof prompt, forms[form],
                         boolean_word(suspended), boolean_word(mfa),
                         boolean_word(admin), boolean_word(owner));
            else if (form == 1)
                snprintf(prompt, sizeof prompt, forms[form],
                         boolean_word(owner), boolean_word(suspended),
                         boolean_word(admin), boolean_word(mfa));
            else if (form == 2)
                snprintf(prompt, sizeof prompt, forms[form],
                         boolean_word(mfa), boolean_word(admin),
                         boolean_word(suspended), boolean_word(owner));
            else
                snprintf(prompt, sizeof prompt, forms[form],
                         boolean_word(suspended), boolean_word(owner),
                         boolean_word(mfa), boolean_word(admin));
            snprintf(state_text, sizeof state_text, "%zu", state);
            if (covered(output, "access_policy_v1", row, "string", decision,
                        prompt, "-", state_text) != 0)
                return -1;
        }
    }
    return 0;
}

static int range_ood(Output *output) {
    static const char *const prompts[16] = {
        "The successor contract is given signed operand -23.",
        "The minute converter is asked to consume 301 minutes.",
        "The ATM checksum rule is handed fractional byte 18.5.",
        "The three-hop chain receives the word granite instead of a byte.",
        "The successor contract is assigned out-of-range operand 400.",
        "The minute converter is given 3.5 minutes.",
        "The ATM checksum rule is asked to checksum the word copper.",
        "The three-hop chain starts from signed operand -41.",
        "The successor contract receives fractional operand 8.25.",
        "The minute converter is given the colour word indigo.",
        "The ATM checksum rule is asked to consume signed operand -6.",
        "The three-hop chain is started at operand 512.",
        "The successor contract is given the mineral word quartz.",
        "The minute converter is asked to convert -9 minutes.",
        "The ATM checksum rule is assigned operand 288.",
        "The three-hop chain is started at fractional operand 2.75."
    };
    static const char *const semantic_cases[16] = {
        "increment_negative", "minutes_above_max", "crc_fractional",
        "compose_text", "increment_above_max", "minutes_fractional",
        "crc_text", "compose_negative", "increment_fractional",
        "minutes_text", "crc_negative", "compose_above_max",
        "increment_text", "minutes_negative", "crc_above_max",
        "compose_fractional"
    };
    static const char *const inputs[16] = {
        "-23", "301", "18.5", "granite", "400", "3.5", "copper", "-41",
        "8.25", "indigo", "-6", "512", "quartz", "-9", "288", "2.75"
    };
    size_t row;
    for (row = 0; row < 16; ++row) {
        if (ood(output, "range_type", row, "range_or_type",
                semantic_cases[row], inputs[row], "-", "-", "-",
                prompts[row]) != 0)
            return -1;
    }
    return 0;
}

static int input_ood(Output *output) {
    static const char *const forms[16] = {
        "The successor contract is named, but no byte operand is supplied.",
        "One successor request names operands 21 and 88 together.",
        "The minute converter is named with no minute quantity.",
        "One minute conversion names quantities 19 and 76 together.",
        "The ATM checksum rule is requested without a message byte.",
        "One ATM checksum request names operands 33 and 104 together.",
        "The three-hop chain is requested without a starting octet.",
        "One three-hop request names starting operands 45 and 119 together.",
        "Policy one reads owner=true. Then mfa=true. Finally suspended=false. Admin has no Boolean.",
        "Policy one reads admin=false. Then mfa=true. Finally suspended=false. Owner has no Boolean.",
        "Policy one reads admin=true. Then owner=false. Finally suspended=false. Mfa has no Boolean.",
        "Policy one reads admin=true. Then owner=false. Finally mfa=true. Suspended has no Boolean.",
        "Policy one first writes admin=true, afterwards writes admin=false. Then owner=true. Then mfa=false. Finally suspended=false.",
        "Policy one first writes owner=true, afterwards writes owner=false. Then admin=false. Then mfa=true. Finally suspended=false.",
        "Policy one first writes mfa=true, afterwards writes mfa=false. Then admin=false. Then owner=true. Finally suspended=false.",
        "Policy one first writes suspended=true, afterwards writes suspended=false. Then admin=true. Then owner=false. Finally mfa=true."
    };
    static const char *const semantic_cases[16] = {
        "increment_missing", "increment_multiple", "minutes_missing",
        "minutes_multiple", "crc_missing", "crc_multiple",
        "compose_missing", "compose_multiple", "policy_missing_admin",
        "policy_missing_owner", "policy_missing_mfa",
        "policy_missing_suspended", "policy_duplicate_admin",
        "policy_duplicate_owner", "policy_duplicate_mfa",
        "policy_duplicate_suspended"
    };
    static const char *const input_a[16] = {
        "-", "21", "-", "19", "-", "33", "-", "45",
        "-", "-", "-", "-", "-", "-", "-", "-"
    };
    static const char *const input_b[16] = {
        "-", "88", "-", "76", "-", "104", "-", "119",
        "-", "-", "-", "-", "-", "-", "-", "-"
    };
    static const char *const states[16] = {
        "-", "-", "-", "-", "-", "-", "-", "-",
        "admin=missing,owner=true,mfa=true,suspended=false",
        "admin=false,owner=missing,mfa=true,suspended=false",
        "admin=true,owner=false,mfa=missing,suspended=false",
        "admin=true,owner=false,mfa=true,suspended=missing",
        "admin=true|false,owner=true,mfa=false,suspended=false",
        "admin=false,owner=true|false,mfa=true,suspended=false",
        "admin=false,owner=true,mfa=true|false,suspended=false",
        "admin=true,owner=false,mfa=true,suspended=true|false"
    };
    size_t row;
    for (row = 0; row < 16; ++row) {
        if (ood(output, "input", row, "missing_or_multiple",
                semantic_cases[row], input_a[row], input_b[row], "-",
                states[row], forms[row]) != 0)
            return -1;
    }
    return 0;
}

static int variant_ood(Output *output) {
    static const char *const forms[16] = {
        "Ask for CRC width 16 on operand %u rather than the ATM rule.",
        "Use poly 0x31 when checksumming operand %u.",
        "Seed the ATM register at one before consuming operand %u.",
        "Consume octet %u MSB-first, then reflect only the finished register.",
        "Invoke policy version two. The admin flag equals true while owner equals false, mfa equals true, and suspended equals false.",
        "Attach guest=true onto policy one. The admin flag equals true while owner equals false, mfa equals true, and suspended equals false.",
        "Ask for a clamped increment of operand %u.",
        "Start from %u seconds and recover the minute count.",
        "Use SAE-J1850 parameters on operand %u instead of ATM.",
        "After the ATM pass on operand %u, xor the register with 255.",
        "Begin on the low bit inside octet %u, walking via reversed poly 0xE0.",
        "Use Dallas-Maxim parameters on operand %u.",
        "Attach auditor=true onto policy one. The admin flag equals false while owner equals true, mfa equals true, and suspended equals false.",
        "Attach guest=true onto policy one. The admin flag equals false while owner equals false, mfa equals false, and suspended equals false.",
        "Ask for a wrapped decrement of operand %u.",
        "Turn %u minutes into an hour count."
    };
    static const char *const semantic_cases[16] = {
        "crc_width16", "crc_polynomial31", "crc_initial_register1",
        "crc_output_reflected_only", "policy_version2", "policy_guest_field",
        "increment_saturating", "duration_reverse", "crc_sae_j1850",
        "crc_xorout255", "crc_lsb_first", "crc_dallas_maxim",
        "policy_auditor_field", "policy_guest_grant",
        "increment_decrement", "duration_to_hours"
    };
    size_t row;
    char prompt[384], input[32];
    for (row = 0; row < 16; ++row) {
        unsigned value = (29u + (unsigned)row * 19u) & 255u;
        const char *state = "-";
        if (row == 4u) {
            snprintf(prompt, sizeof prompt, "%s", forms[row]);
            snprintf(input, sizeof input, "-");
            state = "version=2,admin=true,owner=false,mfa=true,suspended=false";
        } else if (row == 5u) {
            snprintf(prompt, sizeof prompt, "%s", forms[row]);
            snprintf(input, sizeof input, "-");
            state = "guest=true,admin=true,owner=false,mfa=true,suspended=false";
        } else if (row == 12u) {
            snprintf(prompt, sizeof prompt, "%s", forms[row]);
            snprintf(input, sizeof input, "-");
            state = "role=auditor,admin=false,owner=true,mfa=true,suspended=false";
        } else if (row == 13u) {
            snprintf(prompt, sizeof prompt, "%s", forms[row]);
            snprintf(input, sizeof input, "-");
            state = "guest=true,admin=false,owner=false,mfa=false,suspended=false";
        } else {
            snprintf(prompt, sizeof prompt, forms[row], value);
            snprintf(input, sizeof input, "%u", value);
        }
        if (ood(output, "variant", row, "algorithm_variant",
                semantic_cases[row], input, "-", "-", state, prompt) != 0)
            return -1;
    }
    return 0;
}

static int composition_ood(Output *output) {
    static const char *const forms[16] = {
        "Route operand %u as plus three / double / increment.",
        "Route operand %u as increment / plus three / double.",
        "Route operand %u as double / increment / plus three.",
        "Push operand %u through increment, double, plus three, then one extra double.",
        "Route operand %u as double / plus three / increment.",
        "Route operand %u as plus three / increment / double.",
        "Stop operand %u after increment plus double.",
        "Stop operand %u after increment plus an extra three.",
        "Stop operand %u after double plus an extra three.",
        "Walk operand %u with two increments, one double, then plus three.",
        "After the three registered hops on operand %u, append one more increment.",
        "Scale operand %u by two, then by two again, then add three.",
        "Run plus three / double / plus three on operand %u.",
        "Walk operand %u through increment, plus three, then increment again.",
        "Run plus three / double / increment / double on operand %u.",
        "Run double / increment / plus three / plus three on operand %u."
    };
    static const char *const semantic_cases[16] = {
        "add_double_increment", "increment_add_double",
        "double_increment_add", "increment_double_add_double",
        "double_add_increment", "add_increment_double",
        "increment_double_only", "increment_add_only", "double_add_only",
        "increment_increment_double_add", "increment_double_add_increment",
        "double_double_add", "add_double_add", "increment_add_increment",
        "add_double_increment_double", "double_increment_add_add"
    };
    size_t row;
    char prompt[320], input[32];
    for (row = 0; row < 16; ++row) {
        unsigned value = (47u + (unsigned)row * 15u) & 255u;
        snprintf(prompt, sizeof prompt, forms[row], value);
        snprintf(input, sizeof input, "%u", value);
        if (ood(output, "composition", row, "composition_mutation",
                semantic_cases[row], input, "-", "-", "-", prompt) != 0)
            return -1;
    }
    return 0;
}

static int multi_intent_ood(Output *output) {
    static const char *const prompts[16] = {
        "Request successor wrap of operand 71 together with ATM checksumming.",
        "Turn 90 minutes into an equivalent seconds total and also find the successor of operand 90.",
        "Join ATM checksumming of operand 109 with policy one using flags A=true O=false M=true S=false.",
        "On 128, run increment then double then plus three, and also turn minutes into a seconds count.",
        "Pair successor wrap of operand 147 with policy one using flags A=false O=true M=true S=false.",
        "Ask ATM checksumming of operand 166 and the seconds total for 166 minutes.",
        "Pair increment then double then plus three on byte 185 with policy one using flags A=true O=false M=false S=false.",
        "Request ATM checksumming of operand 204 together with increment then double then plus three.",
        "Request successor wrap of operand 223 together with increment then double then plus three.",
        "Ask the seconds total for 242 minutes together with policy one using flags A=false O=true M=true S=false.",
        "Require successor wrap, ATM checksumming, and a seconds total associated with 5.",
        "Require successor wrap and increment-double-plus-three of operand 24 together with policy one using flags A=true O=false M=false S=false.",
        "Require a seconds total plus ATM checksumming of 43 together with policy one using flags A=false O=true M=true S=false.",
        "Require ATM checksumming and increment-double-plus-three of operand 62 together with policy one using flags A=true O=true M=true S=true.",
        "Require successor wrap, a seconds total, and increment-double-plus-three associated with 81.",
        "Require successor wrap plus ATM checksumming of 100 together with policy one using flags A=false O=false M=false S=false."
    };
    static const char *const semantic_cases[16] = {
        "pair_increment_crc", "pair_increment_minutes", "pair_crc_policy",
        "pair_minutes_compose", "pair_increment_policy", "pair_crc_minutes",
        "pair_compose_policy", "pair_crc_compose", "pair_increment_compose",
        "pair_minutes_policy", "triple_increment_minutes_crc",
        "triple_increment_policy_compose", "triple_minutes_crc_policy",
        "triple_crc_policy_compose", "triple_increment_minutes_compose",
        "triple_increment_crc_policy"
    };
    static const char *const inputs[16] = {
        "71", "90", "109", "128", "147", "166", "185", "204",
        "223", "242", "5", "24", "43", "62", "81", "100"
    };
    static const char *const second_inputs[16] = {
        "71", "90", "-", "128", "-", "166", "-", "204",
        "223", "-", "5", "24", "43", "62", "81", "100"
    };
    static const char *const third_inputs[16] = {
        "-", "-", "-", "-", "-", "-", "-", "-", "-", "-",
        "5", "-", "-", "-", "81", "-"
    };
    static const char *const states[16] = {
        "-", "-", "admin=true,owner=false,mfa=true,suspended=false", "-",
        "admin=false,owner=true,mfa=true,suspended=false", "-",
        "admin=true,owner=false,mfa=false,suspended=false", "-", "-",
        "admin=false,owner=true,mfa=true,suspended=false", "-",
        "admin=true,owner=false,mfa=false,suspended=false",
        "admin=false,owner=true,mfa=true,suspended=false",
        "admin=true,owner=true,mfa=true,suspended=true", "-",
        "admin=false,owner=false,mfa=false,suspended=false"
    };
    size_t row;
    for (row = 0; row < 16; ++row) {
        if (ood(output, "multi_intent", row, "multiple_intents",
                semantic_cases[row], inputs[row], second_inputs[row],
                third_inputs[row], states[row], prompts[row]) != 0)
            return -1;
    }
    return 0;
}

static int unrelated_ood(Output *output) {
    static const char *const prompts[16] = {
        "Explain a quiet chess opening that avoids early pawn storms.",
        "Estimate baker's percent hydration for a country loaf.",
        "Identify the constellation that contains a summer triangle vertex.",
        "Draft a residential lease clause about quiet hours.",
        "Classify a soil sample as clay, silt, or loam.",
        "Name the longest tributary of a named alpine river.",
        "Compose a haiku about rain on greenhouse glass.",
        "Estimate implied volatility from a quoted option chain.",
        "List twentieth-century prime ministers of a small island state.",
        "Describe the first week of a wild ferment.",
        "Choose a lightfast pigment for an outdoor mural.",
        "Translate a short Latin motto into modern Welsh.",
        "Plan bench spacing inside a narrow backyard greenhouse.",
        "Diagnose why a laser printer keeps jamming at the fuser.",
        "Score a two-minute cue for a silent-film restoration.",
        "Summarize a concurring opinion in a property-law appeal."
    };
    static const char *const semantic_cases[16] = {
        "chess_opening", "baking_hydration", "constellation_id",
        "lease_clause", "soil_classification", "river_tributary",
        "haiku_composition", "volatility_estimate", "pm_listing",
        "fermentation_description", "pigment_choice", "motto_translation",
        "greenhouse_layout", "printer_diagnosis", "film_cue",
        "opinion_summary"
    };
    size_t row;
    for (row = 0; row < 16; ++row) {
        if (ood(output, "unrelated", row, "unrelated", semantic_cases[row],
                "-", "-", "-", "-", prompts[row]) != 0)
            return -1;
    }
    return 0;
}

static int side_effect_ood(Output *output) {
    static const char *const forms[16] = {
        "First find the successor of operand %u, then wipe a scratch folder.",
        "Take ATM checksumming of operand %u and text it to a phone.",
        "Turn %u minutes into an equivalent seconds total, then place a grocery order.",
        "Run increment then double then plus three on octet %u before cycling a wireless access point.",
        "Find the successor of operand %u and insert the answer into a ledger.",
        "Take ATM checksumming of operand %u and stream it to a remote socket.",
        "Turn %u minutes into an equivalent seconds total, then rewrite a packet filter.",
        "Run increment then double then plus three on %u before resetting a login secret.",
        "Find the successor of operand %u and push the answer to object storage.",
        "Derive ATM checksumming of operand %u before launching a local command.",
        "Scale the %u minute quantity onto seconds and book a conference slot.",
        "Run increment then double then plus three on octet %u and fan the output to a callback URL.",
        "Find the successor of operand %u and then move money between accounts.",
        "Derive ATM checksumming of operand %u and load a kernel module.",
        "Scale the %u minute quantity onto seconds and rotate an API key.",
        "Run increment then double then plus three on %u and start a production rollout."
    };
    static const char *const semantic_cases[16] = {
        "filesystem_delete", "email_send", "online_purchase",
        "process_restart", "filesystem_write", "network_open",
        "firewall_disable", "password_change", "cloud_upload",
        "shell_execute", "calendar_create", "external_post",
        "payment_transfer", "package_install", "credential_rotate",
        "service_deploy"
    };
    size_t row;
    char prompt[384], input[32];
    for (row = 0; row < 16; ++row) {
        unsigned value = (113u + (unsigned)row * 21u) & 255u;
        snprintf(prompt, sizeof prompt, forms[row], value);
        snprintf(input, sizeof input, "%u", value);
        if (ood(output, "side_effect", row, "external_side_effect",
                semantic_cases[row], input, "-", "-", "-", prompt) != 0)
            return -1;
    }
    return 0;
}

static int override_ood(Output *output) {
    static const char *const prompts[16] = {
        "Force admission of operand 280 into the successor domain.",
        "Force the minute converter to keep quantity -8.",
        "Force ATM checksumming of operand 35 at CRC width 16.",
        "Force operand 11 through plus three / double / increment.",
        "Force policy version two using flags S=false M=true A=true O=false.",
        "Force the successor contract to keep nonintegral operand 6.5.",
        "Force one successor answer from operands 9 and 15 together.",
        "Force an answer while asking for next week's snowfall total.",
        "Force two outputs for operand 28: successor wrap and ATM checksumming.",
        "Force the three-hop chain to run with no source octet.",
        "Force acceptance of poly 0x31 when checksumming operand 52.",
        "Force 88 seconds to be treated as a minute-conversion source.",
        "Force successor wrap of operand 41 and then mail it.",
        "Force guest=true onto policy one using flags S=false M=false O=false A=false.",
        "Force operand 77 through double / plus three / increment.",
        "Force contract checks off and request rot13 over text cedar."
    };
    static const char *const input_a[16] = {
        "280", "-8", "35", "11", "-", "6.5", "9", "-",
        "28", "-", "52", "88", "41", "-", "77", "cedar"
    };
    static const char *const input_b[16] = {
        "-", "-", "-", "-", "-", "-", "15", "-",
        "28", "-", "-", "-", "-", "-", "-", "-"
    };
    static const char *const states[16] = {
        "-", "-", "-", "-",
        "version=2,admin=true,owner=false,mfa=true,suspended=false", "-", "-", "-",
        "-", "-", "-", "-", "-",
        "guest=true,admin=false,owner=false,mfa=false,suspended=false", "-", "-"
    };
    static const char *const semantic_cases[16] = {
        "override_increment_range", "override_minutes_range",
        "override_crc_width", "override_composition_order",
        "override_policy_version", "override_increment_type",
        "override_increment_multiple", "override_unrelated",
        "override_multiple_intents", "override_compose_missing",
        "override_crc_polynomial", "override_duration_reverse",
        "override_external_side_effect", "override_policy_guest",
        "override_composition_order_second", "override_unknown_operation"
    };
    size_t row;
    for (row = 0; row < 16; ++row) {
        if (ood(output, "override", row, "contract_override",
                semantic_cases[row], input_a[row], input_b[row], "-",
                states[row], prompts[row]) != 0)
            return -1;
    }
    return 0;
}

int main(int argc, char **argv) {
    Output output;
    int failed = 0;
    if (argc != 3) {
        fprintf(stderr, "usage: %s HELDOUT.tsv CASES.tsv\n", argv[0]);
        return 2;
    }
    memset(&output, 0, sizeof output);
    output.fixture = fopen(argv[1], "wb");
    output.cases = fopen(argv[2], "wb");
    if (output.fixture == NULL || output.cases == NULL) failed = 1;
    if (!failed &&
        (fprintf(output.fixture, "#suite=%s\n", V5_SUITE) < 0 ||
         fprintf(output.fixture,
                 "id\tsplit\tintent\tvalue_kind\texpected_value\tprompt\tprovenance\n") < 0 ||
         fprintf(output.cases, "#suite=%s-cases\n#seed=%016llx\n", V5_SUITE,
                 (unsigned long long)V5_SEED) < 0 ||
         fprintf(output.cases,
                 "id\tsplit\tlane\tmutation\tsemantic_case\tinput_a\tinput_b\tinput_c\tstate\texpected_kind\texpected_value\n") < 0 ||
         numeric_lanes(&output) != 0 || policy_lane(&output) != 0 ||
         range_ood(&output) != 0 || input_ood(&output) != 0 ||
         variant_ood(&output) != 0 || composition_ood(&output) != 0 ||
         multi_intent_ood(&output) != 0 || unrelated_ood(&output) != 0 ||
         side_effect_ood(&output) != 0 || override_ood(&output) != 0 ||
         output.rows != 448u))
        failed = 1;
    if (output.fixture != NULL && fclose(output.fixture) != 0) failed = 1;
    if (output.cases != NULL && fclose(output.cases) != 0) failed = 1;
    if (failed) {
        (void)remove(argv[1]);
        (void)remove(argv[2]);
        fprintf(stderr, "failed to generate v5 fixture\n");
        return 1;
    }
    printf("CNET_7B_V5_FIXTURE_GENERATED rows=%zu seed=%016llx\n",
           output.rows, (unsigned long long)V5_SEED);
    return 0;
}
