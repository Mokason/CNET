#ifndef CNET_JSON_INTERNAL_H
#define CNET_JSON_INTERNAL_H
/* Shared bounded JSON syntax reader; callers enforce field schemas. */
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#define CNETD_JSON_DEPTH_MAX 32u
typedef struct { const unsigned char *p; } JsonCursor;

static void json_ws(JsonCursor *cursor) {
    while (*cursor->p == ' ' || *cursor->p == '\t' ||
           *cursor->p == '\r' || *cursor->p == '\n')
        cursor->p++;
}

static int json_hex(unsigned char c) {
    if (c >= '0' && c <= '9') return (int)(c - '0');
    if (c >= 'a' && c <= 'f') return (int)(c - 'a') + 10;
    if (c >= 'A' && c <= 'F') return (int)(c - 'A') + 10;
    return -1;
}

static int json_hex4(JsonCursor *cursor, uint32_t *value) {
    uint32_t result = 0;
    unsigned i;
    for (i = 0; i < 4; i++) {
        int digit = json_hex(cursor->p[i]);
        if (digit < 0) return -1;
        result = result * 16u + (uint32_t)digit;
    }
    cursor->p += 4;
    *value = result;
    return 0;
}

static int json_emit(char *out, size_t cap, size_t *used,
                     const unsigned char *bytes, size_t count) {
    if (out && (*used >= cap || count >= cap - *used)) return -1;
    if (out) memcpy(out + *used, bytes, count);
    *used += count;
    return 0;
}

static int json_emit_codepoint(char *out, size_t cap, size_t *used,
                               uint32_t cp) {
    unsigned char encoded[4];
    size_t count;
    if (cp == 0 || cp > 0x10ffffu || (cp >= 0xd800u && cp <= 0xdfffu))
        return -1;
    if (cp <= 0x7fu) {
        encoded[0] = (unsigned char)cp;
        count = 1;
    } else if (cp <= 0x7ffu) {
        encoded[0] = (unsigned char)(0xc0u | (cp >> 6));
        encoded[1] = (unsigned char)(0x80u | (cp & 0x3fu));
        count = 2;
    } else if (cp <= 0xffffu) {
        encoded[0] = (unsigned char)(0xe0u | (cp >> 12));
        encoded[1] = (unsigned char)(0x80u | ((cp >> 6) & 0x3fu));
        encoded[2] = (unsigned char)(0x80u | (cp & 0x3fu));
        count = 3;
    } else {
        encoded[0] = (unsigned char)(0xf0u | (cp >> 18));
        encoded[1] = (unsigned char)(0x80u | ((cp >> 12) & 0x3fu));
        encoded[2] = (unsigned char)(0x80u | ((cp >> 6) & 0x3fu));
        encoded[3] = (unsigned char)(0x80u | (cp & 0x3fu));
        count = 4;
    }
    return json_emit(out, cap, used, encoded, count);
}

static int json_copy_utf8(JsonCursor *cursor, char *out, size_t cap,
                          size_t *used) {
    const unsigned char *p = cursor->p;
    unsigned char first = *p;
    size_t count;
    uint32_t cp;
    size_t i;

    if (first < 0x80u) return -1;
    if (first >= 0xc2u && first <= 0xdfu) {
        count = 2;
        cp = first & 0x1fu;
    } else if (first >= 0xe0u && first <= 0xefu) {
        count = 3;
        cp = first & 0x0fu;
    } else if (first >= 0xf0u && first <= 0xf4u) {
        count = 4;
        cp = first & 0x07u;
    } else {
        return -1;
    }
    for (i = 1; i < count; i++) {
        if ((p[i] & 0xc0u) != 0x80u) return -1;
        cp = (cp << 6) | (uint32_t)(p[i] & 0x3fu);
    }
    if ((count == 3 && cp < 0x800u) ||
        (count == 4 && cp < 0x10000u) ||
        (cp >= 0xd800u && cp <= 0xdfffu) || cp > 0x10ffffu)
        return -1;
    if (json_emit(out, cap, used, p, count) != 0) return -1;
    cursor->p += count;
    return 0;
}

static int json_string(JsonCursor *cursor, char *out, size_t cap) {
    size_t used = 0;
    if (*cursor->p != '"') return -1;
    cursor->p++;
    while (*cursor->p && *cursor->p != '"') {
        unsigned char c = *cursor->p++;
        if (c < 0x20u) return -1;
        if (c >= 0x80u) {
            cursor->p--;
            if (json_copy_utf8(cursor, out, cap, &used) != 0) return -1;
            continue;
        }
        if (c != '\\') {
            if (json_emit(out, cap, &used, &c, 1) != 0) return -1;
            continue;
        }
        c = *cursor->p++;
        if (c == '"' || c == '\\' || c == '/') {
            if (json_emit(out, cap, &used, &c, 1) != 0) return -1;
        } else if (c == 'b' || c == 'f' || c == 'n' ||
                   c == 'r' || c == 't') {
            unsigned char decoded = c == 'b' ? '\b' : c == 'f' ? '\f' :
                                    c == 'n' ? '\n' : c == 'r' ? '\r' : '\t';
            if (json_emit(out, cap, &used, &decoded, 1) != 0) return -1;
        } else if (c == 'u') {
            uint32_t cp;
            if (json_hex4(cursor, &cp) != 0) return -1;
            if (cp >= 0xd800u && cp <= 0xdbffu) {
                uint32_t low;
                if (cursor->p[0] != '\\' || cursor->p[1] != 'u') return -1;
                cursor->p += 2;
                if (json_hex4(cursor, &low) != 0 ||
                    low < 0xdc00u || low > 0xdfffu)
                    return -1;
                cp = 0x10000u + ((cp - 0xd800u) << 10) + (low - 0xdc00u);
            }
            if (json_emit_codepoint(out, cap, &used, cp) != 0) return -1;
        } else {
            return -1;
        }
    }
    if (*cursor->p != '"') return -1;
    cursor->p++;
    if (out) {
        if (used >= cap) return -1;
        out[used] = '\0';
    }
    return 0;
}

static int json_value(JsonCursor *cursor, unsigned depth);

static int json_number(JsonCursor *cursor) {
    const unsigned char *p = cursor->p;
    if (*p == '-') p++;
    if (*p == '0') {
        p++;
    } else {
        if (*p < '1' || *p > '9') return -1;
        do p++; while (*p >= '0' && *p <= '9');
    }
    if (*p == '.') {
        p++;
        if (*p < '0' || *p > '9') return -1;
        do p++; while (*p >= '0' && *p <= '9');
    }
    if (*p == 'e' || *p == 'E') {
        p++;
        if (*p == '+' || *p == '-') p++;
        if (*p < '0' || *p > '9') return -1;
        do p++; while (*p >= '0' && *p <= '9');
    }
    cursor->p = p;
    return 0;
}

static int json_array(JsonCursor *cursor, unsigned depth) {
    cursor->p++;
    json_ws(cursor);
    if (*cursor->p == ']') {
        cursor->p++;
        return 0;
    }
    for (;;) {
        if (json_value(cursor, depth + 1) != 0) return -1;
        json_ws(cursor);
        if (*cursor->p == ']') {
            cursor->p++;
            return 0;
        }
        if (*cursor->p++ != ',') return -1;
        json_ws(cursor);
    }
}

static int json_object(JsonCursor *cursor, unsigned depth) {
    cursor->p++;
    json_ws(cursor);
    if (*cursor->p == '}') {
        cursor->p++;
        return 0;
    }
    for (;;) {
        if (json_string(cursor, NULL, 0) != 0) return -1;
        json_ws(cursor);
        if (*cursor->p++ != ':') return -1;
        json_ws(cursor);
        if (json_value(cursor, depth + 1) != 0) return -1;
        json_ws(cursor);
        if (*cursor->p == '}') {
            cursor->p++;
            return 0;
        }
        if (*cursor->p++ != ',') return -1;
        json_ws(cursor);
    }
}

static int json_value(JsonCursor *cursor, unsigned depth) {
    if (depth > CNETD_JSON_DEPTH_MAX) return -1;
    json_ws(cursor);
    if (*cursor->p == '"') return json_string(cursor, NULL, 0);
    if (*cursor->p == '{') return json_object(cursor, depth);
    if (*cursor->p == '[') return json_array(cursor, depth);
    if (*cursor->p == '-' || (*cursor->p >= '0' && *cursor->p <= '9'))
        return json_number(cursor);
    if (strncmp((const char *)cursor->p, "true", 4) == 0) {
        cursor->p += 4;
        return 0;
    }
    if (strncmp((const char *)cursor->p, "false", 5) == 0) {
        cursor->p += 5;
        return 0;
    }
    if (strncmp((const char *)cursor->p, "null", 4) == 0) {
        cursor->p += 4;
        return 0;
    }
    return -1;
}

#endif
