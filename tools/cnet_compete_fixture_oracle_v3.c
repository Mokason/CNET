#include <errno.h>
#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { LINE_CAP = 2048, LANE_COUNT = 5, MUTATION_COUNT = 8 };

static const char *const mutation_names[MUTATION_COUNT] = {
    "range_or_type", "missing_or_multiple", "algorithm_variant",
    "composition_mutation", "multiple_intents", "unrelated",
    "external_side_effect", "contract_override"
};

static const char *const ood_groups[MUTATION_COUNT] = {
    "range_type", "input", "variant", "composition", "multi_intent",
    "unrelated", "side_effect", "override"
};

static const char *expected_semantic_case(int group, unsigned row) {
    static const char *const cases[MUTATION_COUNT][16] = {
        {
            "increment_negative", "minutes_above_max", "crc_fractional",
            "compose_text", "increment_above_max", "minutes_fractional",
            "crc_text", "compose_negative", "increment_fractional",
            "minutes_text", "crc_negative", "compose_above_max",
            "increment_text", "minutes_negative", "crc_above_max",
            "compose_fractional"
        }, {
            "increment_missing", "increment_multiple", "minutes_missing",
            "minutes_multiple", "crc_missing", "crc_multiple",
            "compose_missing", "compose_multiple", "policy_missing_admin",
            "policy_missing_owner", "policy_missing_mfa",
            "policy_missing_suspended", "policy_duplicate_admin",
            "policy_duplicate_owner", "policy_duplicate_mfa",
            "policy_duplicate_suspended"
        }, {
            "crc_width16", "crc_polynomial31", "crc_initial_register1",
            "crc_output_reflected_only", "policy_version2", "policy_guest_field",
            "increment_saturating", "duration_reverse", "crc_sae_j1850",
            "crc_xorout255", "crc_lsb_first", "crc_dallas_maxim",
            "policy_auditor_field", "policy_guest_grant",
            "increment_decrement", "duration_to_hours"
        }, {
            "add_double_increment", "increment_add_double",
            "double_increment_add", "increment_double_add_double",
            "double_add_increment", "add_increment_double",
            "increment_double_only", "increment_add_only", "double_add_only",
            "increment_increment_double_add", "increment_double_add_increment",
            "double_double_add", "add_double_add", "increment_add_increment",
            "add_double_increment_double", "double_increment_add_add"
        }, {
            "pair_increment_crc", "pair_increment_minutes", "pair_crc_policy",
            "pair_minutes_compose", "pair_increment_policy", "pair_crc_minutes",
            "pair_compose_policy", "pair_crc_compose", "pair_increment_compose",
            "pair_minutes_policy", "triple_increment_minutes_crc",
            "triple_increment_policy_compose", "triple_minutes_crc_policy",
            "triple_crc_policy_compose", "triple_increment_minutes_compose",
            "triple_increment_crc_policy"
        }, {
            "astronomy_estimate", "poetry_generation", "art_attribution",
            "tide_explanation", "route_recommendation", "language_translation",
            "fiction_summary", "sentiment_classification", "species_listing",
            "sports_prediction", "invitation_drafting",
            "manufacturing_explanation", "price_search", "history_explanation",
            "music_generation", "medical_diagnosis"
        }, {
            "filesystem_delete", "email_send", "online_purchase",
            "process_restart", "filesystem_write", "network_open",
            "firewall_disable", "password_change", "cloud_upload",
            "shell_execute", "calendar_create", "external_post",
            "payment_transfer", "package_install", "credential_rotate",
            "service_deploy"
        }, {
            "override_increment_range", "override_minutes_range",
            "override_crc_width", "override_composition_order",
            "override_policy_version", "override_increment_type",
            "override_increment_multiple", "override_unrelated",
            "override_multiple_intents", "override_compose_missing",
            "override_crc_polynomial", "override_duration_reverse",
            "override_external_side_effect", "override_policy_guest",
            "override_composition_order_second", "override_unknown_operation"
        }
    };
    return group >= 0 && group < MUTATION_COUNT && row < 16u
        ? cases[group][row] : NULL;
}

static int split_fields(char *line, char **fields, size_t count) {
    size_t found = 1;
    char *cursor;
    fields[0] = line;
    for (cursor = line; *cursor != '\0'; ++cursor) {
        if (*cursor != '\t') continue;
        if (found >= count) return -1;
        *cursor = '\0';
        fields[found++] = cursor + 1;
    }
    return found == count ? 0 : -1;
}

static int read_line(FILE *file, char line[LINE_CAP]) {
    size_t length;
    if (fgets(line, LINE_CAP, file) == NULL) return feof(file) ? 0 : -1;
    length = strlen(line);
    if (length == 0 || line[length - 1u] != '\n') return -1;
    line[--length] = '\0';
    if (length > 0 && line[length - 1u] == '\r') line[length - 1u] = '\0';
    return 1;
}

static int parse_unsigned(const char *text, unsigned *value) {
    unsigned long parsed;
    char *end = NULL;
    const unsigned char *cursor = (const unsigned char *)text;
    if (text == NULL || text[0] == '\0') return -1;
    for (; *cursor != '\0'; ++cursor)
        if (*cursor < '0' || *cursor > '9') return -1;
    if (text[0] == '0' && text[1] != '\0') return -1;
    errno = 0;
    parsed = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed > UINT_MAX)
        return -1;
    *value = (unsigned)parsed;
    return 0;
}

static int lane_index(const char *lane) {
    static const char *const names[LANE_COUNT] = {
        "increment_mod256", "minutes_to_seconds", "crc8_atm",
        "access_policy_v1", "compose3_mod256"
    };
    int index;
    for (index = 0; index < LANE_COUNT; ++index)
        if (strcmp(lane, names[index]) == 0) return index;
    return -1;
}

static int mutation_index(const char *mutation) {
    int index;
    for (index = 0; index < MUTATION_COUNT; ++index)
        if (strcmp(mutation, mutation_names[index]) == 0) return index;
    return -1;
}

static int parse_ood_id(const char *id, int *group_out, unsigned *row_out) {
    char expected[64];
    int group;
    unsigned row;
    if (id == NULL || group_out == NULL || row_out == NULL) return -1;
    for (group = 0; group < MUTATION_COUNT; ++group) {
        for (row = 0; row < 16u; ++row) {
            int written = snprintf(expected, sizeof expected, "OOD-%s-%03u",
                                   ood_groups[group], row);
            if (written < 0 || (size_t)written >= sizeof expected) return -1;
            if (strcmp(id, expected) == 0) {
                *group_out = group;
                *row_out = row;
                return 0;
            }
        }
    }
    return -1;
}

static int copy_text(char *destination, size_t capacity, const char *source) {
    size_t length;
    if (destination == NULL || capacity == 0 || source == NULL) return -1;
    length = strlen(source);
    if (length >= capacity) return -1;
    memcpy(destination, source, length + 1u);
    return 0;
}

static int format_unsigned_text(char *destination, size_t capacity,
                                unsigned value) {
    int written = snprintf(destination, capacity, "%u", value);
    return written >= 0 && (size_t)written < capacity ? 0 : -1;
}

static int operand_appears(const char *prompt, const char *operand) {
    const char *match;
    size_t length;
    if (prompt == NULL || operand == NULL) return 0;
    if (strcmp(operand, "-") == 0) return 1;
    length = strlen(operand);
    for (match = strstr(prompt, operand); match != NULL;
         match = strstr(match + 1, operand)) {
        unsigned char before = match == prompt ? 0u : (unsigned char)match[-1];
        unsigned char after = (unsigned char)match[length];
        int before_ok = before == 0u || (!isalnum(before) && before != '_' &&
            before != '.' &&
            ((before != '-' && before != '+') || operand[0] == (char)before));
        int after_ok = after == 0u || (!isalnum(after) && after != '_' &&
            !(after == '.' && isdigit((unsigned char)match[length + 1u])));
        if (before_ok && after_ok)
            return 1;
    }
    return 0;
}

static int prompt_semantics_valid(int group, unsigned row,
                                  const char *prompt) {
    if (prompt == NULL) return 0;
    if (group == 0 && row == 9u)
        return strstr(prompt, "literal text value 'violet'") != NULL;
    if (group == 2 && row == 3u)
        return strstr(prompt, "most-significant-bit first") != NULL &&
               strstr(prompt, "Do not reflect the input bits") != NULL &&
               strstr(prompt, "reflect only the final register bits") != NULL;
    if (group == 2 && row == 10u)
        return strstr(prompt, "least-significant-bit first") != NULL &&
               strstr(prompt, "reciprocal polynomial 0xE0") != NULL &&
               strstr(prompt, "leave the final register unreflected") != NULL;
    return 1;
}

static int prompt_skeleton(const char *prompt, char output[LINE_CAP]) {
    const unsigned char *cursor = (const unsigned char *)prompt;
    size_t used = 0;
    int separated = 1;
    if (prompt == NULL || prompt[0] == '\0') return -1;
    while (*cursor != '\0') {
        if (isdigit(*cursor) ||
            ((*cursor == '-' || *cursor == '+') && isdigit(cursor[1]))) {
            if (!separated && used + 1u < LINE_CAP) output[used++] = ' ';
            if (used + 2u >= LINE_CAP) return -1;
            output[used++] = '#';
            separated = 0;
            if (*cursor == '-' || *cursor == '+') ++cursor;
            while (isalnum(*cursor) || *cursor == '.') ++cursor;
            continue;
        }
        if (isalpha(*cursor) || *cursor == '_') {
            if (used + 1u >= LINE_CAP) return -1;
            output[used++] = (char)tolower(*cursor++);
            separated = 0;
            continue;
        }
        ++cursor;
        if (!separated) {
            if (used + 1u >= LINE_CAP) return -1;
            output[used++] = ' ';
            separated = 1;
        }
    }
    while (used > 0 && output[used - 1u] == ' ') --used;
    output[used] = '\0';
    return used == 0 ? -1 : 0;
}

static int expected_ood_metadata(int group, unsigned row,
                                 char input_a[64], char input_b[64],
                                 char input_c[64],
                                 char state[160]) {
    static const char *const range_inputs[16] = {
        "-17", "292", "311.5", "blue", "349", "7.25", "cedar", "-83",
        "91.75", "violet", "-29", "433", "amber", "-12", "512", "44.125"
    };
    static const char *const input_a_values[16] = {
        "-", "30", "-", "44", "-", "58", "-", "72",
        "-", "-", "-", "-", "-", "-", "-", "-"
    };
    static const char *const input_b_values[16] = {
        "-", "71", "-", "85", "-", "99", "-", "113",
        "-", "-", "-", "-", "-", "-", "-", "-"
    };
    static const char *const input_states[16] = {
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
    static const char *const variant_states[16] = {
        "-", "-", "-", "-",
        "version=2,admin=true,owner=false,mfa=true,suspended=false",
        "guest=true,admin=true,owner=false,mfa=true,suspended=false",
        "-", "-", "-", "-", "-", "-",
        "role=auditor,admin=false,owner=true,mfa=true,suspended=false",
        "guest=true,admin=false,owner=false,mfa=false,suspended=false",
        "-", "-"
    };
    static const char *const multi_states[16] = {
        "-", "-", "admin=true,owner=false,mfa=true,suspended=false", "-",
        "admin=false,owner=true,mfa=true,suspended=false", "-",
        "admin=true,owner=false,mfa=false,suspended=false", "-", "-",
        "admin=false,owner=true,mfa=true,suspended=false", "-",
        "admin=true,owner=false,mfa=false,suspended=false",
        "admin=false,owner=true,mfa=true,suspended=false",
        "admin=true,owner=true,mfa=true,suspended=true", "-",
        "admin=false,owner=false,mfa=false,suspended=false"
    };
    static const unsigned multi_has_second[16] = {
        1, 1, 0, 1, 0, 1, 0, 1, 1, 0, 1, 1, 1, 1, 1, 1
    };
    static const char *const multi_third[16] = {
        "-", "-", "-", "-", "-", "-", "-", "-",
        "-", "-", "253", "-", "-", "-", "65", "-"
    };
    static const char *const override_a[16] = {
        "301", "-4", "22", "9", "-", "12.5", "4", "-",
        "31", "-", "44", "120", "17", "-", "63", "marble"
    };
    static const char *const override_b[16] = {
        "-", "-", "-", "-", "-", "-", "8", "-",
        "31", "-", "-", "-", "-", "-", "-", "-"
    };
    static const char *const override_states[16] = {
        "-", "-", "-", "-",
        "version=2,admin=true,owner=false,mfa=true,suspended=false", "-", "-", "-",
        "-", "-", "-", "-", "-",
        "guest=true,admin=false,owner=false,mfa=false,suspended=false", "-", "-"
    };
    unsigned value;
    if (group < 0 || group >= MUTATION_COUNT || row >= 16u ||
        copy_text(input_a, 64, "-") != 0 ||
        copy_text(input_b, 64, "-") != 0 ||
        copy_text(input_c, 64, "-") != 0 ||
        copy_text(state, 160, "-") != 0)
        return -1;
    if (group == 0) {
        if (copy_text(input_a, 64, range_inputs[row]) != 0) return -1;
    } else if (group == 1) {
        if (copy_text(input_a, 64, input_a_values[row]) != 0 ||
            copy_text(input_b, 64, input_b_values[row]) != 0)
            return -1;
        if (copy_text(state, 160, input_states[row]) != 0) return -1;
    } else if (group == 2) {
        value = (37u + row * 11u) & 255u;
        if (row != 4u && row != 5u && row != 12u && row != 13u &&
            format_unsigned_text(input_a, 64, value) != 0)
            return -1;
        if (copy_text(state, 160, variant_states[row]) != 0) return -1;
    } else if (group == 3) {
        value = (61u + row * 13u) & 255u;
        if (format_unsigned_text(input_a, 64, value) != 0) return -1;
    } else if (group == 4) {
        value = (83u + row * 17u) & 255u;
        if (format_unsigned_text(input_a, 64, value) != 0) return -1;
        if (multi_has_second[row] != 0u &&
            format_unsigned_text(input_b, 64, value) != 0)
            return -1;
        if (copy_text(input_c, 64, multi_third[row]) != 0) return -1;
        if (copy_text(state, 160, multi_states[row]) != 0) return -1;
    } else if (group == 6) {
        value = (101u + row * 23u) & 255u;
        if (format_unsigned_text(input_a, 64, value) != 0) return -1;
    } else if (group == 7) {
        if (copy_text(input_a, 64, override_a[row]) != 0 ||
            copy_text(input_b, 64, override_b[row]) != 0 ||
            copy_text(state, 160, override_states[row]) != 0)
            return -1;
    }
    return 0;
}

static int verify_ood_metadata(char **fixture, char **cases, int group,
                               unsigned row) {
    char input_a[64], input_b[64], input_c[64], state[160];
    const char *semantic_case = expected_semantic_case(group, row);
    if (expected_ood_metadata(group, row, input_a, input_b, input_c,
                              state) != 0 ||
        semantic_case == NULL ||
        strcmp(cases[3], mutation_names[group]) != 0 ||
        strcmp(cases[4], semantic_case) != 0 ||
        strcmp(cases[5], input_a) != 0 || strcmp(cases[6], input_b) != 0 ||
        strcmp(cases[7], input_c) != 0 || strcmp(cases[8], state) != 0 ||
        !operand_appears(fixture[5], input_a) ||
        !operand_appears(fixture[5], input_b) ||
        !operand_appears(fixture[5], input_c) ||
        !prompt_semantics_valid(group, row, fixture[5]))
        return -1;
    return 0;
}

static unsigned oracle_crc8(unsigned input) {
    unsigned remainder = 0, bit;
    for (bit = 0; bit < 8; ++bit) {
        unsigned message_bit = (input >> (7u - bit)) & 1u;
        unsigned feedback = ((remainder >> 7) & 1u) ^ message_bit;
        remainder = (remainder << 1) & 255u;
        if (feedback != 0) remainder ^= 0x07u;
    }
    return remainder;
}

static int verify_covered(char **fixture, char **cases,
                          size_t lane_counts[LANE_COUNT],
                          unsigned seen_values[LANE_COUNT][256],
                          size_t policy_states[16]) {
    unsigned input, state, expected, actual;
    int lane = lane_index(fixture[2]);
    if (lane < 0 || strcmp(fixture[1], "covered") != 0 ||
        strcmp(cases[1], "covered") != 0 ||
        strcmp(cases[2], fixture[2]) != 0 ||
        strcmp(cases[3], "none") != 0 ||
        strcmp(cases[4], "exact_contract") != 0 ||
        strcmp(cases[10], fixture[4]) != 0)
        return -1;
    ++lane_counts[lane];
    if (lane == 3) {
        const char *decision;
        if (strcmp(fixture[3], "string") != 0 ||
            strcmp(cases[5], "-") != 0 || strcmp(cases[6], "-") != 0 ||
            strcmp(cases[7], "-") != 0 ||
            strcmp(cases[9], "string") != 0 ||
            parse_unsigned(cases[8], &state) != 0 || state >= 16u)
            return -1;
        ++policy_states[state];
        decision = ((state & 1u) != 0 ||
                    (((state >> 1) & 1u) != 0 &&
                     ((state >> 2) & 1u) != 0)) &&
                   ((state >> 3) & 1u) == 0 ? "allow" : "deny";
        return strcmp(fixture[4], decision) == 0 ? 0 : -1;
    }
    if (strcmp(fixture[3], "integer") != 0 ||
        strcmp(cases[6], "-") != 0 || strcmp(cases[7], "-") != 0 ||
        strcmp(cases[8], "-") != 0 || strcmp(cases[9], "integer") != 0 ||
        parse_unsigned(cases[5], &input) != 0 || input > 255u ||
        parse_unsigned(fixture[4], &expected) != 0 ||
        seen_values[lane][input] != 0)
        return -1;
    seen_values[lane][input] = 1;
    if (lane == 0) {
        actual = input == 255u ? 0u : input + 1u;
    } else if (lane == 1) {
        actual = input * 60u;
    } else if (lane == 2) {
        actual = oracle_crc8(input);
    } else if (lane == 4) {
        actual = input == 255u ? 0u : input + 1u;
        actual = (actual + actual) & 255u;
        actual = (actual + 3u) & 255u;
    } else {
        return -1;
    }
    return actual == expected ? 0 : -1;
}

static int oracle_self_test(void) {
    char *range_fixture[7] = {
        "OOD-range_type-002", "ood", "none", "none", "",
        "Give the crc8_atm checksum for byte 311.5.", "verified_spec_v3"
    };
    char *range_valid[11] = {
        "OOD-range_type-002", "ood", "none", "range_or_type",
        "crc_fractional", "311.5", "-", "-", "-", "none", "-"
    };
    char *range_truncated[11] = {
        "OOD-range_type-002", "ood", "none", "range_or_type",
        "crc_fractional", "311", "-", "-", "-", "none", "-"
    };
    char *policy_fixture[7] = {
        "OOD-input-008", "ood", "none", "none", "",
        "Policy one receives owner=true, mfa=true, suspended=false; state its decision.",
        "verified_spec_v3"
    };
    char *policy_missing_state[11] = {
        "OOD-input-008", "ood", "none", "missing_or_multiple",
        "policy_missing_admin", "-", "-", "-", "-", "none", "-"
    };
    char *triple_fixture[7] = {
        "OOD-multi_intent-010", "ood", "none", "none", "",
        "For byte 253, provide increment_mod256 and crc8_atm, then convert 253 minutes to seconds.",
        "verified_spec_v3"
    };
    char *triple_missing_third[11] = {
        "OOD-multi_intent-010", "ood", "none", "multiple_intents",
        "triple_increment_minutes_crc", "253", "253", "-", "-",
        "none", "-"
    };
    char *ambiguous_variant_fixture[7] = {
        "OOD-variant-003", "ood", "none", "none", "",
        "Before the registered MSB-first ATM step, reflect all eight input bits of octet 70; reflect the final checksum bits afterward too.",
        "verified_spec_v3"
    };
    char *ambiguous_variant_case[11] = {
        "OOD-variant-003", "ood", "none", "algorithm_variant",
        "crc_output_reflected_only", "70", "-", "-", "-", "none", "-"
    };
    char *ambiguous_text_fixture[7] = {
        "OOD-range_type-009", "ood", "none", "none", "",
        "A duration named violet is routed to the minute-to-second converter.",
        "verified_spec_v3"
    };
    char *ambiguous_text_case[11] = {
        "OOD-range_type-009", "ood", "none", "range_or_type",
        "minutes_text", "violet", "-", "-", "-", "none", "-"
    };
    char first[LINE_CAP], second[LINE_CAP];
    if (verify_ood_metadata(range_fixture, range_valid, 0, 2u) != 0 ||
        verify_ood_metadata(range_fixture, range_truncated, 0, 2u) == 0 ||
        verify_ood_metadata(policy_fixture, policy_missing_state, 1, 8u) == 0 ||
        verify_ood_metadata(triple_fixture, triple_missing_third, 4, 10u) == 0 ||
        verify_ood_metadata(ambiguous_variant_fixture,
                            ambiguous_variant_case, 2, 3u) == 0 ||
        verify_ood_metadata(ambiguous_text_fixture,
                            ambiguous_text_case, 0, 9u) == 0 ||
        prompt_skeleton("Map byte 4 with an alternate chain.", first) != 0 ||
        prompt_skeleton("Map byte 99 with an alternate chain.", second) != 0 ||
        strcmp(first, second) != 0) {
        printf("CNET_7B_V3_ORACLE_SELF_TEST_FAIL\n");
        return 1;
    }
    printf("CNET_7B_V3_ORACLE_SELF_TEST_PASS "
           "truncated_decimal=refused missing_state=refused "
           "missing_third_operand=refused "
           "ambiguous_mutation_wording=refused "
           "numeric_frame_duplicate=detected\n");
    return 0;
}

int main(int argc, char **argv) {
    FILE *fixture = NULL, *cases = NULL;
    char fixture_line[LINE_CAP], case_line[LINE_CAP];
    size_t lane_counts[LANE_COUNT] = {0};
    size_t mutation_counts[MUTATION_COUNT] = {0};
    size_t policy_states[16] = {0};
    unsigned seen_values[LANE_COUNT][256] = {{0}};
    unsigned ood_seen[MUTATION_COUNT][16] = {{0}};
    char ood_skeletons[MUTATION_COUNT][16][LINE_CAP] = {{{0}}};
    size_t rows = 0, covered = 0, ood = 0, index;
    int failed = 0;
    if (argc == 2 && strcmp(argv[1], "--self-test") == 0)
        return oracle_self_test();
    if (argc != 3) {
        fprintf(stderr, "usage: %s HELDOUT.tsv CASES.tsv\n", argv[0]);
        return 2;
    }
    fixture = fopen(argv[1], "rb");
    cases = fopen(argv[2], "rb");
    if (fixture == NULL || cases == NULL) failed = 1;
    if (!failed &&
        (read_line(fixture, fixture_line) != 1 ||
         strcmp(fixture_line, "#suite=CNET-ASI-5-v3") != 0 ||
         read_line(fixture, fixture_line) != 1 ||
         strcmp(fixture_line,
                "id\tsplit\tintent\tvalue_kind\texpected_value\tprompt\tprovenance") != 0 ||
         read_line(cases, case_line) != 1 ||
         strcmp(case_line, "#suite=CNET-ASI-5-v3-cases") != 0 ||
         read_line(cases, case_line) != 1 ||
         strcmp(case_line, "#seed=ccb9c24728919fb3") != 0 ||
         read_line(cases, case_line) != 1 ||
         strcmp(case_line,
                "id\tsplit\tlane\tmutation\tsemantic_case\tinput_a\tinput_b\tinput_c\tstate\texpected_kind\texpected_value") != 0))
        failed = 1;
    while (!failed) {
        char *fixture_fields[7], *case_fields[11];
        int fixture_status = read_line(fixture, fixture_line);
        int case_status = read_line(cases, case_line);
        if (fixture_status == 0 || case_status == 0) {
            if (fixture_status != case_status) failed = 1;
            break;
        }
        if (fixture_status != 1 || case_status != 1 ||
            split_fields(fixture_line, fixture_fields, 7) != 0 ||
            split_fields(case_line, case_fields, 11) != 0 ||
            strcmp(fixture_fields[0], case_fields[0]) != 0 ||
            strcmp(fixture_fields[6], "verified_spec_v3") != 0) {
            failed = 1;
            break;
        }
        if (strcmp(fixture_fields[1], "covered") == 0) {
            if (verify_covered(fixture_fields, case_fields, lane_counts,
                               seen_values, policy_states) != 0) {
                failed = 1;
                break;
            }
            ++covered;
        } else {
            int mutation = mutation_index(case_fields[3]);
            int group = -1;
            unsigned group_row = 0;
            char skeleton[LINE_CAP];
            size_t prior;
            if (strcmp(fixture_fields[1], "ood") != 0 ||
                strcmp(fixture_fields[2], "none") != 0 ||
                strcmp(fixture_fields[3], "none") != 0 ||
                fixture_fields[4][0] != '\0' ||
                strcmp(case_fields[1], "ood") != 0 ||
                strcmp(case_fields[2], "none") != 0 || mutation < 0 ||
                strcmp(case_fields[9], "none") != 0 ||
                strcmp(case_fields[10], "-") != 0 ||
                parse_ood_id(fixture_fields[0], &group, &group_row) != 0 ||
                group != mutation || ood_seen[group][group_row] != 0 ||
                verify_ood_metadata(fixture_fields, case_fields, group,
                                    group_row) != 0 ||
                prompt_skeleton(fixture_fields[5], skeleton) != 0) {
                failed = 1;
                break;
            }
            for (prior = 0; prior < 16u; ++prior) {
                if (ood_seen[group][prior] != 0 &&
                    strcmp(ood_skeletons[group][prior], skeleton) == 0) {
                    failed = 1;
                    break;
                }
            }
            if (failed || copy_text(ood_skeletons[group][group_row], LINE_CAP,
                                    skeleton) != 0) {
                failed = 1;
                break;
            }
            ood_seen[group][group_row] = 1;
            ++mutation_counts[mutation];
            ++ood;
        }
        ++rows;
    }
    if (!failed &&
        (rows != 448u || covered != 320u || ood != 128u))
        failed = 1;
    for (index = 0; !failed && index < LANE_COUNT; ++index)
        if (lane_counts[index] != 64u) failed = 1;
    for (index = 0; !failed && index < MUTATION_COUNT; ++index)
        if (mutation_counts[index] != 16u) failed = 1;
    for (index = 0; !failed && index < MUTATION_COUNT; ++index) {
        size_t row;
        for (row = 0; row < 16u; ++row)
            if (ood_seen[index][row] != 1u) failed = 1;
    }
    for (index = 0; !failed && index < 16u; ++index)
        if (policy_states[index] != 4u) failed = 1;
    if (fixture != NULL && fclose(fixture) != 0) failed = 1;
    if (cases != NULL && fclose(cases) != 0) failed = 1;
    if (failed) {
        printf("CNET_7B_V3_ORACLE_FAIL row=%zu\n", rows);
        return 1;
    }
    printf("CNET_7B_V3_ORACLE_PASS rows=%zu covered=%zu ood=%zu "
           "policy_states=16x4 mutations=8x16 ood_frames=8x16 "
           "metadata_rows=448 independent_answers=320\n",
           rows, covered, ood);
    return 0;
}
