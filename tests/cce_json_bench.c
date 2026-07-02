#include "../include/cce/cce_cascade.h"
#include "../include/cce/cce_block.h"
#include "../include/cce/cce_tensor.h"
#include "../include/cce/cce_learn.h"
#include <ctype.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <process.h>
#include <io.h>
#define popen _popen
#define pclose _pclose
#endif

#define MAX_JSON_VOCAB 256
#define MAX_CONTEXT 32
#define MAX_JSON_LEN 1024
#define JSON_TERM_CHAR '|'
#define MAX_JSON_STACK 64
#define JSON_URL_FETCH_BUF 131072
#define JSON_URL_DEFAULT_TEXT_KEY "text"

typedef struct {
    char **lines;
    int   count;
    int   cap;
} json_corpus;

typedef struct {
    int char_to_id[256];
    char id_to_char[MAX_JSON_VOCAB];
    int size;
} json_vocab;

static void corpus_init(json_corpus *c) {
    c->lines = NULL;
    c->count = 0;
    c->cap = 0;
}

static void corpus_free(json_corpus *c) {
    if (!c) return;
    for (int i = 0; i < c->count; ++i) free(c->lines[i]);
    free(c->lines);
    c->lines = NULL;
    c->count = c->cap = 0;
}

static char *dup_line(const char *s) {
    size_t n = strlen(s);
    char *out = (char *)malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, s, n + 1);
    return out;
}

static void corpus_add(json_corpus *c, const char *s) {
    if (!s || !*s) return;
    if (c->count >= c->cap) {
        int ncap = c->cap == 0 ? 32 : c->cap * 2;
        char **nlines = (char **)realloc(c->lines, (size_t)ncap * sizeof(char *));
        if (!nlines) return;
        c->lines = nlines;
        c->cap = ncap;
    }
    c->lines[c->count++] = dup_line(s);
}

static void trim_in_place(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r')) s[--n] = 0;
    size_t l = 0;
    while (l < n && isspace((unsigned char)s[l])) l++;
    if (l > 0) memmove(s, s + l, n - l + 1);
    while (n > 0 && isspace((unsigned char)s[n - 1])) s[--n] = 0;
}

static void skip_ws_json(const char *s, int *i, int n);
static bool parse_value_json(const char *s, int *i, int n);

static bool parse_json_string_value(const char *s, int *idx, int n,
                                  char *out, size_t out_cap) {
    if (!s || !idx || *idx < 0 || *idx >= n || s[*idx] != '"' || !out || out_cap == 0) return false;
    (*idx)++;
    size_t out_len = 0;
    while (*idx < n) {
        unsigned char c = (unsigned char)s[*idx];
        if (c == '"') {
            (*idx)++;
            out[out_len] = 0;
            return true;
        }
        if (c == '\\') {
            (*idx)++;
            if (*idx >= n) return false;
            c = (unsigned char)s[*idx];
            if (c == '"' || c == '\\' || c == '/' || c == 'b' || c == 'f' || c == 'n' ||
                c == 'r' || c == 't') {
                if (c == 'b') c = '\b';
                else if (c == 'f') c = '\f';
                else if (c == 'n') c = ' ';
                else if (c == 'r') c = '\r';
                else if (c == 't') c = '\t';
                if (out_len + 1 < out_cap) out[out_len++] = (char)c;
                (*idx)++;
                continue;
            }
            if (c == 'u') {
                if (*idx + 4 >= n) return false;
                (*idx)++;
                int digits = 0;
                while (digits < 4 && *idx < n && isxdigit((unsigned char)s[*idx])) {
                    (*idx)++;
                    digits++;
                }
                if (digits != 4) return false;
                if (out_len + 1 < out_cap) out[out_len++] = '?';
                continue;
            }
            return false;
        }
        if (c < 0x20) return false;
        if (out_len + 1 < out_cap) out[out_len++] = (char)c;
        (*idx)++;
    }
    return false;
}

static bool extract_json_text_field(const char *line, char *out, size_t out_cap) {
    if (!line || !out || out_cap == 0) return false;
    const char *p = line;
    const char *key = "\"" JSON_URL_DEFAULT_TEXT_KEY "\"";
    size_t key_len = sizeof(JSON_URL_DEFAULT_TEXT_KEY) - 1;
    while ((p = strstr(p, key)) != NULL) {
        const char *q = p + key_len + 2;
        while (*q && isspace((unsigned char)*q)) q++;
        if (*q != ':') {
            p = q;
            continue;
        }
        q++;
        while (*q && isspace((unsigned char)*q)) q++;
        if (*q != '"') {
            p = q;
            continue;
        }
        int idx = (int)(q - line);
        if (parse_json_string_value(line, &idx, (int)strlen(line), out, out_cap)) {
            trim_in_place(out);
            return out[0] != 0;
        }
        p = q;
    }
    return false;
}

static bool corpus_from_json_array(json_corpus *c, const char *text, int max_lines) {
    if (!c || !text || max_lines <= 0) return false;
    int n = (int)strlen(text);
    int i = 0;
    skip_ws_json(text, &i, n);
    if (i >= n || text[i] != '[') return false;
    i++;
    int start_count = c->count;
    skip_ws_json(text, &i, n);
    if (i < n && text[i] == ']') return false;

    while (i < n && c->count - start_count < max_lines) {
        skip_ws_json(text, &i, n);
        if (i >= n) break;
        if (text[i] == ']') break;

        int item_start = i;
        if (!parse_value_json(text, &i, n)) break;
        int item_end = i;
        while (item_end > item_start && isspace((unsigned char)text[item_end - 1])) item_end--;
        while (item_start < item_end && isspace((unsigned char)text[item_start])) item_start++;

        if (item_end > item_start) {
            size_t item_len = (size_t)(item_end - item_start);
            char *item = (char *)malloc(item_len + 1);
            if (!item) return false;
            memcpy(item, text + item_start, item_len);
            item[item_len] = 0;
            corpus_add(c, item);
            free(item);
        }

        skip_ws_json(text, &i, n);
        if (i < n && text[i] == ',') {
            i++;
            continue;
        }
        if (i < n && text[i] == ']') break;
    }
    return c->count > start_count;
}

static void vocab_init(json_vocab *v) {
    for (int i = 0; i < 256; ++i) v->char_to_id[i] = -1;
    v->size = 0;
}

static bool vocab_add_char(json_vocab *v, unsigned char ch) {
    if (v->char_to_id[ch] >= 0) return true;
    if (v->size >= MAX_JSON_VOCAB) return false;
    v->char_to_id[ch] = v->size;
    v->id_to_char[v->size] = (char)ch;
    v->size++;
    return true;
}

static void skip_ws_json(const char *s, int *i, int n) {
    while (*i < n && isspace((unsigned char)s[*i])) (*i)++;
}

static bool parse_string_json(const char *s, int *i, int n) {
    if (*i >= n || s[*i] != '"') return false;
    (*i)++;
    while (*i < n) {
        char c = s[*i];
        if (c == '\\') {
            (*i)++;
            if (*i >= n) return false;
            char e = s[*i];
            if (e == 'u') {
                (*i)++;
                for (int k = 0; k < 4; ++k) {
                    if (*i >= n || !isxdigit((unsigned char)s[*i])) return false;
                    (*i)++;
                }
                continue;
            }
            if (e == '"' || e == '\\' || e == '/' || e == 'b' || e == 'f' ||
                e == 'n' || e == 'r' || e == 't') {
                (*i)++;
                continue;
            }
            return false;
        }
        if (c == '"') {
            (*i)++;
            return true;
        }
        if ((unsigned char)c < 0x20) return false;
        (*i)++;
    }
    return false;
}

static bool parse_value_json(const char *s, int *i, int n);

static bool parse_number_json(const char *s, int *i, int n) {
    int start = *i;
    if (*i < n && s[*i] == '-') (*i)++;
    int digits = 0;
    while (*i < n && isdigit((unsigned char)s[*i])) {
        (*i)++;
        digits++;
    }
    if (digits == 0) return false;
    if (*i < n && s[*i] == '.') {
        (*i)++;
        int frac = 0;
        while (*i < n && isdigit((unsigned char)s[*i])) {
            (*i)++;
            frac++;
        }
        if (frac == 0) return false;
    }
    if (*i < n && (s[*i] == 'e' || s[*i] == 'E')) {
        (*i)++;
        if (*i < n && (s[*i] == '+' || s[*i] == '-')) (*i)++;
        int expd = 0;
        while (*i < n && isdigit((unsigned char)s[*i])) {
            (*i)++;
            expd++;
        }
        if (expd == 0) return false;
    }
    return *i > start;
}

static bool parse_array_json(const char *s, int *i, int n) {
    if (*i >= n || s[*i] != '[') return false;
    (*i)++;
    skip_ws_json(s, i, n);
    if (*i < n && s[*i] == ']') {
        (*i)++;
        return true;
    }
    while (*i < n) {
        if (!parse_value_json(s, i, n)) return false;
        skip_ws_json(s, i, n);
        if (*i < n && s[*i] == ']') {
            (*i)++;
            return true;
        }
        if (*i >= n || s[*i] != ',') return false;
        (*i)++;
        skip_ws_json(s, i, n);
    }
    return false;
}

static bool parse_object_json(const char *s, int *i, int n) {
    if (*i >= n || s[*i] != '{') return false;
    (*i)++;
    skip_ws_json(s, i, n);
    if (*i < n && s[*i] == '}') {
        (*i)++;
        return true;
    }
    while (*i < n) {
        if (!parse_string_json(s, i, n)) return false;
        skip_ws_json(s, i, n);
        if (*i >= n || s[*i] != ':') return false;
        (*i)++;
        skip_ws_json(s, i, n);
        if (!parse_value_json(s, i, n)) return false;
        skip_ws_json(s, i, n);
        if (*i < n && s[*i] == '}') {
            (*i)++;
            return true;
        }
        if (*i >= n || s[*i] != ',') return false;
        (*i)++;
        skip_ws_json(s, i, n);
    }
    return false;
}

static bool parse_value_json(const char *s, int *i, int n) {
    if (*i >= n) return false;
    if (s[*i] == '{') return parse_object_json(s, i, n);
    if (s[*i] == '[') return parse_array_json(s, i, n);
    if (s[*i] == '"') return parse_string_json(s, i, n);
    if (s[*i] == 't') return (*i + 4 <= n) && strncmp(s + *i, "true", 4) == 0 ? ((*i += 4), true) : false;
    if (s[*i] == 'f') return (*i + 5 <= n) && strncmp(s + *i, "false", 5) == 0 ? ((*i += 5), true) : false;
    if (s[*i] == 'n') return (*i + 4 <= n) && strncmp(s + *i, "null", 4) == 0 ? ((*i += 4), true) : false;
    return parse_number_json(s, i, n);
}

typedef enum {
    JSON_CTX_OBJECT,
    JSON_CTX_ARRAY
} json_context_kind;

typedef enum {
    JSON_OBJ_KEY_OR_END,
    JSON_OBJ_COLON,
    JSON_OBJ_EXPECT_VALUE,
    JSON_OBJ_COMMA_OR_END
} json_object_state;

typedef enum {
    JSON_ARR_VALUE_OR_END,
    JSON_ARR_COMMA_OR_END
} json_array_state;

typedef struct {
    int kind;
    int sub;
} json_context;

typedef enum {
    JSON_MODE_EXPECT,
    JSON_MODE_STRING,
    JSON_MODE_NUMBER,
    JSON_MODE_TRUE,
    JSON_MODE_FALSE,
    JSON_MODE_NULL
} json_mode;

typedef enum {
    JSON_NUM_SIGN,
    JSON_NUM_INT0,
    JSON_NUM_INT,
    JSON_NUM_FRAC_DOT,
    JSON_NUM_FRAC,
    JSON_NUM_EXP,
    JSON_NUM_EXP_SIGN,
    JSON_NUM_EXP_DIGITS
} json_number_state;

typedef struct {
    json_context stack[MAX_JSON_STACK];
    int stack_depth;
    int mode;
    int num_state;
    int lit_pos;
    bool string_escape;
    int string_unicode_left;
    bool root_complete;
} json_gen_state;

static void json_gen_state_init(json_gen_state *st) {
    memset(st, 0, sizeof(*st));
    st->mode = JSON_MODE_EXPECT;
}

static bool json_gen_is_ws(char c) {
    return isspace((unsigned char)c) != 0;
}

static bool json_gen_is_boundary(char c) {
    return json_gen_is_ws(c) || c == ',' || c == ']' || c == '}';
}

static bool json_gen_starts_value(char c) {
    return c == '{' || c == '[' || c == '"' || c == '-' || isdigit((unsigned char)c) ||
           c == 't' || c == 'f' || c == 'n';
}

static bool json_gen_mark_value_complete(json_gen_state *st) {
    if (st->stack_depth == 0) {
        st->root_complete = true;
        return true;
    }

    json_context *ctx = &st->stack[st->stack_depth - 1];
    if (ctx->kind == JSON_CTX_OBJECT) {
        if (ctx->sub != JSON_OBJ_EXPECT_VALUE) return false;
        ctx->sub = JSON_OBJ_COMMA_OR_END;
        return true;
    }
    if (ctx->kind == JSON_CTX_ARRAY) {
        if (ctx->sub != JSON_ARR_VALUE_OR_END) return false;
        ctx->sub = JSON_ARR_COMMA_OR_END;
        return true;
    }
    return false;
}

static bool json_gen_push(json_gen_state *st, int kind, int sub) {
    if (st->stack_depth >= MAX_JSON_STACK) return false;
    st->stack[st->stack_depth].kind = kind;
    st->stack[st->stack_depth].sub = sub;
    st->stack_depth++;
    st->mode = JSON_MODE_EXPECT;
    return true;
}

static bool json_gen_close_container(json_gen_state *st) {
    if (st->stack_depth == 0) return false;
    st->stack_depth--;
    return json_gen_mark_value_complete(st);
}

static bool json_gen_finish_string(json_gen_state *st) {
    st->mode = JSON_MODE_EXPECT;
    if (st->stack_depth > 0 && st->stack[st->stack_depth - 1].kind == JSON_CTX_OBJECT &&
        st->stack[st->stack_depth - 1].sub == JSON_OBJ_KEY_OR_END) {
        st->stack[st->stack_depth - 1].sub = JSON_OBJ_COLON;
        return true;
    }
    return json_gen_mark_value_complete(st);
}

static bool json_gen_start_string(json_gen_state *st) {
    st->mode = JSON_MODE_STRING;
    st->string_escape = false;
    st->string_unicode_left = 0;
    st->lit_pos = 0;
    return true;
}

static bool json_gen_start_literal(json_gen_state *st, int mode) {
    st->mode = mode;
    st->lit_pos = 1;
    return true;
}

static bool json_gen_apply_value_char(json_gen_state *st, char c);

static bool json_gen_apply_literal(json_gen_state *st, char c, const char *tail, int tail_len) {
    int pos = st->lit_pos - 1;
    if (pos < 0 || pos >= tail_len) return false;
    if (c != tail[pos]) return false;
    st->lit_pos++;
    if (st->lit_pos > tail_len) {
        st->mode = JSON_MODE_EXPECT;
        st->lit_pos = 0;
        return json_gen_mark_value_complete(st);
    }
    return true;
}

static bool json_gen_apply_value_start(json_gen_state *st, char c) {
    if (c == '"') return json_gen_start_string(st);
    if (c == '{') return json_gen_push(st, JSON_CTX_OBJECT, JSON_OBJ_KEY_OR_END);
    if (c == '[') return json_gen_push(st, JSON_CTX_ARRAY, JSON_ARR_VALUE_OR_END);
    if (c == '-') {
        st->mode = JSON_MODE_NUMBER;
        st->num_state = JSON_NUM_SIGN;
        return true;
    }
    if (isdigit((unsigned char)c)) {
        st->mode = JSON_MODE_NUMBER;
        if (c == '0') st->num_state = JSON_NUM_INT0;
        else st->num_state = JSON_NUM_INT;
        return true;
    }
    if (c == 't') return json_gen_start_literal(st, JSON_MODE_TRUE);
    if (c == 'f') return json_gen_start_literal(st, JSON_MODE_FALSE);
    if (c == 'n') return json_gen_start_literal(st, JSON_MODE_NULL);
    return false;
}

static bool json_gen_apply_expect(json_gen_state *st, char c) {
    if (json_gen_is_ws(c)) return true;
    if (st->stack_depth == 0) {
        if (st->root_complete) return false;
        return json_gen_starts_value(c) && json_gen_apply_value_start(st, c);
    }

    json_context *ctx = &st->stack[st->stack_depth - 1];
    if (ctx->kind == JSON_CTX_OBJECT) {
        if (ctx->sub == JSON_OBJ_KEY_OR_END) {
            if (c == '}') return json_gen_close_container(st);
            if (c == '"') return json_gen_apply_value_start(st, c);
            return false;
        }
        if (ctx->sub == JSON_OBJ_COLON) {
            if (c == ':') {
                ctx->sub = JSON_OBJ_EXPECT_VALUE;
                return true;
            }
            return false;
        }
        if (ctx->sub == JSON_OBJ_EXPECT_VALUE) {
            return json_gen_apply_value_start(st, c);
        }
        if (ctx->sub == JSON_OBJ_COMMA_OR_END) {
            if (c == ',') {
                ctx->sub = JSON_OBJ_KEY_OR_END;
                return true;
            }
            if (c == '}') return json_gen_close_container(st);
            return false;
        }
        return false;
    }

    if (ctx->kind == JSON_CTX_ARRAY) {
        if (ctx->sub == JSON_ARR_VALUE_OR_END) {
            if (c == ']') return json_gen_close_container(st);
            return json_gen_apply_value_start(st, c);
        }
        if (ctx->sub == JSON_ARR_COMMA_OR_END) {
            if (c == ',') {
                ctx->sub = JSON_ARR_VALUE_OR_END;
                return true;
            }
            if (c == ']') return json_gen_close_container(st);
            return false;
        }
    }
    return false;
}

static bool json_gen_apply_number(json_gen_state *st, char c) {
    if (json_gen_is_boundary(c)) {
        if (st->num_state != JSON_NUM_INT0 &&
            st->num_state != JSON_NUM_INT &&
            st->num_state != JSON_NUM_FRAC &&
            st->num_state != JSON_NUM_EXP_DIGITS) return false;
        st->mode = JSON_MODE_EXPECT;
        return json_gen_mark_value_complete(st) && json_gen_apply_value_char(st, c);
    }

    if (st->num_state == JSON_NUM_SIGN) {
        if (c == '0') {
            st->num_state = JSON_NUM_INT0;
            return true;
        }
        if (isdigit((unsigned char)c) && c != '0') {
            st->num_state = JSON_NUM_INT;
            return true;
        }
        return false;
    }
    if (st->num_state == JSON_NUM_INT0) {
        if (c == '.') {
            st->num_state = JSON_NUM_FRAC_DOT;
            return true;
        }
        if (c == 'e' || c == 'E') {
            st->num_state = JSON_NUM_EXP;
            return true;
        }
        return false;
    }
    if (st->num_state == JSON_NUM_INT) {
        if (isdigit((unsigned char)c)) return true;
        if (c == '.') {
            st->num_state = JSON_NUM_FRAC_DOT;
            return true;
        }
        if (c == 'e' || c == 'E') {
            st->num_state = JSON_NUM_EXP;
            return true;
        }
        return false;
    }
    if (st->num_state == JSON_NUM_FRAC_DOT) {
        if (isdigit((unsigned char)c)) {
            st->num_state = JSON_NUM_FRAC;
            return true;
        }
        return false;
    }
    if (st->num_state == JSON_NUM_FRAC) {
        if (isdigit((unsigned char)c)) return true;
        if (c == 'e' || c == 'E') {
            st->num_state = JSON_NUM_EXP;
            return true;
        }
        return false;
    }
    if (st->num_state == JSON_NUM_EXP) {
        if (c == '+' || c == '-') {
            st->num_state = JSON_NUM_EXP_SIGN;
            return true;
        }
        if (isdigit((unsigned char)c)) {
            st->num_state = JSON_NUM_EXP_DIGITS;
            return true;
        }
        return false;
    }
    if (st->num_state == JSON_NUM_EXP_SIGN) {
        if (isdigit((unsigned char)c)) {
            st->num_state = JSON_NUM_EXP_DIGITS;
            return true;
        }
        return false;
    }
    if (st->num_state == JSON_NUM_EXP_DIGITS && isdigit((unsigned char)c)) return true;
    return false;
}

static bool json_gen_apply_value_char(json_gen_state *st, char c) {
    if (st->mode == JSON_MODE_EXPECT) return json_gen_apply_expect(st, c);
    if (st->mode == JSON_MODE_STRING) {
        if (st->string_unicode_left > 0) {
            if (!isxdigit((unsigned char)c)) return false;
            st->string_unicode_left--;
            return true;
        }
        if (st->string_escape) {
            if (c == '"' || c == '\\' || c == '/' || c == 'b' || c == 'f' ||
                c == 'n' || c == 'r' || c == 't') {
                st->string_escape = false;
                return true;
            }
            if (c == 'u') {
                st->string_unicode_left = 4;
                st->string_escape = false;
                return true;
            }
            return false;
        }
        if ((unsigned char)c == 0x22) return json_gen_finish_string(st);
        if (c == '\\') {
            st->string_escape = true;
            return true;
        }
        if ((unsigned char)c < 0x20) return false;
        return true;
    }
    if (st->mode == JSON_MODE_NUMBER) return json_gen_apply_number(st, c);
    if (st->mode == JSON_MODE_TRUE) return json_gen_apply_literal(st, c, "rue", 3);
    if (st->mode == JSON_MODE_FALSE) return json_gen_apply_literal(st, c, "alse", 4);
    if (st->mode == JSON_MODE_NULL) return json_gen_apply_literal(st, c, "ull", 3);
    return false;
}

static bool json_gen_step(const json_gen_state *in, char c, json_gen_state *out) {
    if (!in || !out) return false;
    *out = *in;
    return json_gen_apply_value_char(out, c);
}

static bool json_gen_can_finish(const json_gen_state *st) {
    return st && st->mode == JSON_MODE_EXPECT && st->stack_depth == 0 && st->root_complete;
}

static bool json_is_valid(const char *s) {
    if (!s || !*s) return false;
    int n = (int)strlen(s);
    int i = 0;
    skip_ws_json(s, &i, n);
    if (!parse_value_json(s, &i, n)) return false;
    skip_ws_json(s, &i, n);
    return i == n;
}

static bool corpus_from_file(json_corpus *c, const char *path, int max_lines) {
    FILE *f = fopen(path, "r");
    if (!f) return false;
    char buf[4096];
    int count = 0;
    while (count < max_lines && fgets(buf, sizeof(buf), f)) {
        trim_in_place(buf);
        if (!*buf) continue;
        if (!json_is_valid(buf)) continue;
        corpus_add(c, buf);
        count++;
    }
    fclose(f);
    return count > 0;
}

static bool fetch_url_data(const char *url, char **out_buf) {
    if (!url || !out_buf) return false;
    *out_buf = NULL;

    size_t url_len = strlen(url);
    const char *cmd_prefix = "curl.exe -L -sS --max-time 20 --connect-timeout 5 \"";
    const char *cmd_suffix = "\"";
    size_t cmd_len = strlen(cmd_prefix) + url_len + strlen(cmd_suffix) + 1;
    char *cmd = (char *)malloc(cmd_len);
    if (!cmd) return false;
    int written = snprintf(cmd, cmd_len, "%s%s%s", cmd_prefix, url, cmd_suffix);
    if (written <= 0 || (size_t)written >= cmd_len) {
        free(cmd);
        return false;
    }

    FILE *pipe = popen(cmd, "r");
    free(cmd);
    if (!pipe) return false;

    size_t cap = 8192;
    size_t len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) {
        pclose(pipe);
        return false;
    }
    for (;;) {
        char chunk[4096];
        size_t n = fread(chunk, 1, sizeof(chunk), pipe);
        if (n == 0) break;
        if (len + n + 1 > cap) {
            size_t new_cap = cap * 2;
            if (new_cap < len + n + 1) {
                new_cap = len + n + 2048;
            }
            char *tmp = (char *)realloc(buf, new_cap);
            if (!tmp) {
                free(buf);
                pclose(pipe);
                return false;
            }
            buf = tmp;
            cap = new_cap;
        }
        memcpy(buf + len, chunk, n);
        len += n;
    }
    pclose(pipe);

    if (len == 0) {
        free(buf);
        return false;
    }

    buf[len] = 0;
    trim_in_place(buf);
    if (!*buf) {
        free(buf);
        return false;
    }

    *out_buf = buf;
    return true;
}

static bool corpus_from_url(json_corpus *c, const char *url, int max_lines) {
    if (!c || !url || !*url || max_lines <= 0) return false;
    char *payload = NULL;
    if (!fetch_url_data(url, &payload)) return false;

    int count = 0;
    if (payload[0] == '[') {
        int before_count = c->count;
        corpus_from_json_array(c, payload, max_lines);
        count = c->count - before_count;
        free(payload);
        return count > 0;
    }

    if (json_is_valid(payload)) {
        int before_count = c->count;
        if (payload[0] == '{') {
            char text_val[JSON_URL_FETCH_BUF / 4];
            if (extract_json_text_field(payload, text_val, sizeof(text_val))) {
                corpus_add(c, text_val);
            } else {
                corpus_add(c, payload);
            }
            count = c->count - before_count;
        }
        free(payload);
        return count > 0;
    }

    for (char *line = strtok(payload, "\r\n"); line && count < max_lines; line = strtok(NULL, "\r\n")) {
        trim_in_place(line);
        if (!*line) continue;

        if (json_is_valid(line)) {
            if (line[0] == '{') {
                char text_val[JSON_URL_FETCH_BUF / 4];
                if (extract_json_text_field(line, text_val, sizeof(text_val))) {
                    corpus_add(c, text_val);
                } else {
                    corpus_add(c, line);
                }
                count++;
            }
            continue;
        }

        {
            char text_val[JSON_URL_FETCH_BUF / 4];
            if (extract_json_text_field(line, text_val, sizeof(text_val))) {
                corpus_add(c, text_val);
                count++;
            }
        }
    }

    free(payload);
    return count > 0;
}

static bool build_vocab(json_vocab *v, const json_corpus *c) {
    if (!v || !c) return false;
    vocab_init(v);
    if (!vocab_add_char(v, ' ')) return false;
    if (!vocab_add_char(v, JSON_TERM_CHAR)) return false;
    for (int i = 0; i < c->count; ++i) {
        const char *s = c->lines[i];
        for (int j = 0; s[j] != 0; ++j) {
            if (!vocab_add_char(v, (unsigned char)s[j])) return false;
        }
    }
    return v->size > 0;
}

static size_t total_pairs(const json_corpus *c) {
    size_t total = 0;
    for (int i = 0; i < c->count; ++i) total += (size_t)strlen(c->lines[i]) + 1;
    return total;
}

static bool make_dataset(const json_corpus *c, const json_vocab *v, int ctx,
                        float **inputs_out, float **targets_out, size_t *pair_count_out,
                        int *in_dim_out, int *out_dim_out) {
    if (!c || !v || ctx <= 0 || ctx > MAX_CONTEXT || !inputs_out || !targets_out || !pair_count_out || !in_dim_out || !out_dim_out) return false;
    size_t pairs = total_pairs(c);
    if (pairs == 0 || v->size <= 0 || v->size > MAX_JSON_VOCAB) return false;

    int in_dim = ctx * v->size;
    int out_dim = v->size;
    float *inputs = (float *)calloc(pairs * (size_t)in_dim, sizeof(float));
    float *targets = (float *)calloc(pairs * (size_t)out_dim, sizeof(float));
    if (!inputs || !targets) {
        free(inputs);
        free(targets);
        return false;
    }

    size_t pidx = 0;
    for (int sidx = 0; sidx < c->count; ++sidx) {
        const char *s = c->lines[sidx];
        int len = (int)strlen(s);
        for (int p = 0; p <= len; ++p) {
            float *xin = inputs + ((size_t)pidx * (size_t)in_dim);
            char t = (p == len) ? JSON_TERM_CHAR : s[p];
            int tid = v->char_to_id[(unsigned char)t];
            if (tid < 0) tid = v->char_to_id[(unsigned char)JSON_TERM_CHAR];

            for (int k = 0; k < ctx; ++k) {
                int src = p - ctx + 1 + k;
                char ch = (src < 0 || src >= len) ? ' ' : s[src];
                int id = v->char_to_id[(unsigned char)ch];
                if (id >= 0 && id < v->size) xin[k * v->size + id] = 1.0f;
            }

            if (tid >= 0 && tid < out_dim) targets[((size_t)pidx * (size_t)out_dim) + tid] = 1.0f;
            pidx++;
        }
    }

    *inputs_out = inputs;
    *targets_out = targets;
    *pair_count_out = pidx;
    *in_dim_out = in_dim;
    *out_dim_out = out_dim;
    return true;
}

static int argmax(const float *v, int n) {
    if (!v || n <= 0) return -1;
    int best = 0;
    for (int i = 1; i < n; ++i) if (v[i] > v[best]) best = i;
    return best;
}

static int sample_top_p(const float *logits, int n, float temp, float top_p) {
    if (!logits || n <= 0) return 0;
    if (n > MAX_JSON_VOCAB) n = MAX_JSON_VOCAB;
    if (temp <= 0.0001f) temp = 0.0001f;
    if (top_p <= 0.0f || top_p >= 1.0f) top_p = 1.0f;

    float *probs = (float *)malloc((size_t)n * sizeof(float));
    int *order = (int *)malloc((size_t)n * sizeof(int));
    if (!probs || !order) {
        free(probs);
        free(order);
        return 0;
    }

    float mx = -1e30f;
    for (int i = 0; i < n; ++i) {
        if (logits[i] > mx) mx = logits[i];
    }

    float sum = 0.0f;
    for (int i = 0; i < n; ++i) {
        float p = expf((logits[i] - mx) / temp);
        probs[i] = p;
        sum += p;
    }
    if (sum <= 0.0f) {
        free(probs);
        free(order);
        int best = 0;
        for (int i = 1; i < n; ++i) if (logits[i] > logits[best]) best = i;
        return best;
    }
    for (int i = 0; i < n; ++i) probs[i] /= sum;

    for (int i = 0; i < n; ++i) order[i] = i;
    for (int i = 0; i < n - 1; ++i) {
        for (int j = i + 1; j < n; ++j) {
            if (probs[order[j]] > probs[order[i]]) {
                int t = order[i];
                order[i] = order[j];
                order[j] = t;
            }
        }
    }

    float cum = 0.0f;
    int tail = n;
    for (int i = 0; i < n; ++i) {
        cum += probs[order[i]];
        if (cum >= top_p) {
            tail = i + 1;
            break;
        }
    }
    if (tail <= 0) tail = 1;

    float r = (float)rand() / (float)RAND_MAX;
    float acc = 0.0f;
    int chosen = order[tail - 1];
    for (int i = 0; i < tail; ++i) {
        acc += probs[order[i]];
        if (r <= acc || i + 1 == tail) {
            chosen = order[i];
            break;
        }
    }

    free(probs);
    free(order);
    return chosen;
}

static int sample_top_p_with_mask(const float *logits, int n, float temp, float top_p,
                                 const int *allowed, int allowed_n) {
    if (!logits || !allowed || n <= 0 || allowed_n <= 0) return -1;
    float *probs = (float *)malloc((size_t)allowed_n * sizeof(float));
    int *order = (int *)malloc((size_t)allowed_n * sizeof(int));
    if (!probs || !order) {
        free(probs);
        free(order);
        return -1;
    }

    if (top_p <= 0.0f || top_p >= 1.0f) top_p = 1.0f;
    if (temp <= 0.0001f) temp = 0.0001f;

    float mx = -1e30f;
    for (int i = 0; i < allowed_n; ++i) {
        int id = allowed[i];
        if (id >= 0 && id < n && logits[id] > mx) mx = logits[id];
    }

    float sum = 0.0f;
    for (int i = 0; i < allowed_n; ++i) {
        int id = allowed[i];
        if (id < 0 || id >= n) {
            free(probs);
            free(order);
            return -1;
        }
        float p = expf((logits[id] - mx) / temp);
        probs[i] = p;
        sum += p;
    }
    if (sum <= 0.0f) {
        int best = 0;
        for (int i = 1; i < allowed_n; ++i) if (probs[i] > probs[best]) best = i;
        free(probs);
        free(order);
        return allowed[best];
    }
    for (int i = 0; i < allowed_n; ++i) probs[i] /= sum;

    for (int i = 0; i < allowed_n; ++i) order[i] = i;
    for (int i = 0; i < allowed_n - 1; ++i) {
        for (int j = i + 1; j < allowed_n; ++j) {
            if (probs[order[j]] > probs[order[i]]) {
                int t = order[i];
                order[i] = order[j];
                order[j] = t;
            }
        }
    }

    float cum = 0.0f;
    int tail = allowed_n;
    for (int i = 0; i < allowed_n; ++i) {
        cum += probs[order[i]];
        if (cum >= top_p) {
            tail = i + 1;
            break;
        }
    }
    if (tail <= 0) tail = 1;

    float r = (float)rand() / (float)RAND_MAX;
    float acc = 0.0f;
    int chosen = order[tail - 1];
    for (int i = 0; i < tail; ++i) {
        acc += probs[order[i]];
        if (r <= acc || i + 1 == tail) {
            chosen = order[i];
            break;
        }
    }

    int out = allowed[chosen];
    free(probs);
    free(order);
    return out;
}

static void generate_json_sample(const cce_cascade *cas, const json_vocab *v, int ctx, int out_steps,
                                const char *seed, float temp, float top_p,
                                char *out, size_t out_size) {
    if (!cas || !v || !out || out_size < 2) return;
    if (ctx <= 0 || ctx > MAX_CONTEXT || (size_t)ctx > (size_t)MAX_CONTEXT) return;
    int expected_in_dim = ctx * v->size;
    if (expected_in_dim != cas->blocks[0].weights.shape[0]) {
        fprintf(stderr, "generate_json_sample: input-dim mismatch, expected=%d got=%d\n",
                expected_in_dim, cas->blocks[0].weights.shape[0]);
        return;
    }
    if (v->size <= 0 || v->size > MAX_JSON_VOCAB) return;
    int term_id = v->char_to_id[(unsigned char)JSON_TERM_CHAR];
    if (term_id < 0) return;

    memset(out, 0, out_size);
    json_gen_state state;
    json_gen_state_init(&state);
    const char *seed_base = seed ? seed : "";
    int seed_len = (int)strlen(seed_base);
    int seed_used = 0;
    for (; seed_used < seed_len; ++seed_used) {
        json_gen_state next;
        if (!json_gen_step(&state, seed_base[seed_used], &next)) break;
        state = next;
    }

    if (seed_used > 0 && seed_used < (int)out_size && json_gen_can_finish(&state)) {
        memcpy(out, seed_base, (size_t)seed_used);
        out[seed_used] = 0;
        return;
    }

    int ctx_buf[MAX_CONTEXT];
    for (int i = 0; i < ctx; ++i) {
        int si = seed_used - ctx + i;
        unsigned char ch = (si < 0) ? ' ' : (unsigned char)seed_base[si];
        int id = v->char_to_id[ch];
        ctx_buf[i] = (id >= 0) ? id : v->char_to_id[(unsigned char)' '];
    }

    int sx[1] = {expected_in_dim};
    cce_tensor x, y;
    if (cce_tensor_alloc(&x, sx, 1) != CCE_OK) return;
    int debug_gen = getenv("CNET_JSON_DEBUG") != NULL;
    size_t out_len = 0;
    int allowed_ids[MAX_JSON_VOCAB];

    for (int step = 0; step < out_steps; ++step) {
        cce_tensor_zero(&x);
        for (int k = 0; k < ctx; ++k) {
            int id = ctx_buf[k];
            if (id >= 0 && id < v->size) x.data[k * v->size + id] = 1.0f;
        }

        if (cce_cascade_forward(cas, &x, &y) != CCE_OK) {
            fprintf(stderr, "generate_json_sample: forward failed at step %d\n", step);
            break;
        }
        int nlogits = y.numel > 0 ? (int)y.numel : v->size;
        int n_allowed = 0;
        for (int i = 0; i < v->size; ++i) {
            char ch = v->id_to_char[i];
            if (ch == JSON_TERM_CHAR) {
                if (json_gen_can_finish(&state)) allowed_ids[n_allowed++] = i;
                continue;
            }
            json_gen_state next;
            if (json_gen_step(&state, ch, &next) && n_allowed < MAX_JSON_VOCAB) {
                allowed_ids[n_allowed++] = i;
            }
        }
        if (n_allowed == 0) {
            if (json_gen_can_finish(&state)) {
                allowed_ids[n_allowed++] = term_id;
            }
        }
        int ni = sample_top_p_with_mask(y.data, nlogits, temp, top_p, allowed_ids, n_allowed);
        if (ni < 0) {
            if (n_allowed > 0) {
                int best = -1;
                float best_prob = -1e30f;
                for (int i = 0; i < n_allowed; ++i) {
                    int id = allowed_ids[i];
                    if (id < 0 || id >= nlogits) continue;
                    if (best < 0 || y.data[id] > best_prob) {
                        best_prob = y.data[id];
                        best = id;
                    }
                }
                ni = best >= 0 ? best : term_id;
            } else {
                ni = sample_top_p(y.data, nlogits, temp, top_p);
            }
        }
        if (ni < 0 || ni >= v->size) ni = term_id;
        char ch = v->id_to_char[ni];
        if (debug_gen) {
            fprintf(stderr, "[dbg] step=%d sampled=%d char=0x%02x\n", step, ni, (unsigned char)ch);
        }
        if (ch == JSON_TERM_CHAR) {
            if (out_len > 0 || json_gen_can_finish(&state)) {
                cce_tensor_free(&y);
                break;
            }
            cce_tensor_free(&y);
            continue;
        }
        if (out_len + 2 >= out_size) {
            cce_tensor_free(&y);
            break;
        }
        out[out_len] = ch;
        out_len++;
        out[out_len] = 0;
        json_gen_state next;
        if (!json_gen_step(&state, ch, &next)) {
            cce_tensor_free(&y);
            break;
        }
        state = next;

        for (int k = 0; k < ctx - 1; ++k) ctx_buf[k] = ctx_buf[k + 1];
        ctx_buf[ctx - 1] = ni;
        cce_tensor_free(&y);

        if (json_gen_can_finish(&state) && out_len > 0 &&
            !json_gen_is_ws(out[out_len - 1]) && (rand() % 2 == 0)) {
            break;
        }
    }

    cce_tensor_free(&x);
}

static void add_default_corpus(json_corpus *c) {
    static const char *default_json[] = {
        "{\"id\":1,\"name\":\"alice\",\"age\":7,\"active\":true}",
        "{\"id\":2,\"name\":\"bob\",\"score\":14,\"active\":false}",
        "{\"name\":\"carol\",\"age\":32,\"roles\":[\"admin\",\"editor\"],\"active\":true}",
        "{\"user\":{\"name\":\"dana\",\"id\":11},\"meta\":{\"attempts\":3,\"ok\":true}}",
        "{\"a\":1,\"b\":2.5,\"c\":{\"d\":false,\"e\":null}}",
        "{\"list\":[1,2,3,4],\"ok\":true,\"meta\":{\"v\":42}}",
        "{\"config\":{\"mode\":\"fast\",\"retry\":5,\"tags\":[\"core\",\"json\",\"test\"]},\"enabled\":false}",
        "{\"event\":\"open\",\"payload\":{\"id\":\"alpha\",\"tags\":[\"start\",\"init\"],\"count\":1}}",
        "{\"nested\":{\"n1\":{\"n2\":{\"n3\":true}},\"arr\":[{\"x\":1},{\"x\":2}]}}",
        "{\"matrix\":[[1,0,1],[0,1,0],[1,1,0]],\"checksum\":\"f1\"}",
        "{\"person\":{\"first\":\"eli\",\"last\":\"troy\"},\"scores\":[10,20,30],\"valid\":null}",
        "{\"status\":\"ok\",\"items\":[{\"k\":\"a\",\"v\":1},{\"k\":\"b\",\"v\":2}],\"count\":2}",
        "{\"route\":{\"from\":\"north\",\"to\":\"south\",\"distance\":123.45},\"ok\":true}",
        "{\"message\":\"hello, json\",\"repeat\":3,\"arr\":[true,false,true]}",
        "{\"sample\":{\"alpha\":0.5,\"beta\":1.25,\"gamma\":-2.75},\"state\":\"done\"}",
        "{\"commands\":[\"read\",\"write\",\"execute\"],\"user\":null,\"code\":204}",
        "{\"profile\":{\"id\":1001,\"prefs\":[\"json\",\"lm\"],\"admin\":false},\"ok\":true}",
        "{\"metrics\":{\"lat\":12.3,\"lon\":45.7,\"ok\":true},\"tags\":[\"demo\",\"bench\"]}",
        "{\"payload\":{\"text\":\"A tiny benchmark object\",\"flags\":[true,false],\"len\":29}}",
        "{\"errors\":[],\"success\":true,\"count\":0}"
    };
    const int n = (int)(sizeof(default_json) / sizeof(default_json[0]));
    for (int i = 0; i < n; ++i) corpus_add(c, default_json[i]);
}

int main(void) {
    srand(42);
    const char *ext_path = getenv("CNET_JSON_DATA");
    const char *url_path = getenv("CNET_JSON_URL");

    json_corpus corpus;
    corpus_init(&corpus);
    add_default_corpus(&corpus);

    {
        int file_lines_limit = 128;
        const char *env = getenv("CNET_JSON_LINES");
        if (env) {
            int v = atoi(env);
            if (v > 0 && v <= 4096) file_lines_limit = v;
        }

        if (ext_path) {
            json_corpus file_corpus;
            corpus_init(&file_corpus);
            if (corpus_from_file(&file_corpus, ext_path, file_lines_limit)) {
                for (int i = 0; i < file_corpus.count; ++i) corpus_add(&corpus, file_corpus.lines[i]);
            } else {
                printf("No valid JSON lines from %s; keeping built-in corpus only.\n", ext_path);
            }
            corpus_free(&file_corpus);
        }

        if (url_path) {
            json_corpus url_corpus;
            corpus_init(&url_corpus);
            if (corpus_from_url(&url_corpus, url_path, file_lines_limit)) {
                for (int i = 0; i < url_corpus.count; ++i) corpus_add(&corpus, url_corpus.lines[i]);
                printf("Loaded %d JSON samples from %s.\n", url_corpus.count, url_path);
            } else {
                printf("No valid JSON from remote source %s; keeping existing corpus.\n", url_path);
            }
            corpus_free(&url_corpus);
        }
    }

    if (corpus.count == 0) {
        printf("No JSON samples loaded.\n");
        corpus_free(&corpus);
        return 1;
    }

    json_vocab v;
    if (!build_vocab(&v, &corpus)) {
        printf("Could not build vocabulary (character set too large).\n");
        corpus_free(&corpus);
        return 1;
    }

    int ctx = 16;
    {
        const char *env = getenv("CNET_JSON_CTX");
        if (env) {
            int c = atoi(env);
            if (c > 0 && c <= MAX_CONTEXT) ctx = c;
        }
    }
    int hidden = 128;
    {
        const char *env = getenv("CNET_JSON_HID");
        if (env) {
            int h = atoi(env);
            if (h > 0 && h <= 256) hidden = h;
        }
    }
    int epochs = 60;
    {
        const char *env = getenv("CNET_JSON_EPOCHS");
        if (env) {
            int e = atoi(env);
            if (e > 0) epochs = e;
        }
    }
    float lr = 0.02f;
    {
        const char *env = getenv("CNET_JSON_LR");
        if (env) {
            float l = (float)atof(env);
            if (l > 0.0f) lr = l;
        }
    }
    float temp = 0.8f;
    {
        const char *env = getenv("CNET_JSON_TEMP");
        if (env) {
            float t = (float)atof(env);
            if (t > 0.0f) temp = t;
        }
    }
    float top_p = 0.92f;
    {
        const char *env = getenv("CNET_JSON_TOPP");
        if (env) {
            float p = (float)atof(env);
            if (p > 0.0f && p <= 1.0f) top_p = p;
        }
    }
    int trials = 4;
    {
        const char *env = getenv("CNET_JSON_TRIALS");
        if (env) {
            int t = atoi(env);
            if (t > 0 && t <= 64) trials = t;
        }
    }
    int max_len = 320;
    {
        const char *env = getenv("CNET_JSON_MAX_LEN");
        if (env) {
            int m = atoi(env);
            if (m > 8 && m < MAX_JSON_LEN) max_len = m;
        }
    }
    const char *seed = getenv("CNET_JSON_SEED");
    if (!seed) seed = "{";

    float *inputs = NULL;
    float *targets = NULL;
    size_t pair_count = 0;
    int in_dim = 0, out_dim = 0;
    if (!make_dataset(&corpus, &v, ctx, &inputs, &targets, &pair_count, &in_dim, &out_dim)) {
        printf("Failed building dataset.\n");
        corpus_free(&corpus);
        return 1;
    }

    cce_cascade cas;
    const int blocks = 4;
    cce_cascade_init(&cas, blocks);

    cce_block b0;
    cce_block_init_linear(&b0, in_dim, hidden, lr);
    cce_cascade_append(&cas, &b0);
    for (int i = 1; i < blocks - 1; ++i) {
        cce_block bi;
        cce_block_init_linear(&bi, hidden, hidden, lr);
        cce_cascade_append(&cas, &bi);
    }
    cce_block head;
    cce_block_init_linear(&head, hidden, out_dim, lr);
    head.type = CCE_BLOCK_LINEAR_HEAD;
    cce_cascade_append(&cas, &head);

    printf("=== JSON CCE Learn Task ===\n");
    printf("samples=%d pairs=%zu chars=%d ctx=%d hidden=%d in_dim=%d out_dim=%d epochs=%d\n",
           corpus.count, pair_count, v.size, ctx, hidden, in_dim, out_dim, epochs);

    clock_t t0 = clock();
    double loss = cce_train_dynamic(&cas, inputs, targets, pair_count, in_dim, out_dim,
                                    (size_t)epochs, 0.02f, lr, 1, NULL, 0 /*LOCAL*/);
    double secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
    printf("training finished: loss=%.6f in %.2f s\n", loss, secs);

    cce_tensor x, y;
    int xshape[1] = {in_dim};
    cce_tensor_alloc(&x, xshape, 1);
    size_t correct = 0;
    for (size_t p = 0; p < pair_count; ++p) {
        memcpy(x.data, inputs + (p * (size_t)in_dim), (size_t)in_dim * sizeof(float));
        if (cce_cascade_forward(&cas, &x, &y) != CCE_OK) continue;
        int pi = argmax(y.data, out_dim);
        int ti = argmax(targets + (p * (size_t)out_dim), out_dim);
        cce_tensor_free(&y);
        if (pi == ti) correct++;
    }
    cce_tensor_free(&x);

    printf("one-step eval: accuracy=%.3f (%zu/%zu)\n",
           pair_count ? (double)correct / (double)pair_count : 0.0, correct, pair_count);

    int val_count = 0;
    for (int t = 0; t < trials; ++t) {
        char generated[MAX_JSON_LEN];
        int out_cap = max_len < (MAX_JSON_LEN - 1) ? max_len : (MAX_JSON_LEN - 1);
        generate_json_sample(&cas, &v, ctx, out_cap, seed, temp, top_p, generated, sizeof(generated));
        printf("sample[%d] len=%zu: ", t + 1, strlen(generated));
        for (size_t c = 0; c < strlen(generated); ++c) {
            unsigned char ch = (unsigned char)generated[c];
            if (ch >= 32 && ch <= 126) {
                printf("%c", ch);
            } else {
                printf("\\x%02x", ch);
            }
        }
        printf("\n");
        if (generated[0] && json_is_valid(generated)) {
            val_count++;
        }
    }
    printf("generation validity: %d/%d\n", val_count, trials);

    cce_cascade_free(&cas);
    free(inputs);
    free(targets);
    corpus_free(&corpus);

    return 0;
}
