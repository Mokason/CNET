/* Generate C + .NET alphabets from config/json_toolcall_v2.json.
 *
 * Usage: gen_json_toolcall_alphabet [--check]
 * Emits include/json_toolcall_alphabet.inc and
 *        dotnet/Cce/JsonToolCall.Alphabet.g.cs
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#define MKDIR(p) mkdir((p), 0755)
#endif

#define ROOT_CFG "config/json_toolcall_v2.json"
#define C_OUT    "include/json_toolcall_alphabet.inc"
#define CS_OUT   "dotnet/Cce/JsonToolCall.Alphabet.g.cs"
#define MAX_ITEMS 64
#define MAX_STR   1024
#define MAX_FILE  (2 * 1024 * 1024)

typedef struct {
    char tools[MAX_ITEMS][MAX_STR];
    char feats[MAX_ITEMS][MAX_STR];
    char examples[MAX_ITEMS][MAX_STR];
    char unit_name[MAX_STR];
    char input_tag[MAX_STR];
    char goal_tag[MAX_STR];
    int n_tool, n_feat, n_ex;
} Alphabet;

static char *slurp(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    long sz;
    char *buf;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    sz = ftell(f);
    if (sz < 0 || sz > MAX_FILE) { fclose(f); return NULL; }
    rewind(f);
    buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf); fclose(f); return NULL;
    }
    buf[sz] = 0;
    fclose(f);
    if (out_len) *out_len = (size_t)sz;
    return buf;
}

static const char *skip_ws(const char *p) {
    while (*p && isspace((unsigned char)*p)) p++;
    return p;
}

/* Extract a JSON string value starting at opening quote. Writes unescaped. */
static const char *parse_string(const char *p, char *out, size_t out_sz) {
    size_t o = 0;
    if (*p != '"') return NULL;
    p++;
    while (*p && *p != '"') {
        char c = *p++;
        if (c == '\\' && *p) {
            char e = *p++;
            switch (e) {
            case '"': case '\\': case '/': c = e; break;
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            default: c = e; break;
            }
        }
        if (o + 1 < out_sz) out[o++] = c;
    }
    if (*p != '"') return NULL;
    out[o] = 0;
    return p + 1;
}

static const char *find_key(const char *json, const char *key) {
    char pat[128];
    const char *p;
    snprintf(pat, sizeof pat, "\"%s\"", key);
    p = strstr(json, pat);
    if (!p) return NULL;
    p += strlen(pat);
    p = skip_ws(p);
    if (*p != ':') return NULL;
    return skip_ws(p + 1);
}

static int parse_string_array(const char *p, char arr[][MAX_STR], int maxn, int *nout) {
    int n = 0;
    p = skip_ws(p);
    if (*p != '[') return -1;
    p = skip_ws(p + 1);
    while (*p && *p != ']') {
        if (n >= maxn) return -1;
        p = parse_string(p, arr[n], MAX_STR);
        if (!p) return -1;
        n++;
        p = skip_ws(p);
        if (*p == ',') { p = skip_ws(p + 1); continue; }
        if (*p == ']') break;
        return -1;
    }
    if (*p != ']') return -1;
    *nout = n;
    return 0;
}

static int parse_alphabet(const char *json, Alphabet *a) {
    const char *p;
    memset(a, 0, sizeof *a);
    p = find_key(json, "unit_name");
    if (!p || !(p = parse_string(p, a->unit_name, sizeof a->unit_name))) return -1;
    p = find_key(json, "input_tag");
    if (!p || !(p = parse_string(p, a->input_tag, sizeof a->input_tag))) return -1;
    p = find_key(json, "goal_tag");
    if (!p || !(p = parse_string(p, a->goal_tag, sizeof a->goal_tag))) return -1;
    p = find_key(json, "tools");
    if (!p || parse_string_array(p, a->tools, MAX_ITEMS, &a->n_tool) != 0) return -1;
    p = find_key(json, "features");
    if (!p || parse_string_array(p, a->feats, MAX_ITEMS, &a->n_feat) != 0) return -1;
    p = find_key(json, "examples");
    if (!p || parse_string_array(p, a->examples, MAX_ITEMS, &a->n_ex) != 0) return -1;
    if (a->n_tool != a->n_ex) {
        fprintf(stderr, "tools/examples length mismatch\n");
        return -1;
    }
    return 0;
}

static void c_escape(const char *s, char *out, size_t out_sz) {
    size_t o = 0;
    if (o + 1 < out_sz) out[o++] = '"';
    for (; *s && o + 2 < out_sz; s++) {
        if (*s == '\\' || *s == '"') {
            if (o + 3 >= out_sz) break;
            out[o++] = '\\';
            out[o++] = *s;
        } else {
            out[o++] = *s;
        }
    }
    if (o + 1 < out_sz) out[o++] = '"';
    out[o] = 0;
}

static int ensure_parent(const char *path) {
    char tmp[512];
    size_t i, len;
    snprintf(tmp, sizeof tmp, "%s", path);
    len = strlen(tmp);
    for (i = 1; i < len; i++) {
        if (tmp[i] == '/' || tmp[i] == '\\') {
            char c = tmp[i];
            tmp[i] = 0;
            MKDIR(tmp);
            tmp[i] = c;
        }
    }
    return 0;
}

static char *build_c(const Alphabet *a, size_t *out_len) {
    char esc[MAX_STR * 2];
    size_t cap = 65536, len = 0;
    char *buf = (char *)malloc(cap);
    int i;
    if (!buf) return NULL;
#define APP(...) do { \
        char _t[4096]; int _n = snprintf(_t, sizeof _t, __VA_ARGS__); \
        if (_n < 0) { free(buf); return NULL; } \
        if (len + (size_t)_n + 1 > cap) { \
            cap = (len + (size_t)_n + 1) * 2; \
            { char *_nb = (char *)realloc(buf, cap); if (!_nb) { free(buf); return NULL; } buf = _nb; } \
        } \
        memcpy(buf + len, _t, (size_t)_n); len += (size_t)_n; buf[len] = 0; \
    } while (0)

    APP("/* AUTO-GENERATED by tools/gen_json_toolcall_alphabet.c -- do not edit. */\n");
    APP("#define CNET_JTC_N_TOOL_GEN %d\n", a->n_tool);
    APP("#define CNET_JTC_N_FEAT_GEN %d\n", a->n_feat);
    APP("static const char *const CNET_JTC_TOOLS_GEN[%d] = {\n", a->n_tool);
    for (i = 0; i < a->n_tool; i++) {
        c_escape(a->tools[i], esc, sizeof esc);
        APP("    %s%s\n", esc, i + 1 < a->n_tool ? "," : "");
    }
    APP("};\n");
    APP("static const char *const CNET_JTC_FEATS_GEN[%d] = {\n", a->n_feat);
    for (i = 0; i < a->n_feat; i++) {
        c_escape(a->feats[i], esc, sizeof esc);
        APP("    %s%s\n", esc, i + 1 < a->n_feat ? "," : "");
    }
    APP("};\n");
    APP("static const char *const CNET_JTC_EXAMPLES_GEN[%d] = {\n", a->n_tool);
    for (i = 0; i < a->n_tool; i++) {
        c_escape(a->examples[i], esc, sizeof esc);
        APP("    %s%s\n", esc, i + 1 < a->n_tool ? "," : "");
    }
    APP("};\n");
#undef APP
    if (out_len) *out_len = len;
    return buf;
}

static char *build_cs(const Alphabet *a, size_t *out_len) {
    char esc[MAX_STR * 2];
    size_t cap = 65536, len = 0;
    char *buf = (char *)malloc(cap);
    int i;
    if (!buf) return NULL;
#define APP(...) do { \
        char _t[4096]; int _n = snprintf(_t, sizeof _t, __VA_ARGS__); \
        if (_n < 0) { free(buf); return NULL; } \
        if (len + (size_t)_n + 1 > cap) { \
            cap = (len + (size_t)_n + 1) * 2; \
            { char *_nb = (char *)realloc(buf, cap); if (!_nb) { free(buf); return NULL; } buf = _nb; } \
        } \
        memcpy(buf + len, _t, (size_t)_n); len += (size_t)_n; buf[len] = 0; \
    } while (0)

    APP("// <auto-generated />\n");
    APP("// Generated by tools/gen_json_toolcall_alphabet.c from config/json_toolcall_v2.json\n");
    APP("// Do not edit by hand.\n");
    APP("namespace CNET.Cce;\n\n");
    APP("public static partial class JsonToolCall\n{\n");
    APP("    public const int FeatureCountGen = %d;\n", a->n_feat);
    APP("    public const int ToolCountGen = %d;\n", a->n_tool);
    c_escape(a->unit_name, esc, sizeof esc);
    APP("    public const string UnitNameGen = %s;\n", esc);
    c_escape(a->input_tag, esc, sizeof esc);
    APP("    public const string InputTagGen = %s;\n", esc);
    c_escape(a->goal_tag, esc, sizeof esc);
    APP("    public const string GoalTagGen = %s;\n\n", esc);

    APP("    private static readonly string[] ToolNamesGen =\n    {\n");
    for (i = 0; i < a->n_tool; i++) {
        c_escape(a->tools[i], esc, sizeof esc);
        APP("        %s%s\n", esc, i + 1 < a->n_tool ? "," : "");
    }
    APP("    };\n\n");

    APP("    private static readonly string[] FeatureNamesGen =\n    {\n");
    for (i = 0; i < a->n_feat; i++) {
        c_escape(a->feats[i], esc, sizeof esc);
        APP("        %s%s\n", esc, i + 1 < a->n_feat ? "," : "");
    }
    APP("    };\n\n");

    APP("    private static readonly string[] ExampleJsonGen =\n    {\n");
    for (i = 0; i < a->n_tool; i++) {
        c_escape(a->examples[i], esc, sizeof esc);
        APP("        %s%s\n", esc, i + 1 < a->n_tool ? "," : "");
    }
    APP("    };\n");
    APP("}\n");
#undef APP
    if (out_len) *out_len = len;
    return buf;
}

static int files_equal(const char *path, const char *want, size_t want_len) {
    size_t got_len = 0;
    char *got = slurp(path, &got_len);
    int eq;
    if (!got) return 0;
    eq = (got_len == want_len && memcmp(got, want, want_len) == 0);
    free(got);
    return eq;
}

static int write_text(const char *path, const char *data, size_t len) {
    FILE *f;
    ensure_parent(path);
    f = fopen(path, "wb");
    if (!f) return -1;
    if (fwrite(data, 1, len, f) != len) { fclose(f); return -1; }
    fclose(f);
    return 0;
}

int main(int argc, char **argv) {
    int check = 0, i;
    char *json;
    Alphabet a;
    char *c_text, *cs_text;
    size_t c_len = 0, cs_len = 0;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--check") == 0) check = 1;
        else {
            fprintf(stderr, "usage: %s [--check]\n", argv[0]);
            return 2;
        }
    }

    json = slurp(ROOT_CFG, NULL);
    if (!json) {
        fprintf(stderr, "cannot read %s\n", ROOT_CFG);
        return 1;
    }
    if (parse_alphabet(json, &a) != 0) {
        fprintf(stderr, "failed to parse %s\n", ROOT_CFG);
        free(json);
        return 1;
    }
    free(json);

    c_text = build_c(&a, &c_len);
    cs_text = build_cs(&a, &cs_len);
    if (!c_text || !cs_text) {
        free(c_text); free(cs_text);
        return 1;
    }

    if (check) {
        int stale = 0;
        if (!files_equal(C_OUT, c_text, c_len)) {
            fprintf(stderr, "stale generated JSON tool-call alphabet: %s", C_OUT);
            stale = 1;
        }
        if (!files_equal(CS_OUT, cs_text, cs_len)) {
            fprintf(stderr, "%s%s", stale ? ", " : "stale generated JSON tool-call alphabet: ", CS_OUT);
            stale = 1;
        }
        if (stale) {
            fprintf(stderr, "\n");
            free(c_text); free(cs_text);
            return 1;
        }
        printf("JSON_TOOLCALL_ALPHABET_CHECK_PASS\n");
        free(c_text); free(cs_text);
        return 0;
    }

    if (write_text(C_OUT, c_text, c_len) != 0 ||
        write_text(CS_OUT, cs_text, cs_len) != 0) {
        fprintf(stderr, "write failed\n");
        free(c_text); free(cs_text);
        return 1;
    }
    printf("wrote %s\n", C_OUT);
    printf("wrote %s\n", CS_OUT);
    printf("JSON_TOOLCALL_ALPHABET_GEN_OK\n");
    free(c_text); free(cs_text);
    return 0;
}
