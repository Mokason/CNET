#define _POSIX_C_SOURCE 200809L
#include "cnet_kb_recall.h"
#include "cnet_json_internal.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define BANK_MAX (8u * 1024u * 1024u)
#define CHUNK_MAX 2048
#define TOKENS_MAX 12

static int word(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '+';
}

/* This is conservative lexical relevance, not semantic similarity. In
 * particular, preserve negation, numbers and short domain names such as C. */
static int tokens(const char *query, char out[TOKENS_MAX][64]) {
    static const char *stop[] = {
        "a", "an", "the", "i", "you", "your", "we", "our", "it", "its",
        "is", "are", "was", "were", "be", "been", "being", "can", "could",
        "would", "should", "will", "do", "does", "did", "have", "has",
        "what", "when", "where", "which", "about", "with", "from", "to",
        "of", "in", "on", "for", "at", "me", "my", "this", "that",
        "please", "tell", "explain", "describe", "know", "understand", NULL
    };
    int count = 0;
    const unsigned char *p = (const unsigned char *)query;
    while (*p) {
        char term[64];
        size_t n = 0;
        int skip = 0;
        if (*p >= 128) return -1; /* Never drop unsupported query terms. */
        if (!word(*p)) { p++; continue; }
        while (word(*p)) {
            if (n + 1 >= sizeof term) return -1;
            term[n++] = (*p >= 'A' && *p <= 'Z') ? (char)(*p + 32) : (char)*p;
            p++;
        }
        term[n] = 0;
        for (int i = 0; stop[i]; i++) if (!strcmp(term, stop[i])) skip = 1;
        for (int i = 0; i < count; i++) if (!strcmp(term, out[i])) skip = 1;
        if (skip) continue;
        if (count == TOKENS_MAX) return -1;
        strcpy(out[count++], term);
    }
    return count;
}

static int contains_term(const char *text, const char *term) {
    size_t n = strlen(term);
    for (const char *p = text; *p; p++) {
        if ((p == text || ((unsigned char)p[-1] < 128 && !word((unsigned char)p[-1]))) &&
            !strncasecmp(p, term, n) && (unsigned char)p[n] < 128 &&
            !word((unsigned char)p[n])) return 1;
    }
    return 0;
}

static int record(const char *line, char chunk[CHUNK_MAX + 1]) {
    JsonCursor valid = {(const unsigned char *)line};
    JsonCursor c = valid;
    unsigned seen = 0;
    int harvested = 0;
    /* The shared reader checks syntax/UTF-8; this layer checks the schema and
     * duplicate decoded keys (including Unicode-escaped key spellings). */
    if (json_value(&valid, 0) != 0) return 0;
    json_ws(&valid);
    if (*valid.p) return 0;
    json_ws(&c);
    if (*c.p++ != '{') return 0;
    for (;;) {
        char key[32], value[CHUNK_MAX + 1];
        unsigned bit;
        json_ws(&c);
        if (json_string(&c, key, sizeof key) != 0) return 0;
        bit = !strcmp(key, "ts") ? 1u : !strcmp(key, "source") ? 2u :
              !strcmp(key, "chunk") ? 4u : !strcmp(key, "claimed_cert") ? 8u :
              !strcmp(key, "kind") ? 16u : 0u;
        if (!bit || (seen & bit)) return 0;
        seen |= bit;
        json_ws(&c);
        if (*c.p++ != ':') return 0;
        json_ws(&c);
        if (bit == 1u) {
            const unsigned char *start = c.p;
            if (*start < '1' || *start > '9') return 0;
            while (*c.p >= '0' && *c.p <= '9') c.p++;
            if (c.p - start > 19) return 0;
        } else if (bit == 8u) {
            if (*c.p++ != '0') return 0;
        } else {
            if (json_string(&c, value, sizeof value) != 0 || !value[0]) return 0;
            /* Decoded framing/control bytes must not enter native text replies. */
            for (const unsigned char *p = (const unsigned char *)value; *p; p++)
                if (*p < 32 || *p == 127) return 0;
            if (bit == 4u) strcpy(chunk, value);
            if (bit == 16u && strcmp(value, "ingest") && strcmp(value, "miss_harvest")) return 0;
            if (bit == 16u) harvested = !strcmp(value, "miss_harvest");
        }
        json_ws(&c);
        if (*c.p == '}') { c.p++; break; }
        if (*c.p++ != ',') return 0;
    }
    json_ws(&c);
    return seen == 31u && !*c.p && (!harvested || !strncasecmp(chunk, "Q:", 2));
}

int cnet_kb_recall(const char *path, const char *query, char *out, size_t cap) {
    char terms[TOKENS_MAX][64];
    char best[CHUNK_MAX + 1] = "";
    struct stat info;
    int fd, count, result = 0;
    FILE *file;
    char *data;
    size_t size;
    if (!out || !cap) return -1;
    out[0] = 0;
    if (!path || !query || strlen(query) >= 512) return -1;
    count = tokens(query, terms);
    if (count < 2) return 0;
    fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return -1;
    if (fstat(fd, &info) || !S_ISREG(info.st_mode) || info.st_size < 0 ||
        info.st_size > BANK_MAX) { close(fd); return -1; }
    size = (size_t)info.st_size;
    data = calloc(size + 17, 1);
    file = fdopen(fd, "r");
    if (!data || !file) {
        free(data);
        if (file) fclose(file); else close(fd);
        return -1;
    }
    /* A growing/shrinking/NUL-containing snapshot refuses as a whole. */
    if (fread(data, 1, size + 1, file) != size || ferror(file) || memchr(data, 0, size))
        result = -1;
    fclose(file);
    if (result) { free(data); return result; }
    for (char *p = data, *end = data + size; p < end;) {
        char line[16384 + 16] = {0}, chunk[CHUNK_MAX + 1], topic[CHUNK_MAX + 1];
        char *newline = memchr(p, '\n', (size_t)(end - p));
        size_t len;
        int matched = 1;
        if (!newline) break; /* An incomplete append cannot supply a note. */
        len = (size_t)(newline - p);
        if (len >= 16384) { p = newline + 1; continue; }
        memcpy(line, p, len);
        p = newline + 1;
        if (!record(line, chunk)) continue;
        strcpy(topic, chunk);
        char *question = topic;
        while (*question == ' ') question++;
        if (!strncasecmp(question, "Q:", 2)) {
            char *answer = NULL;
            for (char *s = question + 2; *s; s++)
                if (!strncasecmp(s, " A:", 3)) {
                    if (answer) { matched = 0; break; }
                    answer = s;
                }
            if (!answer || !matched) continue;
            char *body = answer + 3;
            while (*body == ' ') body++;
            if (!*body) continue;
            *answer = 0;
        }
        for (int i = 0; i < count; i++)
            if (!contains_term(topic, terms[i])) { matched = 0; break; }
        if (matched) strcpy(best, chunk); /* Append order: most recent match. */
    }
    free(data);
    if (best[0] && strlen(best) < cap) {
        strcpy(out, best);
        result = 1;
    }
    return result;
}
