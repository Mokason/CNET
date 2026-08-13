#include "cnet_compete_runtime.h"

#include "cnet_capsule.h"
#include "cnet_compete_capsules.h"
#include "router.h"

#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RUNTIME_PATH_MAX 1024
#define RUNTIME_LEXEMES_MAX 96
#define RUNTIME_WORD_MAX 32

typedef enum { LEXEME_WORD, LEXEME_NUMBER } LexemeKind;

typedef struct {
    LexemeKind kind;
    char word[RUNTIME_WORD_MAX];
    unsigned long long number;
    int negative;
    int hexadecimal;
    int malformed;
} Lexeme;

typedef struct {
    const HybridAi *coverage;
    size_t checks;
} CompositionGuard;

struct CnetCompeteRuntime {
    CnetCompeteIntentModel *intent;
    CnetBase base;
    HybridAi coverage;
    PrimitiveRegistry registry;
    DagPlan composition;
    DagSource composition_source;
    double composition_input[8];
    int base_initialized;
    int coverage_initialized;
    int registry_initialized;
    int composition_initialized;
};

static size_t unit_domain(CnetCompeteUnit unit) {
    return unit == CNET_COMPETE_UNIT_POLICY ? 16u : 256u;
}

static int join_path(char output[RUNTIME_PATH_MAX], const char *root,
                     const char *leaf) {
    int written;
    if (output == NULL || root == NULL || leaf == NULL || root[0] == '\0')
        return -1;
    written = snprintf(output, RUNTIME_PATH_MAX, "%s/%s", root, leaf);
    return written < 0 || written >= RUNTIME_PATH_MAX ? -1 : 0;
}

static Port byte_port(const char *tag) {
    Port port = {PORT_BINARY_MSB, 8, 1, ""};
    if (port_set_tag(&port, tag) != 0) memset(&port, 0, sizeof port);
    return port;
}

static void encode_msb(double bits[8], unsigned value) {
    unsigned index;
    for (index = 0; index < 8; ++index)
        bits[index] = (double)((value >> (7u - index)) & 1u);
}

static unsigned decode_msb(const double bits[8]) {
    unsigned index, value = 0;
    for (index = 0; index < 8; ++index)
        value = (value << 1) | (bits[index] >= 0.5 ? 1u : 0u);
    return value;
}

static void collect_plan_members(const DagNode *node, char names[4][64],
                                 size_t *count) {
    size_t child;
    if (node == NULL || count == NULL || *count >= 4) return;
    if (node->kind == DAG_PRIMITIVE && node->name != NULL)
        snprintf(names[(*count)++], sizeof names[0], "%s", node->name);
    for (child = 0; child < node->child_count; ++child)
        collect_plan_members(node->children[child], names, count);
}

static int plan_is_compose3(const DagPlan *plan, size_t *member_count) {
    char names[4][64] = {{0}};
    size_t count = 0, index;
    int increment = 0, doubling = 0, add3 = 0;
    if (plan == NULL || plan->root == NULL) return 0;
    collect_plan_members(plan->root, names, &count);
    for (index = 0; index < count; ++index) {
        if (strcmp(names[index], "increment_mod256") == 0) increment = 1;
        else if (strcmp(names[index], "double_mod256") == 0) doubling = 1;
        else if (strcmp(names[index], "add3_mod256") == 0) add3 = 1;
        else return 0;
    }
    if (member_count != NULL) *member_count = count;
    return count == 3 && increment && doubling && add3;
}

static int composition_guard(const char *unit,
                             const BinaryTransformNetwork *network,
                             const double *input, size_t input_length,
                             void *context) {
    static const char *const expected[] = {
        "increment_mod256", "double_mod256", "add3_mod256"
    };
    CompositionGuard *guard = (CompositionGuard *)context;
    if (guard == NULL || guard->coverage == NULL || unit == NULL ||
        network == NULL || input == NULL ||
        network->input_port_count != 1 || network->output_port_count != 1 ||
        guard->checks >= sizeof expected / sizeof expected[0] ||
        strcmp(unit, expected[guard->checks]) != 0)
        return -1;
    if (!hybrid_coverage_admits_exact(guard->coverage, unit,
                                      network->input_ports[0],
                                      network->output_ports[0], input,
                                      input_length))
        return -1;
    ++guard->checks;
    return 0;
}

static int lexemes_scan(const char *text, Lexeme output[RUNTIME_LEXEMES_MAX],
                        size_t *count_out) {
    const unsigned char *cursor = (const unsigned char *)text;
    const unsigned char *begin = cursor;
    size_t count = 0;
    if (text == NULL || output == NULL || count_out == NULL) return -1;
    memset(output, 0, sizeof output[0] * RUNTIME_LEXEMES_MAX);
    while (*cursor != '\0') {
        Lexeme *token;
        if (*cursor >= 128u) return -1;
        if (isalpha(*cursor)) {
            size_t length = 0;
            if (count >= RUNTIME_LEXEMES_MAX) return -1;
            token = &output[count++];
            memset(token, 0, sizeof *token);
            token->kind = LEXEME_WORD;
            while (isalpha(*cursor) || *cursor == '\'') {
                unsigned char character = *cursor++;
                if (length + 1u >= sizeof token->word) return -1;
                token->word[length++] = (char)tolower(character);
            }
            token->word[length] = '\0';
            continue;
        }
        if (isdigit(*cursor)) {
            const unsigned char *number_begin = cursor;
            unsigned long long value = 0;
            int base = 10, overflow = 0;
            if (count >= RUNTIME_LEXEMES_MAX) return -1;
            token = &output[count++];
            memset(token, 0, sizeof *token);
            token->kind = LEXEME_NUMBER;
            token->negative = cursor > begin && cursor[-1] == '-';
            if (cursor[0] == '0' && (cursor[1] == 'x' || cursor[1] == 'X') &&
                isxdigit(cursor[2])) {
                base = 16;
                token->hexadecimal = 1;
                cursor += 2;
            }
            while ((base == 10 && isdigit(*cursor)) ||
                   (base == 16 && isxdigit(*cursor))) {
                unsigned digit;
                if (isdigit(*cursor)) digit = (unsigned)(*cursor - '0');
                else digit = (unsigned)(tolower(*cursor) - 'a' + 10);
                if (value > (ULLONG_MAX - digit) / (unsigned)base)
                    overflow = 1;
                else
                    value = value * (unsigned)base + digit;
                ++cursor;
            }
            token->number = value;
            token->malformed = overflow ||
                (base == 10 &&
                 ((number_begin > begin && number_begin[-1] == '.' &&
                   number_begin - begin >= 2 && isdigit(number_begin[-2])) ||
                  (*cursor == '.' && isdigit(cursor[1]))));
            continue;
        }
        ++cursor;
    }
    *count_out = count;
    return count == 0 ? -1 : 0;
}

static int word_is(const Lexeme *tokens, size_t count, size_t index,
                   const char *word) {
    return index < count && tokens[index].kind == LEXEME_WORD &&
           strcmp(tokens[index].word, word) == 0;
}

static int byte_noun(const Lexeme *tokens, size_t count, size_t index) {
    return word_is(tokens, count, index, "byte") ||
           word_is(tokens, count, index, "bytes") ||
           word_is(tokens, count, index, "octet") ||
           word_is(tokens, count, index, "octets");
}

static int minute_noun(const Lexeme *tokens, size_t count, size_t index) {
    return word_is(tokens, count, index, "minute") ||
           word_is(tokens, count, index, "minutes") ||
           word_is(tokens, count, index, "min");
}

static int numeric_argument_score(const Lexeme *tokens, size_t count,
                                  size_t index, CnetCompeteIntent intent) {
    if (intent == CNET_INTENT_MINUTES) {
        if ((index > 0 && minute_noun(tokens, count, index - 1)) ||
            minute_noun(tokens, count, index + 1))
            return 5;
        if ((index > 1 && minute_noun(tokens, count, index - 2)) ||
            minute_noun(tokens, count, index + 2))
            return 4;
        return 0;
    }
    if ((index > 0 && byte_noun(tokens, count, index - 1)) ||
        byte_noun(tokens, count, index + 1) ||
        (index > 0 && (word_is(tokens, count, index - 1, "input") ||
                       word_is(tokens, count, index - 1, "value"))))
        return 5;
    if ((index > 1 &&
         (byte_noun(tokens, count, index - 2) ||
          word_is(tokens, count, index - 2, "input") ||
          word_is(tokens, count, index - 2, "value"))) ||
        byte_noun(tokens, count, index + 2) ||
        word_is(tokens, count, index + 2, "input") ||
        word_is(tokens, count, index + 2, "value"))
        return 4;
    return 0;
}

static int number_is_constant(const Lexeme *tokens, size_t count,
                              size_t index) {
    static const char *const markers[] = {
        "crc", "bit", "poly", "polynomial", "init", "xorout", "mod",
        "modulo", "add", "plus", "multiply", "times", "double", "stage",
        "hop", "version"
    };
    size_t marker;
    for (marker = 0; marker < sizeof markers / sizeof markers[0]; ++marker) {
        if ((index > 0 && word_is(tokens, count, index - 1, markers[marker])) ||
            word_is(tokens, count, index + 1, markers[marker]))
            return 1;
    }
    return 0;
}

static int parse_numeric_argument(const char *prompt,
                                  CnetCompeteIntent intent,
                                  unsigned *value_out) {
    Lexeme tokens[RUNTIME_LEXEMES_MAX];
    size_t count = 0, index, selected = 0, selected_count = 0;
    int best_score = 0;
    if (lexemes_scan(prompt, tokens, &count) != 0 || value_out == NULL)
        return -1;
    for (index = 0; index < count; ++index) {
        int score;
        if (tokens[index].kind != LEXEME_NUMBER) continue;
        score = numeric_argument_score(tokens, count, index, intent);
        if (score > best_score) {
            best_score = score;
            selected = index;
            selected_count = 1;
        } else if (score != 0 && score == best_score) {
            ++selected_count;
        }
    }
    if (best_score == 0) {
        selected_count = 0;
        for (index = 0; index < count; ++index) {
            if (tokens[index].kind != LEXEME_NUMBER ||
                number_is_constant(tokens, count, index))
                continue;
            if (tokens[index].hexadecimal || tokens[index].negative ||
                tokens[index].malformed || tokens[index].number > 255u)
                return -1;
            selected = index;
            ++selected_count;
        }
    }
    if (selected_count != 1 || tokens[selected].negative ||
        tokens[selected].malformed || tokens[selected].number > 255u)
        return -1;
    *value_out = (unsigned)tokens[selected].number;
    return 0;
}

static int flag_index(const Lexeme *token) {
    if (token == NULL || token->kind != LEXEME_WORD) return -1;
    if (strcmp(token->word, "admin") == 0) return 0;
    if (strcmp(token->word, "owner") == 0) return 1;
    if (strcmp(token->word, "mfa") == 0) return 2;
    if (strcmp(token->word, "suspended") == 0) return 3;
    return -1;
}

static int boolean_value(const Lexeme *token, int *value) {
    if (token->kind == LEXEME_WORD) {
        if (strcmp(token->word, "true") == 0 ||
            strcmp(token->word, "yes") == 0) {
            *value = 1;
            return 0;
        }
        if (strcmp(token->word, "false") == 0 ||
            strcmp(token->word, "no") == 0) {
            *value = 0;
            return 0;
        }
    } else if (!token->negative && !token->hexadecimal && !token->malformed &&
               token->number <= 1u) {
        *value = (int)token->number;
        return 0;
    }
    return -1;
}

static int parse_policy_argument(const char *prompt, unsigned *value_out) {
    Lexeme tokens[RUNTIME_LEXEMES_MAX];
    int values[4] = {0}, seen[4] = {0};
    size_t count = 0, index;
    unsigned encoded = 0;
    if (lexemes_scan(prompt, tokens, &count) != 0 || value_out == NULL)
        return -1;
    for (index = 0; index < count; ++index) {
        int flag = flag_index(&tokens[index]);
        size_t look, limit;
        int found = 0, parsed = 0;
        if (flag < 0) continue;
        if (seen[flag]) return -1;
        limit = index + 4u < count ? index + 4u : count;
        for (look = index + 1u; look < limit; ++look) {
            if (flag_index(&tokens[look]) >= 0) break;
            if (word_is(tokens, count, look, "not")) return -1;
            if (boolean_value(&tokens[look], &parsed) == 0) {
                if (found) return -1;
                found = 1;
                values[flag] = parsed;
            }
        }
        if (!found) return -1;
        seen[flag] = 1;
    }
    for (index = 0; index < 4; ++index) {
        if (!seen[index]) return -1;
        if (values[index]) encoded |= 1u << index;
    }
    *value_out = encoded;
    return 0;
}

int cnet_compete_runtime_load(const char *model_path,
                              const char *metadata_path,
                              const char *capsule_root,
                              CnetCompeteRuntime **runtime_out,
                              CnetCompeteRuntimeReport *report) {
    CnetCompeteRuntime *runtime;
    CnetCompeteIntentReport intent_report;
    CnetCompeteRuntimeReport local;
    size_t skipped = 0;
    int unit;
    if (model_path == NULL || metadata_path == NULL || capsule_root == NULL ||
        runtime_out == NULL)
        return -1;
    *runtime_out = NULL;
    memset(&intent_report, 0, sizeof intent_report);
    memset(&local, 0, sizeof local);
    runtime = (CnetCompeteRuntime *)calloc(1, sizeof *runtime);
    if (runtime == NULL) return -1;
    cnb_init(&runtime->base);
    runtime->base_initialized = 1;
    hybrid_ai_init(&runtime->coverage);
    runtime->coverage_initialized = 1;
    registry_init(&runtime->registry);
    runtime->registry_initialized = 1;
    if (cnet_compete_intent_load(model_path, metadata_path, &runtime->intent,
                                 &intent_report) != 0)
        goto fail;
    for (unit = 0; unit < CNET_COMPETE_UNIT_COUNT; ++unit) {
        CnetCapsuleReport capsule;
        const char *name = cnet_compete_unit_name((CnetCompeteUnit)unit);
        char path[RUNTIME_PATH_MAX];
        memset(&capsule, 0, sizeof capsule);
        if (name == NULL || join_path(path, capsule_root, name) != 0 ||
            cnet_capsule_import(&runtime->base, &runtime->coverage, path,
                                &capsule) != 0 ||
            strcmp(capsule.unit, name) != 0 ||
            strcmp(capsule.scope, "exhaustive") != 0 ||
            capsule.coverage_rows != unit_domain((CnetCompeteUnit)unit) ||
            capsule.payload_bytes == 0)
            goto fail;
        ++local.imported_units;
        local.certified_rows += capsule.coverage_rows;
        local.capsule_payload_bytes += capsule.payload_bytes;
    }
    if (runtime->base.unit_count != CNET_COMPETE_UNIT_COUNT ||
        runtime->coverage.coverage_count != CNET_COMPETE_UNIT_COUNT ||
        cnb_has_unit(&runtime->base, "compose3_mod256") ||
        cnb_load_registry(&runtime->base, &runtime->registry, &skipped) != 0 ||
        skipped != 0 || runtime->registry.count != CNET_COMPETE_UNIT_COUNT)
        goto fail;
    runtime->registry.require_certified = 1;
    runtime->registry.lifecycle_enabled = 1;
    runtime->composition_source.type = byte_port("byte_raw");
    if (runtime->composition_source.type.field_width *
            runtime->composition_source.type.field_count != 8)
        goto fail;
    runtime->composition_source.values = runtime->composition_input;
    if (dag_plan(&runtime->registry, &runtime->composition_source, 1,
                 byte_port("byte_final"), &runtime->composition) != 0)
        goto fail;
    runtime->composition_initialized = 1;
    if (!plan_is_compose3(&runtime->composition,
                          &local.composition_members))
        goto fail;
    runtime->composition.strict = 1;
    local.base_parameters = intent_report.parameters;
    local.base_artifact_bytes = intent_report.artifact_bytes;
    local.intent_threshold = intent_report.threshold;
    if (report != NULL) *report = local;
    *runtime_out = runtime;
    return 0;
fail:
    cnet_compete_runtime_free(runtime);
    return -1;
}

void cnet_compete_runtime_free(CnetCompeteRuntime *runtime) {
    if (runtime == NULL) return;
    if (runtime->composition_initialized) dag_free(&runtime->composition);
    if (runtime->registry_initialized) registry_free(&runtime->registry);
    if (runtime->coverage_initialized) hybrid_ai_free(&runtime->coverage);
    if (runtime->base_initialized) cnb_free(&runtime->base);
    cnet_compete_intent_free(runtime->intent);
    free(runtime);
}

int cnet_compete_runtime_execute(CnetCompeteRuntime *runtime,
                                 const char *prompt,
                                 CnetCompeteResult *result) {
    CnetCompeteIntent intent = CNET_INTENT_ABSTAIN;
    double confidence = 0.0;
    unsigned input = 0, output = 0;
    int classified;
    if (runtime == NULL || prompt == NULL || result == NULL) return -1;
    memset(result, 0, sizeof *result);
    result->intent = CNET_INTENT_ABSTAIN;
    classified = cnet_compete_intent_classify(runtime->intent, prompt, &intent,
                                               &confidence);
    if (classified == 1) return 0;
    if (classified != 0 || intent < CNET_INTENT_INCREMENT ||
        intent >= CNET_INTENT_ABSTAIN || !isfinite(confidence))
        return -1;
    if (intent == CNET_INTENT_POLICY) {
        if (parse_policy_argument(prompt, &input) != 0) return 0;
    } else if (parse_numeric_argument(prompt, intent, &input) != 0) {
        return 0;
    }
    if (intent == CNET_INTENT_COMPOSE3) {
        double result_bits[8] = {0};
        CompositionGuard guard;
        int execution;
        memset(&guard, 0, sizeof guard);
        guard.coverage = &runtime->coverage;
        encode_msb(runtime->composition_input, input);
        runtime->composition.guard.allow = composition_guard;
        runtime->composition.guard.ctx = &guard;
        execution = dag_execute(&runtime->composition,
                                &runtime->composition_source, 1,
                                result_bits, sizeof result_bits /
                                             sizeof result_bits[0]);
        runtime->composition.guard.ctx = NULL;
        if (execution != 0 || guard.checks != 3) return -1;
        output = decode_msb(result_bits);
        result->composition_guard_checks = guard.checks;
    } else {
        CnetCompeteUnit unit;
        switch (intent) {
            case CNET_INTENT_INCREMENT:
                unit = CNET_COMPETE_UNIT_INCREMENT;
                break;
            case CNET_INTENT_MINUTES:
                unit = CNET_COMPETE_UNIT_MINUTES;
                break;
            case CNET_INTENT_CRC8:
                unit = CNET_COMPETE_UNIT_CRC8;
                break;
            case CNET_INTENT_POLICY:
                unit = CNET_COMPETE_UNIT_POLICY;
                break;
            default:
                return -1;
        }
        if (cnet_compete_capsule_eval(&runtime->base, &runtime->coverage,
                                      unit, input, &output) != 0)
            return -1;
    }
    result->answered = 1;
    result->intent = intent;
    result->value = output;
    result->confidence = confidence;
    return 0;
}

int cnet_compete_result_json(const CnetCompeteResult *result,
                             char *output, size_t capacity) {
    int written;
    const char *intent;
    if (result == NULL || output == NULL || capacity == 0) return -1;
    if (!result->answered) {
        written = snprintf(output, capacity, "{\"status\":\"abstain\"}");
    } else {
        intent = cnet_compete_intent_name(result->intent);
        if (intent == NULL || result->intent == CNET_INTENT_ABSTAIN ||
            !isfinite(result->confidence))
            return -1;
        written = snprintf(output, capacity,
                           "{\"status\":\"answer\",\"intent\":\"%s\","
                           "\"value\":%u}", intent, result->value);
    }
    return written < 0 || (size_t)written >= capacity ? -1 : 0;
}
