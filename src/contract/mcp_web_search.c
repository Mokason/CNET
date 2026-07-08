#include "../../include/contract/mcp_web_search.h"
#include "../../include/contract/mcp_wiki.h"
#include "../../include/contract/mcp_utils.h"

#include <stdio.h>
#include <string.h>

static void sanitize_output_snippet(const char *in, char *out, size_t cap) {
    size_t i, j = 0;
    if (!in || !out || cap == 0) return;
    for (i = 0; in[i] != '\0' && j + 1 < cap; ++i) {
        unsigned char c = (unsigned char)in[i];
        if (c == '\n' || c == '\r' || c == '\t') {
            out[j++] = ' ';
            continue;
        }
        if (c == '|') {
            out[j++] = ' ';
            continue;
        }
        if (c == 0x22 || c < 0x20 || c > 0x7e) {
            continue;
        }
        out[j++] = (char)c;
    }
    out[j] = '\0';
    mcp_trim_ws(out);
}

static int append_topic_summary(char *out, size_t cap, const char *topic) {
    size_t current = strlen(out);
    if (current + 1 >= cap) return -1;
    if (out[0] != '\0' && current + 3 < cap) {
        strncat(out, " | ", cap - current - 1);
        current += 3;
    }
    if (strlen(topic) == 0) return 0;
    if (current + strlen(topic) + 1 > cap) return -1;
    strncat(out, topic, cap - current - 1);
    return 0;
}

int port_contract_mcp_web_search(
    const char *query,
    char *results_out, size_t results_cap,
    int *from_cache
) {
    char mem_key[256];
    char safe_query[256];
    char encoded_query[512];
    char url[1024];
    char raw[8192];
    char filtered[1024];
    char topic[768];
    char abstract[768];
    const char *topic_scan;
    int got_topics = 0;

    if (!query || !results_out || results_cap == 0 || !from_cache) return -1;
    *from_cache = 0;
    results_out[0] = '\0';

    mcp_sanitize_cache_key(query, safe_query, sizeof(safe_query));
    if (snprintf(mem_key, sizeof(mem_key), "websearch:%s", safe_query) >= (int)sizeof(mem_key)) {
        snprintf(results_out, results_cap, "Web search failed for unsafe query.");
        return 0;
    }
    if (mcp_recall_fact(mem_key, results_out, results_cap)) {
        *from_cache = 1;
        return 0;
    }

    if (mcp_url_encode_component(safe_query, encoded_query, sizeof(encoded_query)) != 0) {
        snprintf(results_out, results_cap, "Web search failed to sanitize query.");
        return 0;
    }
    if (snprintf(url, sizeof(url),
                 "https://api.duckduckgo.com/?q=%s&format=json&pretty=1",
                 encoded_query) >= (int)sizeof(url)) {
        snprintf(results_out, results_cap, "Web search failed to build request URL.");
        return 0;
    }

    if (mcp_http_get(url, raw, sizeof(raw)) != 0) {
        snprintf(results_out, results_cap, "Web search failed to execute for: %s", query);
        return 0;
    }

    filtered[0] = '\0';
    if (mcp_extract_json_field(raw, "\"AbstractText\"", abstract, sizeof(abstract)) > 0) {
        char safe_abstract[768];
        sanitize_output_snippet(abstract, safe_abstract, sizeof(safe_abstract));
        if (safe_abstract[0] != '\0') {
            strncat(filtered, safe_abstract, sizeof(filtered) - strlen(filtered) - 1);
        }
    }

    topic_scan = strstr(raw, "\"RelatedTopics\"");
    while (topic_scan && got_topics < 3) {
        const char *text_pos = strstr(topic_scan, "\"Text\"");
        if (!text_pos) break;
        if (mcp_extract_json_field(text_pos, "\"Text\"", topic, sizeof(topic)) > 0) {
            char safe_topic[512];
            sanitize_output_snippet(topic, safe_topic, sizeof(safe_topic));
            if (safe_topic[0] != '\0' && append_topic_summary(filtered, sizeof(filtered), safe_topic) == 0) {
                ++got_topics;
            }
        }
        topic_scan = strstr(text_pos + 6, "\"Text\"");
    }

    if (filtered[0] == '\0' || strstr(filtered, "NO_RESULTS")) {
        snprintf(results_out, results_cap, "No useful web results for: %s", query);
        return 0;
    }

    if (strlen(filtered) < 10) {
        snprintf(results_out, results_cap, "No useful web results for: %s", query);
        return 0;
    }

    snprintf(results_out, results_cap, "%s", filtered);
    mcp_memorize_fact(mem_key, results_out);
    return 0;
}


