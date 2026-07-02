#include "../../include/contract/mcp_wiki.h"
#include "../../include/contract/mcp_utils.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_FACTS 128

static int memory_loaded = 0;
static char fact_queries[MAX_FACTS][128];
static char fact_summaries[MAX_FACTS][768];
static size_t fact_count = 0;

static void trim(char *s) {
    mcp_trim_ws(s);
}

static int str_eq_nocase(const char *a, const char *b) {
    if (a == NULL || b == NULL) return 0;
    while (*a != '\0' && *b != '\0') {
        char ca = (char)tolower((unsigned char)*a);
        char cb = (char)tolower((unsigned char)*b);
        if (ca != cb) return 0;
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

static int is_word_boundary(char c) {
    if (c == '\0') return 1;
    return isspace((unsigned char)c) || ispunct((unsigned char)c);
}

static int is_low_value_token(const char *token) {
    const char *skip[] = {
        "is", "are", "was", "were", "who", "what", "the", "a", "an", "and",
        "of", "to", "in", "on", "for", "about", "tell", "me", NULL
    };
    size_t i;
    for (i = 0; skip[i] != NULL; ++i) {
        if (strcmp(token, skip[i]) == 0) return 1;
    }
    return 0;
}

static int contains_whole_word(const char *text, const char *token) {
    size_t n = strlen(token);
    const char *p = text;
    if (!text || !token || n == 0) return 0;

    while ((p = strstr(p, token)) != NULL) {
        char before = (p == text) ? '\0' : *(p - 1);
        char after = *(p + n);
        if (is_word_boundary(before) && is_word_boundary(after)) {
            return 1;
        }
        p += n;
    }
    return 0;
}

static int token_overlap_count(const char *query, const char *stored) {
    char copy[128];
    char *tok;
    int hits = 0;

    if (!query || !stored) return 0;
    strncpy(copy, query, sizeof(copy) - 1);
    copy[sizeof(copy) - 1] = '\0';

    tok = strtok(copy, " ");
    while (tok != NULL) {
        if (strlen(tok) >= 3 && !is_low_value_token(tok) &&
            contains_whole_word(stored, tok)) {
            ++hits;
        }
        tok = strtok(NULL, " ");
    }
    return hits;
}

static void sanitize_fact_summary(const char *in, char *out, size_t cap) {
    size_t i;
    size_t j = 0;
    if (!in || !out || cap == 0) return;
    for (i = 0; in[i] != '\0' && j + 1 < cap; ++i) {
        char c = in[i];
        if (c == '\r' || c == '\n' || c == '|') {
            out[j++] = ' ';
        } else {
            out[j++] = c;
        }
    }
    out[j] = '\0';
    trim(out);
    if (out[0] == '\0') {
        strncpy(out, "[empty]", cap - 1);
        out[cap - 1] = '\0';
    }
}

static int load_memory(void) {
    if (memory_loaded) return 0;

    FILE *f = fopen(MCP_WIKI_MEMORY_FILE, "r");
    if (!f) {
        int rc = mcp_atomic_write_text(MCP_WIKI_MEMORY_FILE, "");
        if (rc != 0) return -1;
        memory_loaded = 1;
        fact_count = 0;
        return 0;
    }

    memory_loaded = 1;
    fact_count = 0;
    while (fact_count < MAX_FACTS) {
        char line[1024];
        char *line_ptr;
        char *sep;
        if (!fgets(line, sizeof(line), f)) break;

        line_ptr = line;
        if (strlen(line) == sizeof(line) - 1 && line[sizeof(line)-2] != '\n' && !feof(f)) {
            int c;
            while ((c = fgetc(f)) != EOF && c != '\n') {}
            continue;
        }

        sep = strchr(line_ptr, '|');
        if (!sep) continue;
        *sep = '\0';
        trim(line_ptr);
        trim(sep + 1);
        if (line_ptr[0] == '\0') continue;
        strncpy(fact_queries[fact_count], line_ptr, sizeof(fact_queries[0]) - 1);
        fact_queries[fact_count][sizeof(fact_queries[0]) - 1] = '\0';
        strncpy(fact_summaries[fact_count], sep + 1, sizeof(fact_summaries[0]) - 1);
        fact_summaries[fact_count][sizeof(fact_summaries[0]) - 1] = '\0';
        fact_count++;
    }
    fclose(f);
    return 0;
}

static int save_memory(void) {
    size_t i;
    char *snapshot = NULL;
    size_t snapshot_cap = 2048;
    size_t used = 0;

    snapshot = (char *)malloc(snapshot_cap);
    if (!snapshot) return -1;
    snapshot[0] = '\0';

    for (i = 0; i < fact_count; ++i) {
        char line[1024];
        size_t line_len;
        int rc = snprintf(
            line,
            sizeof(line),
            "%s|%s\n",
            fact_queries[i],
            fact_summaries[i]
        );
        if (rc <= 0) continue;
        line_len = (size_t)rc;
        while (line_len + used + 1 > snapshot_cap) {
            char *grown = (char *)realloc(snapshot, snapshot_cap * 2);
            if (!grown) {
                free(snapshot);
                return -1;
            }
            snapshot = grown;
            snapshot_cap *= 2;
        }
        memcpy(snapshot + used, line, line_len);
        used += line_len;
    }
    if (snapshot == NULL) return -1;
    if (used == 0) {
        snapshot[0] = '\0';
    } else {
        snapshot[used] = '\0';
    }

    {
        int rc = mcp_atomic_write_text(MCP_WIKI_MEMORY_FILE, snapshot);
        free(snapshot);
        return rc;
    }
}

int mcp_memory_init(void) {
    return load_memory();
}

int mcp_recall_fact(const char *query, char *out_buf, size_t buf_cap) {
    char key[128];
    char lookup_key[128];
    size_t i;

    if (!query || !out_buf || buf_cap == 0) return 0;
    load_memory();
    mcp_sanitize_cache_key(query, key, sizeof(key));
    mcp_normalize_cache_key(key, lookup_key, sizeof(lookup_key));
    if (strlen(lookup_key) == 0) return 0;
    strncpy(key, lookup_key, sizeof(key) - 1);
    key[sizeof(key) - 1] = '\0';

    for (i = 0; i < fact_count; ++i) {
        char fk[128];
        mcp_normalize_cache_key(fact_queries[i], fk, sizeof(fk));
        if (str_eq_nocase(fk, key)) {
            strncpy(out_buf, fact_summaries[i], buf_cap - 1);
            out_buf[buf_cap - 1] = '\0';
            return 1;
        }
        if (token_overlap_count(key, fk) >= 2) {
            strncpy(out_buf, fact_summaries[i], buf_cap - 1);
            out_buf[buf_cap - 1] = '\0';
            return 1;
        }
    }
    return 0;
}

void mcp_memorize_fact(const char *query, const char *summary) {
    char key[128];
    char store_key[128];
    char sanitized_summary[768];
    size_t i;

    if (!query || !summary || summary[0] == '\0') return;
    load_memory();
    mcp_sanitize_cache_key(query, key, sizeof(key));
    mcp_normalize_cache_key(key, store_key, sizeof(store_key));
    if (strlen(store_key) < 2) return;
    strncpy(key, store_key, sizeof(key) - 1);
    key[sizeof(key) - 1] = '\0';
    sanitize_fact_summary(summary, sanitized_summary, sizeof(sanitized_summary));

    for (i = 0; i < fact_count; ++i) {
        char fk[128];
        mcp_normalize_cache_key(fact_queries[i], fk, sizeof(fk));
        if (str_eq_nocase(fk, key)) {
            strncpy(fact_summaries[i], sanitized_summary, sizeof(fact_summaries[i]) - 1);
            fact_summaries[i][sizeof(fact_summaries[i])-1] = '\0';
            save_memory();
            return;
        }
    }

    if (fact_count >= MAX_FACTS) return;
    strncpy(fact_queries[fact_count], key, sizeof(fact_queries[fact_count]) - 1);
    fact_queries[fact_count][sizeof(fact_queries[fact_count]) - 1] = '\0';
    strncpy(fact_summaries[fact_count], sanitized_summary, sizeof(fact_summaries[fact_count]) - 1);
    fact_summaries[fact_count][sizeof(fact_summaries[fact_count]) - 1] = '\0';
    ++fact_count;
    save_memory();
}


