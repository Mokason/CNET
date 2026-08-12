#include "../include/cnet_roe_net.h"

#include "../include/cnet_platform.h"  /* CNET_HAVE_CURL */
#if CNET_HAVE_CURL
#include <curl/curl.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct MemBuf {
    char *data;
    size_t len;
};

#if CNET_HAVE_CURL
static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    struct MemBuf *m = (struct MemBuf *)userdata;
    size_t n = size * nmemb;
    char *p;
    if (!m || !ptr || n == 0) return 0;
    p = (char *)realloc(m->data, m->len + n + 1);
    if (!p) return 0;
    m->data = p;
    memcpy(m->data + m->len, ptr, n);
    m->len += n;
    m->data[m->len] = '\0';
    return n;
}

static int http_get(const char *url, long timeout_ms, struct MemBuf *out) {
    CURL *curl;
    CURLcode rc;
    long code = 0;
    if (!url || !out) return -1;
    memset(out, 0, sizeof *out);
    curl = curl_easy_init();
    if (!curl) return -2;
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, out);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms > 0 ? timeout_ms : 15000L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "CNET-ROE-ASI/1.0");
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    rc = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK || code >= 400) return -3;
    return 0;
}

static int http_post_json(const char *url, const char *body, long timeout_ms,
                          struct MemBuf *out) {
    CURL *curl;
    CURLcode rc;
    struct curl_slist *hdrs = NULL;
    long code = 0;
    if (!url || !body || !out) return -1;
    memset(out, 0, sizeof *out);
    curl = curl_easy_init();
    if (!curl) return -2;
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, out);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms > 0 ? timeout_ms : 60000L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "CNET-ROE-ASI/1.0");
    rc = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK || code >= 400) return -3;
    return 0;
}

/* Extract JSON string value for key "response" or "AbstractText" (simple). */
#else
/* No libcurl: the live-teacher HTTP transport is absent. These return the
 * same failure codes the real ones use for a dead endpoint, so callers'
 * fallback paths are exercised identically rather than via a new branch. */
static int http_get(const char *url, long timeout_ms, struct MemBuf *out) {
    (void)url; (void)timeout_ms;
    /* Zero the sink even on failure: callers declare `struct MemBuf mb;`
       uninitialised and rely on the transport to define it, so a stub that
       returned without touching it would hand back garbage to any caller whose
       error handling is less careful than it looks. */
    if (out) memset(out, 0, sizeof *out);
    return -1;
}
static int http_post_json(const char *url, const char *body, long timeout_ms,
                          struct MemBuf *out) {
    (void)url; (void)body; (void)timeout_ms;
    if (out) memset(out, 0, sizeof *out);
    return -1;
}
#endif /* CNET_HAVE_CURL */

static int json_string_field(const char *json, const char *key, char *out, size_t cap) {
    char pat[96];
    const char *p, *start, *end;
    size_t n;
    if (!json || !key || !out || !cap) return -1;
    out[0] = 0;
    snprintf(pat, sizeof pat, "\"%s\"", key);
    p = strstr(json, pat);
    if (!p) return -1;
    p = strchr(p + strlen(pat), ':');
    if (!p) return -1;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != '"') return -1;
    start = p + 1;
    end = start;
    while (*end && *end != '"') {
        if (*end == '\\' && end[1]) end += 2;
        else end++;
    }
    if (*end != '"') return -1;
    n = (size_t)(end - start);
    if (n >= cap) n = cap - 1;
    memcpy(out, start, n);
    out[n] = 0;
    /* unescape common sequences lightly */
    {
        char *w = out, *r = out;
        while (*r) {
            if (r[0] == '\\' && r[1] == 'n') {
                *w++ = '\n';
                r += 2;
            } else if (r[0] == '\\' && r[1] == '"') {
                *w++ = '"';
                r += 2;
            } else if (r[0] == '\\' && r[1] == '\\') {
                *w++ = '\\';
                r += 2;
            } else {
                *w++ = *r++;
            }
        }
        *w = 0;
    }
    return out[0] ? 0 : -1;
}

static void url_encode(const char *in, char *out, size_t cap) {
    static const char *hex = "0123456789ABCDEF";
    size_t o = 0;
    if (!in || !out || !cap) return;
    while (*in && o + 4 < cap) {
        unsigned char c = (unsigned char)*in++;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out[o++] = (char)c;
        } else if (c == ' ') {
            out[o++] = '+';
        } else {
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 15];
        }
    }
    out[o] = 0;
}

void roe_net_init(RoeNet *N) {
    if (!N) return;
    memset(N, 0, sizeof *N);
    snprintf(N->llm_url, sizeof N->llm_url, "http://127.0.0.1:11434/api/generate");
    snprintf(N->llm_model, sizeof N->llm_model, "qwen2.5:7b");
    snprintf(N->lookup_url, sizeof N->lookup_url,
             "https://api.duckduckgo.com/?q=%%s&format=json&no_html=1&skip_disambig=1");
    N->timeout_ms = 45000;
    N->enable_llm = 0;
    N->enable_lookup = 0;
    N->think = 0; /* teacher path: short answers; cloud V4 flash needs think:false */
}

void roe_net_from_env(RoeNet *N) {
    const char *e;
    roe_net_init(N);
    e = getenv("ROE_LIVE");
    if (e && (e[0] == '1' || e[0] == 'y' || e[0] == 'Y')) {
        N->enable_llm = 1;
        N->enable_lookup = 1;
    }
    /* Open chat = residual teacher on organic misses (still never self-CERT) */
    e = getenv("CNET_OPEN_CHAT");
    if (e && (e[0] == '1' || e[0] == 'y' || e[0] == 'Y')) {
        N->enable_llm = 1;
        /* lookup optional; keep off by default for chat unless ROE_LOOKUP=1 */
    }
    e = getenv("ROE_OPEN_CHAT");
    if (e && (e[0] == '1' || e[0] == 'y' || e[0] == 'Y'))
        N->enable_llm = 1;
    e = getenv("ROE_LLM");
    if (e && e[0] == '1') N->enable_llm = 1;
    if (e && e[0] == '0') N->enable_llm = 0;
    e = getenv("ROE_LOOKUP");
    if (e && e[0] == '1') N->enable_lookup = 1;
    if (e && e[0] == '0') N->enable_lookup = 0;
    e = getenv("ROE_LLM_URL");
    if (e && e[0]) snprintf(N->llm_url, sizeof N->llm_url, "%s", e);
    e = getenv("ROE_LLM_MODEL");
    if (e && e[0]) snprintf(N->llm_model, sizeof N->llm_model, "%s", e);
    e = getenv("ROE_LOOKUP_URL");
    if (e && e[0]) snprintf(N->lookup_url, sizeof N->lookup_url, "%s", e);
    e = getenv("ROE_TIMEOUT_MS");
    if (e && e[0]) N->timeout_ms = strtol(e, NULL, 10);
    e = getenv("ROE_LLM_THINK");
    if (e && e[0]) {
        if (e[0] == '1' || e[0] == 'y' || e[0] == 'Y') N->think = 1;
        else N->think = 0;
    }
    /* Cloud deepseek flash defaults to empty answers unless think is off. */
    if (strstr(N->llm_model, ":cloud") && !getenv("ROE_LLM_THINK")) N->think = 0;
}

int roe_net_llm(RoeNet *N, const char *prompt, char *answer, size_t answer_cap,
                uint64_t *tokens_est) {
    char sys_prompt[512];
    struct MemBuf mb;
    int rc = -1;
    size_t plen;

    if (tokens_est) *tokens_est = 0;
    if (!N || !prompt || !answer || !answer_cap) return -1;
    answer[0] = 0;
    if (!N->enable_llm) {
        snprintf(N->last_err, sizeof N->last_err, "llm disabled");
        return -2;
    }

    memset(&mb, 0, sizeof mb);

    snprintf(sys_prompt, sizeof sys_prompt,
             "You are an external teacher for ROE-ASI / CNET. Your draft is UNTRUSTED "
             "(never self-CERT). Teach clearly and completely: finish your thought, "
             "cover the question, use short paragraphs if needed. No markdown headers. "
             "Query: ");
    plen = strlen(sys_prompt) + strlen(prompt);
    /* escape quotes in prompt lightly */
    {
        char pq[2500];
        char body[12288];
        size_t i, j = 0;
        const char *think_json = N->think ? "true" : "false";
        int teach = 1;
        int npred = 512; /* full teach default — was 120 and cut answers off */
        double temp = 0.35;
        const char *ep;
        ep = getenv("CNET_TEACHER_ON_MISS");
        if (ep && (ep[0] == '0' || ep[0] == 'n' || ep[0] == 'N')) teach = 0;
        ep = getenv("CNET_OPEN_CHAT");
        if (ep && (ep[0] == '1' || ep[0] == 'y' || ep[0] == 'Y')) teach = 1;
        ep = getenv("ROE_OPEN_CHAT");
        if (ep && (ep[0] == '1' || ep[0] == 'y' || ep[0] == 'Y')) teach = 1;
        if (teach) npred = 1024;
        ep = getenv("ROE_LLM_NUM_PREDICT");
        if (ep && ep[0]) npred = (int)strtol(ep, NULL, 10);
        if (npred < 64) npred = 64;
        if (npred > 2048) npred = 2048;
        ep = getenv("ROE_LLM_TEMPERATURE");
        if (ep && ep[0]) temp = strtod(ep, NULL);
        for (i = 0; prompt[i] && j + 2 < sizeof pq; i++) {
            if (prompt[i] == '"' || prompt[i] == '\\') pq[j++] = '\\';
            pq[j++] = prompt[i];
        }
        pq[j] = 0;
        /* Ollama /api/generate — include think for cloud V4 models */
        if ((size_t)snprintf(body, sizeof body,
                             "{\"model\":\"%s\",\"prompt\":\"%s%s\",\"stream\":false,"
                             "\"think\":%s,"
                             "\"options\":{\"num_predict\":%d,\"temperature\":%.2f}}",
                             N->llm_model, sys_prompt, pq, think_json, npred, temp) >=
            sizeof body) {
            snprintf(N->last_err, sizeof N->last_err, "llm body too large");
            return -5;
        }
        rc = http_post_json(N->llm_url, body, N->timeout_ms, &mb);
    }

    if (rc != 0 || !mb.data) {
        snprintf(N->last_err, sizeof N->last_err, "llm http fail rc=%d", rc);
        N->n_llm_fail++;
        free(mb.data);
        return -3;
    }
    if (json_string_field(mb.data, "response", answer, answer_cap) != 0) {
        /* OpenAI-compatible / chat message.content */
        if (json_string_field(mb.data, "content", answer, answer_cap) != 0) {
            snprintf(N->last_err, sizeof N->last_err, "llm parse fail");
            N->n_llm_fail++;
            free(mb.data);
            return -4;
        }
    }
    {
        /* trim whitespace */
        char *a = answer;
        size_t n;
        while (*a == ' ' || *a == '\n' || *a == '\r' || *a == '\t') a++;
        if (a != answer) memmove(answer, a, strlen(a) + 1);
        n = strlen(answer);
        while (n > 0 && (answer[n - 1] == ' ' || answer[n - 1] == '\n' ||
                         answer[n - 1] == '\r'))
            answer[--n] = 0;
    }
    if (tokens_est) *tokens_est = (uint64_t)(plen / 4 + strlen(answer) / 4 + 16);
    N->tokens_prompt_est += (uint64_t)(plen / 4 + 8);
    N->tokens_completion_est += (uint64_t)(strlen(answer) / 4 + 1);
    N->n_llm_ok++;
    free(mb.data);
    return answer[0] ? 0 : -5;
}

int roe_net_lookup(RoeNet *N, const char *query, char *snippet, size_t cap,
                   uint64_t *tokens_est) {
    char enc[512], url[1024];
    struct MemBuf mb;
    char abstract[1024], answer[1024];
    int rc;

    if (tokens_est) *tokens_est = 0;
    if (!N || !query || !snippet || !cap) return -1;
    snippet[0] = 0;
    if (!N->enable_lookup) {
        snprintf(N->last_err, sizeof N->last_err, "lookup disabled");
        return -2;
    }
    url_encode(query, enc, sizeof enc);
    if (strstr(N->lookup_url, "%s"))
        snprintf(url, sizeof url, N->lookup_url, enc);
    else
        snprintf(url, sizeof url, "%s%s", N->lookup_url, enc);

    rc = http_get(url, N->timeout_ms > 20000 ? 20000 : N->timeout_ms, &mb);
    if (rc != 0 || !mb.data) {
        snprintf(N->last_err, sizeof N->last_err, "lookup http fail");
        N->n_lookup_fail++;
        free(mb.data);
        return -3;
    }
    abstract[0] = answer[0] = 0;
    (void)json_string_field(mb.data, "AbstractText", abstract, sizeof abstract);
    (void)json_string_field(mb.data, "Answer", answer, sizeof answer);
    if (abstract[0])
        snprintf(snippet, cap, "%s", abstract);
    else if (answer[0])
        snprintf(snippet, cap, "%s", answer);
    else {
        /* RelatedTopics first Text */
        const char *t = strstr(mb.data, "\"Text\":\"");
        if (t) {
            t += 8;
            size_t i = 0;
            while (t[i] && t[i] != '"' && i + 1 < cap) {
                snippet[i] = t[i];
                i++;
            }
            snippet[i] = 0;
        }
    }
    free(mb.data);
    if (!snippet[0]) {
        snprintf(N->last_err, sizeof N->last_err, "lookup empty");
        N->n_lookup_fail++;
        return -4;
    }
    if (tokens_est) *tokens_est = (uint64_t)(strlen(query) / 4 + strlen(snippet) / 4 + 4);
    N->n_lookup_ok++;
    return 0;
}
