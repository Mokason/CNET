#ifndef CNET_MCP_EVIDENCE_INTERNAL_H
#define CNET_MCP_EVIDENCE_INTERNAL_H
/* Private schema layer on the shared bounded JSON reader. No transport,
 * network permissions, content execution or implicit raw-text fallback. */
#include "cnet_json_internal.h"
#include "cce/cce_campaign_provenance.h"

typedef struct { char key[32]; const unsigned char *begin, *end; } ReadField;
typedef struct { char url[2049], title[513], text[24001], time[65], sha[65]; } ReadSource;

static int read_object(JsonCursor *c, ReadField *fields, unsigned max) {
    unsigned n = 0;
    json_ws(c);
    if (*c->p++ != '{') return -1;
    json_ws(c);
    if (*c->p == '}') { c->p++; return 0; }
    for (;;) {
        if (n == max || json_string(c, fields[n].key, sizeof fields[n].key)) return -1;
        for (unsigned i = 0; i < n; i++)
            if (!strcmp(fields[i].key, fields[n].key)) return -1;
        json_ws(c);
        if (*c->p++ != ':') return -1;
        json_ws(c);
        fields[n].begin = c->p;
        if (json_value(c, 0)) return -1;
        fields[n++].end = c->p;
        json_ws(c);
        if (*c->p == '}') { c->p++; return (int)n; }
        if (*c->p++ != ',') return -1;
        json_ws(c);
    }
}

static int read_document(const char *s) {
    if (!s || strlen(s) >= 262144) return 0;
    JsonCursor c = {(const unsigned char *)s};
    if (json_value(&c, 0)) return 0;
    json_ws(&c);
    return !*c.p;
}

static const ReadField *read_field(const ReadField *f, int n, const char *key) {
    for (int i = 0; i < n; i++) if (!strcmp(f[i].key, key)) return &f[i];
    return NULL;
}

static int read_literal(const ReadField *f, const char *s) {
    return f && (size_t)(f->end - f->begin) == strlen(s) &&
        !memcmp(f->begin, s, strlen(s));
}

static int read_string(const ReadField *f, char *out, size_t cap) {
    if (!f) return 0;
    JsonCursor c = {f->begin};
    return !json_string(&c, out, cap) && c.p == f->end;
}

static int read_string_is(const ReadField *f, const char *s) {
    char value[64];
    return read_string(f, value, sizeof value) && !strcmp(value, s);
}

static int read_tool(const char *s) {
    return s && (!strcmp(s, "cnet_safe_wiki_search") || !strcmp(s, "cnet_safe_web_read"));
}

static int read_no_controls(const char *s) {
    for (; *s; s++) if ((unsigned char)*s < 32 || (unsigned char)*s == 127) return 0;
    return 1;
}

static int read_arguments(const char *tool, const char *args) {
    ReadField f[1]; char value[2049];
    if (!read_tool(tool) || !read_document(args) || strlen(args) >= 15000) return 0;
    JsonCursor c = {(const unsigned char *)args};
    if (read_object(&c, f, 1) != 1) return 0;
    int wiki = !strcmp(tool, "cnet_safe_wiki_search");
    if (strcmp(f[0].key, wiki ? "query" : "url") ||
        !read_string(&f[0], value, wiki ? 801 : sizeof value) || !value[0] ||
        !read_no_controls(value)) return 0;
    int nonspace = 0;
    for (const char *p = value; *p; p++) if (*p != ' ') nonspace = 1;
    return nonspace && (wiki || !strncmp(value, "https://", 8));
}

static int read_utc_stamp(const char *s) {
    static const unsigned days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (strlen(s) < 20) return 0;
    unsigned v[6] = {0};
    const unsigned starts[] = {0,5,8,11,14,17}, widths[] = {4,2,2,2,2,2};
    if (s[4] != '-' || s[7] != '-' || s[10] != 'T' || s[13] != ':' || s[16] != ':') return 0;
    for (unsigned i = 0; i < 6; i++) for (unsigned j = 0; j < widths[i]; j++) {
        char ch = s[starts[i] + j];
        if (ch < '0' || ch > '9') return 0;
        v[i] = v[i] * 10 + (unsigned)(ch - '0');
    }
    if (!v[0] || v[1] < 1 || v[1] > 12 || !v[2] || v[3] > 23 || v[4] > 59 || v[5] > 59) return 0;
    unsigned maxday = days[v[1]-1] + (v[1] == 2 && v[0] % 4 == 0 && (v[0] % 100 != 0 || v[0] % 400 == 0));
    if (v[2] > maxday) return 0;
    s += 19;
    if (*s == '.') {
        unsigned digits = 0; s++;
        while (*s >= '0' && *s <= '9') { digits++; s++; }
        if (!digits || digits > 7) return 0;
    }
    return !strcmp(s, "Z") || !strcmp(s, "+00:00");
}

static int read_source(JsonCursor *c, ReadSource *source) {
    ReadField f[5];
    if (read_object(c, f, 5) != 5 ||
        !read_string(read_field(f, 5, "url"), source->url, sizeof source->url) ||
        !read_string(read_field(f, 5, "title"), source->title, sizeof source->title) ||
        !read_string(read_field(f, 5, "text"), source->text, sizeof source->text) ||
        !read_string(read_field(f, 5, "retrieved_at"), source->time, sizeof source->time) ||
        !read_string(read_field(f, 5, "sha256"), source->sha, sizeof source->sha)) return 0;
    if (strncmp(source->url, "https://", 8) || !source->url[8] ||
        !read_no_controls(source->url) || !read_no_controls(source->time) ||
        !read_utc_stamp(source->time) || !source->text[0] || strlen(source->sha) != 64) return 0;
    /* The approved backend emits ASCII URI serialization, never raw Unicode
     * authority/path separators that a downstream line parser might split. */
    for (size_t i = 0; source->url[i]; i++) if ((unsigned char)source->url[i] >= 127) return 0;
    for (unsigned i = 0; i < 64; i++)
        if (!((source->sha[i] >= '0' && source->sha[i] <= '9') ||
              (source->sha[i] >= 'a' && source->sha[i] <= 'f'))) return 0;
    char digest[65];
    return !cce_sha256_bytes_hex(source->text, strlen(source->text), digest) &&
        !strcmp(digest, source->sha);
}

static int read_evidence(const char *s, const char *tool, ReadSource *first) {
    ReadField f[6]; char name[64]; ReadSource source;
    if (!read_document(s)) return 0;
    JsonCursor c = {(const unsigned char *)s};
    if (read_object(&c, f, 6) != 6 ||
        !read_string_is(read_field(f, 6, "schema"), "cnet.web-evidence.v1") ||
        !read_string(read_field(f, 6, "tool"), name, sizeof name) ||
        !read_tool(name) || (tool && strcmp(tool, name)) ||
        !read_string_is(read_field(f, 6, "status"), "ok") ||
        !read_literal(read_field(f, 6, "trusted"), "false") ||
        !read_literal(read_field(f, 6, "certified"), "false")) return 0;
    const ReadField *sources = read_field(f, 6, "sources");
    if (!sources || *sources->begin != '[') return 0;
    c.p = sources->begin + 1;
    unsigned count = 0;
    for (;;) {
        if (count == 3 || !read_source(&c, &source)) return 0;
        if (count++ == 0 && first) *first = source;
        json_ws(&c);
        if (*c.p == ']') return c.p + 1 == sources->end;
        if (*c.p++ != ',') return 0;
    }
}

static int read_response(const char *raw, const char *tool, char *out, size_t cap) {
    ReadField frame[3], result[3];
    if (!read_document(raw)) return 0;
    JsonCursor c = {(const unsigned char *)raw};
    if (read_object(&c, frame, 3) != 3 ||
        !read_string_is(read_field(frame, 3, "jsonrpc"), "2.0") ||
        !read_literal(read_field(frame, 3, "id"), "1")) return 0;
    const ReadField *r = read_field(frame, 3, "result");
    if (!r) return 0;
    c.p = r->begin;
    int n = read_object(&c, result, 3);
    if (n != 3 || !read_literal(read_field(result, n, "isError"), "false")) return 0;
    const ReadField *e = read_field(result, n, "structuredContent");
    const ReadField *content = read_field(result, n, "content");
    if (!e || !content || *content->begin != '[' || e->end - e->begin < 1 ||
        (size_t)(e->end - e->begin) >= cap) return 0;
    /* Consume only structured evidence; content is compatibility data, never
     * an alternative success channel. Malformed JSON already refused above. */
    size_t bytes = (size_t)(e->end - e->begin);
    memcpy(out, e->begin, bytes); out[bytes] = 0;
    if (!read_evidence(out, tool, NULL)) { out[0] = 0; return 0; }
    return 1;
}
#endif
