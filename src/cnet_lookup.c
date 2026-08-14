#include "cnet_lookup.h"

#include "cce/cce_campaign_provenance.h"

#include <ctype.h>
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *data;
    size_t length;
} LookupBody;

static size_t write_body(char *ptr, size_t size, size_t nmemb, void *userdata) {
    LookupBody *body = (LookupBody *)userdata;
    size_t add = size * nmemb;
    char *grown;
    if (body == NULL || ptr == NULL) return 0;
    if (body->length + add > CNET_LOOKUP_BODY_MAX) return 0;
    grown = (char *)realloc(body->data, body->length + add + 1u);
    if (grown == NULL) return 0;
    body->data = grown;
    memcpy(body->data + body->length, ptr, add);
    body->length += add;
    body->data[body->length] = '\0';
    return add;
}

static void copy_text(char *dst, size_t cap, const char *src) {
    size_t n;
    if (dst == NULL || cap == 0) return;
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    n = strlen(src);
    if (n >= cap) n = cap - 1u;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static int scheme_allowed(const char *url) {
    return url != NULL &&
           (strncmp(url, "http://", 7) == 0 ||
            strncmp(url, "https://", 8) == 0 ||
            strncmp(url, "file://", 7) == 0);
}

static void extract_host(const char *url, char *host, size_t cap) {
    const char *start, *end;
    if (strncmp(url, "file://", 7) == 0) {
        copy_text(host, cap, "local-file");
        return;
    }
    start = strstr(url, "://");
    if (start == NULL) {
        copy_text(host, cap, "unknown");
        return;
    }
    start += 3;
    end = start;
    while (*end != '\0' && *end != '/' && *end != ':' && *end != '?') ++end;
    {
        size_t n = (size_t)(end - start);
        if (n >= cap) n = cap - 1u;
        memcpy(host, start, n);
        host[n] = '\0';
    }
    if (host[0] == '\0') copy_text(host, cap, "unknown");
}

static int bind_integer(const char *body, char *value, size_t cap) {
    const char *cursor;
    unsigned long number;
    char *end = NULL;
    if (body == NULL) return 0;
    for (cursor = body; *cursor != '\0'; ++cursor) {
        if (!isdigit((unsigned char)*cursor)) continue;
        number = strtoul(cursor, &end, 10);
        if (end == cursor) continue;
        if (number > 4294967295UL) return 0;
        snprintf(value, cap, "%lu", number);
        return 1;
    }
    return 0;
}

static int bind_token(const char *body, char *value, size_t cap) {
    const char *cursor, *start;
    size_t n;
    if (body == NULL) return 0;
    for (cursor = body; *cursor != '\0'; ++cursor) {
        if (!(isalnum((unsigned char)*cursor) || *cursor == '_' ||
              *cursor == '.' || *cursor == '-'))
            continue;
        start = cursor;
        while (isalnum((unsigned char)*cursor) || *cursor == '_' ||
               *cursor == '.' || *cursor == '-')
            ++cursor;
        n = (size_t)(cursor - start);
        if (n == 0) continue;
        if (n >= cap) n = cap - 1u;
        memcpy(value, start, n);
        value[n] = '\0';
        return 1;
    }
    return 0;
}

static int bind_year(const char *body, char *value, size_t cap) {
    const char *cursor;
    unsigned year;
    if (body == NULL) return 0;
    for (cursor = body; *cursor != '\0'; ++cursor) {
        if (cursor[1] == '\0' || cursor[2] == '\0' || cursor[3] == '\0') break;
        if (!isdigit((unsigned char)cursor[0]) ||
            !isdigit((unsigned char)cursor[1]) ||
            !isdigit((unsigned char)cursor[2]) ||
            !isdigit((unsigned char)cursor[3]))
            continue;
        if (cursor > body && isdigit((unsigned char)cursor[-1])) continue;
        if (isdigit((unsigned char)cursor[4])) continue;
        year = (unsigned)(cursor[0] - '0') * 1000u +
               (unsigned)(cursor[1] - '0') * 100u +
               (unsigned)(cursor[2] - '0') * 10u +
               (unsigned)(cursor[3] - '0');
        if (year < 1000u || year > 2099u) continue;
        snprintf(value, cap, "%u", year);
        return 1;
    }
    return 0;
}

static int bind_line(const char *body, char *value, size_t cap) {
    const char *start, *end;
    size_t n;
    if (body == NULL) return 0;
    start = body;
    while (*start == ' ' || *start == '\t' || *start == '\n' || *start == '\r')
        ++start;
    if (*start == '\0') return 0;
    end = start;
    while (*end != '\0' && *end != '\n' && *end != '\r') ++end;
    n = (size_t)(end - start);
    while (n > 0 && (start[n - 1u] == ' ' || start[n - 1u] == '\t')) --n;
    if (n == 0) return 0;
    if (n >= cap) n = cap - 1u;
    memcpy(value, start, n);
    value[n] = '\0';
    return 1;
}

static void fill_snippet(const char *body, char *snippet, size_t cap) {
    size_t i, o = 0;
    if (body == NULL || snippet == NULL || cap == 0) return;
    for (i = 0; body[i] != '\0' && o + 1u < cap && o < 120u; ++i) {
        char c = body[i];
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
        if (c == ' ' && (o == 0 || snippet[o - 1u] == ' ')) continue;
        snippet[o++] = c;
    }
    snippet[o] = '\0';
}

static int abstain(CnetLookupReport *report, const char *reason) {
    if (report != NULL) {
        report->bound = 0;
        copy_text(report->refusal, sizeof report->refusal, reason);
        report->value[0] = '\0';
    }
    return 1;
}

int cnet_lookup_execute(const char *url, CnetLookupBind bind,
                        CnetLookupReport *report) {
    LookupBody body;
    CURL *curl;
    CURLcode rc;
    long status = 0;
    int bound = 0;
    if (report == NULL) return -1;
    memset(report, 0, sizeof *report);
    memset(&body, 0, sizeof body);
    if (url == NULL || url[0] == '\0') return abstain(report, "empty_url");
    copy_text(report->url, sizeof report->url, url);
    extract_host(url, report->host, sizeof report->host);
    if (!scheme_allowed(url)) return abstain(report, "scheme");
    curl = curl_easy_init();
    if (curl == NULL) return -1;
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 10000L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 5000L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "cnet-web-lookup-v1");
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_UNRESTRICTED_AUTH, 0L);
    curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE,
                     (curl_off_t)CNET_LOOKUP_BODY_MAX);
    if (curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https,file") !=
            CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR,
                         "http,https,file") != CURLE_OK) {
        curl_easy_cleanup(curl);
        return abstain(report, "scheme");
    }
    rc = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    report->http_status = status;
    if (rc != CURLE_OK || body.data == NULL) {
        free(body.data);
        return abstain(report, "fetch");
    }
    report->bytes = body.length;
    if (strncmp(url, "file://", 7) != 0 && status != 200) {
        free(body.data);
        return abstain(report, "http_status");
    }
    if (cce_sha256_bytes_hex(body.data, body.length, report->sha256) != 0) {
        free(body.data);
        return -1;
    }
    fill_snippet(body.data, report->snippet, sizeof report->snippet);
    if (bind == CNET_LOOKUP_BIND_INTEGER)
        bound = bind_integer(body.data, report->value, sizeof report->value);
    else if (bind == CNET_LOOKUP_BIND_TOKEN)
        bound = bind_token(body.data, report->value, sizeof report->value);
    else if (bind == CNET_LOOKUP_BIND_LINE)
        bound = bind_line(body.data, report->value, sizeof report->value);
    else if (bind == CNET_LOOKUP_BIND_YEAR)
        bound = bind_year(body.data, report->value, sizeof report->value);
    free(body.data);
    if (!bound) return abstain(report, "unbindable");
    report->bound = 1;
    report->refusal[0] = '\0';
    return 0;
}

int cnet_lookup_speak(const CnetLookupReport *report, char *out, size_t cap) {
    int written;
    if (out == NULL || cap == 0) return -1;
    out[0] = '\0';
    if (report == NULL || !report->bound || report->value[0] == '\0' ||
        report->sha256[0] == '\0')
        return -1;
    written = snprintf(out, cap,
                       "Marble reports %s from %s (%s, sha256=%s).",
                       report->value, report->host[0] ? report->host : "source",
                       CNET_LOOKUP_CONTRACT, report->sha256);
    if (written < 0 || (size_t)written >= cap) {
        out[0] = '\0';
        return -1;
    }
    return 0;
}
