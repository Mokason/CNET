#ifndef JSON_VALIDATE_H
#define JSON_VALIDATE_H
/* Independent JSON well-formedness oracle. Verbatim recursive-descent validator
   lifted from tests/cce_json_bench.c (all functions static). Used to confirm
   the contract-built JSON is well-formed by a parser that knows nothing about
   how it was produced. */
#include <stdbool.h>
#include <string.h>
#include <ctype.h>

static void skip_ws_json(const char *s, int *i, int n);
static bool parse_value_json(const char *s, int *i, int n);

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

static bool json_is_valid(const char *s) {
    if (!s || !*s) return false;
    int n = (int)strlen(s);
    int i = 0;
    skip_ws_json(s, &i, n);
    if (!parse_value_json(s, &i, n)) return false;
    skip_ws_json(s, &i, n);
    return i == n;
}

#endif /* JSON_VALIDATE_H */
