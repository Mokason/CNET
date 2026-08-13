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

static int number_is_constant(const Lexeme *tokens, size_t count,
                              size_t index) {
    static const char *const markers[] = {
        "crc", "bit", "uint", "poly", "polynomial", "init", "xorout",
        "mod", "modulo", "add", "plus", "multiply", "times", "double",
        "compose", "stage", "hop", "version", "wrap", "wraps",
        "wrapping", "wraparound", "overflow", "boundary", "limit"
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
    for (index = 0; index < count; ++index) {
        if (index == selected || tokens[index].kind != LEXEME_NUMBER ||
            number_is_constant(tokens, count, index))
            continue;
        if (intent == CNET_INTENT_COMPOSE3 &&
            strstr(prompt, "+1") != NULL && strstr(prompt, "*2") != NULL &&
            strstr(prompt, "+3") != NULL &&
            tokens[index].number >= 1u && tokens[index].number <= 3u)
            continue;
        return -1;
    }
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

static int lexeme_is_value(const Lexeme *tokens, size_t count, size_t index,
                           unsigned value, const char *word) {
    return index < count &&
           ((tokens[index].kind == LEXEME_NUMBER &&
             !tokens[index].negative && !tokens[index].malformed &&
             tokens[index].number == value) ||
            word_is(tokens, count, index, word));
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

static int compose_operations_ordered(const char *prompt,
                                      const Lexeme *tokens, size_t count) {
    static const char *const compose_words[] = {
        "compose", "composition", "chain", "pipeline", "stage", "hop",
        "sequence"
    };
    OperationPositions increment = {0}, doubling = {0}, add_three = {0};
    const char *symbol_increment, *symbol_double, *symbol_add_three;
    int descriptor, byte = 0, three = 0;
    size_t index;
    for (index = 0; index < count; ++index) {
        if (byte_noun(tokens, count, index) ||
            word_is(tokens, count, index, "uint"))
            byte = 1;
        if (lexeme_is_value(tokens, count, index, 3u, "three")) three = 1;
        if (word_is(tokens, count, index, "increment") ||
            word_is(tokens, count, index, "successor"))
            add_operation(&increment, index);
        if (word_is(tokens, count, index, "double") ||
            word_is(tokens, count, index, "doubling") ||
            word_is(tokens, count, index, "twice"))
            add_operation(&doubling, index);
        if ((word_is(tokens, count, index, "add") ||
             word_is(tokens, count, index, "plus")) &&
            lexeme_is_value(tokens, count, index + 1u, 1u, "one"))
            add_operation(&increment, index);
        if ((word_is(tokens, count, index, "multiply") ||
             word_is(tokens, count, index, "times")) &&
            lexeme_is_value(tokens, count, index + 1u, 2u, "two"))
            add_operation(&doubling, index);
        if ((word_is(tokens, count, index, "add") ||
             word_is(tokens, count, index, "plus") ||
             word_is(tokens, count, index, "offset")) &&
            lexeme_is_value(tokens, count, index + 1u, 3u, "three"))
            add_operation(&add_three, index);
    }
    descriptor = has_any_word(tokens, count, compose_words,
                              sizeof compose_words / sizeof compose_words[0]);
    if (has_word(tokens, count, "offset") && descriptor &&
        add_three.count == 0)
        for (index = 0; index < count; ++index)
            if (word_is(tokens, count, index, "offset")) {
                add_operation(&add_three, index);
                break;
            }
    if (increment.count + doubling.count + add_three.count != 0)
        return increment.count == 1 && doubling.count == 1 &&
               add_three.count == 1 && increment.last < doubling.first &&
               doubling.last < add_three.first;
    symbol_increment = strstr(prompt, "+1");
    symbol_double = strstr(prompt, "*2");
    symbol_add_three = strstr(prompt, "+3");
    if (symbol_increment != NULL || symbol_double != NULL ||
        symbol_add_three != NULL)
        return symbol_increment != NULL && symbol_double != NULL &&
               symbol_add_three != NULL && symbol_increment < symbol_double &&
               symbol_double < symbol_add_three;
    if (contains_ascii_casefold(prompt, "compose3_mod256")) return 1;
    return descriptor && byte && three;
}

static int policy_version_supported(const Lexeme *tokens, size_t count) {
    size_t index;
    for (index = 0; index < count; ++index) {
        if (tokens[index].kind == LEXEME_NUMBER &&
            tokens[index].number > 1u)
            return 0;
        if (word_is(tokens, count, index, "version") ||
            word_is(tokens, count, index, "v")) {
            if (lexeme_is_value(tokens, count, index + 1u, 1u, "one"))
                continue;
            return 0;
        }
    }
    return 1;
}

static int parameter_value_after(const Lexeme *tokens, size_t count,
                                 size_t index, unsigned *value_out,
                                 int *negative_out) {
    size_t look, limit = index + 3u < count ? index + 3u : count;
    *negative_out = 0;
    for (look = index + 1u; look < limit; ++look) {
        if (tokens[look].kind == LEXEME_NUMBER) {
            if (tokens[look].malformed) return -1;
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

static int crc_configuration_supported(const Lexeme *tokens, size_t count) {
    size_t index;
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
            if (found < 0 || (found > 0 && (negative || value != 7u)))
                return 0;
        }
        if (word_is(tokens, count, index, "init") ||
            word_is(tokens, count, index, "xorout")) {
            found = parameter_value_after(tokens, count, index, &value,
                                          &negative);
            if (found < 0 || (found > 0 && (negative || value != 0u)))
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
        "deployment", "calendar", "directory", "file"
    };
    static const char *const increment_words[] = {
        "increment", "successor", "advance", "next", "following", "follows",
        "after", "increase", "adding", "forward", "move", "ahead", "cycle",
        "wrap"
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
        "sequence", "double", "doubling", "twice", "multiply", "times"
    };
    Lexeme tokens[RUNTIME_LEXEMES_MAX];
    size_t count = 0;
    int increment, minutes, seconds, crc, policy, compose;
    int named_increment, named_minutes, named_crc, named_policy, named_compose;
    if (lexemes_scan(prompt, tokens, &count) != 0) return 0;
    if (has_word(tokens, count, "both") || has_word(tokens, count, "also") ||
        has_word(tokens, count, "respectively") ||
        has_any_word(tokens, count, forbidden_words,
                     sizeof forbidden_words / sizeof forbidden_words[0]))
        return 0;
    named_increment = contains_ascii_casefold(prompt, "increment_mod256");
    named_minutes = contains_ascii_casefold(prompt, "minutes_to_seconds");
    named_crc = contains_ascii_casefold(prompt, "crc8_atm");
    named_policy = contains_ascii_casefold(prompt, "access_policy_v1");
    named_compose = contains_ascii_casefold(prompt, "compose3_mod256");
    increment = named_increment || has_any_word(
        tokens, count, increment_words,
        sizeof increment_words / sizeof increment_words[0]);
    minutes = named_minutes || has_word(tokens, count, "minute") ||
              has_word(tokens, count, "minutes") ||
              has_word(tokens, count, "min");
    seconds = named_minutes || has_word(tokens, count, "second") ||
              has_word(tokens, count, "seconds") ||
              has_word(tokens, count, "sec");
    crc = named_crc || has_any_word(tokens, count, crc_words,
                                    sizeof crc_words / sizeof crc_words[0]);
    policy = named_policy || has_any_word(
        tokens, count, policy_words,
        sizeof policy_words / sizeof policy_words[0]);
    compose = named_compose || has_any_word(
        tokens, count, compose_words,
        sizeof compose_words / sizeof compose_words[0]);
    switch (intent) {
        case CNET_INTENT_INCREMENT:
            return increment && !crc && !minutes && !seconds && !policy &&
                   !compose;
        case CNET_INTENT_MINUTES:
            return minutes && seconds && !increment && !crc && !policy &&
                   !compose;
        case CNET_INTENT_CRC8:
            if (!crc || increment || minutes || seconds || policy || compose)
                return 0;
            return !has_word(tokens, count, "bytes") &&
                   !has_word(tokens, count, "octets") &&
                   !has_word(tokens, count, "array") &&
                   !has_word(tokens, count, "file") &&
                   !has_word(tokens, count, "sequence") &&
                   !has_word(tokens, count, "two") &&
                   crc_configuration_supported(tokens, count);
        case CNET_INTENT_POLICY:
            return policy && !increment && !minutes && !seconds && !crc &&
                   !compose && policy_version_supported(tokens, count) &&
                   !has_word(tokens, count, "guest") &&
                   !has_word(tokens, count, "role") &&
                   !has_word(tokens, count, "group");
        case CNET_INTENT_COMPOSE3:
            return !crc && !minutes && !seconds && !policy &&
                   compose_operations_ordered(prompt, tokens, count);
        default:
            return 0;
    }
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
    if (!contract_semantics_match(prompt, intent)) return 0;
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
