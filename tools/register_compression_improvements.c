#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#define SOURCE "cnet_model_compression_phase_log"
#define PLAN_PATH "CNET-Improvement-Plan-Model-Compression.md"
#define MAX_PHASES 64

typedef struct {
    char** items;
    size_t count;
    size_t cap;
} string_list;

typedef struct {
    int present;
    int phase_number;
    char* source_phase;
    char* date;
    char* status;
    char* summary;
    string_list artifacts;
    string_list tests;
    string_list pending;
} phase_record;

static void* xmalloc(size_t n) {
    void* p = malloc(n ? n : 1);
    if (!p) {
        fprintf(stderr, "out of memory\n");
        exit(2);
    }
    return p;
}

static void* xrealloc(void* ptr, size_t n) {
    void* p = realloc(ptr, n ? n : 1);
    if (!p) {
        fprintf(stderr, "out of memory\n");
        exit(2);
    }
    return p;
}

static char* xstrdup(const char* s) {
    size_t n = strlen(s) + 1;
    char* out = (char*)xmalloc(n);
    memcpy(out, s, n);
    return out;
}

static void list_push(string_list* list, char* item) {
    if (list->count == list->cap) {
        size_t next = list->cap ? list->cap * 2 : 4;
        list->items = (char**)xrealloc(list->items, next * sizeof(char*));
        list->cap = next;
    }
    list->items[list->count++] = item;
}

static void list_free(string_list* list) {
    size_t i;
    for (i = 0; i < list->count; ++i) {
        free(list->items[i]);
    }
    free(list->items);
    memset(list, 0, sizeof(*list));
}

static void record_free(phase_record* record) {
    free(record->source_phase);
    free(record->date);
    free(record->status);
    free(record->summary);
    list_free(&record->artifacts);
    list_free(&record->tests);
    list_free(&record->pending);
    memset(record, 0, sizeof(*record));
}

static char lower_ascii(char ch) {
    return (char)tolower((unsigned char)ch);
}

static int starts_phase_word(const char* text) {
    return lower_ascii(text[0]) == 'p' && lower_ascii(text[1]) == 'h' &&
           lower_ascii(text[2]) == 'a' && lower_ascii(text[3]) == 's' &&
           lower_ascii(text[4]) == 'e';
}

static int extract_phase_number(const char* phase) {
    size_t i;
    for (i = 0; phase[i]; ++i) {
        size_t j;
        int value = 0;
        if (!starts_phase_word(phase + i)) {
            continue;
        }
        j = i + 5;
        while (phase[j] == ' ' || phase[j] == '_' || phase[j] == '-') {
            ++j;
        }
        if (!isdigit((unsigned char)phase[j])) {
            continue;
        }
        while (isdigit((unsigned char)phase[j])) {
            value = value * 10 + (phase[j] - '0');
            ++j;
        }
        return value;
    }
    return -1;
}

static const char* phase_title(int phase_number) {
    switch (phase_number) {
        case 1: return "Counterfactual Routing Head";
        case 2: return "Sparse Per-Specialist KV Routing";
        case 3: return "Narrative Coherence Contracts";
        case 4: return "Uncertainty-Aware Activation + Hermes Integration";
        case 5: return "Improvement Engine Integration";
        default: return "CNET Compression Phase";
    }
}

static double phase_priority(int phase_number) {
    switch (phase_number) {
        case 1: return 0.95;
        case 2: return 0.94;
        case 3: return 0.87;
        case 4: return 0.84;
        case 5: return 0.81;
        default: return 0.75;
    }
}

static void append_char(char** buf, size_t* len, size_t* cap, char ch) {
    if (*len + 2 > *cap) {
        size_t next = *cap ? *cap * 2 : 32;
        *buf = (char*)xrealloc(*buf, next);
        *cap = next;
    }
    (*buf)[(*len)++] = ch;
    (*buf)[*len] = '\0';
}

static char* parse_json_string_at(const char** cursor) {
    const char* p = *cursor;
    char* out = NULL;
    size_t len = 0;
    size_t cap = 0;

    if (*p != '"') {
        return NULL;
    }
    ++p;
    while (*p && *p != '"') {
        if (*p == '\\') {
            ++p;
            if (*p == '"' || *p == '\\' || *p == '/') {
                append_char(&out, &len, &cap, *p++);
            } else if (*p == 'n') {
                append_char(&out, &len, &cap, '\n');
                ++p;
            } else if (*p == 'r') {
                append_char(&out, &len, &cap, '\r');
                ++p;
            } else if (*p == 't') {
                append_char(&out, &len, &cap, '\t');
                ++p;
            } else if (*p == 'u') {
                int k;
                append_char(&out, &len, &cap, '?');
                ++p;
                for (k = 0; k < 4 && isxdigit((unsigned char)*p); ++k) {
                    ++p;
                }
            } else if (*p) {
                append_char(&out, &len, &cap, *p++);
            }
        } else {
            append_char(&out, &len, &cap, *p++);
        }
    }
    if (*p != '"') {
        free(out);
        return NULL;
    }
    ++p;
    if (!out) {
        out = xstrdup("");
    }
    *cursor = p;
    return out;
}

static const char* skip_ws(const char* p) {
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
        ++p;
    }
    return p;
}

static const char* find_key_value(const char* json, const char* key) {
    char pattern[128];
    const char* p;
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    p = json;
    while ((p = strstr(p, pattern)) != NULL) {
        const char* q = skip_ws(p + strlen(pattern));
        if (*q == ':') {
            return skip_ws(q + 1);
        }
        p = p + 1;
    }
    return NULL;
}

static char* json_get_string(const char* json, const char* key) {
    const char* p = find_key_value(json, key);
    if (!p || *p != '"') {
        return NULL;
    }
    return parse_json_string_at(&p);
}

static string_list json_get_string_array(const char* json, const char* key) {
    string_list out;
    const char* p = find_key_value(json, key);
    memset(&out, 0, sizeof(out));
    if (!p || *p != '[') {
        return out;
    }
    ++p;
    for (;;) {
        p = skip_ws(p);
        if (*p == ']') {
            break;
        }
        if (*p == ',') {
            ++p;
            continue;
        }
        if (*p == '"') {
            char* item = parse_json_string_at(&p);
            if (item) {
                list_push(&out, item);
            }
            continue;
        }
        while (*p && *p != ',' && *p != ']') {
            ++p;
        }
    }
    return out;
}

static int parse_record_line(const char* line, phase_record* record) {
    char* phase = json_get_string(line, "phase");
    char* summary;
    int phase_number;

    memset(record, 0, sizeof(*record));
    if (!phase) {
        return 0;
    }
    phase_number = extract_phase_number(phase);
    if (phase_number < 1 || phase_number >= MAX_PHASES) {
        free(phase);
        return 0;
    }

    summary = json_get_string(line, "summary");
    if (!summary) {
        summary = json_get_string(line, "notes");
    }

    record->present = 1;
    record->phase_number = phase_number;
    record->source_phase = phase;
    record->date = json_get_string(line, "date");
    record->status = json_get_string(line, "status");
    record->summary = summary ? summary : xstrdup("");
    record->artifacts = json_get_string_array(line, "artifacts");
    record->tests = json_get_string_array(line, "tests");
    record->pending = json_get_string_array(line, "pending");

    if (!record->date) {
        record->date = xstrdup("");
    }
    if (!record->status) {
        record->status = xstrdup("implemented");
    }
    return 1;
}

static char* read_file_or_null(const char* path, size_t* size_out) {
    FILE* f = fopen(path, "rb");
    char* data;
    long size;
    size_t got;

    if (!f) {
        if (size_out) {
            *size_out = 0;
        }
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    size = ftell(f);
    if (size < 0) {
        fclose(f);
        return NULL;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    data = (char*)xmalloc((size_t)size + 1);
    got = fread(data, 1, (size_t)size, f);
    fclose(f);
    data[got] = '\0';
    if (size_out) {
        *size_out = got;
    }
    return data;
}

static int load_records(const char* log_path, phase_record records[MAX_PHASES]) {
    size_t size = 0;
    char* data = read_file_or_null(log_path, &size);
    char* line;
    int count = 0;
    (void)size;

    if (!data) {
        fprintf(stderr, "could not read %s\n", log_path);
        return -1;
    }

    line = data;
    while (line && *line) {
        char* next = strchr(line, '\n');
        phase_record parsed;
        if (next) {
            *next = '\0';
        }
        if (*line) {
            size_t n = strlen(line);
            if (n && line[n - 1] == '\r') {
                line[n - 1] = '\0';
            }
            if (parse_record_line(line, &parsed)) {
                int phase = parsed.phase_number;
                if (!records[phase].present) {
                    ++count;
                }
                record_free(&records[phase]);
                records[phase] = parsed;
            }
        }
        line = next ? next + 1 : NULL;
    }

    free(data);
    return count;
}

static void ensure_parent_dirs(const char* path) {
    char* copy = xstrdup(path);
    char* p = copy;
    if (*p == '/') {
        ++p;
    }
    for (; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            if (copy[0] && mkdir(copy, 0777) != 0 && errno != EEXIST) {
                fprintf(stderr, "could not create directory %s: %s\n", copy, strerror(errno));
                free(copy);
                exit(2);
            }
            *p = '/';
        }
    }
    free(copy);
}

static FILE* open_output(const char* path) {
    FILE* f;
    ensure_parent_dirs(path);
    f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "could not write %s: %s\n", path, strerror(errno));
        exit(2);
    }
    return f;
}

static void json_escape(FILE* f, const char* s) {
    const unsigned char* p = (const unsigned char*)s;
    for (; *p; ++p) {
        switch (*p) {
            case '"': fputs("\\\"", f); break;
            case '\\': fputs("\\\\", f); break;
            case '\b': fputs("\\b", f); break;
            case '\f': fputs("\\f", f); break;
            case '\n': fputs("\\n", f); break;
            case '\r': fputs("\\r", f); break;
            case '\t': fputs("\\t", f); break;
            default:
                if (*p < 0x20 || *p > 0x7e) {
                    fprintf(f, "\\u%04x", (unsigned int)*p);
                } else {
                    fputc(*p, f);
                }
                break;
        }
    }
}

static void json_string(FILE* f, const char* s) {
    fputc('"', f);
    json_escape(f, s ? s : "");
    fputc('"', f);
}

static void json_string_array(FILE* f, const string_list* list) {
    size_t i;
    fputc('[', f);
    for (i = 0; i < list->count; ++i) {
        if (i) {
            fputc(',', f);
        }
        json_string(f, list->items[i]);
    }
    fputc(']', f);
}

static void write_tags(FILE* f, int phase_number) {
    fprintf(f, "[\"cnet\",\"model-compression\",\"phase-%d\"", phase_number);
    switch (phase_number) {
        case 1:
            fputs(",\"counterfactual-routing\",\"hallucination-reduction\"", f);
            break;
        case 2:
            fputs(",\"sparse-kv\",\"long-context\"", f);
            break;
        case 3:
            fputs(",\"narrative-coherence\",\"creative-fidelity\"", f);
            break;
        case 4:
            fputs(",\"uncertainty\",\"hermes-hosting\",\"compression-gradients\"", f);
            break;
        case 5:
            fputs(",\"suggestion-registry\",\"training-data\",\"self-improvement\"", f);
            break;
        default:
            break;
    }
    fputc(']', f);
}

static void write_record_id(FILE* f, int phase_number) {
    fprintf(f, "cnet-compression-phase-%d", phase_number);
}

static void write_suggestion_row(FILE* f, const phase_record* r) {
    fputs("{\"id\":\"", f);
    write_record_id(f, r->phase_number);
    fputs("\",\"title\":", f);
    json_string(f, phase_title(r->phase_number));
    fputs(",\"source\":\"" SOURCE "\",\"plan\":\"" PLAN_PATH "\",\"status\":", f);
    json_string(f, r->status);
    fprintf(f, ",\"priority\":%.2f", phase_priority(r->phase_number));
    fputs(",\"description\":", f);
    json_string(f, r->summary);
    fputs(",\"evidence\":{\"date\":", f);
    json_string(f, r->date);
    fputs(",\"tests\":", f);
    json_string_array(f, &r->tests);
    fputs(",\"artifacts\":", f);
    json_string_array(f, &r->artifacts);
    fputs("},\"next_steps\":", f);
    json_string_array(f, &r->pending);
    fputs(",\"tags\":", f);
    write_tags(f, r->phase_number);
    fputs("}\n", f);
}

static void write_training_response(FILE* f, const phase_record* r) {
    fputc('"', f);
    json_escape(f, "Implemented ");
    json_escape(f, phase_title(r->phase_number));
    json_escape(f, " for CNET model compression.");
    if (r->summary && r->summary[0]) {
        json_escape(f, " ");
        json_escape(f, r->summary);
    }
    if (r->artifacts.count) {
        size_t i;
        json_escape(f, " Artifacts: ");
        for (i = 0; i < r->artifacts.count; ++i) {
            if (i) {
                json_escape(f, ", ");
            }
            json_escape(f, r->artifacts.items[i]);
        }
        json_escape(f, ".");
    }
    if (r->tests.count) {
        size_t i;
        json_escape(f, " Verification: ");
        for (i = 0; i < r->tests.count; ++i) {
            if (i) {
                json_escape(f, "; ");
            }
            json_escape(f, r->tests.items[i]);
        }
        json_escape(f, ".");
    }
    if (r->pending.count) {
        size_t i;
        json_escape(f, " Remaining work: ");
        for (i = 0; i < r->pending.count; ++i) {
            if (i) {
                json_escape(f, "; ");
            }
            json_escape(f, r->pending.items[i]);
        }
        json_escape(f, ".");
    }
    fputc('"', f);
}

static void write_training_row(FILE* f, const phase_record* r) {
    fputs("{\"instruction\":", f);
    fputc('"', f);
    fprintf(f, "Implement and verify CNET model compression Phase %d: ", r->phase_number);
    json_escape(f, phase_title(r->phase_number));
    fputs(".\"", f);
    fputs(",\"response\":", f);
    write_training_response(f, r);
    fputs(",\"metadata\":{\"id\":\"", f);
    write_record_id(f, r->phase_number);
    fputs("\",\"source\":\"" SOURCE "\",\"plan\":\"" PLAN_PATH "\",\"phase\":", f);
    json_string(f, r->source_phase);
    fprintf(f, ",\"phase_number\":%d,\"status\":", r->phase_number);
    json_string(f, r->status);
    fputs(",\"date\":", f);
    json_string(f, r->date);
    fputs(",\"tags\":", f);
    write_tags(f, r->phase_number);
    fputs("}}\n", f);
}

static int write_suggestions(const char* path, phase_record records[MAX_PHASES]) {
    FILE* f = open_output(path);
    int i;
    int count = 0;
    for (i = 0; i < MAX_PHASES; ++i) {
        if (records[i].present) {
            write_suggestion_row(f, &records[i]);
            ++count;
        }
    }
    fclose(f);
    return count;
}

static void write_preserved_training_rows(FILE* f, char* existing) {
    char* line = existing;
    while (line && *line) {
        char* next = strchr(line, '\n');
        if (next) {
            *next = '\0';
        }
        if (*line) {
            size_t n = strlen(line);
            if (n && line[n - 1] == '\r') {
                line[n - 1] = '\0';
            }
            if (*line && !strstr(line, SOURCE)) {
                fputs(line, f);
                fputc('\n', f);
            }
        }
        line = next ? next + 1 : NULL;
    }
}

static int write_training_data(const char* path, phase_record records[MAX_PHASES]) {
    size_t size = 0;
    char* existing = read_file_or_null(path, &size);
    FILE* f = open_output(path);
    int i;
    int count = 0;
    (void)size;

    if (existing) {
        write_preserved_training_rows(f, existing);
        free(existing);
    }
    for (i = 0; i < MAX_PHASES; ++i) {
        if (records[i].present) {
            write_training_row(f, &records[i]);
            ++count;
        }
    }
    fclose(f);
    return count;
}

static void usage(const char* argv0) {
    fprintf(stderr,
            "usage: %s [--log path] [--training-data path] [--suggestions path] [--dry-run]\n",
            argv0);
}

int main(int argc, char** argv) {
    const char* log_path = "implementation_log.jsonl";
    const char* training_path = "training_data.jsonl";
    const char* suggestions_path = "suggestions/cnet_compression_suggestions.jsonl";
    int dry_run = 0;
    phase_record records[MAX_PHASES];
    int phase_count;
    int suggestions_count = 0;
    int training_count = 0;
    int i;

    memset(records, 0, sizeof(records));
    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--log") == 0 && i + 1 < argc) {
            log_path = argv[++i];
        } else if (strcmp(argv[i], "--training-data") == 0 && i + 1 < argc) {
            training_path = argv[++i];
        } else if (strcmp(argv[i], "--suggestions") == 0 && i + 1 < argc) {
            suggestions_path = argv[++i];
        } else if (strcmp(argv[i], "--dry-run") == 0) {
            dry_run = 1;
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    phase_count = load_records(log_path, records);
    if (phase_count <= 0) {
        fprintf(stderr, "no phase records found in %s\n", log_path);
        return 1;
    }

    if (!dry_run) {
        suggestions_count = write_suggestions(suggestions_path, records);
        training_count = write_training_data(training_path, records);
    } else {
        suggestions_count = phase_count;
        training_count = phase_count;
    }

    printf("phase records: %d (", phase_count);
    for (i = 0; i < MAX_PHASES; ++i) {
        if (records[i].present) {
            printf("%s", i == 1 ? "" : ", ");
            printf("cnet-compression-phase-%d", i);
        }
    }
    printf(")\n");
    printf("suggestions: %d -> %s\n", suggestions_count, suggestions_path);
    printf("training rows: %d generated -> %s\n", training_count, training_path);

    for (i = 0; i < MAX_PHASES; ++i) {
        record_free(&records[i]);
    }
    return 0;
}
