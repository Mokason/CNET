#include "cnet_compete_independence.h"

#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

enum {
    AUDIT_LINE_MAX = 4096,
    AUDIT_TEXT_MAX = 1024,
    AUDIT_WORD_MAX = 48,
    AUDIT_TOKEN_MAX = 96,
    AUDIT_DOCUMENT_MAX = 8192,
    AUDIT_SOURCE_BYTES_MAX = 4 * 1024 * 1024
};

typedef struct {
    char raw[AUDIT_TEXT_MAX];
    char normalized[AUDIT_TEXT_MAX];
    uint64_t tokens[AUDIT_TOKEN_MAX];
    uint64_t sorted_tokens[AUDIT_TOKEN_MAX];
    size_t token_count;
    char origin[160];
} AuditDocument;

typedef struct {
    AuditDocument *documents;
    size_t count;
    size_t capacity;
} AuditSet;

static void set_error(char *error, size_t capacity, const char *format, ...) {
    va_list arguments;
    if (error == NULL || capacity == 0) return;
    va_start(arguments, format);
    (void)vsnprintf(error, capacity, format, arguments);
    va_end(arguments);
}

static uint64_t fnv_update(uint64_t hash, const void *data, size_t length) {
    const unsigned char *bytes = (const unsigned char *)data;
    size_t index;
    for (index = 0; index < length; ++index) {
        hash ^= bytes[index];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static int compare_u64(const void *left, const void *right) {
    uint64_t a = *(const uint64_t *)left;
    uint64_t b = *(const uint64_t *)right;
    return (a > b) - (a < b);
}

static const char *canonical_word(const char *word) {
    static const struct {
        const char *from;
        const char *to;
    } forms[] = {
        {"bytes", "byte"}, {"octets", "octet"},
        {"minutes", "minute"}, {"mins", "minute"},
        {"seconds", "second"}, {"secs", "second"},
        {"true", "<bool>"}, {"false", "<bool>"},
        {"yes", "<bool>"}, {"no", "<bool>"}
    };
    size_t index;
    for (index = 0; index < sizeof forms / sizeof forms[0]; ++index)
        if (strcmp(word, forms[index].from) == 0) return forms[index].to;
    return word;
}

static int append_token(char output[AUDIT_TEXT_MAX], size_t *used,
                        char tokens[AUDIT_TOKEN_MAX][AUDIT_WORD_MAX],
                        size_t *token_count, const char *token) {
    size_t length = strlen(token);
    if (length == 0 || length >= AUDIT_WORD_MAX ||
        *token_count >= AUDIT_TOKEN_MAX ||
        *used + length + (*used != 0 ? 1u : 0u) >= AUDIT_TEXT_MAX)
        return -1;
    if (*used != 0) output[(*used)++] = ' ';
    memcpy(output + *used, token, length + 1u);
    *used += length;
    memcpy(tokens[*token_count], token, length + 1u);
    ++*token_count;
    return 0;
}

static int append_format_placeholder(
    const unsigned char **cursor, char output[AUDIT_TEXT_MAX], size_t *used,
    char tokens[AUDIT_TOKEN_MAX][AUDIT_WORD_MAX], size_t *token_count) {
    const unsigned char *p = *cursor + 1u;
    const char *token = NULL;
    if (*p == '%') {
        *cursor = p + 1u;
        return 0;
    }
    while (*p != '\0' && strchr("-+ #0*.0123456789hljztL", *p) != NULL)
        ++p;
    if (strchr("diuoxXfFeEgGaA", *p) != NULL) token = "<num>";
    else if (*p == 's') token = "<bool>";
    else {
        *cursor = *p != '\0' ? p + 1u : p;
        return 0;
    }
    *cursor = p + 1u;
    return append_token(output, used, tokens, token_count, token);
}

static int normalize_document(const char *input, AuditDocument *document,
                              size_t *token_count_out) {
    char tokens[AUDIT_TOKEN_MAX][AUDIT_WORD_MAX];
    const unsigned char *cursor = (const unsigned char *)input;
    size_t used = 0, token_count = 0, index;
    if (input == NULL || document == NULL || input[0] == '\0') return -1;
    document->normalized[0] = '\0';
    document->token_count = 0;
    while (*cursor != '\0') {
        char word[AUDIT_WORD_MAX];
        size_t length = 0;
        const char *canonical;
        if (*cursor >= 128u) return -1;
        if (*cursor == '%') {
            if (append_format_placeholder(&cursor, document->normalized,
                                          &used, tokens, &token_count) != 0)
                return -1;
            continue;
        }
        if (isdigit(*cursor)) {
            while (isalnum(*cursor) || *cursor == 'x' || *cursor == 'X')
                ++cursor;
            if (append_token(document->normalized, &used, tokens,
                             &token_count, "<num>") != 0)
                return -1;
            continue;
        }
        if (!isalpha(*cursor)) {
            ++cursor;
            continue;
        }
        while (isalpha(*cursor) || *cursor == '\'') {
            unsigned char character = *cursor++;
            if (length + 1u >= sizeof word) return -1;
            word[length++] = (char)tolower(character);
        }
        word[length] = '\0';
        canonical = canonical_word(word);
        if (append_token(document->normalized, &used, tokens, &token_count,
                         canonical) != 0)
            return -1;
    }
    if (token_count == 0) return -1;
    document->token_count = token_count;
    for (index = 0; index < token_count; ++index) {
        uint64_t hash = UINT64_C(14695981039346656037);
        hash = fnv_update(hash, tokens[index], strlen(tokens[index]));
        document->tokens[index] = hash;
        document->sorted_tokens[index] = hash;
    }
    qsort(document->sorted_tokens, token_count,
          sizeof document->sorted_tokens[0], compare_u64);
    if (token_count_out != NULL) *token_count_out = token_count;
    return 0;
}

static void audit_set_free(AuditSet *set) {
    if (set == NULL) return;
    free(set->documents);
    memset(set, 0, sizeof *set);
}

static int audit_set_init(AuditSet *set) {
    if (set == NULL) return -1;
    memset(set, 0, sizeof *set);
    set->capacity = 128;
    set->documents = (AuditDocument *)calloc(set->capacity,
                                              sizeof set->documents[0]);
    return set->documents == NULL ? -1 : 0;
}

static int audit_set_add(AuditSet *set, const char *text, const char *path,
                         size_t line, size_t minimum_tokens) {
    AuditDocument candidate;
    size_t token_count = 0;
    int written;
    if (set == NULL || text == NULL || path == NULL ||
        strlen(text) >= sizeof candidate.raw ||
        set->count >= AUDIT_DOCUMENT_MAX)
        return -1;
    memset(&candidate, 0, sizeof candidate);
    memcpy(candidate.raw, text, strlen(text) + 1u);
    if (normalize_document(text, &candidate, &token_count) != 0)
        return -1;
    if (token_count < minimum_tokens) return 0;
    written = snprintf(candidate.origin, sizeof candidate.origin, "%s:%zu",
                       path, line);
    if (written < 0 || (size_t)written >= sizeof candidate.origin) return -1;
    if (set->count == set->capacity) {
        size_t capacity = set->capacity * 2u;
        AuditDocument *grown;
        if (capacity > AUDIT_DOCUMENT_MAX) capacity = AUDIT_DOCUMENT_MAX;
        grown = (AuditDocument *)realloc(set->documents,
                                         capacity * sizeof grown[0]);
        if (grown == NULL) return -1;
        set->documents = grown;
        set->capacity = capacity;
    }
    set->documents[set->count++] = candidate;
    return 0;
}

static int split_fields(char *line, char *fields[7]) {
    size_t found = 1;
    char *cursor;
    fields[0] = line;
    for (cursor = line; *cursor != '\0'; ++cursor) {
        if (*cursor != '\t') continue;
        if (found >= 7) return -1;
        *cursor = '\0';
        fields[found++] = cursor + 1;
    }
    return found == 7 ? 0 : -1;
}

static int regular_file_bounded(const char *path, size_t maximum) {
    struct stat status;
    if (path == NULL || lstat(path, &status) != 0 ||
        !S_ISREG(status.st_mode) || status.st_size <= 0 ||
        (unsigned long long)status.st_size > maximum)
        return -1;
    return 0;
}

static int load_fixture_prompts(
    const char *path, AuditSet *set, int candidate,
    CnetCompeteIndependenceReport *report, char *error,
    size_t error_capacity) {
    static const char expected_header[] =
        "id\tsplit\tintent\tvalue_kind\texpected_value\tprompt\tprovenance";
    FILE *file = NULL;
    char line[AUDIT_LINE_MAX];
    size_t line_number = 0, before;
    int result = CNET_INDEPENDENCE_ERR_FORMAT;
    if (regular_file_bounded(path, AUDIT_SOURCE_BYTES_MAX) != 0 ||
        (file = fopen(path, "rb")) == NULL) {
        set_error(error, error_capacity, "cannot open fixture %s: %s", path,
                  strerror(errno));
        return CNET_INDEPENDENCE_ERR_IO;
    }
    before = set->count;
    while (fgets(line, sizeof line, file) != NULL) {
        size_t length = strlen(line), index;
        char *fields[7];
        ++line_number;
        if (length == 0 || line[length - 1u] != '\n') {
            set_error(error, error_capacity,
                      "unterminated or oversized fixture line %s:%zu", path,
                      line_number);
            goto done;
        }
        line[--length] = '\0';
        if (length > 0 && line[length - 1u] == '\r') line[--length] = '\0';
        if (line_number == 1) {
            if (strncmp(line, "#suite=", 7) != 0 || line[7] == '\0') {
                set_error(error, error_capacity, "fixture suite missing %s",
                          path);
                goto done;
            }
            continue;
        }
        if (line_number == 2) {
            if (strcmp(line, expected_header) != 0) {
                set_error(error, error_capacity, "fixture header mismatch %s",
                          path);
                goto done;
            }
            continue;
        }
        if (split_fields(line, fields) != 0 || fields[5][0] == '\0') {
            set_error(error, error_capacity, "malformed fixture row %s:%zu",
                      path, line_number);
            goto done;
        }
        if (candidate) {
            for (index = before; index < set->count; ++index) {
                if (strcmp(set->documents[index].raw, fields[5]) == 0) {
                    if (report != NULL) ++report->duplicate_prompts;
                    set_error(error, error_capacity,
                              "duplicate candidate prompt %s:%zu and %s",
                              path, line_number,
                              set->documents[index].origin);
                    result = CNET_INDEPENDENCE_ERR_DUPLICATE;
                    goto done;
                }
            }
        }
        if (audit_set_add(set, fields[5], path, line_number, 1) != 0) {
            set_error(error, error_capacity,
                      "invalid or excessive fixture prompt %s:%zu", path,
                      line_number);
            goto done;
        }
        if (!candidate) {
            AuditDocument *added = &set->documents[set->count - 1u];
            int written = snprintf(added->origin, sizeof added->origin,
                                   "%s:%s", fields[6], fields[0]);
            const unsigned char *origin =
                (const unsigned char *)added->origin;
            if (fields[6][0] == '\0' || written < 0 ||
                (size_t)written >= sizeof added->origin) {
                set_error(error, error_capacity,
                          "invalid fixture provenance %s:%zu", path,
                          line_number);
                goto done;
            }
            for (; *origin != '\0'; ++origin)
                if (*origin < 0x20u || *origin >= 0x7fu) {
                    set_error(error, error_capacity,
                              "unsafe fixture provenance %s:%zu", path,
                              line_number);
                    goto done;
                }
        }
    }
    if (ferror(file)) {
        set_error(error, error_capacity, "fixture read failed %s", path);
        result = CNET_INDEPENDENCE_ERR_IO;
        goto done;
    }
    if (line_number < 3 || set->count == before) {
        set_error(error, error_capacity, "fixture has no prompts %s", path);
        goto done;
    }
    result = CNET_INDEPENDENCE_OK;
done:
    if (fclose(file) != 0 && result == CNET_INDEPENDENCE_OK) {
        set_error(error, error_capacity, "fixture close failed %s", path);
        result = CNET_INDEPENDENCE_ERR_IO;
    }
    return result;
}

static int read_source(const char *path, unsigned char **bytes_out,
                       size_t *length_out, char *error,
                       size_t error_capacity) {
    struct stat status;
    FILE *file;
    unsigned char *bytes;
    size_t length;
    if (path == NULL || bytes_out == NULL || length_out == NULL ||
        lstat(path, &status) != 0 || !S_ISREG(status.st_mode) ||
        status.st_size <= 0 || status.st_size > AUDIT_SOURCE_BYTES_MAX) {
        set_error(error, error_capacity, "invalid development source %s",
                  path != NULL ? path : "(null)");
        return CNET_INDEPENDENCE_ERR_IO;
    }
    length = (size_t)status.st_size;
    bytes = (unsigned char *)malloc(length + 1u);
    if (bytes == NULL) {
        set_error(error, error_capacity, "development source allocation");
        return CNET_INDEPENDENCE_ERR_IO;
    }
    file = fopen(path, "rb");
    if (file == NULL || fread(bytes, 1, length, file) != length ||
        fgetc(file) != EOF) {
        if (file != NULL) (void)fclose(file);
        free(bytes);
        set_error(error, error_capacity, "cannot read development source %s",
                  path);
        return CNET_INDEPENDENCE_ERR_IO;
    }
    if (fclose(file) != 0) {
        free(bytes);
        set_error(error, error_capacity,
                  "cannot close development source %s", path);
        return CNET_INDEPENDENCE_ERR_IO;
    }
    if (memchr(bytes, '\0', length) != NULL) {
        free(bytes);
        set_error(error, error_capacity, "NUL in development source %s", path);
        return CNET_INDEPENDENCE_ERR_FORMAT;
    }
    bytes[length] = '\0';
    *bytes_out = bytes;
    *length_out = length;
    return CNET_INDEPENDENCE_OK;
}

static int skip_quoted(const unsigned char *bytes, size_t length,
                       size_t *offset, unsigned char quote,
                       size_t *line, char *error, size_t error_capacity,
                       const char *path) {
    size_t index = *offset + 1u;
    while (index < length) {
        if (bytes[index] == '\n') ++*line;
        if (bytes[index] == '\\') {
            index += index + 1u < length ? 2u : 1u;
            continue;
        }
        if (bytes[index] == quote) {
            *offset = index + 1u;
            return CNET_INDEPENDENCE_OK;
        }
        ++index;
    }
    set_error(error, error_capacity, "unterminated literal %s:%zu", path,
              *line);
    return CNET_INDEPENDENCE_ERR_FORMAT;
}

static int decode_escape(const unsigned char *bytes, size_t length,
                         size_t *offset, char *output, size_t *used) {
    unsigned char value;
    size_t index = *offset;
    if (index >= length) return -1;
    value = bytes[index++];
    if (value == '\n') {
        *offset = index;
        return 0;
    }
    if (value == 'n' || value == 'r' || value == 't') value = ' ';
    else if (value >= '0' && value <= '7') {
        size_t digits = 1;
        while (index < length && digits < 3 && bytes[index] >= '0' &&
               bytes[index] <= '7') {
            ++index;
            ++digits;
        }
        value = ' ';
    } else if (value == 'x') {
        size_t digits = 0;
        while (index < length && isxdigit(bytes[index])) {
            ++index;
            ++digits;
        }
        if (digits == 0) return -1;
        value = ' ';
    }
    if (*used + 1u >= AUDIT_TEXT_MAX) return -1;
    output[(*used)++] = (char)value;
    *offset = index;
    return 0;
}

static int load_source_literals(const char *path, AuditSet *set, char *error,
                                size_t error_capacity) {
    unsigned char *bytes = NULL;
    size_t length = 0, offset = 0, line = 1;
    int result = read_source(path, &bytes, &length, error, error_capacity);
    if (result != CNET_INDEPENDENCE_OK) return result;
    while (offset < length) {
        if (bytes[offset] == '\n') {
            ++line;
            ++offset;
            continue;
        }
        if (bytes[offset] == '/' && offset + 1u < length &&
            bytes[offset + 1u] == '/') {
            offset += 2u;
            while (offset < length && bytes[offset] != '\n') ++offset;
            continue;
        }
        if (bytes[offset] == '/' && offset + 1u < length &&
            bytes[offset + 1u] == '*') {
            offset += 2u;
            while (offset + 1u < length &&
                   !(bytes[offset] == '*' && bytes[offset + 1u] == '/')) {
                if (bytes[offset] == '\n') ++line;
                ++offset;
            }
            if (offset + 1u >= length) {
                set_error(error, error_capacity,
                          "unterminated comment %s:%zu", path, line);
                result = CNET_INDEPENDENCE_ERR_FORMAT;
                goto done;
            }
            offset += 2u;
            continue;
        }
        if (bytes[offset] == '\'') {
            result = skip_quoted(bytes, length, &offset, '\'', &line, error,
                                 error_capacity, path);
            if (result != CNET_INDEPENDENCE_OK) goto done;
            continue;
        }
        if (bytes[offset] == '"') {
            char literal[AUDIT_TEXT_MAX];
            size_t literal_line = line, used = 0;
            ++offset;
            while (offset < length && bytes[offset] != '"') {
                if (bytes[offset] == '\n') {
                    set_error(error, error_capacity,
                              "newline in string literal %s:%zu", path,
                              line);
                    result = CNET_INDEPENDENCE_ERR_FORMAT;
                    goto done;
                }
                if (bytes[offset] == '\\') {
                    ++offset;
                    if (decode_escape(bytes, length, &offset, literal, &used) !=
                        0) {
                        set_error(error, error_capacity,
                                  "invalid string escape %s:%zu", path, line);
                        result = CNET_INDEPENDENCE_ERR_FORMAT;
                        goto done;
                    }
                    continue;
                }
                if (used + 1u >= sizeof literal) {
                    set_error(error, error_capacity,
                              "oversized string literal %s:%zu", path, line);
                    result = CNET_INDEPENDENCE_ERR_FORMAT;
                    goto done;
                }
                literal[used++] = (char)bytes[offset++];
            }
            if (offset >= length) {
                set_error(error, error_capacity,
                          "unterminated string literal %s:%zu", path, line);
                result = CNET_INDEPENDENCE_ERR_FORMAT;
                goto done;
            }
            ++offset;
            literal[used] = '\0';
            if (used != 0 && audit_set_add(set, literal, path, literal_line,
                                           4) != 0) {
                set_error(error, error_capacity,
                          "invalid development literal %s:%zu", path,
                          literal_line);
                result = CNET_INDEPENDENCE_ERR_FORMAT;
                goto done;
            }
            continue;
        }
        ++offset;
    }
    result = CNET_INDEPENDENCE_OK;
done:
    free(bytes);
    return result;
}

static size_t token_edit_distance(const AuditDocument *left,
                                  const AuditDocument *right) {
    size_t previous[AUDIT_TOKEN_MAX + 1u];
    size_t current[AUDIT_TOKEN_MAX + 1u];
    size_t row, column;
    for (column = 0; column <= right->token_count; ++column)
        previous[column] = column;
    for (row = 1; row <= left->token_count; ++row) {
        current[0] = row;
        for (column = 1; column <= right->token_count; ++column) {
            size_t deletion = previous[column] + 1u;
            size_t insertion = current[column - 1u] + 1u;
            size_t substitution = previous[column - 1u] +
                (left->tokens[row - 1u] == right->tokens[column - 1u] ?
                     0u : 1u);
            size_t best = deletion < insertion ? deletion : insertion;
            if (substitution < best) best = substitution;
            current[column] = best;
        }
        memcpy(previous, current,
               (right->token_count + 1u) * sizeof previous[0]);
    }
    return previous[right->token_count];
}

static size_t multiset_intersection(const AuditDocument *left,
                                    const AuditDocument *right) {
    size_t left_index = 0, right_index = 0, intersection = 0;
    while (left_index < left->token_count &&
           right_index < right->token_count) {
        uint64_t a = left->sorted_tokens[left_index];
        uint64_t b = right->sorted_tokens[right_index];
        if (a == b) {
            ++intersection;
            ++left_index;
            ++right_index;
        } else if (a < b) {
            ++left_index;
        } else {
            ++right_index;
        }
    }
    return intersection;
}

static size_t longest_common_run(const AuditDocument *left,
                                 const AuditDocument *right) {
    size_t previous[AUDIT_TOKEN_MAX + 1u] = {0};
    size_t current[AUDIT_TOKEN_MAX + 1u] = {0};
    size_t row, column, maximum = 0;
    for (row = 1; row <= left->token_count; ++row) {
        memset(current, 0,
               (right->token_count + 1u) * sizeof current[0]);
        for (column = 1; column <= right->token_count; ++column) {
            if (left->tokens[row - 1u] == right->tokens[column - 1u]) {
                current[column] = previous[column - 1u] + 1u;
                if (current[column] > maximum) maximum = current[column];
            }
        }
        memcpy(previous, current,
               (right->token_count + 1u) * sizeof previous[0]);
    }
    return maximum;
}

static int documents_overlap(const AuditDocument *candidate,
                             const AuditDocument *reference,
                             const char **metric_out) {
    size_t maximum, distance, intersection, union_count, run;
    if (strcmp(candidate->normalized, reference->normalized) == 0) {
        *metric_out = "canonical";
        return 1;
    }
    maximum = candidate->token_count > reference->token_count ?
        candidate->token_count : reference->token_count;
    distance = token_edit_distance(candidate, reference);
    if (maximum > 0 && (maximum - distance) * 100u >= maximum * 75u) {
        *metric_out = "token_levenshtein>=0.75";
        return 1;
    }
    if (candidate->token_count >= 6u && reference->token_count >= 6u) {
        intersection = multiset_intersection(candidate, reference);
        union_count = candidate->token_count + reference->token_count -
                      intersection;
        if (union_count > 0 && intersection * 100u >= union_count * 80u) {
            *metric_out = "token_jaccard>=0.80";
            return 1;
        }
    }
    run = longest_common_run(candidate, reference);
    if (run >= 6u) {
        *metric_out = "shared_six_token_run";
        return 1;
    }
    if (reference->token_count >= 4u &&
        run * 100u >= reference->token_count * 80u) {
        *metric_out = "reference_80pct_contiguous";
        return 1;
    }
    return 0;
}

int cnet_compete_independence_export_exclusions(
    const char *output_path,
    const char *const *fixture_paths, size_t fixture_count,
    const char *const *development_sources, size_t development_source_count,
    size_t *prompt_count,
    char *error, size_t error_capacity) {
    AuditSet exclusions;
    FILE *output = NULL;
    size_t index, document;
    int result = CNET_INDEPENDENCE_ERR_ARGUMENT;
    memset(&exclusions, 0, sizeof exclusions);
    if (prompt_count != NULL) *prompt_count = 0;
    if (error != NULL && error_capacity > 0) error[0] = '\0';
    if (output_path == NULL || output_path[0] == '\0' ||
        fixture_paths == NULL || fixture_count == 0 ||
        development_sources == NULL || development_source_count == 0) {
        set_error(error, error_capacity, "export inputs are required");
        return result;
    }
    if (audit_set_init(&exclusions) != 0) {
        set_error(error, error_capacity, "export allocation failed");
        return CNET_INDEPENDENCE_ERR_IO;
    }
    for (index = 0; index < fixture_count; ++index) {
        if (fixture_paths[index] == NULL || fixture_paths[index][0] == '\0')
            goto done;
        result = load_fixture_prompts(fixture_paths[index], &exclusions, 0,
                                      NULL, error, error_capacity);
        if (result != CNET_INDEPENDENCE_OK) goto done;
    }
    for (index = 0; index < development_source_count; ++index) {
        if (development_sources[index] == NULL ||
            development_sources[index][0] == '\0')
            goto done;
        result = load_source_literals(development_sources[index], &exclusions,
                                      error, error_capacity);
        if (result != CNET_INDEPENDENCE_OK) goto done;
    }
    output = fopen(output_path, "wb");
    if (output == NULL) {
        set_error(error, error_capacity, "cannot create exclusions %s: %s",
                  output_path, strerror(errno));
        result = CNET_INDEPENDENCE_ERR_IO;
        goto done;
    }
    if (fprintf(output, "#suite=CNET-ASI-5-excluded-prompts-v1\n") < 0 ||
        fprintf(output,
                "id\tsplit\tintent\tvalue_kind\texpected_value\tprompt\tprovenance\n") < 0) {
        result = CNET_INDEPENDENCE_ERR_IO;
        goto done;
    }
    for (document = 0; document < exclusions.count; ++document) {
        char prompt[AUDIT_TEXT_MAX];
        const unsigned char *source =
            (const unsigned char *)exclusions.documents[document].raw;
        size_t used = 0;
        while (*source != '\0') {
            unsigned char character = *source++;
            if (character >= 128u) {
                set_error(error, error_capacity,
                          "non-ASCII exclusion %s",
                          exclusions.documents[document].origin);
                result = CNET_INDEPENDENCE_ERR_FORMAT;
                goto done;
            }
            if (character < 0x20u || character == 0x7fu) character = ' ';
            if (used + 1u >= sizeof prompt) {
                result = CNET_INDEPENDENCE_ERR_FORMAT;
                goto done;
            }
            prompt[used++] = (char)character;
        }
        prompt[used] = '\0';
        {
            const unsigned char *origin = (const unsigned char *)
                exclusions.documents[document].origin;
            for (; *origin != '\0'; ++origin)
                if (*origin < 0x20u || *origin >= 0x7fu) {
                    set_error(error, error_capacity,
                              "unsafe exclusion origin");
                    result = CNET_INDEPENDENCE_ERR_FORMAT;
                    goto done;
                }
        }
        if (fprintf(output,
                    "excluded-%04zu\tdevelopment\tnone\tnone\t\t%s\t%s\n",
                    document, prompt,
                    exclusions.documents[document].origin) < 0) {
            result = CNET_INDEPENDENCE_ERR_IO;
            goto done;
        }
    }
    if (fflush(output) != 0 || ferror(output)) {
        result = CNET_INDEPENDENCE_ERR_IO;
        goto done;
    }
    result = CNET_INDEPENDENCE_OK;
done:
    if (output != NULL && fclose(output) != 0 &&
        result == CNET_INDEPENDENCE_OK)
        result = CNET_INDEPENDENCE_ERR_IO;
    if (result != CNET_INDEPENDENCE_OK) (void)remove(output_path);
    if (result == CNET_INDEPENDENCE_OK && prompt_count != NULL)
        *prompt_count = exclusions.count;
    audit_set_free(&exclusions);
    return result;
}

int cnet_compete_independence_audit(
    const char *candidate_fixture,
    const char *const *prior_fixtures, size_t prior_fixture_count,
    const char *const *development_sources, size_t development_source_count,
    CnetCompeteIndependenceReport *report,
    char *error, size_t error_capacity) {
    AuditSet candidates, exclusions;
    CnetCompeteIndependenceReport local;
    size_t candidate_index, exclusion_index, index;
    int result = CNET_INDEPENDENCE_ERR_ARGUMENT;
    memset(&candidates, 0, sizeof candidates);
    memset(&exclusions, 0, sizeof exclusions);
    memset(&local, 0, sizeof local);
    if (report != NULL) memset(report, 0, sizeof *report);
    if (error != NULL && error_capacity > 0) error[0] = '\0';
    if (candidate_fixture == NULL || candidate_fixture[0] == '\0' ||
        (prior_fixture_count == 0 && development_source_count == 0) ||
        (prior_fixture_count != 0 && prior_fixtures == NULL) ||
        (development_source_count != 0 && development_sources == NULL)) {
        set_error(error, error_capacity, "audit inputs are required");
        return CNET_INDEPENDENCE_ERR_ARGUMENT;
    }
    for (index = 0; index < prior_fixture_count; ++index)
        if (prior_fixtures[index] == NULL || prior_fixtures[index][0] == '\0')
            return CNET_INDEPENDENCE_ERR_ARGUMENT;
    for (index = 0; index < development_source_count; ++index)
        if (development_sources[index] == NULL ||
            development_sources[index][0] == '\0')
            return CNET_INDEPENDENCE_ERR_ARGUMENT;
    if (audit_set_init(&candidates) != 0 || audit_set_init(&exclusions) != 0) {
        set_error(error, error_capacity, "audit allocation failed");
        result = CNET_INDEPENDENCE_ERR_IO;
        goto done;
    }
    result = load_fixture_prompts(candidate_fixture, &candidates, 1, &local,
                                  error, error_capacity);
    if (result != CNET_INDEPENDENCE_OK) goto done;
    for (index = 0; index < prior_fixture_count; ++index) {
        result = load_fixture_prompts(prior_fixtures[index], &exclusions, 0,
                                      &local, error, error_capacity);
        if (result != CNET_INDEPENDENCE_OK) goto done;
    }
    for (index = 0; index < development_source_count; ++index) {
        result = load_source_literals(development_sources[index], &exclusions,
                                      error, error_capacity);
        if (result != CNET_INDEPENDENCE_OK) goto done;
    }
    local.candidate_prompts = candidates.count;
    local.excluded_prompts = exclusions.count;
    for (candidate_index = 0; candidate_index < candidates.count;
         ++candidate_index) {
        for (exclusion_index = 0; exclusion_index < exclusions.count;
             ++exclusion_index) {
            const char *metric = NULL;
            if (!documents_overlap(&candidates.documents[candidate_index],
                                   &exclusions.documents[exclusion_index],
                                   &metric))
                continue;
            if (strcmp(metric, "canonical") == 0)
                ++local.canonical_matches;
            else
                ++local.near_matches;
            set_error(error, error_capacity,
                      "prompt overlap metric=%s candidate=%s reference=%s",
                      metric, candidates.documents[candidate_index].origin,
                      exclusions.documents[exclusion_index].origin);
            result = CNET_INDEPENDENCE_ERR_OVERLAP;
            goto done;
        }
    }
    result = CNET_INDEPENDENCE_OK;
done:
    if (local.candidate_prompts == 0) local.candidate_prompts = candidates.count;
    if (local.excluded_prompts == 0) local.excluded_prompts = exclusions.count;
    if (report != NULL) *report = local;
    audit_set_free(&candidates);
    audit_set_free(&exclusions);
    return result;
}
