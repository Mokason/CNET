#include "cnet_compete_eval.h"
#include "cnet_compete_artifacts.h"

#include "cce/cce_campaign_provenance.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define EVAL_LINE_MAX (CNET_COMPETE_EVAL_OUTPUT_MAX * 2u + 512u)
#define EVAL_JOURNAL_MAX (16u * 1024u * 1024u)
#define EVAL_JOURNAL_HEADER_MAX 2048u

typedef struct {
    const unsigned char *cursor;
} JsonParser;

static void set_error(char *error, size_t capacity, const char *format, ...) {
    va_list arguments;
    if (error == NULL || capacity == 0) return;
    va_start(arguments, format);
    (void)vsnprintf(error, capacity, format, arguments);
    va_end(arguments);
}

static int copy_text(char *output, size_t capacity, const char *input) {
    size_t length;
    if (output == NULL || capacity == 0 || input == NULL) return -1;
    length = strlen(input);
    if (length >= capacity) return -1;
    memcpy(output, input, length + 1u);
    return 0;
}

static int split_fields(char *line, char **fields, size_t expected) {
    char *cursor;
    size_t count = 1;
    fields[0] = line;
    for (cursor = line; *cursor != '\0'; ++cursor) {
        if (*cursor != '\t') continue;
        if (count >= expected) return -1;
        *cursor = '\0';
        fields[count++] = cursor + 1;
    }
    return count == expected ? 0 : -1;
}

static int lane_for_intent(const char *intent, CnetCompeteLane *lane) {
    static const char *const names[CNET_COMPETE_LANE_OOD] = {
        "increment_mod256", "minutes_to_seconds", "crc8_atm",
        "access_policy_v1", "compose3_mod256"
    };
    int index;
    for (index = 0; index < CNET_COMPETE_LANE_OOD; ++index) {
        if (strcmp(intent, names[index]) == 0) {
            *lane = (CnetCompeteLane)index;
            return 0;
        }
    }
    return -1;
}

int cnet_compete_eval_load_fixture(const char *path,
                                   CnetCompeteEvalFixture *fixture,
                                   char *error, size_t error_capacity) {
    CnetCompeteFixtureReport validation;
    CnetCompeteEvalRow *rows = NULL;
    char validation_error[256], line[1024];
    FILE *file = NULL;
    size_t line_number = 0, count = 0;
    int rc = -1;
    if (fixture == NULL || path == NULL) return -1;
    memset(fixture, 0, sizeof *fixture);
    memset(&validation, 0, sizeof validation);
    if (cnet_compete_validate_fixture(path, &validation, validation_error,
                                      sizeof validation_error) != 0) {
        set_error(error, error_capacity, "fixture refused: %s", validation_error);
        return -1;
    }
    rows = (CnetCompeteEvalRow *)calloc(validation.total_rows, sizeof rows[0]);
    file = fopen(path, "rb");
    if (rows == NULL || file == NULL) {
        set_error(error, error_capacity, "fixture allocation/open failed");
        goto done;
    }
    while (fgets(line, sizeof line, file) != NULL) {
        char *fields[7];
        size_t length = strlen(line);
        CnetCompeteEvalRow *row;
        char *end = NULL;
        ++line_number;
        if (length == 0 || line[length - 1u] != '\n') goto malformed;
        line[--length] = '\0';
        if (length > 0 && line[length - 1u] == '\r') line[--length] = '\0';
        if (line_number <= 2) continue;
        if (count >= validation.total_rows || split_fields(line, fields, 7) != 0)
            goto malformed;
        row = &rows[count];
        if (copy_text(row->id, sizeof row->id, fields[0]) != 0 ||
            copy_text(row->prompt, sizeof row->prompt, fields[5]) != 0)
            goto malformed;
        if (strcmp(fields[1], "ood") == 0) {
            row->lane = CNET_COMPETE_LANE_OOD;
            row->value_kind = CNET_EVAL_VALUE_NONE;
            if (copy_text(row->intent, sizeof row->intent, "none") != 0)
                goto malformed;
        } else {
            if (lane_for_intent(fields[2], &row->lane) != 0 ||
                copy_text(row->intent, sizeof row->intent, fields[2]) != 0)
                goto malformed;
            if (strcmp(fields[3], "integer") == 0) {
                errno = 0;
                row->expected_integer = strtol(fields[4], &end, 10);
                if (errno != 0 || end == fields[4] || *end != '\0')
                    goto malformed;
                row->value_kind = CNET_EVAL_VALUE_INTEGER;
            } else if (strcmp(fields[3], "string") == 0) {
                row->value_kind = CNET_EVAL_VALUE_STRING;
                if (copy_text(row->expected_string, sizeof row->expected_string,
                              fields[4]) != 0)
                    goto malformed;
            } else {
                goto malformed;
            }
        }
        ++count;
    }
    if (ferror(file) || count != validation.total_rows) goto malformed;
    if (fclose(file) != 0) {
        file = NULL;
        set_error(error, error_capacity, "fixture close failed");
        goto done;
    }
    file = NULL;
    fixture->rows = rows;
    fixture->count = count;
    rows = NULL;
    rc = 0;
    goto done;
malformed:
    set_error(error, error_capacity, "fixture parse mismatch at line %zu",
              line_number);
done:
    if (file != NULL) (void)fclose(file);
    free(rows);
    return rc;
}

void cnet_compete_eval_free_fixture(CnetCompeteEvalFixture *fixture) {
    if (fixture == NULL) return;
    free(fixture->rows);
    memset(fixture, 0, sizeof *fixture);
}

static void json_ws(JsonParser *parser) {
    while (*parser->cursor == ' ' || *parser->cursor == '\t' ||
           *parser->cursor == '\r' || *parser->cursor == '\n')
        ++parser->cursor;
}

static int hex_digit(unsigned char character) {
    if (character >= '0' && character <= '9') return character - '0';
    character = (unsigned char)tolower(character);
    if (character >= 'a' && character <= 'f') return character - 'a' + 10;
    return -1;
}

static int append_utf8(unsigned codepoint, char *output, size_t capacity,
                       size_t *used) {
    unsigned char encoded[4];
    size_t length;
    if (codepoint == 0 || codepoint > 0x10ffffu ||
        (codepoint >= 0xd800u && codepoint <= 0xdfffu))
        return -1;
    if (codepoint <= 0x7fu) {
        encoded[0] = (unsigned char)codepoint;
        length = 1;
    } else if (codepoint <= 0x7ffu) {
        encoded[0] = (unsigned char)(0xc0u | (codepoint >> 6));
        encoded[1] = (unsigned char)(0x80u | (codepoint & 0x3fu));
        length = 2;
    } else if (codepoint <= 0xffffu) {
        encoded[0] = (unsigned char)(0xe0u | (codepoint >> 12));
        encoded[1] = (unsigned char)(0x80u | ((codepoint >> 6) & 0x3fu));
        encoded[2] = (unsigned char)(0x80u | (codepoint & 0x3fu));
        length = 3;
    } else {
        encoded[0] = (unsigned char)(0xf0u | (codepoint >> 18));
        encoded[1] = (unsigned char)(0x80u | ((codepoint >> 12) & 0x3fu));
        encoded[2] = (unsigned char)(0x80u | ((codepoint >> 6) & 0x3fu));
        encoded[3] = (unsigned char)(0x80u | (codepoint & 0x3fu));
        length = 4;
    }
    if (output != NULL) {
        if (*used + length >= capacity) return -1;
        memcpy(output + *used, encoded, length);
    }
    *used += length;
    return 0;
}

static int parse_hex4(JsonParser *parser, unsigned *value) {
    int digit, index;
    unsigned result = 0;
    for (index = 0; index < 4; ++index) {
        if (parser->cursor[index] == '\0') return -1;
        digit = hex_digit(parser->cursor[index]);
        if (digit < 0) return -1;
        result = (result << 4) | (unsigned)digit;
    }
    parser->cursor += 4;
    *value = result;
    return 0;
}

static int json_string(JsonParser *parser, char *output, size_t capacity) {
    size_t used = 0;
    if (*parser->cursor++ != '"') return -1;
    while (*parser->cursor != '\0' && *parser->cursor != '"') {
        unsigned codepoint = *parser->cursor++;
        if (codepoint < 0x20u) return -1;
        if (codepoint == '\\') {
            unsigned escape = *parser->cursor++;
            switch (escape) {
                case '"': codepoint = '"'; break;
                case '\\': codepoint = '\\'; break;
                case '/': codepoint = '/'; break;
                case 'b': codepoint = '\b'; break;
                case 'f': codepoint = '\f'; break;
                case 'n': codepoint = '\n'; break;
                case 'r': codepoint = '\r'; break;
                case 't': codepoint = '\t'; break;
                case 'u': {
                    unsigned first, second;
                    if (parse_hex4(parser, &first) != 0) return -1;
                    if (first >= 0xd800u && first <= 0xdbffu) {
                        if (parser->cursor[0] != '\\' ||
                            parser->cursor[1] != 'u')
                            return -1;
                        parser->cursor += 2;
                        if (parse_hex4(parser, &second) != 0 ||
                            second < 0xdc00u || second > 0xdfffu)
                            return -1;
                        codepoint = 0x10000u + ((first - 0xd800u) << 10) +
                                    (second - 0xdc00u);
                    } else {
                        codepoint = first;
                    }
                    break;
                }
                default: return -1;
            }
        }
        if (append_utf8(codepoint, output, capacity, &used) != 0) return -1;
    }
    if (*parser->cursor++ != '"') return -1;
    if (output != NULL) {
        if (used >= capacity) return -1;
        output[used] = '\0';
    }
    return 0;
}

static int json_literal(JsonParser *parser, const char *literal) {
    size_t length = strlen(literal);
    if (strncmp((const char *)parser->cursor, literal, length) != 0) return -1;
    parser->cursor += length;
    return 0;
}

static int json_number_skip(JsonParser *parser) {
    const unsigned char *start = parser->cursor;
    if (*parser->cursor == '-') ++parser->cursor;
    if (*parser->cursor == '0') {
        ++parser->cursor;
        if (isdigit(*parser->cursor)) return -1;
    } else if (*parser->cursor >= '1' && *parser->cursor <= '9') {
        while (isdigit(*parser->cursor)) ++parser->cursor;
    } else {
        return -1;
    }
    if (*parser->cursor == '.') {
        ++parser->cursor;
        if (!isdigit(*parser->cursor)) return -1;
        while (isdigit(*parser->cursor)) ++parser->cursor;
    }
    if (*parser->cursor == 'e' || *parser->cursor == 'E') {
        ++parser->cursor;
        if (*parser->cursor == '+' || *parser->cursor == '-') ++parser->cursor;
        if (!isdigit(*parser->cursor)) return -1;
        while (isdigit(*parser->cursor)) ++parser->cursor;
    }
    return parser->cursor == start ? -1 : 0;
}

static int json_skip(JsonParser *parser, unsigned depth);

static int json_array_skip(JsonParser *parser, unsigned depth) {
    if (*parser->cursor++ != '[') return -1;
    json_ws(parser);
    if (*parser->cursor == ']') { ++parser->cursor; return 0; }
    for (;;) {
        if (json_skip(parser, depth + 1u) != 0) return -1;
        json_ws(parser);
        if (*parser->cursor == ']') { ++parser->cursor; return 0; }
        if (*parser->cursor++ != ',') return -1;
        json_ws(parser);
    }
}

static int json_object_skip(JsonParser *parser, unsigned depth) {
    if (*parser->cursor++ != '{') return -1;
    json_ws(parser);
    if (*parser->cursor == '}') { ++parser->cursor; return 0; }
    for (;;) {
        if (json_string(parser, NULL, 0) != 0) return -1;
        json_ws(parser);
        if (*parser->cursor++ != ':') return -1;
        json_ws(parser);
        if (json_skip(parser, depth + 1u) != 0) return -1;
        json_ws(parser);
        if (*parser->cursor == '}') { ++parser->cursor; return 0; }
        if (*parser->cursor++ != ',') return -1;
        json_ws(parser);
    }
}

static int json_skip(JsonParser *parser, unsigned depth) {
    if (depth > 32u) return -1;
    json_ws(parser);
    switch (*parser->cursor) {
        case '"': return json_string(parser, NULL, 0);
        case '{': return json_object_skip(parser, depth);
        case '[': return json_array_skip(parser, depth);
        case 't': return json_literal(parser, "true");
        case 'f': return json_literal(parser, "false");
        case 'n': return json_literal(parser, "null");
        default: return json_number_skip(parser);
    }
}

static int json_long(JsonParser *parser, long *value_out) {
    unsigned long long value = 0, limit = (unsigned long long)LONG_MAX;
    int negative = 0;
    if (*parser->cursor == '-') { negative = 1; ++parser->cursor; }
    if (*parser->cursor == '0') {
        ++parser->cursor;
        if (isdigit(*parser->cursor)) return -1;
    } else if (*parser->cursor >= '1' && *parser->cursor <= '9') {
        do {
            unsigned digit = (unsigned)(*parser->cursor++ - '0');
            if (value > (limit - digit) / 10u) return -1;
            value = value * 10u + digit;
        } while (isdigit(*parser->cursor));
    } else {
        return -1;
    }
    if (*parser->cursor == '.' || *parser->cursor == 'e' ||
        *parser->cursor == 'E')
        return -1;
    if (negative) {
        if (value > (unsigned long long)LONG_MAX + 1u) return -1;
        *value_out = value == (unsigned long long)LONG_MAX + 1u
                         ? LONG_MIN : -(long)value;
    } else {
        *value_out = (long)value;
    }
    return 0;
}

int cnet_compete_eval_parse_result(const char *json,
                                   CnetCompeteParsedResult *result) {
    JsonParser parser;
    char status[16] = "";
    unsigned seen = 0;
    if (json == NULL || result == NULL) return -1;
    memset(result, 0, sizeof *result);
    parser.cursor = (const unsigned char *)json;
    json_ws(&parser);
    if (*parser.cursor++ != '{') return -1;
    json_ws(&parser);
    if (*parser.cursor == '}') return -1;
    for (;;) {
        char key[16];
        unsigned bit;
        if (json_string(&parser, key, sizeof key) != 0) return -1;
        if (strcmp(key, "status") == 0) bit = 1u;
        else if (strcmp(key, "intent") == 0) bit = 2u;
        else if (strcmp(key, "value") == 0) bit = 4u;
        else return -1;
        if (seen & bit) return -1;
        seen |= bit;
        json_ws(&parser);
        if (*parser.cursor++ != ':') return -1;
        json_ws(&parser);
        if (bit == 1u) {
            if (json_string(&parser, status, sizeof status) != 0) return -1;
        } else if (bit == 2u) {
            if (json_string(&parser, result->intent,
                            sizeof result->intent) != 0)
                return -1;
        } else if (*parser.cursor == '"') {
            if (json_string(&parser, result->string,
                            sizeof result->string) != 0)
                return -1;
            result->value_kind = CNET_EVAL_VALUE_STRING;
        } else {
            if (json_long(&parser, &result->integer) != 0) return -1;
            result->value_kind = CNET_EVAL_VALUE_INTEGER;
        }
        json_ws(&parser);
        if (*parser.cursor == '}') { ++parser.cursor; break; }
        if (*parser.cursor++ != ',') return -1;
        json_ws(&parser);
    }
    json_ws(&parser);
    if (*parser.cursor != '\0') return -1;
    if (strcmp(status, "abstain") == 0) {
        if (seen != 1u) return -1;
        result->answered = 0;
        result->value_kind = CNET_EVAL_VALUE_NONE;
        return 0;
    }
    if (strcmp(status, "answer") != 0 || seen != 7u ||
        result->intent[0] == '\0' ||
        result->value_kind == CNET_EVAL_VALUE_NONE)
        return -1;
    if (result->value_kind == CNET_EVAL_VALUE_STRING) {
        if (strcmp(result->intent, "access_policy_v1") != 0 ||
            (strcmp(result->string, "allow") != 0 &&
             strcmp(result->string, "deny") != 0))
            return -1;
    } else if (strcmp(result->intent, "minutes_to_seconds") == 0) {
        if (result->integer < 0 || result->integer > 15300 ||
            result->integer % 60 != 0)
            return -1;
    } else if (strcmp(result->intent, "increment_mod256") == 0 ||
               strcmp(result->intent, "crc8_atm") == 0 ||
               strcmp(result->intent, "compose3_mod256") == 0) {
        if (result->integer < 0 || result->integer > 255) return -1;
    } else {
        return -1;
    }
    result->answered = 1;
    return 0;
}

int cnet_compete_eval_response_correct(const CnetCompeteEvalRow *row,
                                       const CnetCompeteParsedResult *result) {
    if (row == NULL || result == NULL) return 0;
    if (row->lane == CNET_COMPETE_LANE_OOD) return !result->answered;
    if (!result->answered || strcmp(row->intent, result->intent) != 0 ||
        row->value_kind != result->value_kind)
        return 0;
    if (row->value_kind == CNET_EVAL_VALUE_INTEGER)
        return row->expected_integer == result->integer;
    if (row->value_kind == CNET_EVAL_VALUE_STRING)
        return strcmp(row->expected_string, result->string) == 0;
    return 0;
}

static int find_content_message(JsonParser *parser, char *content,
                                size_t capacity) {
    char role[32] = "";
    unsigned seen = 0;
    if (*parser->cursor++ != '{') return -1;
    json_ws(parser);
    if (*parser->cursor == '}') return -1;
    for (;;) {
        char key[32];
        if (json_string(parser, key, sizeof key) != 0) return -1;
        json_ws(parser);
        if (*parser->cursor++ != ':') return -1;
        json_ws(parser);
        if (strcmp(key, "content") == 0) {
            if ((seen & 1u) || json_string(parser, content, capacity) != 0)
                return -1;
            seen |= 1u;
        } else if (strcmp(key, "role") == 0) {
            if ((seen & 2u) || json_string(parser, role, sizeof role) != 0)
                return -1;
            seen |= 2u;
        } else if (json_skip(parser, 1) != 0) {
            return -1;
        }
        json_ws(parser);
        if (*parser->cursor == '}') {
            ++parser->cursor;
            return seen == 3u && strcmp(role, "assistant") == 0 ? 0 : -1;
        }
        if (*parser->cursor++ != ',') return -1;
        json_ws(parser);
    }
}

static int find_content_choice(JsonParser *parser, char *content,
                               size_t capacity) {
    char finish_reason[32] = "";
    unsigned seen = 0;
    if (*parser->cursor++ != '{') return -1;
    json_ws(parser);
    if (*parser->cursor == '}') return -1;
    for (;;) {
        char key[32];
        if (json_string(parser, key, sizeof key) != 0) return -1;
        json_ws(parser);
        if (*parser->cursor++ != ':') return -1;
        json_ws(parser);
        if (strcmp(key, "message") == 0) {
            if ((seen & 1u) ||
                find_content_message(parser, content, capacity) != 0)
                return -1;
            seen |= 1u;
        } else if (strcmp(key, "finish_reason") == 0) {
            if ((seen & 2u) ||
                json_string(parser, finish_reason, sizeof finish_reason) != 0)
                return -1;
            seen |= 2u;
        } else if (json_skip(parser, 1) != 0) {
            return -1;
        }
        json_ws(parser);
        if (*parser->cursor == '}') {
            ++parser->cursor;
            return seen == 3u && strcmp(finish_reason, "stop") == 0 ? 0 : -1;
        }
        if (*parser->cursor++ != ',') return -1;
        json_ws(parser);
    }
}

static int find_content_choices(JsonParser *parser, char *content,
                                size_t capacity) {
    if (*parser->cursor++ != '[') return -1;
    json_ws(parser);
    if (*parser->cursor == ']') return -1;
    if (find_content_choice(parser, content, capacity) != 0) return -1;
    json_ws(parser);
    if (*parser->cursor++ != ']') return -1;
    return 0;
}

int cnet_compete_eval_extract_chat_content(const char *response,
                                           const char *expected_model,
                                           char *content, size_t capacity) {
    JsonParser parser;
    char model[1024] = "";
    unsigned seen = 0;
    if (response == NULL || expected_model == NULL || content == NULL ||
        capacity == 0)
        return -1;
    content[0] = '\0';
    parser.cursor = (const unsigned char *)response;
    json_ws(&parser);
    if (*parser.cursor++ != '{') return -1;
    json_ws(&parser);
    if (*parser.cursor == '}') return -1;
    for (;;) {
        char key[32];
        if (json_string(&parser, key, sizeof key) != 0) return -1;
        json_ws(&parser);
        if (*parser.cursor++ != ':') return -1;
        json_ws(&parser);
        if (strcmp(key, "choices") == 0) {
            if ((seen & 1u) ||
                find_content_choices(&parser, content, capacity) != 0)
                return -1;
            seen |= 1u;
        } else if (strcmp(key, "model") == 0) {
            if ((seen & 2u) ||
                json_string(&parser, model, sizeof model) != 0)
                return -1;
            seen |= 2u;
        } else if (json_skip(&parser, 1) != 0) {
            return -1;
        }
        json_ws(&parser);
        if (*parser.cursor == '}') {
            ++parser.cursor;
            json_ws(&parser);
            return seen == 3u && strcmp(model, expected_model) == 0 &&
                           *parser.cursor == '\0'
                       ? 0 : -1;
        }
        if (*parser.cursor++ != ',') return -1;
        json_ws(&parser);
    }
}

static int json_unsigned_long_long(JsonParser *parser,
                                   unsigned long long *value_out) {
    unsigned long long value = 0;
    if (!isdigit(*parser->cursor)) return -1;
    if (*parser->cursor == '0' && isdigit(parser->cursor[1])) return -1;
    do {
        unsigned digit = (unsigned)(*parser->cursor++ - '0');
        if (value > (ULLONG_MAX - digit) / 10u) return -1;
        value = value * 10u + digit;
    } while (isdigit(*parser->cursor));
    if (*parser->cursor == '.' || *parser->cursor == 'e' ||
        *parser->cursor == 'E')
        return -1;
    *value_out = value;
    return 0;
}

static int models_meta_matches(JsonParser *parser,
                               unsigned long long expected_parameters,
                               const char *expected_ftype) {
    char ftype[32] = "";
    unsigned long long parameters = 0;
    unsigned seen = 0;
    if (*parser->cursor++ != '{') return 0;
    json_ws(parser);
    if (*parser->cursor == '}') return 0;
    for (;;) {
        char key[32];
        if (json_string(parser, key, sizeof key) != 0) return 0;
        json_ws(parser);
        if (*parser->cursor++ != ':') return 0;
        json_ws(parser);
        if (strcmp(key, "n_params") == 0) {
            if ((seen & 1u) ||
                json_unsigned_long_long(parser, &parameters) != 0)
                return 0;
            seen |= 1u;
        } else if (strcmp(key, "ftype") == 0) {
            if ((seen & 2u) || json_string(parser, ftype, sizeof ftype) != 0)
                return 0;
            seen |= 2u;
        } else if (json_skip(parser, 2) != 0) {
            return 0;
        }
        json_ws(parser);
        if (*parser->cursor == '}') {
            ++parser->cursor;
            return seen == 3u && parameters == expected_parameters &&
                   strcmp(ftype, expected_ftype) == 0;
        }
        if (*parser->cursor++ != ',') return 0;
        json_ws(parser);
    }
}

static int models_entry_matches(JsonParser *parser, const char *expected_model,
                                unsigned long long expected_parameters,
                                const char *expected_ftype) {
    char id[1024] = "";
    unsigned seen = 0;
    int meta_matches = 0;
    if (*parser->cursor++ != '{') return 0;
    json_ws(parser);
    if (*parser->cursor == '}') return 0;
    for (;;) {
        char key[32];
        if (json_string(parser, key, sizeof key) != 0) return 0;
        json_ws(parser);
        if (*parser->cursor++ != ':') return 0;
        json_ws(parser);
        if (strcmp(key, "id") == 0) {
            if ((seen & 1u) || json_string(parser, id, sizeof id) != 0)
                return 0;
            seen |= 1u;
        } else if (strcmp(key, "meta") == 0) {
            if ((seen & 2u)) return 0;
            meta_matches = models_meta_matches(parser, expected_parameters,
                                                expected_ftype);
            if (!meta_matches) return 0;
            seen |= 2u;
        } else if (json_skip(parser, 1) != 0) {
            return 0;
        }
        json_ws(parser);
        if (*parser->cursor == '}') {
            ++parser->cursor;
            return seen == 3u && meta_matches &&
                   strcmp(id, expected_model) == 0;
        }
        if (*parser->cursor++ != ',') return 0;
        json_ws(parser);
    }
}

static int models_data_matches(JsonParser *parser, const char *expected_model,
                               unsigned long long expected_parameters,
                               const char *expected_ftype) {
    if (*parser->cursor++ != '[') return 0;
    json_ws(parser);
    if (!models_entry_matches(parser, expected_model, expected_parameters,
                              expected_ftype))
        return 0;
    json_ws(parser);
    if (*parser->cursor++ != ']') return 0;
    return 1;
}

int cnet_compete_eval_models_response_matches(
    const char *response, const char *expected_model,
    unsigned long long expected_parameters, const char *expected_ftype) {
    JsonParser parser;
    int found = 0;
    if (response == NULL || expected_model == NULL || expected_ftype == NULL)
        return 0;
    parser.cursor = (const unsigned char *)response;
    json_ws(&parser);
    if (*parser.cursor++ != '{') return 0;
    json_ws(&parser);
    if (*parser.cursor == '}') return 0;
    for (;;) {
        char key[32];
        if (json_string(&parser, key, sizeof key) != 0) return 0;
        json_ws(&parser);
        if (*parser.cursor++ != ':') return 0;
        json_ws(&parser);
        if (strcmp(key, "data") == 0) {
            if (found ||
                !models_data_matches(&parser, expected_model,
                                     expected_parameters, expected_ftype))
                return 0;
            found = 1;
        } else if (json_skip(&parser, 1) != 0) {
            return 0;
        }
        json_ws(&parser);
        if (*parser.cursor == '}') {
            ++parser.cursor;
            json_ws(&parser);
            return found && *parser.cursor == '\0';
        }
        if (*parser.cursor++ != ',') return 0;
        json_ws(&parser);
    }
}

int cnet_compete_eval_json_escape(const char *input, char *output,
                                  size_t capacity) {
    static const char hex[] = "0123456789abcdef";
    const unsigned char *cursor = (const unsigned char *)input;
    size_t used = 0;
    if (input == NULL || output == NULL || capacity == 0) return -1;
    while (*cursor != '\0') {
        unsigned char character = *cursor++;
        const char *escape = NULL;
        if (character == '"') escape = "\\\"";
        else if (character == '\\') escape = "\\\\";
        else if (character == '\b') escape = "\\b";
        else if (character == '\f') escape = "\\f";
        else if (character == '\n') escape = "\\n";
        else if (character == '\r') escape = "\\r";
        else if (character == '\t') escape = "\\t";
        if (escape != NULL) {
            size_t length = strlen(escape);
            if (used + length >= capacity) return -1;
            memcpy(output + used, escape, length);
            used += length;
        } else if (character < 0x20u) {
            if (used + 6u >= capacity) return -1;
            output[used++] = '\\'; output[used++] = 'u';
            output[used++] = '0'; output[used++] = '0';
            output[used++] = hex[character >> 4];
            output[used++] = hex[character & 15u];
        } else {
            if (used + 1u >= capacity) return -1;
            output[used++] = (char)character;
        }
    }
    output[used] = '\0';
    return 0;
}

int cnet_compete_eval_hex_encode(const char *input, char *output,
                                 size_t capacity) {
    static const char hex[] = "0123456789abcdef";
    size_t index, length;
    if (input == NULL || output == NULL) return -1;
    length = strlen(input);
    if (length > (SIZE_MAX - 1u) / 2u || length * 2u + 1u > capacity)
        return -1;
    for (index = 0; index < length; ++index) {
        unsigned char byte = (unsigned char)input[index];
        output[index * 2u] = hex[byte >> 4];
        output[index * 2u + 1u] = hex[byte & 15u];
    }
    output[length * 2u] = '\0';
    return 0;
}

int cnet_compete_eval_hex_decode(const char *input, char *output,
                                 size_t capacity) {
    size_t index, length;
    if (input == NULL || output == NULL) return -1;
    length = strlen(input);
    if ((length & 1u) != 0 || length / 2u + 1u > capacity) return -1;
    for (index = 0; index < length; index += 2u) {
        int high = hex_digit((unsigned char)input[index]);
        int low = hex_digit((unsigned char)input[index + 1u]);
        if (high < 0 || low < 0 || (high == 0 && low == 0)) return -1;
        output[index / 2u] = (char)((high << 4) | low);
    }
    output[length / 2u] = '\0';
    return 0;
}

int cnet_compete_eval_file_sha256(const char *path, char output[65]) {
    return cce_sha256_file_hex(path, output);
}

int cnet_compete_eval_regular_file_size(const char *path,
                                        unsigned long long *size_out) {
    struct stat status;
    if (path == NULL || size_out == NULL || cnet_lstat(path, &status) != 0 ||
        !S_ISREG(status.st_mode) || status.st_size < 0)
        return -1;
    *size_out = (unsigned long long)status.st_size;
    return 0;
}

int cnet_compete_eval_verify_frozen(char *error, size_t error_capacity) {
    static const struct { const char *path; const char *sha; } files[] = {
        {CNET_COMPETE_FROZEN_FIXTURE_PATH, CNET_COMPETE_FIXTURE_SHA256},
        {CNET_COMPETE_FROZEN_SYSTEM_PATH, CNET_COMPETE_SYSTEM_SHA256},
        {CNET_COMPETE_FROZEN_GENERATOR_PATH, CNET_COMPETE_GENERATOR_SHA256}
    };
    char actual[65];
    size_t index;
    for (index = 0; index < sizeof files / sizeof files[0]; ++index) {
        if (cnet_compete_eval_file_sha256(files[index].path, actual) != 0 ||
            strcmp(actual, files[index].sha) != 0) {
            set_error(error, error_capacity, "frozen digest mismatch: %s",
                      files[index].path);
            return -1;
        }
    }
    return 0;
}

static int artifact_directory_exact(const char *path,
                                    const char *const *names,
                                    const int *directories, size_t count) {
    DIR *directory;
    struct dirent *entry;
    struct stat path_status;
    unsigned long long seen = 0;
    int descriptor, rc = -1;
    if (path == NULL || names == NULL || directories == NULL ||
        count == 0 || count >= 64u || cnet_lstat(path, &path_status) != 0 ||
        !S_ISDIR(path_status.st_mode))
        return -1;
    directory = opendir(path);
    if (directory == NULL) return -1;
    descriptor = cnet_dirfd(directory);
    if (descriptor < 0) goto done;
    for (;;) {
        struct stat status;
        size_t index;
        int matched = 0;
        errno = 0;
        entry = readdir(directory);
        if (entry == NULL) {
            if (errno != 0) goto done;
            break;
        }
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0)
            continue;
        for (index = 0; index < count; ++index) {
            if (strcmp(entry->d_name, names[index]) != 0) continue;
            if ((seen & (UINT64_C(1) << index)) != 0 ||
                cnet_fstatat_nofollow(descriptor, path, entry->d_name,
                                      &status) != 0 ||
                (directories[index] ? !S_ISDIR(status.st_mode)
                                    : !S_ISREG(status.st_mode)))
                goto done;
            seen |= UINT64_C(1) << index;
            matched = 1;
            break;
        }
        if (!matched) goto done;
    }
    if (seen == ((UINT64_C(1) << count) - 1u))
        rc = 0;
done:
    if (closedir(directory) != 0) rc = -1;
    return rc;
}

static int artifact_tree_exact(const char *root) {
    static const char *const root_names[] = {
        "intent.wlm", "intent.meta", "artifacts.sha256", "capsules"
    };
    static const int root_directories[] = {0, 0, 0, 1};
    static const char *const capsule_names[] = {
        ".complete", "access_policy_v1", "add3_mod256", "crc8_atm",
        "double_mod256", "increment_mod256", "minutes_to_seconds"
    };
    static const int capsule_directories[] = {0, 1, 1, 1, 1, 1, 1};
    static const char *const unit_names[] = {"manifest.cknow", "unit.cnb"};
    static const int unit_directories[] = {0, 0};
    char path[CNET_COMPETE_EVAL_PATH_MAX];
    int written;
    size_t index;
    written = snprintf(path, sizeof path, "%s/capsules", root);
    if (artifact_directory_exact(
            root, root_names, root_directories,
            sizeof root_names / sizeof root_names[0]) != 0 ||
        written < 0 || (size_t)written >= sizeof path ||
        artifact_directory_exact(
            path, capsule_names, capsule_directories,
            sizeof capsule_names / sizeof capsule_names[0]) != 0)
        return -1;
    for (index = 1; index < sizeof capsule_names / sizeof capsule_names[0];
         ++index) {
        written = snprintf(path, sizeof path, "%s/capsules/%s", root,
                           capsule_names[index]);
        if (written < 0 || (size_t)written >= sizeof path ||
            artifact_directory_exact(
                path, unit_names, unit_directories,
                sizeof unit_names / sizeof unit_names[0]) != 0)
            return -1;
    }
    return 0;
}

static int verify_artifact_manifest_root(
    const char *manifest_path, const char *artifact_root,
    unsigned long long *total_bytes, unsigned long long *capsule_bytes,
    char manifest_sha256[65], char *error, size_t error_capacity) {
    char line[1200];
    unsigned long long total = 0, capsules = 0, manifest_bytes = 0;
    FILE *file = NULL;
    size_t count = 0;
    int rc = -1;
    if (manifest_path == NULL || artifact_root == NULL ||
        total_bytes == NULL || capsule_bytes == NULL || manifest_sha256 == NULL)
        return -1;
    if (artifact_tree_exact(artifact_root) != 0) goto done;
    if (cnet_compete_eval_regular_file_size(manifest_path, &manifest_bytes) != 0)
        goto done;
    total = manifest_bytes;
    file = fopen(manifest_path, "rb");
    if (file == NULL) goto done;
    while (fgets(line, sizeof line, file) != NULL) {
        char actual[65], expected[65], expected_path[1024], actual_path[1024];
        char *path;
        unsigned long long bytes;
        size_t length = strlen(line), character;
        int written;
        if (length == 0 || line[length - 1u] != '\n' ||
            count >= CNET_COMPETE_ARTIFACT_FILE_COUNT)
            goto done;
        line[--length] = '\0';
        if (length < 67u || line[64] != ' ' || line[65] != ' ' ||
            line[66] == '\0')
            goto done;
        memcpy(expected, line, 64);
        expected[64] = '\0';
        for (character = 0; character < 64; ++character)
            if (hex_digit((unsigned char)expected[character]) < 0 ||
                (expected[character] >= 'A' && expected[character] <= 'F'))
                goto done;
        path = line + 66;
        written = snprintf(expected_path, sizeof expected_path, "%s/%s",
                           CNET_COMPETE_MANIFEST_DECLARED_ROOT,
                           cnet_compete_artifact_member(count));
        if (written < 0 || (size_t)written >= sizeof expected_path ||
            strcmp(path, expected_path) != 0)
            goto done;
        written = snprintf(actual_path, sizeof actual_path, "%s/%s",
                           artifact_root, cnet_compete_artifact_member(count));
        if (written < 0 || (size_t)written >= sizeof actual_path ||
            cnet_compete_eval_file_sha256(actual_path, actual) != 0 ||
            strcmp(actual, expected) != 0 ||
            cnet_compete_eval_regular_file_size(actual_path, &bytes) != 0 ||
            ULLONG_MAX - total < bytes)
            goto done;
        total += bytes;
        if (count >= 2u) {
            if (ULLONG_MAX - capsules < bytes) goto done;
            capsules += bytes;
        }
        ++count;
    }
    if (ferror(file) || count != CNET_COMPETE_ARTIFACT_FILE_COUNT) goto done;
    if (fclose(file) != 0) { file = NULL; goto done; }
    file = NULL;
    if (cnet_compete_eval_file_sha256(manifest_path, manifest_sha256) != 0)
        goto done;
    *total_bytes = total;
    *capsule_bytes = capsules;
    rc = 0;
done:
    if (file != NULL) (void)fclose(file);
    if (rc != 0)
        set_error(error, error_capacity, "artifact manifest refused: %s",
                  manifest_path == NULL ? "(null)" : manifest_path);
    return rc;
}

int cnet_compete_eval_verify_artifact_manifest(
    const char *manifest_path, unsigned long long *total_bytes,
    unsigned long long *capsule_bytes, char manifest_sha256[65],
    char *error, size_t error_capacity) {
    return verify_artifact_manifest_root(
        manifest_path, CNET_COMPETE_FROZEN_ARTIFACT_ROOT,
        total_bytes, capsule_bytes,
        manifest_sha256, error, error_capacity);
}

static int snapshot_path(char output[CNET_COMPETE_EVAL_PATH_MAX],
                         const char *root, const char *leaf) {
    int written = snprintf(output, CNET_COMPETE_EVAL_PATH_MAX, "%s/%s",
                           root, leaf);
    return written < 0 || written >= CNET_COMPETE_EVAL_PATH_MAX ? -1 : 0;
}

static int snapshot_copy_regular(const char *source, const char *destination) {
    unsigned char buffer[65536];
    struct stat status;
    int input = -1, output = -1, rc = -1;
    input = open(source, O_RDONLY | CNET_O_NOFOLLOW);
    if (input < 0 || fstat(input, &status) != 0 || !S_ISREG(status.st_mode) ||
        status.st_size <= 0 ||
        (unsigned long long)status.st_size > 16u * 1024u * 1024u)
        goto done;
    output = open(destination, O_WRONLY | O_CREAT | O_EXCL | CNET_O_NOFOLLOW, 0600);
    if (output < 0) goto done;
    for (;;) {
        ssize_t count = read(input, buffer, sizeof buffer);
        size_t offset = 0;
        if (count < 0) goto done;
        if (count == 0) break;
        while (offset < (size_t)count) {
            ssize_t written = write(output, buffer + offset,
                                    (size_t)count - offset);
            if (written <= 0) goto done;
            offset += (size_t)written;
        }
    }
    if (cnet_fsync(output) != 0) goto done;
    rc = 0;
done:
    if (output >= 0 && close(output) != 0) rc = -1;
    if (input >= 0 && close(input) != 0) rc = -1;
    return rc;
}

static int snapshot_copy_verified(const char *source, const char *destination,
                                  const char *expected_sha256) {
    char actual[65];
    return snapshot_copy_regular(source, destination) == 0 &&
           cnet_compete_eval_file_sha256(destination, actual) == 0 &&
           strcmp(actual, expected_sha256) == 0 ? 0 : -1;
}

void cnet_compete_eval_snapshot_destroy(CnetCompeteEvalSnapshot *snapshot) {
    static const char *const directories[] = {
        "artifacts/capsules/access_policy_v1",
        "artifacts/capsules/add3_mod256",
        "artifacts/capsules/crc8_atm",
        "artifacts/capsules/double_mod256",
        "artifacts/capsules/increment_mod256",
        "artifacts/capsules/minutes_to_seconds",
        "artifacts/capsules",
        "artifacts"
    };
    static const char prefix[] = "/tmp/cnet-asi5-snapshot-";
    char root[CNET_COMPETE_EVAL_PATH_MAX];
    char path[CNET_COMPETE_EVAL_PATH_MAX];
    size_t index;
    struct stat status;
    if (snapshot == NULL || strlen(snapshot->root) != strlen(prefix) + 6u ||
        strncmp(snapshot->root, prefix, strlen(prefix)) != 0 ||
        strchr(snapshot->root + strlen(prefix), '/') != NULL ||
        cnet_lstat(snapshot->root, &status) != 0 || !S_ISDIR(status.st_mode) ||
        copy_text(root, sizeof root, snapshot->root) != 0)
        return;
    for (index = 0; index < CNET_COMPETE_ARTIFACT_FILE_COUNT; ++index) {
        int written = snprintf(path, sizeof path, "%s/artifacts/%s", root,
                               cnet_compete_artifact_member(index));
        if (written >= 0 && (size_t)written < sizeof path)
            (void)unlink(path);
    }
    if (snapshot_path(path, root, "artifacts/artifacts.sha256") == 0)
        (void)unlink(path);
    if (snapshot_path(path, root, "fixture_generator.c") == 0)
        (void)unlink(path);
    if (snapshot_path(path, root, "baseline_system.txt") == 0)
        (void)unlink(path);
    if (snapshot_path(path, root, "heldout.tsv") == 0)
        (void)unlink(path);
    for (index = 0; index < sizeof directories / sizeof directories[0];
         ++index) {
        if (snapshot_path(path, root, directories[index]) == 0)
            (void)rmdir(path);
    }
    (void)rmdir(root);
    memset(snapshot, 0, sizeof *snapshot);
}

int cnet_compete_eval_snapshot_create(CnetCompeteEvalSnapshot *snapshot,
                                      int include_artifacts,
                                      char *error, size_t error_capacity) {
    static const char *const artifact_directories[] = {
        "artifacts", "artifacts/capsules",
        "artifacts/capsules/access_policy_v1",
        "artifacts/capsules/add3_mod256",
        "artifacts/capsules/crc8_atm",
        "artifacts/capsules/double_mod256",
        "artifacts/capsules/increment_mod256",
        "artifacts/capsules/minutes_to_seconds"
    };
    char template[] = "/tmp/cnet-asi5-snapshot-XXXXXX";
    char source[CNET_COMPETE_EVAL_PATH_MAX];
    char destination[CNET_COMPETE_EVAL_PATH_MAX];
    size_t index;
    if (snapshot == NULL || (include_artifacts != 0 && include_artifacts != 1))
        return -1;
    memset(snapshot, 0, sizeof *snapshot);
    if (cnet_mkdtemp(template) == NULL ||
        copy_text(snapshot->root, sizeof snapshot->root, template) != 0 ||
        snapshot_path(snapshot->fixture, snapshot->root, "heldout.tsv") != 0 ||
        snapshot_path(snapshot->system, snapshot->root,
                      "baseline_system.txt") != 0 ||
        snapshot_path(snapshot->generator, snapshot->root,
                      "fixture_generator.c") != 0 ||
        snapshot_copy_verified(CNET_COMPETE_FROZEN_FIXTURE_PATH,
                               snapshot->fixture,
                               CNET_COMPETE_FIXTURE_SHA256) != 0 ||
        snapshot_copy_verified(CNET_COMPETE_FROZEN_SYSTEM_PATH,
                               snapshot->system,
                               CNET_COMPETE_SYSTEM_SHA256) != 0 ||
        snapshot_copy_verified(CNET_COMPETE_FROZEN_GENERATOR_PATH,
                               snapshot->generator,
                               CNET_COMPETE_GENERATOR_SHA256) != 0)
        goto fail;
    if (!include_artifacts) return 0;
    if (snapshot_path(snapshot->artifact_root, snapshot->root, "artifacts") != 0)
        goto fail;
    snapshot->has_artifacts = 1;
    for (index = 0;
         index < sizeof artifact_directories / sizeof artifact_directories[0];
         ++index) {
        if (snapshot_path(destination, snapshot->root,
                          artifact_directories[index]) != 0 ||
            cnet_mkdir(destination, 0700) != 0)
            goto fail;
    }
    if (snapshot_path(snapshot->manifest, snapshot->artifact_root,
                      "artifacts.sha256") != 0 ||
        snapshot_copy_regular(CNET_COMPETE_FROZEN_ARTIFACT_ROOT
                                  "/artifacts.sha256",
                              snapshot->manifest) != 0)
        goto fail;
    for (index = 0; index < CNET_COMPETE_ARTIFACT_FILE_COUNT; ++index) {
        if (snapshot_path(source, CNET_COMPETE_FROZEN_ARTIFACT_ROOT,
                          cnet_compete_artifact_member(index)) != 0 ||
            snapshot_path(destination, snapshot->artifact_root,
                          cnet_compete_artifact_member(index)) != 0 ||
            snapshot_copy_regular(source, destination) != 0)
            goto fail;
    }
    if (verify_artifact_manifest_root(
            snapshot->manifest, snapshot->artifact_root,
            &snapshot->artifact_bytes, &snapshot->capsule_bytes,
            snapshot->artifact_sha256, error, error_capacity) != 0 ||
        snapshot_path(snapshot->model, snapshot->artifact_root, "intent.wlm") != 0 ||
        snapshot_path(snapshot->metadata, snapshot->artifact_root,
                      "intent.meta") != 0 ||
        snapshot_path(snapshot->capsule_root, snapshot->artifact_root,
                      "capsules") != 0)
        goto fail;
    return 0;
fail:
    set_error(error, error_capacity, "evaluation snapshot refused");
    cnet_compete_eval_snapshot_destroy(snapshot);
    return -1;
}

int cnet_compete_eval_snapshot_verify(
    const CnetCompeteEvalSnapshot *snapshot,
    char *error, size_t error_capacity) {
    char actual[65], artifact_sha[65];
    unsigned long long artifact_bytes = 0, capsule_bytes = 0;
    if (snapshot == NULL || snapshot->root[0] == '\0' ||
        cnet_compete_eval_file_sha256(snapshot->fixture, actual) != 0 ||
        strcmp(actual, CNET_COMPETE_FIXTURE_SHA256) != 0 ||
        cnet_compete_eval_file_sha256(snapshot->system, actual) != 0 ||
        strcmp(actual, CNET_COMPETE_SYSTEM_SHA256) != 0 ||
        cnet_compete_eval_file_sha256(snapshot->generator, actual) != 0 ||
        strcmp(actual, CNET_COMPETE_GENERATOR_SHA256) != 0)
        goto fail;
    if (snapshot->has_artifacts &&
        (verify_artifact_manifest_root(
             snapshot->manifest, snapshot->artifact_root,
             &artifact_bytes, &capsule_bytes, artifact_sha,
             error, error_capacity) != 0 ||
         artifact_bytes != snapshot->artifact_bytes ||
         capsule_bytes != snapshot->capsule_bytes ||
         strcmp(artifact_sha, snapshot->artifact_sha256) != 0))
        goto fail;
    return 0;
fail:
    set_error(error, error_capacity, "evaluation snapshot changed");
    return -1;
}

uint64_t cnet_compete_eval_now_ns(void) {
    struct timespec timestamp;
    if (clock_gettime(CLOCK_MONOTONIC, &timestamp) != 0) return 0;
    return (uint64_t)timestamp.tv_sec * UINT64_C(1000000000) +
           (uint64_t)timestamp.tv_nsec;
}

int cnet_compete_eval_git_identity_valid(const char *commit,
                                         const char *tree) {
    size_t index;
    if (commit == NULL || tree == NULL || strlen(commit) != 40u ||
        strlen(tree) != 40u)
        return 0;
    for (index = 0; index < 40u; ++index) {
        if (hex_digit((unsigned char)commit[index]) < 0 ||
            hex_digit((unsigned char)tree[index]) < 0 ||
            (commit[index] >= 'A' && commit[index] <= 'F') ||
            (tree[index] >= 'A' && tree[index] <= 'F'))
            return 0;
    }
    return 1;
}

static int sync_directory_exact(const char *path) {
    struct stat status;
    int descriptor, rc;
    if (path == NULL || cnet_lstat(path, &status) != 0 ||
        !S_ISDIR(status.st_mode) || status.st_uid != cnet_geteuid() ||
        (status.st_mode & 0022) != 0)
        return -1;
    descriptor = open(path, O_RDONLY | CNET_O_DIRECTORY | CNET_O_NOFOLLOW);
    if (descriptor < 0) return -1;
    rc = cnet_fsync(descriptor);
    if (close(descriptor) != 0) rc = -1;
    return rc;
}

static int fsync_parent_directory(const char *path);

int cnet_compete_eval_prepare_results_directory(char *error,
                                                size_t error_capacity) {
    static const char *const directories[] = {
        "/home/marble/.local",
        "/home/marble/.local/state",
        "/home/marble/.local/state/cnet",
        CNET_COMPETE_STATE_ROOT,
        CNET_COMPETE_RELEASE_BIN_DIRECTORY,
        CNET_COMPETE_EVIDENCE_DIRECTORY,
        CNET_COMPETE_INPUT_DIRECTORY,
        CNET_COMPETE_RESULTS_DIRECTORY
    };
    struct stat status;
    size_t index;
    (void)umask(0077);
    for (index = 0; index < sizeof directories / sizeof directories[0];
         ++index) {
        if (cnet_mkdir(directories[index], 0700) != 0 &&
            errno != EEXIST)
            goto fail;
        if (cnet_lstat(directories[index], &status) != 0 ||
            !S_ISDIR(status.st_mode) || status.st_uid != cnet_geteuid() ||
            (index <= 1u ? (status.st_mode & 0022) != 0
                         : (status.st_mode & 0777) != 0700))
            goto fail;
    }
    for (index = sizeof directories / sizeof directories[0]; index > 0;
         --index)
        if (sync_directory_exact(directories[index - 1u]) != 0)
            goto fail;
    return 0;
fail:
    set_error(error, error_capacity,
              "canonical results directory refused");
    return -1;
}

static int parse_unsigned(const char *text, unsigned long long *value) {
    char *end = NULL;
    const unsigned char *cursor;
    if (text == NULL || *text == '\0' ||
        (text[0] == '0' && text[1] != '\0'))
        return -1;
    for (cursor = (const unsigned char *)text; *cursor != '\0'; ++cursor)
        if (!isdigit(*cursor)) return -1;
    errno = 0;
    *value = strtoull(text, &end, 10);
    return errno == 0 && end != text && *end == '\0' ? 0 : -1;
}

static int header_equal(const CnetCompeteJournalHeader *left,
                        const CnetCompeteJournalHeader *right) {
    return strcmp(left->backend, right->backend) == 0 &&
           strcmp(left->identity, right->identity) == 0 &&
           left->parameters == right->parameters &&
           left->model_bytes == right->model_bytes &&
           left->artifact_bytes == right->artifact_bytes &&
           left->capsule_payload_bytes == right->capsule_payload_bytes &&
           left->capsule_artifact_bytes == right->capsule_artifact_bytes &&
           left->imported_units == right->imported_units &&
           left->certified_rows == right->certified_rows &&
           left->composition_members == right->composition_members &&
           left->residual_calls == right->residual_calls;
}

static int format_journal_header(char *output, size_t capacity,
                                 const CnetCompeteJournalHeader *header) {
    int written;
    if (output == NULL || capacity == 0 || header == NULL ||
        header->backend[0] == '\0' || header->identity[0] == '\0' ||
        strchr(header->backend, '\n') != NULL ||
        strchr(header->backend, '\r') != NULL ||
        strchr(header->backend, '\t') != NULL ||
        strchr(header->identity, '\n') != NULL ||
        strchr(header->identity, '\r') != NULL ||
        strchr(header->identity, '\t') != NULL)
        return -1;
    written = snprintf(
                output, capacity,
                "CNET_ASI5_RESULTS 4\n"
                "suite %s\n"
                "backend %s\n"
                "fixture_sha256 %s\n"
                "identity %s\n"
                "parameters %llu\n"
                "model_bytes %llu\n"
                "artifact_bytes %llu\n"
                "capsule_payload_bytes %zu\n"
                "capsule_artifact_bytes %llu\n"
                "imported_units %zu\n"
                "certified_rows %zu\n"
                "composition_members %zu\n"
                "residual_calls %zu\n"
                "id\tlatency_ns\trc\tguard_checks\toutput_hex"
                "\trecord_sha256\n",
                CNET_COMPETE_SUITE_ID, header->backend,
                CNET_COMPETE_FIXTURE_SHA256, header->identity,
                header->parameters, header->model_bytes,
                header->artifact_bytes, header->capsule_payload_bytes,
                header->capsule_artifact_bytes,
                header->imported_units,
                header->certified_rows, header->composition_members,
                header->residual_calls);
    return written >= 0 && (size_t)written < capacity ? written : -1;
}

static int journal_chain_seed(const CnetCompeteJournalHeader *header,
                              char output[65]) {
    char canonical[EVAL_JOURNAL_HEADER_MAX];
    int length = format_journal_header(canonical, sizeof canonical, header);
    return length >= 0
               ? cce_sha256_bytes_hex(canonical, (size_t)length, output)
               : -1;
}

static int write_journal_header(FILE *file,
                                const CnetCompeteJournalHeader *header) {
    char canonical[EVAL_JOURNAL_HEADER_MAX];
    int length = format_journal_header(canonical, sizeof canonical, header);
    if (length < 0 || fwrite(canonical, 1, (size_t)length, file) !=
                          (size_t)length ||
        fflush(file) != 0)
        return -1;
    return cnet_fsync(fileno(file));
}

static int read_line_exact(FILE *file, char **line, size_t *capacity,
                           const char *expected) {
    ssize_t length = cnet_getline(line, capacity, file);
    size_t expected_length = strlen(expected);
    if (length < 0 || (size_t)length != expected_length + 1u ||
        memchr(*line, '\0', (size_t)length) != NULL ||
        (*line)[length - 1] != '\n')
        return -1;
    (*line)[length - 1] = '\0';
    return strcmp(*line, expected) == 0 ? 0 : -1;
}

static int read_header_value(FILE *file, char **line, size_t *capacity,
                             const char *key, char *output,
                             size_t output_capacity) {
    ssize_t length = cnet_getline(line, capacity, file);
    size_t key_length = strlen(key);
    if (length < 0 || (size_t)length <= key_length + 1u ||
        memchr(*line, '\0', (size_t)length) != NULL ||
        (*line)[length - 1] != '\n' ||
        strncmp(*line, key, key_length) != 0 ||
        (*line)[key_length] != ' ')
        return -1;
    (*line)[length - 1] = '\0';
    return copy_text(output, output_capacity, *line + key_length + 1u);
}

static int read_header_number(FILE *file, char **line, size_t *capacity,
                              const char *key, unsigned long long *value) {
    char text[64];
    return read_header_value(file, line, capacity, key, text, sizeof text) == 0
               ? parse_unsigned(text, value) : -1;
}

static int parse_journal_header(FILE *file,
                                CnetCompeteJournalHeader *header,
                                char **line, size_t *capacity) {
    char text[128];
    unsigned long long number;
    memset(header, 0, sizeof *header);
    if (read_line_exact(file, line, capacity, "CNET_ASI5_RESULTS 4") != 0 ||
        read_header_value(file, line, capacity, "suite", text,
                          sizeof text) != 0 ||
        strcmp(text, CNET_COMPETE_SUITE_ID) != 0 ||
        read_header_value(file, line, capacity, "backend", header->backend,
                          sizeof header->backend) != 0 ||
        read_header_value(file, line, capacity, "fixture_sha256", text,
                          sizeof text) != 0 ||
        strcmp(text, CNET_COMPETE_FIXTURE_SHA256) != 0 ||
        read_header_value(file, line, capacity, "identity", header->identity,
                          sizeof header->identity) != 0 ||
        read_header_number(file, line, capacity, "parameters",
                           &header->parameters) != 0 ||
        read_header_number(file, line, capacity, "model_bytes",
                           &header->model_bytes) != 0 ||
        read_header_number(file, line, capacity, "artifact_bytes",
                           &header->artifact_bytes) != 0 ||
        read_header_number(file, line, capacity, "capsule_payload_bytes",
                           &number) != 0 || number > SIZE_MAX)
        return -1;
    header->capsule_payload_bytes = (size_t)number;
    if (read_header_number(file, line, capacity, "capsule_artifact_bytes",
                           &header->capsule_artifact_bytes) != 0)
        return -1;
    if (read_header_number(file, line, capacity, "imported_units", &number) != 0 ||
        number > SIZE_MAX)
        return -1;
    header->imported_units = (size_t)number;
    if (read_header_number(file, line, capacity, "certified_rows", &number) != 0 ||
        number > SIZE_MAX) return -1;
    header->certified_rows = (size_t)number;
    if (read_header_number(file, line, capacity, "composition_members",
                           &number) != 0 || number > SIZE_MAX)
        return -1;
    header->composition_members = (size_t)number;
    if (read_header_number(file, line, capacity, "residual_calls", &number) != 0 ||
        number > SIZE_MAX) return -1;
    header->residual_calls = (size_t)number;
    return read_line_exact(file, line, capacity,
                           "id\tlatency_ns\trc\tguard_checks\toutput_hex"
                           "\trecord_sha256");
}

static int parse_journal_row(char *line, const CnetCompeteEvalRow *expected,
                             CnetCompeteJournalRow *output) {
    char *fields[5];
    unsigned long long latency, rc, guards;
    memset(output, 0, sizeof *output);
    if (split_fields(line, fields, 5) != 0 ||
        strcmp(fields[0], expected->id) != 0 ||
        parse_unsigned(fields[1], &latency) != 0 ||
        latency > UINT64_MAX ||
        parse_unsigned(fields[2], &rc) != 0 || rc > INT_MAX ||
        parse_unsigned(fields[3], &guards) != 0 || guards > SIZE_MAX ||
        copy_text(output->id, sizeof output->id, fields[0]) != 0 ||
        cnet_compete_eval_hex_decode(fields[4], output->output,
                                     sizeof output->output) != 0)
        return -1;
    output->latency_ns = (uint64_t)latency;
    output->run_rc = (int)rc;
    output->guard_checks = (size_t)guards;
    return 0;
}

static int valid_sha256(const char *text) {
    size_t index;
    if (text == NULL || strlen(text) != 64u) return 0;
    for (index = 0; index < 64u; ++index)
        if (hex_digit((unsigned char)text[index]) < 0 ||
            (text[index] >= 'A' && text[index] <= 'F'))
            return 0;
    return 1;
}

static int journal_anchor_write(const char *path, size_t record_count,
                                const char chain[65]) {
    char temporary[PATH_MAX], text[192];
    struct stat status;
    FILE *file = NULL;
    int descriptor = -1, written, rc = -1;
    if (path == NULL || !valid_sha256(chain)) return -1;
    if (cnet_lstat(path, &status) == 0) {
        if (!S_ISREG(status.st_mode) || status.st_uid != cnet_geteuid() ||
            status.st_nlink != 1 || (status.st_mode & 0777) != 0600)
            return -1;
    } else if (errno != ENOENT) {
        return -1;
    }
    written = snprintf(text, sizeof text,
                       "CNET_ASI5_ANCHOR 1\nrecords %zu\nchain %s\n",
                       record_count, chain);
    if (written < 0 || (size_t)written >= sizeof text ||
        snprintf(temporary, sizeof temporary, "%s.tmp.XXXXXX", path) < 0 ||
        strlen(temporary) >= sizeof temporary)
        return -1;
    descriptor = mkstemp(temporary);
    if (descriptor < 0 || cnet_fchmod(descriptor, 0600) != 0 ||
        (file = fdopen(descriptor, "wb")) == NULL)
        goto done;
    descriptor = -1;
    if (fwrite(text, 1, (size_t)written, file) != (size_t)written ||
        fflush(file) != 0 || cnet_fsync(fileno(file)) != 0) {
        (void)fclose(file);
        file = NULL;
        goto done;
    }
    if (fclose(file) != 0) {
        file = NULL;
        goto done;
    }
    file = NULL;
    if (rename(temporary, path) != 0 || fsync_parent_directory(path) != 0)
        goto done;
    rc = 0;
done:
    if (file != NULL) (void)fclose(file);
    else if (descriptor >= 0) (void)close(descriptor);
    if (rc != 0) (void)unlink(temporary);
    return rc;
}

/* Returns 1 only when the anchor is absent, 0 when valid, and -1 otherwise. */
static int journal_anchor_read(const char *path, size_t *record_count,
                               char chain[65]) {
    struct stat descriptor_status, path_status;
    FILE *file = NULL;
    char *line = NULL, count_text[64];
    size_t capacity = 0;
    unsigned long long parsed_count;
    int descriptor = -1, rc = -1;
    if (path == NULL || record_count == NULL || chain == NULL) return -1;
    descriptor = open(path, O_RDONLY | CNET_O_NOFOLLOW);
    if (descriptor < 0) return errno == ENOENT ? 1 : -1;
    if (fstat(descriptor, &descriptor_status) != 0 ||
        cnet_lstat(path, &path_status) != 0 ||
        !S_ISREG(descriptor_status.st_mode) ||
        !S_ISREG(path_status.st_mode) ||
        descriptor_status.st_uid != cnet_geteuid() ||
        path_status.st_uid != cnet_geteuid() ||
        descriptor_status.st_nlink != 1 || path_status.st_nlink != 1 ||
        (descriptor_status.st_mode & 0777) != 0600 ||
        (path_status.st_mode & 0777) != 0600 ||
        descriptor_status.st_dev != path_status.st_dev ||
        descriptor_status.st_ino != path_status.st_ino ||
        (file = fdopen(descriptor, "rb")) == NULL)
        goto done;
    descriptor = -1;
    if (read_line_exact(file, &line, &capacity, "CNET_ASI5_ANCHOR 1") != 0 ||
        read_header_value(file, &line, &capacity, "records", count_text,
                          sizeof count_text) != 0 ||
        parse_unsigned(count_text, &parsed_count) != 0 ||
        parsed_count > SIZE_MAX ||
        read_header_value(file, &line, &capacity, "chain", chain, 65) != 0 ||
        !valid_sha256(chain) || fgetc(file) != EOF || ferror(file))
        goto done;
    if (fclose(file) != 0) {
        file = NULL;
        goto done;
    }
    file = NULL;
    *record_count = (size_t)parsed_count;
    rc = 0;
done:
    if (file != NULL) (void)fclose(file);
    else if (descriptor >= 0) (void)close(descriptor);
    free(line);
    return rc;
}

static int journal_record_digest(const char previous[65], const char *record,
                                 char output[65]) {
    static const char domain[] = "CNET_ASI5_JOURNAL_RECORD_V1\n";
    unsigned char *input = NULL;
    size_t domain_length = sizeof domain - 1u;
    size_t record_length, input_length, offset = 0;
    int rc = -1;
    if (!valid_sha256(previous) || record == NULL ||
        strchr(record, '\n') != NULL || strchr(record, '\r') != NULL)
        return -1;
    record_length = strlen(record);
    if (record_length > EVAL_LINE_MAX ||
        record_length > SIZE_MAX - domain_length - 66u)
        return -1;
    input_length = domain_length + 64u + 1u + record_length + 1u;
    input = (unsigned char *)malloc(input_length);
    if (input == NULL) return -1;
    memcpy(input + offset, domain, domain_length);
    offset += domain_length;
    memcpy(input + offset, previous, 64u);
    offset += 64u;
    input[offset++] = '\n';
    memcpy(input + offset, record, record_length);
    offset += record_length;
    input[offset++] = '\n';
    if (offset == input_length)
        rc = cce_sha256_bytes_hex(input, input_length, output);
    free(input);
    return rc;
}

static int journal_record_verify(char *line, char chain[65]) {
    char expected[65], *separator;
    if (line == NULL || chain == NULL ||
        (separator = strrchr(line, '\t')) == NULL || separator == line ||
        !valid_sha256(separator + 1u))
        return -1;
    *separator = '\0';
    if (journal_record_digest(chain, line, expected) != 0 ||
        strcmp(expected, separator + 1u) != 0)
        return -1;
    memcpy(chain, expected, sizeof expected);
    return 0;
}

static int journal_record_write(CnetCompeteJournal *journal,
                                const char *record) {
    char digest[65];
    if (journal == NULL || journal->file == NULL || record == NULL ||
        journal_record_digest(journal->record_sha256, record, digest) != 0 ||
        fprintf(journal->file, "%s\t%s\n", record, digest) < 0 ||
        fflush(journal->file) != 0 || cnet_fsync(fileno(journal->file)) != 0 ||
        journal->record_count == SIZE_MAX ||
        journal_anchor_write(journal->anchor_path,
                             journal->record_count + 1u, digest) != 0)
        return -1;
    memcpy(journal->record_sha256, digest, sizeof digest);
    ++journal->record_count;
    return 0;
}

static int repair_partial_tail(int file_descriptor) {
    struct stat status;
    off_t position;
    unsigned char buffer[4096];
    if (fstat(file_descriptor, &status) != 0 || !S_ISREG(status.st_mode) ||
        status.st_size <= 0 ||
        (unsigned long long)status.st_size > EVAL_JOURNAL_MAX)
        return -1;
    position = status.st_size;
    while (position > 0) {
        size_t chunk = position < (off_t)sizeof buffer
                           ? (size_t)position : sizeof buffer;
        off_t start = position - (off_t)chunk;
        size_t index;
        if (cnet_pread(file_descriptor, buffer, chunk, start) != (ssize_t)chunk)
            return -1;
        if (position == status.st_size && buffer[chunk - 1u] == '\n')
            return 0;
        for (index = chunk; index > 0; --index) {
            if (buffer[index - 1u] == '\n') {
                off_t truncate_at = start + (off_t)index;
                if (ftruncate(file_descriptor, truncate_at) != 0 ||
                    cnet_fsync(file_descriptor) != 0)
                    return -1;
                return 0;
            }
        }
        position = start;
    }
    return -1;
}

static int journal_prefix_matches(int file_descriptor) {
    static const char prefix[] = "CNET_ASI5_RESULTS 4\n";
    char actual[sizeof prefix - 1u];
    return cnet_pread(file_descriptor, actual, sizeof actual, 0) ==
               (ssize_t)sizeof actual &&
           memcmp(actual, prefix, sizeof actual) == 0;
}

static int journal_header_matches_descriptor(
    int file_descriptor, const CnetCompeteJournalHeader *expected) {
    CnetCompeteJournalHeader actual;
    FILE *file = NULL;
    char *line = NULL;
    size_t capacity = 0;
    int duplicate = -1, matches = 0;
    if (file_descriptor < 0 || expected == NULL) return 0;
    duplicate = dup(file_descriptor);
    if (duplicate >= 0 && lseek(duplicate, 0, SEEK_SET) >= 0)
        file = fdopen(duplicate, "rb");
    if (file != NULL &&
        parse_journal_header(file, &actual, &line, &capacity) == 0 &&
        header_equal(&actual, expected))
        matches = 1;
    if (file != NULL) {
        if (fclose(file) != 0) matches = 0;
    } else if (duplicate >= 0) {
        (void)close(duplicate);
    }
    free(line);
    return matches;
}

static int fsync_parent_directory(const char *path) {
    char directory[PATH_MAX], *slash;
    int descriptor, rc;
    if (copy_text(directory, sizeof directory, path) != 0) return -1;
    slash = strrchr(directory, '/');
    if (slash == NULL) {
        if (copy_text(directory, sizeof directory, ".") != 0) return -1;
    } else if (slash == directory) {
        slash[1] = '\0';
    } else {
        *slash = '\0';
    }
    descriptor = open(directory, O_RDONLY | CNET_O_DIRECTORY | CNET_O_NOFOLLOW);
    if (descriptor < 0) return -1;
    rc = cnet_fsync(descriptor);
    if (close(descriptor) != 0) rc = -1;
    return rc;
}

static int publish_journal_header(
    const char *path, const CnetCompeteJournalHeader *header) {
    char temporary[PATH_MAX];
    FILE *file = NULL;
    int descriptor = -1, result = -1;
    struct stat status;
    if (path == NULL || header == NULL ||
        snprintf(temporary, sizeof temporary, "%s.tmp.XXXXXX", path) < 0 ||
        strlen(temporary) >= sizeof temporary)
        return -1;
    descriptor = mkstemp(temporary);
    if (descriptor < 0 || cnet_fchmod(descriptor, 0600) != 0 ||
        (file = fdopen(descriptor, "w+b")) == NULL)
        goto done;
    descriptor = -1;
    if (write_journal_header(file, header) != 0) {
        (void)fclose(file);
        file = NULL;
        goto done;
    }
    if (fclose(file) != 0) {
        file = NULL;
        goto done;
    }
    file = NULL;
    if (cnet_lstat(path, &status) == 0) {
        errno = EEXIST;
        goto done;
    }
    if (errno != ENOENT || rename(temporary, path) != 0 ||
        fsync_parent_directory(path) != 0)
        goto done;
    result = open(path, O_RDWR | O_APPEND | CNET_O_NOFOLLOW);
done:
    if (file != NULL) (void)fclose(file);
    else if (descriptor >= 0) (void)close(descriptor);
    if (result < 0) (void)unlink(temporary);
    return result;
}

static int journal_parse_descriptor(
    int file_descriptor, const char *label,
    const CnetCompeteEvalFixture *fixture, int allow_partial,
    CnetCompeteJournalHeader *header, CnetCompeteJournalRow **rows_out,
    size_t *row_count_out, size_t *guard_checks_out,
    int *complete_out, int *pending_out, char chain_out[65],
    size_t *record_count_out, size_t anchor_record_count,
    const char *anchor_chain, int *anchor_prefix_out,
    char *error, size_t error_capacity) {
    struct stat status;
    CnetCompeteJournalRow *rows = NULL;
    FILE *file = NULL;
    char *line = NULL;
    char chain[65];
    size_t capacity = 0, count = 0, guards = 0, records = 0;
    int anchor_prefix = 0;
    int complete = 0, pending = 0, duplicate = -1, rc = -1;
    if (file_descriptor < 0 || fstat(file_descriptor, &status) != 0 ||
        !S_ISREG(status.st_mode) ||
        status.st_size <= 0 ||
        (unsigned long long)status.st_size > EVAL_JOURNAL_MAX ||
        fixture == NULL || header == NULL)
        return -1;
    rows = rows_out == NULL ? NULL :
        (CnetCompeteJournalRow *)calloc(fixture->count, sizeof rows[0]);
    if (rows_out != NULL && rows == NULL) return -1;
    duplicate = dup(file_descriptor);
    if (duplicate >= 0 && lseek(duplicate, 0, SEEK_SET) >= 0)
        file = fdopen(duplicate, "rb");
    if (file == NULL ||
        parse_journal_header(file, header, &line, &capacity) != 0 ||
        journal_chain_seed(header, chain) != 0) {
        if (file == NULL && duplicate >= 0) (void)close(duplicate);
        set_error(error, error_capacity, "result header refused: %s", label);
        goto done;
    }
    if (anchor_chain != NULL && anchor_record_count == 0)
        anchor_prefix = strcmp(chain, anchor_chain) == 0;
    for (;;) {
        ssize_t line_length = cnet_getline(&line, &capacity, file);
        size_t length;
        CnetCompeteJournalRow local;
        if (line_length < 0) break;
        length = (size_t)line_length;
        if (length == 0 || length > EVAL_LINE_MAX ||
            memchr(line, '\0', length) != NULL ||
            line[length - 1u] != '\n')
            goto malformed;
        line[--length] = '\0';
        if (journal_record_verify(line, chain) != 0) goto malformed;
        ++records;
        if (anchor_chain != NULL && records == anchor_record_count)
            anchor_prefix = strcmp(chain, anchor_chain) == 0;
        if (strncmp(line, "issued\t", 7) == 0) {
            if (complete || pending || count >= fixture->count ||
                strcmp(line + 7, fixture->rows[count].id) != 0)
                goto malformed;
            pending = 1;
            continue;
        }
        if (strncmp(line, "tainted\t", 8) == 0) goto malformed;
        if (strncmp(line, "complete\t", 9) == 0) {
            unsigned long long declared;
            if (parse_unsigned(line + 9, &declared) != 0 ||
                declared != fixture->count || count != fixture->count ||
                complete || pending)
                goto malformed;
            complete = 1;
            continue;
        }
        if (complete || !pending || count >= fixture->count ||
            parse_journal_row(line, &fixture->rows[count], &local) != 0)
            goto malformed;
        if (SIZE_MAX - guards < local.guard_checks) goto malformed;
        guards += local.guard_checks;
        if (rows != NULL) rows[count] = local;
        pending = 0;
        ++count;
    }
    if (ferror(file) || (!allow_partial && (!complete || pending)) ||
        (complete && count != fixture->count))
        goto malformed;
    if (fclose(file) != 0) {
        file = NULL;
        goto malformed;
    }
    file = NULL;
    if (rows_out != NULL) { *rows_out = rows; rows = NULL; }
    if (row_count_out != NULL) *row_count_out = count;
    if (guard_checks_out != NULL) *guard_checks_out = guards;
    if (complete_out != NULL) *complete_out = complete;
    if (pending_out != NULL) *pending_out = pending;
    if (chain_out != NULL) memcpy(chain_out, chain, sizeof chain);
    if (record_count_out != NULL) *record_count_out = records;
    if (anchor_prefix_out != NULL) *anchor_prefix_out = anchor_prefix;
    rc = 0;
    goto done;
malformed:
    set_error(error, error_capacity, "result journal malformed: %s", label);
done:
    if (file != NULL) (void)fclose(file);
    free(line);
    free(rows);
    return rc;
}

int cnet_compete_journal_open(CnetCompeteJournal *journal,
                              const char *path,
                              const CnetCompeteEvalFixture *fixture,
                              const CnetCompeteJournalHeader *header,
                              char *error, size_t error_capacity) {
    struct stat status;
    char lock_path[PATH_MAX], anchor_chain[65];
    size_t anchor_records = 0;
    int file_descriptor = -1, created = 0, pending = 0, attempt;
    int anchor_status, anchor_prefix = 0;
    if (journal == NULL || path == NULL || fixture == NULL || header == NULL)
        return -1;
    memset(journal, 0, sizeof *journal);
    journal->lock_descriptor = -1;
    journal->header = *header;
    if (snprintf(lock_path, sizeof lock_path, "%s.lock", path) < 0 ||
        strlen(lock_path) >= sizeof lock_path ||
        snprintf(journal->anchor_path, sizeof journal->anchor_path,
                 "%s.anchor", path) < 0 ||
        strlen(journal->anchor_path) >= sizeof journal->anchor_path)
        return -1;
    journal->lock_descriptor = open(
        lock_path, O_RDWR | O_CREAT | CNET_O_NOFOLLOW, 0600);
    if (journal->lock_descriptor < 0 ||
        fstat(journal->lock_descriptor, &status) != 0 ||
        !S_ISREG(status.st_mode) || status.st_uid != cnet_geteuid() ||
        status.st_nlink != 1 || (status.st_mode & 0777) != 0600 ||
        cnet_flock(journal->lock_descriptor, CNET_LOCK_EX | CNET_LOCK_NB) != 0) {
        if (journal->lock_descriptor >= 0)
            (void)close(journal->lock_descriptor);
        journal->lock_descriptor = -1;
        set_error(error, error_capacity, "result journal locked/refused: %s",
                  path);
        return -1;
    }
    journal->lock_owned = 1;
    {
        struct stat path_status, anchor_status;
        int path_exists = cnet_lstat(path, &path_status) == 0;
        int path_error = errno;
        int anchor_exists = cnet_lstat(journal->anchor_path, &anchor_status) == 0;
        int anchor_error = errno;
        if ((!path_exists && path_error != ENOENT) ||
            (!anchor_exists && anchor_error != ENOENT) ||
            (!path_exists && anchor_exists)) {
            cnet_compete_journal_close(journal);
            set_error(error, error_capacity,
                      "result journal companion mismatch: %s", path);
            return -1;
        }
    }
    for (attempt = 0; attempt < 2 && file_descriptor < 0; ++attempt) {
        file_descriptor = open(path, O_RDWR | O_APPEND | CNET_O_NOFOLLOW);
        if (file_descriptor >= 0) break;
        if (errno != ENOENT) {
            cnet_compete_journal_close(journal);
            return -1;
        }
        file_descriptor = publish_journal_header(path, header);
        if (file_descriptor >= 0) { created = 1; break; }
        if (errno != EEXIST) {
            cnet_compete_journal_close(journal);
            return -1;
        }
    }
    if (file_descriptor < 0 || fstat(file_descriptor, &status) != 0 ||
        !S_ISREG(status.st_mode) || status.st_uid != cnet_geteuid() ||
        status.st_nlink != 1 || (status.st_mode & 0777) != 0600 ||
        cnet_flock(file_descriptor, CNET_LOCK_EX | CNET_LOCK_NB) != 0) {
        if (file_descriptor >= 0) (void)close(file_descriptor);
        cnet_compete_journal_close(journal);
        set_error(error, error_capacity, "result journal locked/refused: %s", path);
        return -1;
    }
    if (created) {
        journal->file = fdopen(file_descriptor, "a+b");
        if (journal->file == NULL) {
            (void)close(file_descriptor);
            cnet_compete_journal_close(journal);
            return -1;
        }
        if (journal_chain_seed(header, journal->record_sha256) != 0 ||
            journal_anchor_write(journal->anchor_path, 0,
                                 journal->record_sha256) != 0) {
            cnet_compete_journal_close(journal);
            return -1;
        }
        return 0;
    }
    anchor_status = journal_anchor_read(journal->anchor_path, &anchor_records,
                                        anchor_chain);
    if (anchor_status == 1 && journal_chain_seed(header, anchor_chain) != 0)
        anchor_status = -1;
    if (anchor_status < 0 || !journal_prefix_matches(file_descriptor) ||
        !journal_header_matches_descriptor(file_descriptor, header) ||
        repair_partial_tail(file_descriptor) != 0 ||
        journal_parse_descriptor(
            file_descriptor, path, fixture, 1, &journal->header, NULL,
            &journal->completed_rows, &journal->guard_checks,
            &journal->complete, &pending, journal->record_sha256,
            &journal->record_count, anchor_records, anchor_chain,
            &anchor_prefix,
            error, error_capacity) != 0 ||
        !header_equal(&journal->header, header) || !anchor_prefix ||
        anchor_records > journal->record_count ||
        (anchor_status == 1 && journal->record_count != 0)) {
        (void)close(file_descriptor);
        cnet_compete_journal_close(journal);
        set_error(error, error_capacity, "resume identity mismatch: %s", path);
        return -1;
    }
    if ((anchor_status == 1 || anchor_records < journal->record_count) &&
        journal_anchor_write(journal->anchor_path, journal->record_count,
                             journal->record_sha256) != 0) {
        (void)close(file_descriptor);
        cnet_compete_journal_close(journal);
        return -1;
    }
    if (journal->complete) { (void)close(file_descriptor); return 0; }
    journal->file = fdopen(file_descriptor, "a+b");
    if (journal->file == NULL) {
        (void)close(file_descriptor);
        cnet_compete_journal_close(journal);
        return -1;
    }
    if (pending) {
        journal->issued = 1;
        if (copy_text(journal->issued_id, sizeof journal->issued_id,
                      fixture->rows[journal->completed_rows].id) != 0 ||
            cnet_compete_journal_append(
                journal, &fixture->rows[journal->completed_rows],
                0, 5, 0, "") != 0) {
            cnet_compete_journal_close(journal);
            return -1;
        }
        journal->interrupted_rows = 1;
    }
    return 0;
}

int cnet_compete_journal_issue(CnetCompeteJournal *journal,
                               const CnetCompeteEvalRow *row) {
    char record[CNET_COMPETE_EVAL_ID_MAX + 8u];
    int written;
    if (journal == NULL || journal->file == NULL || journal->complete ||
        journal->issued || row == NULL || row->id[0] == '\0')
        return -1;
    written = snprintf(record, sizeof record, "issued\t%s", row->id);
    if (written < 0 || (size_t)written >= sizeof record ||
        journal_record_write(journal, record) != 0 ||
        copy_text(journal->issued_id, sizeof journal->issued_id, row->id) != 0)
        return -1;
    journal->issued = 1;
    return 0;
}

int cnet_compete_journal_append(CnetCompeteJournal *journal,
                                const CnetCompeteEvalRow *row,
                                uint64_t latency_ns, int run_rc,
                                size_t guard_checks, const char *output) {
    char *hex = NULL, *record = NULL;
    size_t capacity, length, record_capacity;
    int rc = -1, written;
    if (journal == NULL || journal->file == NULL || journal->complete ||
        !journal->issued || row == NULL || output == NULL || run_rc < 0 ||
        strcmp(journal->issued_id, row->id) != 0)
        return -1;
    length = strlen(output);
    if (length >= CNET_COMPETE_EVAL_OUTPUT_MAX ||
        length > (SIZE_MAX - 1u) / 2u)
        return -1;
    capacity = length * 2u + 1u;
    hex = (char *)malloc(capacity);
    if (hex == NULL || cnet_compete_eval_hex_encode(output, hex, capacity) != 0)
        goto done;
    if (capacity > SIZE_MAX - CNET_COMPETE_EVAL_ID_MAX - 96u) goto done;
    record_capacity = capacity + CNET_COMPETE_EVAL_ID_MAX + 96u;
    record = (char *)malloc(record_capacity);
    if (record == NULL) goto done;
    written = snprintf(record, record_capacity, "%s\t%llu\t%d\t%zu\t%s",
                       row->id, (unsigned long long)latency_ns, run_rc,
                       guard_checks, hex);
    if (written < 0 || (size_t)written >= record_capacity ||
        journal_record_write(journal, record) != 0)
        goto done;
    ++journal->completed_rows;
    journal->guard_checks += guard_checks;
    journal->issued = 0;
    journal->issued_id[0] = '\0';
    rc = 0;
done:
    free(record);
    free(hex);
    return rc;
}

int cnet_compete_journal_finish(CnetCompeteJournal *journal,
                                size_t expected_rows) {
    char record[64];
    int written;
    if (journal == NULL || journal->file == NULL || journal->complete ||
        journal->issued ||
        journal->completed_rows != expected_rows)
        return -1;
    written = snprintf(record, sizeof record, "complete\t%zu", expected_rows);
    if (written < 0 || (size_t)written >= sizeof record ||
        journal_record_write(journal, record) != 0)
        return -1;
    journal->complete = 1;
    return 0;
}

int cnet_compete_journal_fail(CnetCompeteJournal *journal,
                              const char *reason) {
    const unsigned char *cursor = (const unsigned char *)reason;
    char record[256];
    int written;
    if (journal == NULL || journal->file == NULL || journal->complete ||
        reason == NULL || *reason == '\0')
        return -1;
    for (; *cursor != '\0'; ++cursor)
        if (!isalnum(*cursor) && *cursor != '_') return -1;
    written = snprintf(record, sizeof record, "tainted\t%s", reason);
    if (written < 0 || (size_t)written >= sizeof record ||
        journal_record_write(journal, record) != 0)
        return -1;
    journal->complete = 1;
    return 0;
}

void cnet_compete_journal_close(CnetCompeteJournal *journal) {
    if (journal == NULL) return;
    if (journal->file != NULL) (void)fclose(journal->file);
    journal->file = NULL;
    if (journal->lock_owned && journal->lock_descriptor >= 0)
        (void)close(journal->lock_descriptor);
    journal->lock_descriptor = -1;
    journal->lock_owned = 0;
}

int cnet_compete_journal_read(const char *path,
                              const CnetCompeteEvalFixture *fixture,
                              CnetCompeteJournalHeader *header,
                              CnetCompeteJournalRow **rows_out,
                              size_t *row_count_out,
                              char *error, size_t error_capacity) {
    char anchor_path[PATH_MAX], anchor_chain[65], tail_chain[65];
    size_t guards = 0, anchor_records = 0, records = 0;
    struct stat status;
    int complete = 0, descriptor, rc;
    if (path == NULL || rows_out == NULL || row_count_out == NULL ||
        snprintf(anchor_path, sizeof anchor_path, "%s.anchor", path) < 0 ||
        strlen(anchor_path) >= sizeof anchor_path ||
        journal_anchor_read(anchor_path, &anchor_records, anchor_chain) != 0)
        return -1;
    *rows_out = NULL;
    *row_count_out = 0;
    descriptor = open(path, O_RDONLY | CNET_O_NOFOLLOW);
    if (descriptor < 0 || fstat(descriptor, &status) != 0 ||
        !S_ISREG(status.st_mode) || status.st_uid != cnet_geteuid() ||
        status.st_nlink != 1 || (status.st_mode & 0777) != 0600 ||
        cnet_flock(descriptor, CNET_LOCK_SH | CNET_LOCK_NB) != 0) {
        if (descriptor >= 0) (void)close(descriptor);
        return -1;
    }
    rc = journal_parse_descriptor(
        descriptor, path, fixture, 0, header, rows_out, row_count_out,
        &guards, &complete, NULL, tail_chain, &records, anchor_records,
        anchor_chain, NULL, error, error_capacity);
    if (rc == 0 && (records != anchor_records ||
                    strcmp(tail_chain, anchor_chain) != 0)) {
        free(*rows_out);
        *rows_out = NULL;
        *row_count_out = 0;
        rc = -1;
    }
    if (close(descriptor) != 0) rc = -1;
    return rc;
}
