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
#include <sys/stat.h>

#define RUNTIME_PATH_MAX 1024
#define RUNTIME_LEXEMES_MAX 96
#define RUNTIME_WORD_MAX 32

typedef enum { LEXEME_WORD, LEXEME_NUMBER } LexemeKind;

typedef struct {
    LexemeKind kind;
    char word[RUNTIME_WORD_MAX];
    unsigned long long number;
    size_t source_begin;
    size_t source_end;
    int negative;
    int hexadecimal;
    int malformed;
} Lexeme;

typedef struct {
    const HybridAi *coverage;
    size_t checks;
    CnetCompeteRefusal refusal;
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
    if (guard == NULL) return -1;
    if (guard->coverage == NULL || unit == NULL || network == NULL ||
        input == NULL || network->input_port_count != 1 ||
        network->output_port_count != 1 ||
        guard->checks >= sizeof expected / sizeof expected[0] ||
        strcmp(unit, expected[guard->checks]) != 0) {
        guard->refusal = CNET_COMPETE_REFUSAL_EXECUTION;
        return -1;
    }
    if (!hybrid_coverage_admits_exact(guard->coverage, unit,
                                      network->input_ports[0],
                                      network->output_ports[0], input,
                                      input_length)) {
        guard->refusal = CNET_COMPETE_REFUSAL_COVERAGE;
        return -1;
    }
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
            token->source_begin = (size_t)(cursor - begin);
            while (isalpha(*cursor) || *cursor == '\'') {
                unsigned char character = *cursor++;
                if (length + 1u >= sizeof token->word) return -1;
                token->word[length++] = (char)tolower(character);
            }
            token->word[length] = '\0';
            token->source_end = (size_t)(cursor - begin);
            continue;
        }
        if (isdigit(*cursor)) {
            const unsigned char *number_begin = cursor;
            const unsigned char *sign = cursor;
            unsigned long long value = 0;
            int base = 10, overflow = 0;
            if (count >= RUNTIME_LEXEMES_MAX) return -1;
            token = &output[count++];
            memset(token, 0, sizeof *token);
            token->kind = LEXEME_NUMBER;
            token->source_begin = (size_t)(cursor - begin);
            while (sign > begin && isspace(sign[-1])) --sign;
            if (sign > begin && sign[-1] == '-') {
                const unsigned char *dash = sign - 1u;
                token->negative = sign != number_begin || dash == begin ||
                    (!isalnum(dash[-1]) && dash[-1] != '_');
            }
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
            token->source_end = (size_t)(cursor - begin);
            token->malformed = overflow ||
                (base == 10 &&
                 ((number_begin > begin && number_begin[-1] == '.') ||
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

static int second_noun(const Lexeme *tokens, size_t count, size_t index) {
    return word_is(tokens, count, index, "second") ||
           word_is(tokens, count, index, "seconds") ||
           word_is(tokens, count, index, "sec");
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

static int number_adjacent_word(const Lexeme *tokens, size_t count,
                                size_t index, const char *word) {
    return (index > 0 && word_is(tokens, count, index - 1u, word)) ||
           word_is(tokens, count, index + 1u, word) ||
           (index > 1 && word_is(tokens, count, index - 2u, word)) ||
           word_is(tokens, count, index + 2u, word);
}

static int number_is_supported_constant(const char *prompt,
                                        const Lexeme *tokens, size_t count,
                                        size_t index,
                                        CnetCompeteIntent intent) {
    const Lexeme *number = &tokens[index];
    if (number->kind != LEXEME_NUMBER || number->negative ||
        number->malformed)
        return 0;
    if (number->number == 8u &&
        number_adjacent_word(tokens, count, index, "uint"))
        return 1;
    if (number->number == 8u &&
        word_is(tokens, count, index + 1u, "bit") &&
        ((index > 0u && word_is(tokens, count, index - 1u, "unsigned")) ||
         byte_noun(tokens, count, index + 2u) ||
         word_is(tokens, count, index + 2u, "value") ||
         word_is(tokens, count, index + 2u, "register")))
        return 1;
    if (number->number == 256u &&
        (number_adjacent_word(tokens, count, index, "mod") ||
         number_adjacent_word(tokens, count, index, "modulo")))
        return 1;
    if (intent == CNET_INTENT_MINUTES && number->number == 60u &&
        (number_adjacent_word(tokens, count, index, "times") ||
         number_adjacent_word(tokens, count, index, "multiply") ||
         number_adjacent_word(tokens, count, index, "per")))
        return 1;
    if (intent == CNET_INTENT_CRC8) {
        if (number->number == 8u &&
            (number_adjacent_word(tokens, count, index, "crc") ||
             number_adjacent_word(tokens, count, index, "width")))
            return 1;
        if (number->number == 7u &&
            (number_adjacent_word(tokens, count, index, "poly") ||
             number_adjacent_word(tokens, count, index, "polynomial")))
            return 1;
        if (number->number == 0u &&
            (number_adjacent_word(tokens, count, index, "init") ||
             number_adjacent_word(tokens, count, index, "initial") ||
             number_adjacent_word(tokens, count, index, "xor") ||
             number_adjacent_word(tokens, count, index, "xorout")))
            return 1;
    }
    if (intent == CNET_INTENT_COMPOSE3) {
        if (number->number == 1u &&
            (number_adjacent_word(tokens, count, index, "add") ||
             number_adjacent_word(tokens, count, index, "plus") ||
             (prompt != NULL && strstr(prompt, "+1") != NULL)))
            return 1;
        if (number->number == 2u &&
            (number_adjacent_word(tokens, count, index, "multiply") ||
             number_adjacent_word(tokens, count, index, "times") ||
             number_adjacent_word(tokens, count, index, "scale") ||
             (prompt != NULL && strstr(prompt, "*2") != NULL)))
            return 1;
        if (number->number == 3u &&
            (number_adjacent_word(tokens, count, index, "add") ||
             number_adjacent_word(tokens, count, index, "plus") ||
             number_adjacent_word(tokens, count, index, "offset") ||
             number_adjacent_word(tokens, count, index, "compose") ||
             number_adjacent_word(tokens, count, index, "stage") ||
             number_adjacent_word(tokens, count, index, "hop") ||
             (prompt != NULL && strstr(prompt, "+3") != NULL)))
            return 1;
    }
    return 0;
}

static int number_word(const Lexeme *tokens, size_t count, size_t index);
static int word_in_list(const Lexeme *tokens, size_t count, size_t index,
                        const char *const *words, size_t word_count);
static int number_word_context_supported(const Lexeme *tokens, size_t count,
                                         size_t index,
                                         CnetCompeteIntent intent);

static int numeric_assertion_supported(const Lexeme *tokens, size_t count,
                                       CnetCompeteIntent intent) {
    static const char *const response_words[] = {
        "return", "give", "report", "state", "provide", "output",
        "produce", "answer", "tell"
    };
    static const char *const glue_words[] = {
        "the", "its", "that", "result", "output", "value", "answer",
        "exactly", "bit", "step", "width", "poly", "polynomial",
        "register", "init", "initial", "xor", "xorout", "final", "as",
        "is", "equal", "equals", "of", "to", "at", "with", "on", "for"
    };
    static const char *const output_nouns[] = {
        "successor", "second", "seconds", "checksum", "code", "total",
        "count", "duration", "outcome", "decision", "permission",
        "result", "output", "value", "answer"
    };
    static const char *const assertion_links[] = {
        "as", "is", "equal", "equals", "the", "its", "that", "exactly",
        "bit"
    };
    size_t index;
    for (index = 0; index < count; ++index) {
        size_t value;
        if (!word_in_list(tokens, count, index, response_words,
                          sizeof response_words / sizeof response_words[0]))
            continue;
        value = index + 1u;
        while (value < count &&
               word_in_list(tokens, count, value, glue_words,
                            sizeof glue_words / sizeof glue_words[0]))
            ++value;
        if (value < count &&
            (tokens[value].kind == LEXEME_NUMBER ||
             number_word(tokens, count, value))) {
            size_t object, limit = value + 6u < count ? value + 6u : count;
            int typed_crc_output = intent == CNET_INTENT_CRC8 &&
                word_is(tokens, count, value, "one") &&
                byte_noun(tokens, count, value + 1u);
            for (object = value + 2u; typed_crc_output && object < limit;
                 ++object)
                if (word_is(tokens, count, object, "atm") ||
                    word_is(tokens, count, object, "check") ||
                    word_is(tokens, count, object, "checksum") ||
                    word_is(tokens, count, object, "code"))
                    break;
            if (typed_crc_output && object < limit) continue;
            return 0;
        }
    }
    for (index = 0; index < count; ++index) {
        size_t asserted;
        if (!word_in_list(tokens, count, index, output_nouns,
                          sizeof output_nouns / sizeof output_nouns[0]))
            continue;
        if (word_is(tokens, count, index, "duration") &&
            index + 2u < count &&
            tokens[index + 1u].kind == LEXEME_NUMBER &&
            (word_is(tokens, count, index + 2u, "minute") ||
             word_is(tokens, count, index + 2u, "minutes")))
            continue;
        if ((word_is(tokens, count, index, "value") ||
             word_is(tokens, count, index, "duration") ||
             word_is(tokens, count, index, "count")) &&
            index > 0u &&
            (word_is(tokens, count, index - 1u, "input") ||
             word_is(tokens, count, index - 1u, "byte") ||
             word_is(tokens, count, index - 1u, "octet") ||
             word_is(tokens, count, index - 1u, "operand") ||
             word_is(tokens, count, index - 1u, "datum") ||
             word_is(tokens, count, index - 1u, "register") ||
             word_is(tokens, count, index - 1u, "source") ||
             word_is(tokens, count, index - 1u, "bit") ||
             word_is(tokens, count, index - 1u, "integer") ||
             word_is(tokens, count, index - 1u, "minute") ||
             word_is(tokens, count, index - 1u, "minutes")))
            continue;
        if (word_is(tokens, count, index, "value") && index > 1u &&
            tokens[index - 1u].kind == LEXEME_NUMBER &&
            !tokens[index - 1u].negative &&
            !tokens[index - 1u].malformed &&
            tokens[index - 1u].number == 8u &&
            word_is(tokens, count, index - 2u, "uint"))
            continue;
        asserted = index + 1u;
        while (asserted < count &&
               word_in_list(tokens, count, asserted, assertion_links,
                            sizeof assertion_links /
                                sizeof assertion_links[0]))
            ++asserted;
        if (asserted < count &&
            (tokens[asserted].kind == LEXEME_NUMBER ||
             number_word(tokens, count, asserted)))
            return 0;
    }
    return 1;
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
        if (intent == CNET_INTENT_MINUTES) {
            for (index = 0; index < count; ++index) {
                if (tokens[index].kind != LEXEME_NUMBER) continue;
                if ((index > 0 && second_noun(tokens, count, index - 1)) ||
                    second_noun(tokens, count, index + 1) ||
                    (index > 1 &&
                     second_noun(tokens, count, index - 2)) ||
                    second_noun(tokens, count, index + 2))
                    return -1;
            }
        }
        selected_count = 0;
            for (index = 0; index < count; ++index) {
                if (tokens[index].kind != LEXEME_NUMBER ||
                    number_is_supported_constant(prompt, tokens, count,
                                                 index, intent))
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
    for (index = 0; index < count; ++index) {
        if (index == selected || tokens[index].kind != LEXEME_NUMBER ||
            number_is_supported_constant(prompt, tokens, count, index,
                                         intent))
            continue;
        return -1;
    }
    for (index = 0; index < count; ++index)
        if (tokens[index].kind == LEXEME_WORD &&
            number_word(tokens, count, index) &&
            !number_word_context_supported(tokens, count, index, intent))
            return -1;
    if (!numeric_assertion_supported(tokens, count, intent))
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
    int boolean_used[RUNTIME_LEXEMES_MAX] = {0};
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
                boolean_used[look] = 1;
            }
        }
        if (!found) return -1;
        seen[flag] = 1;
    }
    for (index = 0; index < 4; ++index) {
        if (!seen[index]) return -1;
        if (values[index]) encoded |= 1u << index;
    }
    for (index = 0; index < count; ++index) {
        int ignored;
        if (boolean_value(&tokens[index], &ignored) != 0 ||
            boolean_used[index])
            continue;
        if (tokens[index].kind == LEXEME_NUMBER &&
            tokens[index].number == 1u && index > 0 &&
            (word_is(tokens, count, index - 1u, "policy") ||
             word_is(tokens, count, index - 1u, "rule") ||
             word_is(tokens, count, index - 1u, "version") ||
             word_is(tokens, count, index - 1u, "revision") ||
             word_is(tokens, count, index - 1u, "v")))
            continue;
        return -1;
    }
    *value_out = encoded;
    return 0;
}

static int contains_ascii_casefold(const char *text, const char *needle) {
    size_t needle_length, offset;
    if (text == NULL || needle == NULL || needle[0] == '\0') return 0;
    needle_length = strlen(needle);
    for (; *text != '\0'; ++text) {
        for (offset = 0; offset < needle_length; ++offset) {
            unsigned char left = (unsigned char)text[offset];
            unsigned char right = (unsigned char)needle[offset];
            if (left == '\0' || left >= 128u || right >= 128u) break;
            if (left >= 'A' && left <= 'Z')
                left = (unsigned char)(left - 'A' + 'a');
            if (right >= 'A' && right <= 'Z')
                right = (unsigned char)(right - 'A' + 'a');
            if (left != right) break;
        }
        if (offset == needle_length) return 1;
    }
    return 0;
}

static int quoted_numeric_literal(const char *text) {
    const unsigned char *cursor = (const unsigned char *)text;
    if (text == NULL) return 1;
    while (*cursor != '\0') {
        unsigned char quote = *cursor;
        const unsigned char *end;
        int numeric = 0;
        if (quote != '"' && quote != '`' && quote != '\'') {
            ++cursor;
            continue;
        }
        if (quote == '\'' && cursor > (const unsigned char *)text &&
            isalpha(cursor[-1]) && isalpha(cursor[1])) {
            ++cursor;
            continue;
        }
        end = cursor + 1u;
        while (*end != '\0' && *end != quote) {
            if (isdigit(*end)) numeric = 1;
            ++end;
        }
        if (*end == '\0') return 1;
        if (numeric) return 1;
        cursor = end + 1u;
    }
    return 0;
}

static int has_word(const Lexeme *tokens, size_t count, const char *word) {
    size_t index;
    for (index = 0; index < count; ++index)
        if (word_is(tokens, count, index, word)) return 1;
    return 0;
}

static int has_any_word(const Lexeme *tokens, size_t count,
                        const char *const *words, size_t word_count) {
    size_t index;
    for (index = 0; index < word_count; ++index)
        if (has_word(tokens, count, words[index])) return 1;
    return 0;
}

static int word_in_list(const Lexeme *tokens, size_t count, size_t index,
                        const char *const *words, size_t word_count) {
    size_t word;
    for (word = 0; word < word_count; ++word)
        if (word_is(tokens, count, index, words[word])) return 1;
    return 0;
}

static int contract_scaffolding_word(const Lexeme *tokens, size_t count,
                                     size_t index) {
    static const char *const words[] = {
        "a", "an", "and", "answer", "apply", "are", "as", "ask", "at",
        "be", "been", "being", "by", "calculate", "can", "compute",
        "consider", "could", "datum", "derive", "determine", "do", "does",
        "equal", "equals", "equivalent", "evaluate", "exact", "exactly",
        "find", "for", "from", "give", "given", "how", "if", "immediate",
        "in", "input", "into", "is", "it", "its", "just", "kindly", "let",
        "many", "map", "may", "might", "need", "needed", "now", "of", "on",
        "operand", "output", "please", "process", "provide", "registered",
        "report", "request", "required", "result", "return", "should",
        "show", "simply", "single", "source", "state", "stored", "suppose",
        "target", "tell", "that", "the", "this", "through", "to", "total",
        "under", "using", "value", "want", "what", "when", "whether",
        "whole", "with", "would"
    };
    return word_in_list(tokens, count, index, words,
                        sizeof words / sizeof words[0]);
}

/* Certified requests use a closed positive vocabulary.  The learned router is
   deliberately not treated as permission to accept an otherwise unknown verb
   or noun: an unrecognized word can denote an external side effect even when
   it is not present in a finite denylist. */
static int contract_vocabulary_supported(const Lexeme *tokens, size_t count,
                                         CnetCompeteIntent intent) {
    static const char *const increment[] = {
        "a", "add", "advance", "after", "ahead", "and", "arithmetic",
        "at", "bit", "by", "byte", "contract", "cycle", "cyclic",
        "datum", "eight", "exactly", "fifty", "find", "following", "for",
        "forward", "holding", "hundred", "immediate", "in", "increment",
        "input", "it", "map", "mod", "modulo", "move", "octet", "of",
        "one", "operand", "output", "overflow", "position", "raise",
        "register", "registered", "result", "return", "single", "six",
        "step", "stored", "successor", "take", "the", "through", "two",
        "uint", "under", "unity", "unsigned", "value", "with", "wrap",
        "wraparound", "wrapped", "follows", "next", "representable",
        "plus", "succeeding", "incremented", "incrementing", "uint8",
        "after", "integer", "number", "numeric", "cyclic"
    };
    static const char *const minutes[] = {
        "a", "apply", "by", "contain", "contains", "convert", "count",
        "does", "duration", "during", "elapsed", "exact", "express",
        "from", "give", "how", "in", "input", "integer", "into", "is",
        "factor", "fixed", "interval", "its", "many", "map", "mapping",
        "measure",
        "measuring", "min", "minute", "minutes", "obtain", "of", "onto",
        "multiply", "per", "quantity", "registered", "report",
        "same", "scale", "sec", "second", "seconds", "sixty", "source",
        "span", "target", "that", "the", "time", "to", "translate", "units",
        "using", "whole", "equals", "equal", "please", "are", "is"
    };
    static const char *const crc[] = {
        "and", "apply", "as", "at", "atm", "bit", "both", "byte",
        "calculate", "check", "checksum", "code", "compute", "crc",
        "cyclic",
        "datum", "derive", "eight", "ends", "false", "final", "for",
        "init", "initial", "initialized", "input", "its", "non", "octet", "of",
        "on", "one", "polynomial", "process", "produce", "redundancy",
        "refin", "reflected", "refout", "register", "return", "rule",
        "seven", "single", "the", "to", "under", "unsigned", "use", "using", "v",
        "width", "with", "x", "xor", "xorout", "zero", "please", "atm"
    };
    static const char *const policy[] = {
        "access", "admin", "adjudicate", "allow", "apply",
        "authorization", "boolean", "certified", "decide", "decision",
        "deny", "determine", "entry", "evaluate", "false", "finally",
        "flags", "for", "four", "from", "given", "is", "its", "mfa",
        "next", "one", "or", "outcome", "owner", "permission", "policy",
        "reads", "receives", "resolve", "return", "rule", "security",
        "state", "suspended", "switches", "the", "then", "these", "to",
        "true", "tuple", "under", "uses", "v", "where", "granted",
        "permitted", "whether", "please", "yes", "no"
    };
    static const char *const compose[] = {
        "a", "add", "addition", "advance", "advances", "an", "and",
        "applies", "apply", "as", "at", "b", "begins", "begin", "by",
        "byte", "c", "chain", "comes", "compose", "composition",
        "consecutive", "consumes", "compute", "computes", "datum",
        "double", "doubles", "doubling", "finally", "first", "for",
        "from", "hop", "hops", "in", "increase", "increment", "input",
        "intermediate", "into", "it", "its", "large", "last", "make",
        "map", "mod", "modulo", "multiplication", "multiply", "next",
        "octet", "offset", "on", "one", "order", "ordered", "pass",
        "performs", "pipeline", "plus", "produce", "produces", "raise",
        "raises", "registered", "result", "route", "scale", "second",
        "sequence", "stage", "starting", "successor", "take", "that",
        "the", "then", "these", "third", "this", "three", "through", "times",
        "to", "transform", "twice", "two", "uint", "unity", "unsigned",
        "use", "value", "with", "please", "followed", "uint8", "then"
    };
    const char *const *allowed = NULL;
    size_t allowed_count = 0, index;
    switch (intent) {
        case CNET_INTENT_INCREMENT:
            allowed = increment;
            allowed_count = sizeof increment / sizeof increment[0];
            break;
        case CNET_INTENT_MINUTES:
            allowed = minutes;
            allowed_count = sizeof minutes / sizeof minutes[0];
            break;
        case CNET_INTENT_CRC8:
            allowed = crc;
            allowed_count = sizeof crc / sizeof crc[0];
            break;
        case CNET_INTENT_POLICY:
            allowed = policy;
            allowed_count = sizeof policy / sizeof policy[0];
            break;
        case CNET_INTENT_COMPOSE3:
            allowed = compose;
            allowed_count = sizeof compose / sizeof compose[0];
            break;
        default:
            return 0;
    }
    for (index = 0; index < count; ++index) {
        if (tokens[index].kind == LEXEME_NUMBER) continue;
        if (!contract_scaffolding_word(tokens, count, index) &&
            !word_in_list(tokens, count, index, allowed, allowed_count))
            return 0;
    }
    return 1;
}

static int continuation_clauses_supported(const Lexeme *tokens, size_t count,
                                          CnetCompeteIntent intent) {
    static const char *const coordinators[] = {
        "and", "then", "also", "afterward", "afterwards", "finally"
    };
    static const char *const response_words[] = {
        "return", "give", "report", "state", "provide", "output",
        "produce", "answer", "tell", "what"
    };
    static const char *const increment_words[] = {
        "wrap", "wrapping", "wraparound"
    };
    static const char *const crc_words[] = {
        "final", "xor", "xorout", "initial", "init", "polynomial",
        "poly", "width", "non", "no", "reflected", "refin", "refout"
    };
    static const char *const policy_words[] = {
        "admin", "owner", "mfa", "suspended", "allow", "deny",
        "decision", "outcome", "permission"
    };
    size_t index;
    for (index = 0; index < count; ++index) {
        size_t next;
        if (!word_in_list(tokens, count, index, coordinators,
                          sizeof coordinators / sizeof coordinators[0]))
            continue;
        next = index + 1u;
        while (next < count &&
               (word_is(tokens, count, next, "the") ||
                word_is(tokens, count, next, "its") ||
                word_is(tokens, count, next, "that")))
            ++next;
        if (next >= count || tokens[next].kind != LEXEME_WORD) return 0;
        if (word_in_list(tokens, count, next, response_words,
                         sizeof response_words / sizeof response_words[0]))
            continue;
        if (intent == CNET_INTENT_INCREMENT &&
            word_in_list(tokens, count, next, increment_words,
                         sizeof increment_words / sizeof increment_words[0]))
            continue;
        if (intent == CNET_INTENT_CRC8 &&
            word_in_list(tokens, count, next, crc_words,
                         sizeof crc_words / sizeof crc_words[0]))
            continue;
        if (intent == CNET_INTENT_POLICY &&
            word_in_list(tokens, count, next, policy_words,
                         sizeof policy_words / sizeof policy_words[0]))
            continue;
        return 0;
    }
    return 1;
}

static int explicit_input_value_follows(const Lexeme *tokens, size_t count,
                                        size_t index) {
    size_t look, limit = index + 5u < count ? index + 5u : count;
    for (look = index + 1u; look < limit; ++look) {
        if (tokens[look].kind == LEXEME_NUMBER ||
            word_is(tokens, count, look, "true") ||
            word_is(tokens, count, look, "false"))
            return 1;
    }
    return 0;
}

static int response_objects_supported(const Lexeme *tokens, size_t count) {
    static const char *const response_words[] = {
        "return", "give", "report", "state", "provide", "output",
        "produce", "answer", "tell"
    };
    static const char *const input_objects[] = {
        "input", "operand", "datum", "source", "register", "byte",
        "octet", "admin", "owner", "mfa", "suspended", "flags",
        "switches", "tuple"
    };
    static const char *const output_objects[] = {
        "result", "output", "answer", "successor", "second", "seconds",
        "checksum", "code", "total", "count", "duration", "outcome",
        "decision", "permission", "value"
    };
    static const char *const strong_output_objects[] = {
        "result", "output", "answer", "outcome", "decision", "permission"
    };
    static const char *const operation_objects[] = {
        "increment", "crc", "policy", "access", "composition", "compose",
        "conversion", "mapping", "successor", "checksum", "decision"
    };
    size_t index;
    for (index = 0u; index < count; ++index) {
        size_t object, limit, previous;
        int governed_output_noun = 0;
        if (!word_in_list(tokens, count, index, response_words,
                          sizeof response_words / sizeof response_words[0]))
            continue;
        if (word_in_list(tokens, count, index, output_objects,
                         sizeof output_objects / sizeof output_objects[0])) {
            for (previous = index > 7u ? index - 7u : 0u;
                 previous < index; ++previous)
                if (word_in_list(tokens, count, previous, response_words,
                                 sizeof response_words /
                                     sizeof response_words[0]))
                    governed_output_noun = 1;
            if (governed_output_noun) continue;
        }
        object = index + 1u;
        limit = index + 7u < count ? index + 7u : count;
        for (; object < limit; ++object) {
            if (word_is(tokens, count, object, "and") ||
                word_is(tokens, count, object, "then"))
                break;
            if (word_in_list(tokens, count, object, operation_objects,
                             sizeof operation_objects /
                                 sizeof operation_objects[0]) ||
                word_in_list(tokens, count, object, output_objects,
                             sizeof output_objects /
                                 sizeof output_objects[0]))
                break;
            if ((word_is(tokens, count, object, "byte") ||
                 word_is(tokens, count, object, "octet")) && object > index &&
                (word_is(tokens, count, object - 1u, "single") ||
                 word_is(tokens, count, object - 1u, "unsigned") ||
                 word_is(tokens, count, object - 1u, "one") ||
                 word_is(tokens, count, object - 1u, "eight") ||
                 word_is(tokens, count, object - 1u, "representable") ||
                 word_in_list(tokens, count, object + 1u, output_objects,
                              sizeof output_objects /
                                  sizeof output_objects[0])))
                break;
            if (word_in_list(tokens, count, object, input_objects,
                             sizeof input_objects /
                                 sizeof input_objects[0]) ||
                word_is(tokens, count, object, "stored") ||
                word_is(tokens, count, object, "holding"))
                return 0;
        }
    }
    for (index = 0u; index < count; ++index) {
        size_t look, limit;
        int safe_reference = 0, assertion = 0, active = 0;
        if (!word_in_list(tokens, count, index, output_objects,
                          sizeof output_objects / sizeof output_objects[0]))
            continue;
        if (word_is(tokens, count, index, "value") && index > 0u &&
            (word_is(tokens, count, index - 1u, "input") ||
             word_is(tokens, count, index - 1u, "byte") ||
             word_is(tokens, count, index - 1u, "octet") ||
             word_is(tokens, count, index - 1u, "operand") ||
             word_is(tokens, count, index - 1u, "source")))
            continue;
        active = word_in_list(tokens, count, index, strong_output_objects,
                              sizeof strong_output_objects /
                                  sizeof strong_output_objects[0]);
        limit = index + 6u < count ? index + 6u : count;
        for (look = index + 1u; !active && look < limit; ++look)
            if (word_is(tokens, count, look, "is") ||
                word_is(tokens, count, look, "as") ||
                word_is(tokens, count, look, "equal") ||
                word_is(tokens, count, look, "equals"))
                active = 1;
        for (look = index > 4u ? index - 4u : 0u;
             !active && look < index; ++look)
            if (word_in_list(tokens, count, look, response_words,
                             sizeof response_words /
                                 sizeof response_words[0]) ||
                word_is(tokens, count, look, "target"))
                active = 1;
        if (!active) continue;
        limit = index + 8u < count ? index + 8u : count;
        for (look = index + 1u; look < limit; ++look) {
            if (word_is(tokens, count, look, "and") ||
                word_is(tokens, count, look, "then") ||
                word_is(tokens, count, look, "when") ||
                word_is(tokens, count, look, "where") ||
                word_is(tokens, count, look, "if"))
                break;
            if (word_is(tokens, count, look, "is") ||
                word_is(tokens, count, look, "as") ||
                word_is(tokens, count, look, "equal") ||
                word_is(tokens, count, look, "equals"))
                assertion = 1;
            if (word_is(tokens, count, look, "for") ||
                word_is(tokens, count, look, "of") ||
                word_is(tokens, count, look, "from") ||
                word_is(tokens, count, look, "on") ||
                word_is(tokens, count, look, "under") ||
                word_is(tokens, count, look, "using") ||
                word_is(tokens, count, look, "with"))
                safe_reference = 1;
            if (word_is(tokens, count, look, "stored") ||
                word_is(tokens, count, look, "holding"))
                return 0;
            if (!word_in_list(tokens, count, look, input_objects,
                              sizeof input_objects /
                                  sizeof input_objects[0]))
                continue;
            if (assertion || !safe_reference ||
                !explicit_input_value_follows(tokens, count, look))
                return 0;
        }
    }
    return 1;
}

static int lexeme_is_value(const Lexeme *tokens, size_t count, size_t index,
                           unsigned value, const char *word) {
    return index < count &&
           ((tokens[index].kind == LEXEME_NUMBER &&
             !tokens[index].negative && !tokens[index].malformed &&
             tokens[index].number == value) ||
            word_is(tokens, count, index, word) ||
            (value == 1u && word_is(tokens, count, index, "unity")));
}

static int operation_value_follows(const Lexeme *tokens, size_t count,
                                   size_t index, unsigned value,
                                   const char *word) {
    size_t look, limit = index + 5u < count ? index + 5u : count;
    for (look = index + 1u; look < limit; ++look)
        if (lexeme_is_value(tokens, count, look, value, word)) return 1;
    return 0;
}

typedef struct {
    size_t first;
    size_t last;
    size_t count;
} OperationPositions;

static void add_operation(OperationPositions *positions, size_t index) {
    if (positions->count == 0) positions->first = index;
    positions->last = index;
    ++positions->count;
}

static int compose_glue_word(const Lexeme *tokens, size_t count,
                             size_t index) {
    static const char *const glue[] = {
        "then", "next", "finally", "last", "first", "second", "third",
        "and", "followed", "by", "it", "its", "that", "the", "result",
        "output", "make", "as", "large", "one", "unity", "two", "three",
        "a", "an", "applies", "b", "c", "comes", "consumes", "datum",
        "hop", "intermediate", "into", "produces", "route", "stage",
        "value"
    };
    size_t word;
    for (word = 0; word < sizeof glue / sizeof glue[0]; ++word)
        if (word_is(tokens, count, index, glue[word])) return 1;
    return 0;
}

static int compose_tail_word(const Lexeme *tokens, size_t count,
                             size_t index) {
    static const char *const tail[] = {
        "three", "by", "it", "the", "result", "output", "mod", "modulo",
        "two", "hundred", "fifty", "six", "to", "on", "for", "with",
        "under", "byte", "bytes", "octet", "octets", "input", "value",
        "datum", "operand", "uint", "bit", "register", "arithmetic",
        "wrap", "wrapping", "wraparound", "overflow", "cyclic", "exactly",
        "last", "offset", "consumes", "of", "unsigned"
    };
    size_t word;
    for (word = 0; word < sizeof tail / sizeof tail[0]; ++word)
        if (word_is(tokens, count, index, tail[word])) return 1;
    return 0;
}

static int symbolic_compose_operations_exact(const char *prompt) {
    static const struct {
        unsigned char symbol;
        unsigned value;
    } expected[] = {{'+', 1u}, {'*', 2u}, {'+', 3u}};
    const unsigned char *cursor = (const unsigned char *)prompt;
    const unsigned char *final_operation_end = NULL;
    size_t operation = 0;
    if (prompt == NULL) return 0;
    while (*cursor != '\0') {
        const unsigned char *value_begin;
        unsigned value = 0;
        if (*cursor != '+' && *cursor != '*' && *cursor != '/' &&
            *cursor != '%' && *cursor != '^') {
            ++cursor;
            continue;
        }
        if (operation >= sizeof expected / sizeof expected[0] ||
            *cursor != expected[operation].symbol)
            return 0;
        ++cursor;
        while (isspace(*cursor)) ++cursor;
        value_begin = cursor;
        while (isdigit(*cursor)) {
            if (value > (UINT_MAX - (unsigned)(*cursor - '0')) / 10u)
                return 0;
            value = value * 10u + (unsigned)(*cursor - '0');
            ++cursor;
        }
        if (cursor == value_begin || value != expected[operation].value ||
            isalpha(*cursor) || *cursor == '_')
            return 0;
        ++operation;
        if (operation == sizeof expected / sizeof expected[0])
            final_operation_end = cursor;
    }
    if (operation != sizeof expected / sizeof expected[0] ||
        final_operation_end == NULL)
        return 0;
    for (cursor = final_operation_end; *cursor != '\0'; ++cursor) {
        unsigned char character = *cursor;
        if (!isspace(character) && character != '.' && character != ',' &&
            character != ';' && character != ':' && character != '?' &&
            character != '!' && character != ')' && character != ']' &&
            character != '}')
            return 0;
    }
    return 1;
}

static int compose_operation_word(const Lexeme *tokens, size_t count,
                                  size_t index) {
    static const char *const operations[] = {
        "add", "addition", "advance", "advances", "increase", "increment",
        "raise", "raises", "successor", "double", "doubles", "doubling",
        "twice", "multiplication", "multiply", "times", "scale", "offset",
        "transform", "map", "take", "make", "performs"
    };
    return word_in_list(tokens, count, index, operations,
                        sizeof operations / sizeof operations[0]);
}

static int named_compose_request_supported(const Lexeme *tokens,
                                           size_t count) {
    static const char *const input_words[] = {
        "to", "the", "input", "unsigned", "byte", "octet", "value",
        "operand", "datum"
    };
    static const char *const coordinators[] = {"and", "then"};
    static const char *const response_words[] = {
        "return", "give", "report", "state", "provide", "output",
        "produce", "answer", "tell"
    };
    static const char *const response_objects[] = {
        "the", "its", "that", "result", "output", "value", "answer"
    };
    size_t index = 0;

    if (word_is(tokens, count, 0u, "map") &&
        word_is(tokens, count, 1u, "input") &&
        word_is(tokens, count, 2u, "value") && count == 9u &&
        tokens[3].kind == LEXEME_NUMBER && !tokens[3].negative &&
        !tokens[3].malformed && !tokens[3].hexadecimal &&
        tokens[3].number <= 255u && word_is(tokens, count, 4u, "with") &&
        word_is(tokens, count, 5u, "compose") &&
        tokens[6].kind == LEXEME_NUMBER && !tokens[6].negative &&
        !tokens[6].malformed && tokens[6].number == 3u &&
        word_is(tokens, count, 7u, "mod") &&
        tokens[8].kind == LEXEME_NUMBER && !tokens[8].negative &&
        !tokens[8].malformed && tokens[8].number == 256u)
        return 1;
    /* The registered-name surface is intentionally a small formal grammar:
       "apply compose3_mod256 [to] [the] [input] [byte] N" followed only by
       an optional response request.  Natural-language operation descriptions
       are handled by the independently checked three-operation branch. */
    if (!word_is(tokens, count, index++, "apply") ||
        !word_is(tokens, count, index++, "compose") || index >= count ||
        tokens[index].kind != LEXEME_NUMBER || tokens[index].negative ||
        tokens[index].malformed || tokens[index++].number != 3u ||
        !word_is(tokens, count, index++, "mod") || index >= count ||
        tokens[index].kind != LEXEME_NUMBER || tokens[index].negative ||
        tokens[index].malformed || tokens[index++].number != 256u)
        return 0;
    while (index < count && tokens[index].kind == LEXEME_WORD &&
           word_in_list(tokens, count, index, input_words,
                        sizeof input_words / sizeof input_words[0]))
        ++index;
    if (index >= count || tokens[index].kind != LEXEME_NUMBER ||
        tokens[index].negative || tokens[index].malformed ||
        tokens[index].hexadecimal || tokens[index].number > 255u)
        return 0;
    ++index;
    if (index == count) return 1;
    if (word_in_list(tokens, count, index, coordinators,
                     sizeof coordinators / sizeof coordinators[0]))
        ++index;
    if (index >= count ||
        !word_in_list(tokens, count, index, response_words,
                      sizeof response_words / sizeof response_words[0]))
        return 0;
    ++index;
    while (index < count && tokens[index].kind == LEXEME_WORD &&
           word_in_list(tokens, count, index, response_objects,
                        sizeof response_objects / sizeof response_objects[0]))
        ++index;
    return index == count;
}

static int compose_hyphen_supported(const unsigned char *prompt,
                                    const unsigned char *hyphen) {
    static const char *const compounds[] = {
        "three-stage", "one-byte", "eight-bit", "unsigned-byte",
        "unsigned-octet", "plus-three"
    };
    const unsigned char *begin = hyphen, *end = hyphen + 1u;
    char compound[RUNTIME_WORD_MAX * 2u];
    size_t length, index;
    if (prompt == NULL || hyphen == NULL || *hyphen != '-') return 0;
    while (begin > prompt && isalpha(begin[-1])) --begin;
    while (isalpha(*end)) ++end;
    length = (size_t)(end - begin);
    if (begin == hyphen || end == hyphen + 1u ||
        length >= sizeof compound)
        return 0;
    for (index = 0; index < length; ++index)
        compound[index] = (char)tolower(begin[index]);
    compound[length] = '\0';
    for (index = 0; index < sizeof compounds / sizeof compounds[0]; ++index)
        if (strcmp(compound, compounds[index]) == 0) return 1;
    return 0;
}

static int compose_descriptor_linked(const char *prompt,
                                     const Lexeme *left,
                                     const Lexeme *right) {
    size_t index;
    if (prompt == NULL || left == NULL || right == NULL ||
        left->source_end > right->source_begin)
        return 0;
    for (index = left->source_end; index < right->source_begin; ++index)
        if (!isspace((unsigned char)prompt[index]) && prompt[index] != '-')
            return 0;
    return 1;
}

static int compose_operations_ordered(const char *prompt,
                                      const Lexeme *tokens, size_t count) {
    static const char *const unsupported_operations[] = {
        "decrement", "subtract", "halve", "half", "divide", "division",
        "rotate", "shift", "negate", "invert", "triple", "quadruple",
        "square", "cube", "mask", "xor"
    };
    OperationPositions increment = {0}, doubling = {0}, add_three = {0};
    const char *symbol_increment, *symbol_double, *symbol_add_three;
    const unsigned char *operator_cursor;
    size_t index;
    if (prompt == NULL) return 0;
    operator_cursor = (const unsigned char *)prompt;
    for (; *operator_cursor != '\0'; ++operator_cursor) {
        if (*operator_cursor == '~' || *operator_cursor == '&' ||
            *operator_cursor == '|' || *operator_cursor == '<' ||
            *operator_cursor == '>' || *operator_cursor == '=' ||
            *operator_cursor == '!')
            return 0;
        if (*operator_cursor == '-' &&
            !compose_hyphen_supported((const unsigned char *)prompt,
                                      operator_cursor))
            return 0;
    }
    if (strchr(prompt, '+') != NULL || strchr(prompt, '*') != NULL ||
        strchr(prompt, '/') != NULL || strchr(prompt, '%') != NULL ||
        strchr(prompt, '^') != NULL) {
        for (index = 0; index < count; ++index)
            if (compose_operation_word(tokens, count, index)) return 0;
        return symbolic_compose_operations_exact(prompt);
    }
    if (has_any_word(tokens, count, unsupported_operations,
                     sizeof unsupported_operations /
                         sizeof unsupported_operations[0]))
        return 0;
    for (index = 0; index < count; ++index) {
        size_t descriptor;
        if (!word_is(tokens, count, index, "stage") &&
            !word_is(tokens, count, index, "stages") &&
            !word_is(tokens, count, index, "hop") &&
            !word_is(tokens, count, index, "hops"))
            continue;
        descriptor = index > 0 ? index - 1u : count;
        if (descriptor < count &&
            compose_descriptor_linked(prompt, &tokens[descriptor],
                                      &tokens[index]) &&
            ((tokens[descriptor].kind == LEXEME_NUMBER &&
              tokens[descriptor].number != 3u) ||
             (tokens[descriptor].kind == LEXEME_WORD &&
              number_word(tokens, count, descriptor) &&
              !word_is(tokens, count, descriptor, "three"))))
            return 0;
    }
    for (index = 0; index < count; ++index) {
        if (word_is(tokens, count, index, "increment") ||
            word_is(tokens, count, index, "successor") ||
            ((word_is(tokens, count, index, "advance") ||
              word_is(tokens, count, index, "advances")) &&
             operation_value_follows(tokens, count, index, 1u, "one")) ||
            ((word_is(tokens, count, index, "add") ||
              word_is(tokens, count, index, "plus") ||
              word_is(tokens, count, index, "raise") ||
              word_is(tokens, count, index, "increase")) &&
             operation_value_follows(tokens, count, index, 1u, "one")))
            add_operation(&increment, index);
        if (word_is(tokens, count, index, "double") ||
            word_is(tokens, count, index, "doubles") ||
            word_is(tokens, count, index, "doubling") ||
            word_is(tokens, count, index, "twice") ||
            ((word_is(tokens, count, index, "multiply") ||
              word_is(tokens, count, index, "multiplication") ||
              word_is(tokens, count, index, "times") ||
              word_is(tokens, count, index, "scale")) &&
             operation_value_follows(tokens, count, index, 2u, "two")))
            add_operation(&doubling, index);
        if ((word_is(tokens, count, index, "add") ||
             word_is(tokens, count, index, "addition") ||
             word_is(tokens, count, index, "plus") ||
             word_is(tokens, count, index, "offset") ||
             word_is(tokens, count, index, "raise") ||
             word_is(tokens, count, index, "raises") ||
             word_is(tokens, count, index, "increase")) &&
            operation_value_follows(tokens, count, index, 3u, "three"))
            add_operation(&add_three, index);
    }
    if (increment.count + doubling.count + add_three.count != 0) {
        if (increment.count != 1 || doubling.count != 1 ||
            add_three.count != 1 || increment.last >= doubling.first ||
            doubling.last >= add_three.first)
            return 0;
        for (index = increment.last + 1u; index < doubling.first; ++index)
            if (tokens[index].kind == LEXEME_WORD &&
                !compose_glue_word(tokens, count, index))
                return 0;
        for (index = doubling.last + 1u; index < add_three.first; ++index)
            if (tokens[index].kind == LEXEME_WORD &&
                !compose_glue_word(tokens, count, index))
                return 0;
        for (index = add_three.last + 1u; index < count; ++index)
            if (tokens[index].kind == LEXEME_WORD &&
                !compose_tail_word(tokens, count, index))
                return 0;
        return 1;
    }
    symbol_increment = strstr(prompt, "+1");
    symbol_double = strstr(prompt, "*2");
    symbol_add_three = strstr(prompt, "+3");
    if (symbol_increment != NULL || symbol_double != NULL ||
        symbol_add_three != NULL)
        return 0;
    if (contains_ascii_casefold(prompt, "compose3_mod256"))
        return named_compose_request_supported(tokens, count);
    return 0;
}

static int policy_version_supported(const Lexeme *tokens, size_t count) {
    size_t index;
    for (index = 0; index < count; ++index) {
        if (tokens[index].kind == LEXEME_NUMBER &&
            tokens[index].number > 1u)
            return 0;
        if (word_is(tokens, count, index, "version") ||
            word_is(tokens, count, index, "revision") ||
            word_is(tokens, count, index, "v")) {
            if (lexeme_is_value(tokens, count, index + 1u, 1u, "one"))
                continue;
            return 0;
        }
    }
    return 1;
}

static int policy_outcome_request_supported(const Lexeme *tokens,
                                            size_t count) {
    size_t index, allow_count = 0, deny_count = 0;
    for (index = 0; index < count; ++index) {
        if (word_is(tokens, count, index, "allow")) ++allow_count;
        if (word_is(tokens, count, index, "deny")) ++deny_count;
    }
    if (allow_count == 0 && deny_count == 0) return 1;
    /* "allow or deny" is an open request for the computed decision.  A lone
       requested outcome is an output assertion, not part of the four-Boolean
       certified input, so it must not be silently ignored. */
    return allow_count == 1u && deny_count == 1u &&
           has_word(tokens, count, "or");
}

static int parameter_value_after(const Lexeme *tokens, size_t count,
                                 size_t index, unsigned *value_out,
                                 int *negative_out) {
    size_t look, limit = index + 3u < count ? index + 3u : count;
    *negative_out = 0;
    for (look = index + 1u; look < limit; ++look) {
        if (tokens[look].kind == LEXEME_NUMBER) {
            if (tokens[look].malformed || tokens[look].number > UINT_MAX)
                return -1;
            *value_out = (unsigned)tokens[look].number;
            *negative_out = tokens[look].negative;
            return 1;
        }
        if (word_is(tokens, count, look, "zero")) *value_out = 0u;
        else if (word_is(tokens, count, look, "one")) *value_out = 1u;
        else if (word_is(tokens, count, look, "seven")) *value_out = 7u;
        else if (word_is(tokens, count, look, "eight")) *value_out = 8u;
        else if (word_is(tokens, count, look, "sixteen")) *value_out = 16u;
        else continue;
        return 1;
    }
    return 0;
}

static int assignment_fields_supported(const char *prompt,
                                       const char *const *allowed,
                                       size_t allowed_count) {
    const unsigned char *cursor = (const unsigned char *)prompt;
    if (prompt == NULL || allowed == NULL || allowed_count == 0) return 0;
    while (*cursor != '\0') {
        const unsigned char *end, *begin;
        char field[RUNTIME_WORD_MAX];
        size_t length, index;
        int matched = 0;
        if (*cursor++ != '=') continue;
        end = cursor - 1u;
        while (end > (const unsigned char *)prompt && isspace(end[-1])) --end;
        begin = end;
        while (begin > (const unsigned char *)prompt &&
               (isalnum(begin[-1]) || begin[-1] == '_'))
            --begin;
        length = (size_t)(end - begin);
        if (length == 0 || length >= sizeof field) return 0;
        for (index = 0; index < length; ++index)
            field[index] = (char)tolower(begin[index]);
        field[length] = '\0';
        for (index = 0; index < allowed_count; ++index)
            if (strcmp(field, allowed[index]) == 0) {
                matched = 1;
                break;
            }
        if (!matched) return 0;
    }
    return 1;
}

static int boolean_parameter_after(const Lexeme *tokens, size_t count,
                                   size_t index, int *value_out) {
    size_t look, limit = index + 3u < count ? index + 3u : count;
    if (value_out == NULL) return -1;
    for (look = index + 1u; look < limit; ++look)
        if (boolean_value(&tokens[look], value_out) == 0) return 1;
    return 0;
}

static int crc_with_clauses_supported(const Lexeme *tokens, size_t count) {
    static const char *const starters[] = {
        "width", "poly", "polynomial", "init", "initial", "register",
        "xor", "xorout", "final", "zero", "no", "non", "the", "atm",
        "crc", "refin", "refout", "reflected", "input", "byte", "octet",
        "single", "one", "source"
    };
    size_t index;
    for (index = 0; index < count; ++index) {
        size_t next;
        if (!word_is(tokens, count, index, "with")) continue;
        next = index + 1u;
        if (next >= count || tokens[next].kind != LEXEME_WORD ||
            !word_in_list(tokens, count, next, starters,
                          sizeof starters / sizeof starters[0]))
            return 0;
    }
    return 1;
}

static int crc_contract_vocabulary_supported(const Lexeme *tokens,
                                             size_t count) {
    static const char *const vocabulary[] = {
        "crc", "atm", "checksum", "check", "code", "result", "value",
        "output", "byte", "bytes", "octet", "octets", "datum", "input",
        "operand", "single", "one", "unsigned", "eight", "bit", "bits",
        "width", "polynomial", "poly", "seven", "initial", "initialized",
        "initialize", "init", "zero", "register", "final", "xor", "xorout",
        "refin", "refout", "non", "no", "reflected", "reflection", "reflect",
        "rule", "standard", "algorithm", "cyclic", "redundancy", "for", "of",
        "on", "over", "from", "to", "with", "using", "use", "under", "as",
        "the", "a", "an", "its", "and", "return", "give", "report",
        "state", "provide", "produce", "derive", "calculate", "compute",
        "process", "apply", "at", "both", "ends", "true", "false"
    };
    size_t anchor = count, index;
    for (index = 0; index < count; ++index)
        if (word_is(tokens, count, index, "crc") ||
            word_is(tokens, count, index, "atm") ||
            word_is(tokens, count, index, "checksum") ||
            word_is(tokens, count, index, "polynomial") ||
            word_is(tokens, count, index, "poly") ||
            word_is(tokens, count, index, "redundancy")) {
            anchor = index;
            break;
        }
    if (anchor == count) return 0;
    for (index = anchor; index < count; ++index) {
        if (tokens[index].kind == LEXEME_NUMBER ||
            word_in_list(tokens, count, index, vocabulary,
                         sizeof vocabulary / sizeof vocabulary[0]))
            continue;
        return 0;
    }
    return 1;
}

static int crc_configuration_supported(const char *prompt,
                                       const Lexeme *tokens, size_t count) {
    static const char *const unsupported[] = {
        "lsb", "least", "reciprocal", "dallas", "maxim", "j1850",
        "sae", "autosar", "cdma", "rohc", "bluetooth", "variant", "mode"
    };
    static const char *const assignment_fields[] = {
        "width", "poly", "polynomial", "init", "initial", "xor",
        "xorout", "refin", "refout", "input", "value", "byte", "octet"
    };
    size_t index;
    if (has_any_word(tokens, count, unsupported,
                     sizeof unsupported / sizeof unsupported[0]))
        return 0;
    if (!assignment_fields_supported(
            prompt, assignment_fields,
            sizeof assignment_fields / sizeof assignment_fields[0]) ||
        !crc_with_clauses_supported(tokens, count) ||
        !crc_contract_vocabulary_supported(tokens, count))
        return 0;
    if (has_word(tokens, count, "sixteen"))
        return 0;
    for (index = 0; index < count; ++index) {
        unsigned value = 0;
        int found, negative = 0;
        if (word_is(tokens, count, index, "crc")) {
            found = parameter_value_after(tokens, count, index, &value,
                                          &negative);
            if (found < 0 || (found > 0 && value != 8u)) return 0;
        }
        if (word_is(tokens, count, index, "poly") ||
            word_is(tokens, count, index, "polynomial")) {
            found = parameter_value_after(tokens, count, index, &value,
                                          &negative);
            if (found <= 0 || negative || value != 7u)
                return 0;
        }
        if (word_is(tokens, count, index, "width")) {
            found = parameter_value_after(tokens, count, index, &value,
                                          &negative);
            if (found <= 0 || negative || value != 8u) return 0;
        }
        if (word_is(tokens, count, index, "init") ||
            word_is(tokens, count, index, "initial") ||
            word_is(tokens, count, index, "initialize") ||
            word_is(tokens, count, index, "initialized") ||
            word_is(tokens, count, index, "xorout") ||
            word_is(tokens, count, index, "xor")) {
            if (word_is(tokens, count, index, "initialized") && index > 0 &&
                word_is(tokens, count, index - 1u, "zero"))
                continue;
            found = parameter_value_after(tokens, count, index, &value,
                                          &negative);
            if (found <= 0 || negative || value != 0u)
                return 0;
        }
        if (word_is(tokens, count, index, "reflected") ||
            word_is(tokens, count, index, "reflection") ||
            word_is(tokens, count, index, "reflect")) {
            if (index == 0 ||
                (!word_is(tokens, count, index - 1u, "non") &&
                 !word_is(tokens, count, index - 1u, "no")))
                return 0;
        }
        if (word_is(tokens, count, index, "refin") ||
            word_is(tokens, count, index, "refout")) {
            int boolean = 0;
            found = boolean_parameter_after(tokens, count, index, &boolean);
            if (found <= 0 || boolean != 0) return 0;
        }
    }
    return 1;
}

static int crc_identity_supported(const char *prompt,
                                  const Lexeme *tokens, size_t count) {
    size_t index;
    if (contains_ascii_casefold(prompt, "crc8_atm") ||
        has_word(tokens, count, "atm"))
        return 1;
    for (index = 0; index < count; ++index) {
        unsigned value = 0;
        int found, negative = 0;
        if (!word_is(tokens, count, index, "poly") &&
            !word_is(tokens, count, index, "polynomial"))
            continue;
        found = parameter_value_after(tokens, count, index, &value,
                                      &negative);
        if (found > 0 && !negative && value == 7u) return 1;
    }
    return 0;
}

static int words_after(const Lexeme *tokens, size_t count, size_t index,
                       const char *one, const char *two,
                       const char *three, const char *four) {
    return word_is(tokens, count, index + 1u, one) &&
           word_is(tokens, count, index + 2u, two) &&
           word_is(tokens, count, index + 3u, three) &&
           word_is(tokens, count, index + 4u, four);
}

static int number_word(const Lexeme *tokens, size_t count, size_t index) {
    static const char *const words[] = {
        "zero", "one", "two", "three", "four", "five", "six", "seven",
        "eight", "nine", "ten", "eleven", "twelve", "thirteen",
        "fourteen", "fifteen", "sixteen", "seventeen", "eighteen",
        "nineteen", "twenty", "thirty", "forty", "fifty", "sixty",
        "seventy", "eighty", "ninety", "hundred", "thousand"
    };
    size_t word;
    for (word = 0; word < sizeof words / sizeof words[0]; ++word)
        if (word_is(tokens, count, index, words[word])) return 1;
    return 0;
}

static int word_part_of_modulo_256(const Lexeme *tokens, size_t count,
                                   size_t index) {
    size_t start, minimum = index > 3u ? index - 3u : 0u;
    for (start = minimum; start <= index && start + 3u < count; ++start) {
        if (!word_is(tokens, count, start, "two") ||
            !word_is(tokens, count, start + 1u, "hundred") ||
            !word_is(tokens, count, start + 2u, "fifty") ||
            !word_is(tokens, count, start + 3u, "six") || index < start ||
            index > start + 3u)
            continue;
        if ((start > 0u &&
             (word_is(tokens, count, start - 1u, "mod") ||
              word_is(tokens, count, start - 1u, "modulo"))) ||
            (start > 1u &&
             (word_is(tokens, count, start - 2u, "mod") ||
              word_is(tokens, count, start - 2u, "modulo"))))
            return 1;
    }
    return 0;
}

static int number_linked_to_operation(const Lexeme *tokens, size_t count,
                                      size_t index,
                                      const char *const *operations,
                                      size_t operation_count) {
    size_t distance, operation;
    if (index == 0u || !word_is(tokens, count, index - 1u, "by")) return 0;
    for (distance = 2u; distance <= 4u && distance <= index; ++distance)
        for (operation = 0u; operation < operation_count; ++operation)
            if (word_is(tokens, count, index - distance,
                        operations[operation]))
                return 1;
    return 0;
}

static int number_word_context_supported(const Lexeme *tokens, size_t count,
                                         size_t index,
                                         CnetCompeteIntent intent) {
    static const char *const multiply_operations[] = {
        "multiplication", "multiply", "scale", "times"
    };
    static const char *const add_three_operations[] = {
        "add", "addition", "offset", "raise", "raises", "increase"
    };
    if (!number_word(tokens, count, index)) return 1;
    if ((intent == CNET_INTENT_INCREMENT ||
         intent == CNET_INTENT_COMPOSE3) &&
        word_part_of_modulo_256(tokens, count, index))
        return 1;
    if (word_is(tokens, count, index, "one") ||
        word_is(tokens, count, index, "unity")) {
        if (byte_noun(tokens, count, index + 1u)) return 1;
        if (intent == CNET_INTENT_INCREMENT || intent == CNET_INTENT_COMPOSE3)
            return number_adjacent_word(tokens, count, index, "add") ||
                   number_adjacent_word(tokens, count, index, "advance") ||
                   number_adjacent_word(tokens, count, index, "advances") ||
                   number_adjacent_word(tokens, count, index, "by") ||
                   number_adjacent_word(tokens, count, index, "forward") ||
                   number_adjacent_word(tokens, count, index, "raise") ||
                   number_adjacent_word(tokens, count, index, "increase") ||
                   number_adjacent_word(tokens, count, index, "step") ||
                   number_adjacent_word(tokens, count, index, "plus");
        if (intent == CNET_INTENT_CRC8)
            return number_adjacent_word(tokens, count, index, "init") ||
                   number_adjacent_word(tokens, count, index, "initial") ||
                   number_adjacent_word(tokens, count, index, "register");
        if (intent == CNET_INTENT_POLICY)
            return number_adjacent_word(tokens, count, index, "policy") ||
                   number_adjacent_word(tokens, count, index, "rule") ||
                   number_adjacent_word(tokens, count, index, "version") ||
                   number_adjacent_word(tokens, count, index, "revision");
        return 0;
    }
    if (word_is(tokens, count, index, "two"))
        return intent == CNET_INTENT_COMPOSE3 &&
               (number_adjacent_word(tokens, count, index, "multiply") ||
                number_adjacent_word(tokens, count, index,
                                     "multiplication") ||
                number_adjacent_word(tokens, count, index, "scale") ||
                number_adjacent_word(tokens, count, index, "times") ||
                number_linked_to_operation(
                    tokens, count, index, multiply_operations,
                    sizeof multiply_operations /
                        sizeof multiply_operations[0]));
    if (word_is(tokens, count, index, "three"))
        return intent == CNET_INTENT_COMPOSE3 &&
               (number_adjacent_word(tokens, count, index, "add") ||
                number_adjacent_word(tokens, count, index, "addition") ||
                number_adjacent_word(tokens, count, index, "plus") ||
                number_adjacent_word(tokens, count, index, "offset") ||
                number_adjacent_word(tokens, count, index, "raise") ||
                number_adjacent_word(tokens, count, index, "raises") ||
                number_adjacent_word(tokens, count, index, "increase") ||
                number_adjacent_word(tokens, count, index, "stage") ||
                number_adjacent_word(tokens, count, index, "hop") ||
                number_linked_to_operation(
                    tokens, count, index, add_three_operations,
                    sizeof add_three_operations /
                        sizeof add_three_operations[0]));
    if (word_is(tokens, count, index, "four"))
        return (intent == CNET_INTENT_POLICY &&
                (number_adjacent_word(tokens, count, index, "switches") ||
                 number_adjacent_word(tokens, count, index, "flags"))) ||
               (intent == CNET_INTENT_COMPOSE3 &&
                (number_adjacent_word(tokens, count, index, "stage") ||
                 number_adjacent_word(tokens, count, index, "hop")));
    if (word_is(tokens, count, index, "seven"))
        return intent == CNET_INTENT_CRC8 &&
               (number_adjacent_word(tokens, count, index, "poly") ||
                number_adjacent_word(tokens, count, index, "polynomial"));
    if (word_is(tokens, count, index, "eight"))
        return (intent == CNET_INTENT_INCREMENT ||
                intent == CNET_INTENT_CRC8) &&
               (number_adjacent_word(tokens, count, index, "crc") ||
                number_adjacent_word(tokens, count, index, "width") ||
                (word_is(tokens, count, index + 1u, "bit") &&
                 (intent == CNET_INTENT_INCREMENT ||
                  (index > 0u &&
                   (word_is(tokens, count, index - 1u, "unsigned") ||
                    word_is(tokens, count, index - 1u, "atm") ||
                    word_is(tokens, count, index - 1u, "crc"))) ||
                  byte_noun(tokens, count, index + 2u) ||
                  word_is(tokens, count, index + 2u, "value") ||
                  word_is(tokens, count, index + 2u, "register"))));
    if (word_is(tokens, count, index, "sixteen"))
        return (intent == CNET_INTENT_INCREMENT ||
                intent == CNET_INTENT_CRC8) &&
               (number_adjacent_word(tokens, count, index, "bit") ||
                number_adjacent_word(tokens, count, index, "crc") ||
                byte_noun(tokens, count, index + 1u));
    if (word_is(tokens, count, index, "zero"))
        return intent == CNET_INTENT_CRC8 &&
               (number_adjacent_word(tokens, count, index, "init") ||
                number_adjacent_word(tokens, count, index, "initial") ||
                number_adjacent_word(tokens, count, index, "initialized") ||
                number_adjacent_word(tokens, count, index, "register") ||
                number_adjacent_word(tokens, count, index, "xor") ||
                number_adjacent_word(tokens, count, index, "xorout"));
    if (word_is(tokens, count, index, "sixty"))
        return intent == CNET_INTENT_MINUTES &&
               ((((word_is(tokens, count, index + 1u, "second") ||
                   word_is(tokens, count, index + 1u, "seconds")) &&
                  word_is(tokens, count, index + 2u, "per") &&
                  (word_is(tokens, count, index + 3u, "minute") ||
                   word_is(tokens, count, index + 3u, "minutes")))) ||
                (index > 0u &&
                 (word_is(tokens, count, index - 1u, "by") ||
                  word_is(tokens, count, index - 1u, "times") ||
                  word_is(tokens, count, index - 1u, "multiply"))));
    return 0;
}

static int byte_configuration_supported(const Lexeme *tokens, size_t count) {
    size_t index;
    if (has_word(tokens, count, "signed")) return 0;
    for (index = 0; index < count; ++index) {
        unsigned value = 0;
        int found, negative = 0;
        if (word_is(tokens, count, index, "mod") ||
            word_is(tokens, count, index, "modulo")) {
            found = parameter_value_after(tokens, count, index, &value,
                                          &negative);
            if (found < 0 || (found > 0 && (negative || value != 256u)))
                return 0;
            if (found == 0 && number_word(tokens, count, index + 1u)) {
                if (!words_after(tokens, count, index, "two", "hundred",
                                 "fifty", "six"))
                    return 0;
                continue;
            }
            if (found == 0 && index + 1u < count &&
                tokens[index + 1u].kind == LEXEME_WORD &&
                !word_is(tokens, count, index + 1u, "arithmetic") &&
                !word_is(tokens, count, index + 1u, "byte") &&
                !word_is(tokens, count, index + 1u, "bytes") &&
                !word_is(tokens, count, index + 1u, "wrap") &&
                !word_is(tokens, count, index + 1u, "wrapping") &&
                !word_is(tokens, count, index + 1u, "wraparound"))
                return 0;
        }
        if (tokens[index].kind == LEXEME_NUMBER && index + 1u < count &&
            word_is(tokens, count, index + 1u, "bit") &&
            (tokens[index].negative || tokens[index].malformed ||
             tokens[index].number != 8u))
            return 0;
        if (word_is(tokens, count, index, "bit") && index > 0 &&
            tokens[index - 1u].kind == LEXEME_WORD &&
            !word_is(tokens, count, index - 1u, "eight"))
            return 0;
    }
    return 1;
}

static int has_byte_identity(const char *prompt, const Lexeme *tokens,
                             size_t count) {
    size_t index;
    if (contains_ascii_casefold(prompt, "increment_mod256") ||
        contains_ascii_casefold(prompt, "crc8_atm") ||
        contains_ascii_casefold(prompt, "compose3_mod256"))
        return 1;
    for (index = 0; index < count; ++index)
        if (byte_noun(tokens, count, index) ||
            word_is(tokens, count, index, "uint") ||
            word_is(tokens, count, index, "uint8") ||
            (word_is(tokens, count, index, "eight") &&
             (word_is(tokens, count, index + 1u, "bit") ||
              (index > 0 && word_is(tokens, count, index - 1u, "bit")))) ||
            (tokens[index].kind == LEXEME_NUMBER &&
             !tokens[index].negative && !tokens[index].malformed &&
             tokens[index].number == 8u &&
             number_adjacent_word(tokens, count, index, "bit")))
            return 1;
    return 0;
}

static int has_policy_action(const char *prompt, const Lexeme *tokens,
                             size_t count) {
    static const char *const actions[] = {
        "access", "adjudicate", "policy", "permission", "authorize",
        "authorization", "security", "entry", "decision", "allowed",
        "allow", "deny", "granted", "permitted", "decide"
    };
    return contains_ascii_casefold(prompt, "access_policy_v1") ||
           has_any_word(tokens, count, actions,
                        sizeof actions / sizeof actions[0]);
}

static int policy_assignments_supported(const char *prompt) {
    const unsigned char *cursor = (const unsigned char *)prompt;
    if (prompt == NULL) return 0;
    while (*cursor != '\0') {
        const unsigned char *end, *begin;
        char field[RUNTIME_WORD_MAX];
        size_t length;
        if (*cursor++ != '=') continue;
        end = cursor - 1u;
        while (end > (const unsigned char *)prompt && isspace(end[-1])) --end;
        begin = end;
        while (begin > (const unsigned char *)prompt &&
               (isalnum(begin[-1]) || begin[-1] == '_'))
            --begin;
        length = (size_t)(end - begin);
        if (length == 0 || length >= sizeof field) return 0;
        {
            size_t index;
            for (index = 0; index < length; ++index)
                field[index] = (char)tolower(begin[index]);
            field[length] = '\0';
        }
        if (strcmp(field, "admin") != 0 && strcmp(field, "owner") != 0 &&
            strcmp(field, "mfa") != 0 &&
            strcmp(field, "suspended") != 0)
            return 0;
    }
    return 1;
}

static int policy_data_words_supported(const Lexeme *tokens, size_t count) {
    static const char *const introducers[] = {
        "with", "has", "have", "receives", "receive", "received", "uses",
        "use", "using", "tuple", "flags", "flag", "switches", "switch",
        "inputs", "input", "values", "value", "where", "when", "for",
        "from"
    };
    static const char *const scaffolding[] = {
        "with", "has", "have", "receives", "receive", "received", "uses",
        "use", "using", "tuple", "flags", "flag", "switches", "switch",
        "inputs", "input", "values", "value", "where", "when", "for",
        "from", "given", "set", "state", "of", "the", "four", "to",
        "and", "or", "is", "are", "return", "decide", "determine",
        "evaluate", "evaluation", "resolve", "apply", "under", "outcome",
        "decision", "permission", "access", "entry", "allow", "allowed",
        "deny", "authorization", "security", "its", "result", "policy",
        "rule", "version", "revision", "v", "then", "next", "finally",
        "reads", "granted", "permitted", "please", "whether", "is", "are"
    };
    size_t first_flag = count, zone_start = count, index;
    for (index = 0; index < count; ++index)
        if (flag_index(&tokens[index]) >= 0) {
            first_flag = index;
            break;
        }
    if (first_flag == count) return 0;
    zone_start = first_flag;
    for (index = 0; index < first_flag; ++index) {
        size_t look, limit;
        if (word_in_list(tokens, count, index, introducers,
                         sizeof introducers / sizeof introducers[0]))
            zone_start = index + 1u;
        if (!word_is(tokens, count, index, "policy") &&
            !word_is(tokens, count, index, "rule") &&
            !word_is(tokens, count, index, "version") &&
            !word_is(tokens, count, index, "revision") &&
            !word_is(tokens, count, index, "v"))
            continue;
        limit = index + 4u < first_flag ? index + 4u : first_flag;
        for (look = index + 1u; look < limit; ++look)
            if (lexeme_is_value(tokens, count, look, 1u, "one"))
                zone_start = look + 1u;
    }
    for (index = zone_start; index < count; ++index) {
        int boolean;
        if (flag_index(&tokens[index]) >= 0 ||
            boolean_value(&tokens[index], &boolean) == 0 ||
            (tokens[index].kind == LEXEME_WORD &&
             word_in_list(tokens, count, index, scaffolding,
                          sizeof scaffolding / sizeof scaffolding[0])))
            continue;
        if (word_is(tokens, count, index, "one") &&
            (number_adjacent_word(tokens, count, index, "policy") ||
             number_adjacent_word(tokens, count, index, "rule") ||
             number_adjacent_word(tokens, count, index, "version") ||
             number_adjacent_word(tokens, count, index, "revision")))
            continue;
        return 0;
    }
    return 1;
}

static int raw_cursor_within_casefold(const unsigned char *text,
                                      const unsigned char *cursor,
                                      const char *literal) {
    size_t length = strlen(literal), offset;
    const unsigned char *candidate;
    for (candidate = text; *candidate != '\0'; ++candidate) {
        for (offset = 0; offset < length; ++offset) {
            unsigned char left = candidate[offset];
            unsigned char right = (unsigned char)literal[offset];
            if (left == '\0') break;
            if (left >= 'A' && left <= 'Z') left = (unsigned char)(left + 32u);
            if (right >= 'A' && right <= 'Z')
                right = (unsigned char)(right + 32u);
            if (left != right) break;
        }
        if (offset == length && cursor >= candidate &&
            cursor < candidate + length)
            return 1;
    }
    return 0;
}

static int raw_hyphen_supported(const unsigned char *prompt,
                                const unsigned char *cursor) {
    static const char *const compounds[] = {
        "add-one", "byte-increment", "cyclic-redundancy", "eight-bit",
        "minute-to-second", "modulo-byte", "non-reflected", "one-byte",
        "policy-one", "plus-three", "polynomial-seven", "three-stage", "unsigned-octet",
        "zero-initialized", "crc-8", "crc-8/atm"
    };
    size_t index;
    for (index = 0; index < sizeof compounds / sizeof compounds[0]; ++index)
        if (raw_cursor_within_casefold(prompt, cursor, compounds[index]))
            return 1;
    return 0;
}

static int raw_contract_syntax_supported(const char *prompt,
                                         CnetCompeteIntent intent) {
    const unsigned char *begin = (const unsigned char *)prompt;
    const unsigned char *cursor;
    const char *registered_name = NULL;
    if (prompt == NULL) return 0;
    if (intent == CNET_INTENT_INCREMENT)
        registered_name = "increment_mod256";
    else if (intent == CNET_INTENT_MINUTES)
        registered_name = "minutes_to_seconds";
    else if (intent == CNET_INTENT_CRC8)
        registered_name = "crc8_atm";
    else if (intent == CNET_INTENT_POLICY)
        registered_name = "access_policy_v1";
    else if (intent == CNET_INTENT_COMPOSE3)
        registered_name = "compose3_mod256";
    else
        return 0;
    for (cursor = begin; *cursor != '\0'; ++cursor) {
        unsigned char character = *cursor;
        if (isalnum(character) || isspace(character) || character == '.' ||
            character == ',' || character == ';' || character == ':' ||
            character == '?' || character == '(' || character == ')' ||
            character == '[' || character == ']' || character == '{' ||
            character == '}')
            continue;
        if (character == '\'' && cursor > begin && isalpha(cursor[-1]) &&
            isalpha(cursor[1]))
            continue;
        if (character == '-' && raw_hyphen_supported(begin, cursor)) continue;
        if (character == '_' &&
            raw_cursor_within_casefold(begin, cursor, registered_name))
            continue;
        if (character == '=' &&
            (intent == CNET_INTENT_CRC8 || intent == CNET_INTENT_POLICY))
            continue;
        if (character == '/' &&
            ((intent == CNET_INTENT_CRC8 &&
              raw_cursor_within_casefold(begin, cursor, "crc-8/atm")) ||
             intent == CNET_INTENT_COMPOSE3))
            continue;
        if ((character == '+' || character == '*') &&
            intent == CNET_INTENT_COMPOSE3)
            continue;
        return 0;
    }
    return 1;
}

/* The learned base proposes one intent; this typed semantic envelope must
   independently prove that the request denotes exactly that certified
   contract. It never widens a capsule domain. */
static int contract_semantics_match(const char *prompt,
                                    CnetCompeteIntent intent) {
    static const char *const forbidden_words[] = {
        "delete", "remove", "restart", "send", "email", "purchase", "buy",
        "upload", "open", "network", "shell", "write", "create", "post",
        "transfer", "payment", "install", "firewall", "password",
        "credential", "credentials", "secret", "cloud", "deploy",
        "deployment", "calendar", "directory", "file", "ignore",
        "override", "bypass", "disable", "pretend", "fabricate",
        "unsupported", "freely", "markdown", "prose", "disregard",
        "force", "list", "application", "applications", "compare",
        "describe", "summarize", "explain", "discuss", "publish",
        "notify", "save", "store", "not", "minus"
    };
    static const char *const increment_words[] = {
        "increment", "successor", "advance", "following", "follows",
        "after", "increase", "raise", "adding", "forward", "move", "ahead",
        "cycle", "wrap"
    };
    static const char *const crc_words[] = {
        "crc", "checksum", "atm", "polynomial", "redundancy"
    };
    static const char *const policy_words[] = {
        "access", "policy", "permission", "authorize", "allowed", "allow",
        "deny", "admin", "owner", "mfa", "suspended"
    };
    static const char *const compose_words[] = {
        "compose", "composition", "chain", "pipeline", "stage", "hop",
        "sequence"
    };
    Lexeme tokens[RUNTIME_LEXEMES_MAX];
    size_t count = 0, index;
    int increment, minutes, seconds, crc, policy, compose;
    int ordered_compose;
    int named_increment, named_minutes, named_crc, named_policy, named_compose;
    if (!raw_contract_syntax_supported(prompt, intent) ||
        lexemes_scan(prompt, tokens, &count) != 0 ||
        quoted_numeric_literal(prompt) || has_word(tokens, count, "text") ||
        has_word(tokens, count, "string") ||
        has_word(tokens, count, "character") ||
        has_word(tokens, count, "quoted") ||
        has_word(tokens, count, "literal"))
        return 0;
    if (has_any_word(tokens, count, forbidden_words,
                     sizeof forbidden_words / sizeof forbidden_words[0]))
        return 0;
    if (!contract_vocabulary_supported(tokens, count, intent) ||
        !response_objects_supported(tokens, count))
        return 0;
    named_increment = contains_ascii_casefold(prompt, "increment_mod256");
    named_minutes = contains_ascii_casefold(prompt, "minutes_to_seconds");
    named_crc = contains_ascii_casefold(prompt, "crc8_atm");
    named_policy = contains_ascii_casefold(prompt, "access_policy_v1");
    named_compose = contains_ascii_casefold(prompt, "compose3_mod256");
    increment = named_increment || has_any_word(
        tokens, count, increment_words,
        sizeof increment_words / sizeof increment_words[0]);
    for (index = 0; !increment && index < count; ++index)
        if ((word_is(tokens, count, index, "add") ||
             word_is(tokens, count, index, "plus")) &&
            operation_value_follows(tokens, count, index, 1u, "one"))
            increment = 1;
    for (index = 0; !increment && index < count; ++index)
        if (word_is(tokens, count, index, "next") &&
            (word_is(tokens, count, index + 1u, "representable") ||
             byte_noun(tokens, count, index + 1u) ||
             byte_noun(tokens, count, index + 2u)))
            increment = 1;
    minutes = named_minutes || has_word(tokens, count, "minute") ||
              has_word(tokens, count, "minutes") ||
              has_word(tokens, count, "min");
    seconds = named_minutes || has_word(tokens, count, "seconds") ||
              has_word(tokens, count, "sec");
    if (minutes && has_word(tokens, count, "second")) seconds = 1;
    crc = named_crc || has_any_word(tokens, count, crc_words,
                                    sizeof crc_words / sizeof crc_words[0]);
    policy = named_policy || has_any_word(
        tokens, count, policy_words,
        sizeof policy_words / sizeof policy_words[0]);
    ordered_compose = compose_operations_ordered(prompt, tokens, count);
    compose = named_compose || ordered_compose || has_any_word(
        tokens, count, compose_words,
        sizeof compose_words / sizeof compose_words[0]);
    switch (intent) {
        case CNET_INTENT_INCREMENT:
            return increment && has_byte_identity(prompt, tokens, count) &&
                   !crc && !minutes && !seconds && !policy &&
                   !compose && byte_configuration_supported(tokens, count) &&
                   continuation_clauses_supported(tokens, count, intent) &&
                   !has_word(tokens, count, "saturating") &&
                   !has_word(tokens, count, "saturate") &&
                   !has_word(tokens, count, "clamp") &&
                   !has_word(tokens, count, "clamped") &&
                   !has_word(tokens, count, "decrement") &&
                   !has_word(tokens, count, "predecessor") &&
                   !has_word(tokens, count, "subtract") &&
                   !has_word(tokens, count, "maximum");
        case CNET_INTENT_MINUTES:
            return minutes && seconds && !increment && !crc && !policy &&
                   !compose &&
                   continuation_clauses_supported(tokens, count, intent) &&
                   !has_word(tokens, count, "hour") &&
                   !has_word(tokens, count, "hours") &&
                   !has_word(tokens, count, "reverse") &&
                   !has_word(tokens, count, "backwards");
        case CNET_INTENT_CRC8:
            if (!crc || increment || minutes || seconds || policy || compose)
                return 0;
            return has_byte_identity(prompt, tokens, count) &&
                   crc_identity_supported(prompt, tokens, count) &&
                   continuation_clauses_supported(tokens, count, intent) &&
                   !has_word(tokens, count, "bytes") &&
                   !has_word(tokens, count, "octets") &&
                   !has_word(tokens, count, "array") &&
                   !has_word(tokens, count, "file") &&
                   !has_word(tokens, count, "sequence") &&
                   !has_word(tokens, count, "two") &&
                   crc_configuration_supported(prompt, tokens, count);
        case CNET_INTENT_POLICY:
            return policy && has_policy_action(prompt, tokens, count) &&
                   policy_assignments_supported(prompt) &&
                   policy_data_words_supported(tokens, count) &&
                   continuation_clauses_supported(tokens, count, intent) &&
                   !increment && !minutes && !seconds && !crc &&
                   !compose && policy_version_supported(tokens, count) &&
                   policy_outcome_request_supported(tokens, count) &&
                   !has_word(tokens, count, "guest") &&
                   !has_word(tokens, count, "auditor") &&
                   !has_word(tokens, count, "role") &&
                   !has_word(tokens, count, "group");
        case CNET_INTENT_COMPOSE3:
            return !crc && !minutes && !seconds && !policy &&
                   has_byte_identity(prompt, tokens, count) &&
                   byte_configuration_supported(tokens, count) &&
                   !has_word(tokens, count, "again") &&
                   !has_word(tokens, count, "repeat") &&
                   !has_word(tokens, count, "repeated") &&
                   !has_word(tokens, count, "more") &&
                   !has_word(tokens, count, "extra") &&
                   ordered_compose;
        default:
            return 0;
    }
}

static int resolve_contract_semantics(const char *prompt,
                                      CnetCompeteIntent *intent_out) {
    CnetCompeteIntent candidate = CNET_INTENT_ABSTAIN;
    int intent, matches = 0;
    if (prompt == NULL || intent_out == NULL) return -1;
    *intent_out = CNET_INTENT_ABSTAIN;
    for (intent = CNET_INTENT_INCREMENT; intent < CNET_INTENT_ABSTAIN;
         ++intent) {
        if (!contract_semantics_match(prompt, (CnetCompeteIntent)intent))
            continue;
        candidate = (CnetCompeteIntent)intent;
        ++matches;
    }
    if (matches != 1) return 1;
    *intent_out = candidate;
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
    {
        struct stat metadata_status;
        if (lstat(metadata_path, &metadata_status) != 0 ||
            !S_ISREG(metadata_status.st_mode) || metadata_status.st_size <= 0 ||
            (unsigned long long)metadata_status.st_size > SIZE_MAX ||
            intent_report.artifact_bytes >
                SIZE_MAX - (size_t)metadata_status.st_size)
            goto fail;
        local.base_artifact_bytes = intent_report.artifact_bytes +
                                    (size_t)metadata_status.st_size;
    }
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

static void diagnostic_reset(CnetCompeteDiagnostic *diagnostic) {
    if (diagnostic == NULL) return;
    memset(diagnostic, 0, sizeof *diagnostic);
    diagnostic->proposed_intent = CNET_INTENT_ABSTAIN;
    diagnostic->semantic_intent = CNET_INTENT_ABSTAIN;
}

static int runtime_execute_internal(CnetCompeteRuntime *runtime,
                                    const char *prompt,
                                    CnetCompeteResult *result,
                                    CnetCompeteDiagnostic *diagnostic) {
    CnetCompeteIntent intent = CNET_INTENT_ABSTAIN;
    CnetCompeteIntent semantic_intent = CNET_INTENT_ABSTAIN;
    double confidence = 0.0;
    unsigned input = 0, output = 0;
    int classified;
    if (runtime == NULL || prompt == NULL || result == NULL) return -1;
    memset(result, 0, sizeof *result);
    result->intent = CNET_INTENT_ABSTAIN;
    diagnostic_reset(diagnostic);
    classified = cnet_compete_intent_classify(runtime->intent, prompt, &intent,
                                               &confidence);
    if (diagnostic != NULL) {
        diagnostic->proposed_intent = intent;
        diagnostic->confidence = confidence;
    }
    if (classified < 0 || !isfinite(confidence)) {
        if (diagnostic != NULL)
            diagnostic->refusal = CNET_COMPETE_REFUSAL_EXECUTION;
        return -1;
    }
    if (classified != 0) {
        if (diagnostic != NULL)
            diagnostic->refusal = CNET_COMPETE_REFUSAL_INTENT_PROPOSAL;
        return 0;
    }
    classified = resolve_contract_semantics(prompt, &semantic_intent);
    if (diagnostic != NULL) diagnostic->semantic_intent = semantic_intent;
    if (classified < 0) {
        if (diagnostic != NULL)
            diagnostic->refusal = CNET_COMPETE_REFUSAL_EXECUTION;
        return -1;
    }
    if (classified != 0) {
        if (diagnostic != NULL)
            diagnostic->refusal = CNET_COMPETE_REFUSAL_SEMANTIC_FRAME;
        return 0;
    }
    if (intent != semantic_intent) {
        if (diagnostic != NULL)
            diagnostic->refusal = CNET_COMPETE_REFUSAL_INTENT_DISAGREEMENT;
        return 0;
    }
    if (intent == CNET_INTENT_POLICY) {
        if (parse_policy_argument(prompt, &input) != 0) {
            if (diagnostic != NULL)
                diagnostic->refusal = CNET_COMPETE_REFUSAL_ARGUMENT;
            return 0;
        }
    } else if (parse_numeric_argument(prompt, intent, &input) != 0) {
        if (diagnostic != NULL)
            diagnostic->refusal = CNET_COMPETE_REFUSAL_ARGUMENT;
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
        if (execution != 0) {
            if (diagnostic != NULL)
                diagnostic->refusal = guard.refusal != CNET_COMPETE_REFUSAL_NONE
                                          ? guard.refusal
                                          : CNET_COMPETE_REFUSAL_EXECUTION;
            return -1;
        }
        if (guard.checks != 3) {
            if (diagnostic != NULL)
                diagnostic->refusal = CNET_COMPETE_REFUSAL_COVERAGE;
            return -1;
        }
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
        classified = cnet_compete_capsule_eval(&runtime->base,
                                               &runtime->coverage, unit,
                                               input, &output);
        if (classified != 0) {
            if (diagnostic != NULL)
                diagnostic->refusal = classified > 0
                                          ? CNET_COMPETE_REFUSAL_COVERAGE
                                          : CNET_COMPETE_REFUSAL_EXECUTION;
            return -1;
        }
    }
    result->answered = 1;
    result->intent = intent;
    result->value = output;
    result->confidence = confidence;
    return 0;
}

int cnet_compete_runtime_execute(CnetCompeteRuntime *runtime,
                                 const char *prompt,
                                 CnetCompeteResult *result) {
    return runtime_execute_internal(runtime, prompt, result, NULL);
}

int cnet_compete_runtime_execute_diagnostic(
    CnetCompeteRuntime *runtime, const char *prompt,
    CnetCompeteResult *result, CnetCompeteDiagnostic *diagnostic) {
    if (diagnostic == NULL) return -1;
    return runtime_execute_internal(runtime, prompt, result, diagnostic);
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
        if (result->intent == CNET_INTENT_POLICY) {
            if (result->value > 1u) return -1;
            written = snprintf(output, capacity,
                               "{\"status\":\"answer\",\"intent\":\"%s\","
                               "\"value\":\"%s\"}", intent,
                               result->value ? "allow" : "deny");
        } else {
            written = snprintf(output, capacity,
                               "{\"status\":\"answer\",\"intent\":\"%s\","
                               "\"value\":%u}", intent, result->value);
        }
    }
    return written < 0 || (size_t)written >= capacity ? -1 : 0;
}
