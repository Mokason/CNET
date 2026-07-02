#include "../../include/contract/mcp_wiki.h"
#include "../../include/contract/mcp_utils.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static void trim(char *s) {
    if (!s) return;
    mcp_trim_ws(s);
}

static void filter_summary(const char *raw, char *out, size_t cap) {
    size_t n = 0;
    int periods = 0;
    if (!raw || !out || cap == 0) return;
    out[0] = '\0';
    for (size_t i = 0; raw[i] && n + 1 < cap; ++i) {
        char c = raw[i];
        out[n++] = c;
        if (c == '.') {
            periods++;
            if (periods >= 2 && n > 40) break;
        }
        if (n > 480) break;
    }
    out[n] = '\0';
    trim(out);
    if (strlen(out) < 5) {
        strncpy(out, raw, cap - 1);
        out[cap - 1] = '\0';
    }
}

static int is_word_equal(const char *a, const char *b) {
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

static int token_is_skippable(const char *tok) {
    const char *skip[] = {"who", "is", "what", "the", "about", "tell", "me",
                          "search", "for", "wikipedia", NULL};
    size_t i;
    for (i = 0; skip[i] != NULL; ++i) {
        if (is_word_equal(tok, skip[i])) return 1;
    }
    return 0;
}

static int build_wiki_title(const char *query, char *out, size_t out_cap) {
    int nw = 0;
    char tmp[256];
    char *tok;
    char words[64][32];
    int i, w;

    if (!query || !out || out_cap == 0) return -1;
    out[0] = '\0';

    strncpy(tmp, query, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    tok = strtok(tmp, " ,.?");
    while (tok && nw < 32) {
        size_t tok_len;
        if (!token_is_skippable(tok)) {
            tok_len = strlen(tok);
            if (tok_len >= sizeof(words[0])) {
                tok_len = sizeof(words[0]) - 1;
            }
            memcpy(words[nw], tok, tok_len);
            words[nw][tok_len] = '\0';
            ++nw;
        }
        tok = strtok(NULL, " ,.?");
    }

    w = 0;
    for (i = 0; i < nw; ++i) {
        size_t k = strlen(words[i]);
        if (k == 0) continue;
        if (i > 0 && w + 1 < (int)out_cap) out[w++] = '_';
        if (w + (int)k >= (int)out_cap) break;
        memcpy(out + w, words[i], k);
        w += (int)k;
    }
    out[w] = '\0';
    return (w < 2) ? -1 : 0;
}

static int fetch_wikipedia_extract(const char *raw_query, char *extract, size_t cap) {
    char title[256];
    char encoded_title[512];
    char url[1024];
    char raw[4096];
    char raw_extract[1024];

    if (!raw_query || !extract || cap == 0) return -1;
    if (build_wiki_title(raw_query, title, sizeof(title)) != 0) {
        snprintf(extract, cap, "LOOKUP_FAILED");
        return 0;
    }
    if (mcp_url_encode_component(title, encoded_title, sizeof(encoded_title)) != 0) {
        snprintf(extract, cap, "LOOKUP_FAILED");
        return 0;
    }
    if (snprintf(url, sizeof(url),
                 "https://en.wikipedia.org/api/rest_v1/page/summary/%s",
                 encoded_title) >= (int)sizeof(url)) {
        snprintf(extract, cap, "LOOKUP_FAILED");
        return 0;
    }

    if (mcp_http_get(url, raw, sizeof(raw)) != 0) {
        snprintf(extract, cap, "LOOKUP_FAILED");
        return 0;
    }

    if (mcp_extract_json_field(raw, "\"extract\"", raw_extract, sizeof(raw_extract)) <= 0) {
        snprintf(extract, cap, "LOOKUP_FAILED");
        return 0;
    }
    trim(raw_extract);
    if (strlen(raw_extract) < 8) {
        snprintf(extract, cap, "LOOKUP_FAILED");
        return 0;
    }

    filter_summary(raw_extract, extract, cap);
    return 0;
}

int port_contract_mcp_wiki_lookup(
    const char *query,
    char *summary_out, size_t summary_out_cap,
    int *from_memory
) {
    char summary[768];
    summary[0] = '\0';
    if (!query || !summary_out || summary_out_cap == 0 || !from_memory) return -1;

    *from_memory = 0;
    summary_out[0] = '\0';
    mcp_recall_fact(query, summary, sizeof(summary));
    if (summary[0]) {
        *from_memory = 1;
        strncpy(summary_out, summary, summary_out_cap - 1);
        summary_out[summary_out_cap - 1] = '\0';
        return 0;
    }

    if (fetch_wikipedia_extract(query, summary, sizeof(summary)) != 0) {
        snprintf(summary_out, summary_out_cap, "LOOKUP_FAILED");
        summary_out[summary_out_cap - 1] = '\0';
        return 0;
    }
    if (strlen(summary) == 0 || strcmp(summary, "LOOKUP_FAILED") == 0) {
        snprintf(summary_out, summary_out_cap, "%s", summary);
        summary_out[summary_out_cap - 1] = '\0';
        return 0;
    }
    snprintf(summary_out, summary_out_cap, "%s", summary);
    summary_out[summary_out_cap - 1] = '\0';
    mcp_memorize_fact(query, summary_out);
    return 0;
}


