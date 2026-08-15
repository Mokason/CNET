#include "cnet_lookup.h"

#include "cce/cce_campaign_provenance.h"

#include <ctype.h>
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#endif

typedef struct {
    char *data;
    size_t length;
    int blocked_ip;
    int blocked_redirect;
} LookupXfer;

static size_t write_body(char *ptr, size_t size, size_t nmemb, void *userdata) {
    LookupXfer *xfer = (LookupXfer *)userdata;
    size_t add = size * nmemb;
    char *grown;
    if (xfer == NULL || ptr == NULL) return 0;
    if (xfer->length + add > CNET_LOOKUP_BODY_MAX) return 0;
    grown = (char *)realloc(xfer->data, xfer->length + add + 1u);
    if (grown == NULL) return 0;
    xfer->data = grown;
    memcpy(xfer->data + xfer->length, ptr, add);
    xfer->length += add;
    xfer->data[xfer->length] = '\0';
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

static void lower_copy(char *dst, size_t cap, const char *src) {
    size_t i = 0;
    if (dst == NULL || cap == 0) return;
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    while (src[i] != '\0' && i + 1u < cap) {
        dst[i] = (char)tolower((unsigned char)src[i]);
        ++i;
    }
    dst[i] = '\0';
}

static int scheme_allowed(const char *url, unsigned flags) {
    if (url == NULL) return 0;
    if (strncmp(url, "http://", 7) == 0 || strncmp(url, "https://", 8) == 0)
        return 1;
    if ((flags & CNET_LOOKUP_F_ALLOW_FILE) && strncmp(url, "file://", 7) == 0)
        return 1;
    return 0;
}

static int parse_ipv4(const char *s, unsigned octets[4]) {
    unsigned a, b, c, d;
    int n;
    char extra;
    if (s == NULL || octets == NULL) return 0;
    n = sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra);
    if (n != 4) return 0;
    if (a > 255u || b > 255u || c > 255u || d > 255u) return 0;
    octets[0] = a;
    octets[1] = b;
    octets[2] = c;
    octets[3] = d;
    return 1;
}

static int ipv4_blocked(const unsigned octets[4]) {
    if (octets == NULL) return 1;
    if (octets[0] == 127u) return 1;                 /* 127.0.0.0/8 */
    if (octets[0] == 169u && octets[1] == 254u) return 1; /* 169.254.0.0/16 */
    return 0;
}

static int ipv6_blocked(const struct in6_addr *addr) {
    const unsigned char *b;
    unsigned mapped[4];
    int i, zero = 1;
    if (addr == NULL) return 1;
    b = addr->s6_addr;
    /* ::1 */
    for (i = 0; i < 15; ++i)
        if (b[i] != 0) zero = 0;
    if (zero && b[15] == 1) return 1;
    /* fe80::/10 */
    if (b[0] == 0xfeu && (b[1] & 0xc0u) == 0x80u) return 1;
    /* IPv4-mapped ::ffff:a.b.c.d */
    for (i = 0; i < 10; ++i)
        if (b[i] != 0) return 0;
    if (b[10] == 0xffu && b[11] == 0xffu) {
        mapped[0] = b[12];
        mapped[1] = b[13];
        mapped[2] = b[14];
        mapped[3] = b[15];
        return ipv4_blocked(mapped);
    }
    return 0;
}

int cnet_lookup_host_allowed(const char *host) {
    char norm[128];
    size_t n;
    unsigned octets[4];
    struct in6_addr addr6;
    const char *check;
    char *pct;
    if (host == NULL || host[0] == '\0') return 0;
    lower_copy(norm, sizeof norm, host);
    n = strlen(norm);
    while (n > 0 && norm[n - 1u] == '.') {
        norm[n - 1u] = '\0';
        --n;
    }
    if (n == 0) return 0;
    if (strcmp(norm, "localhost") == 0) return 0;
    if (strcmp(norm, "metadata.google.internal") == 0) return 0;
    check = norm;
    pct = strchr(norm, '%');
    if (pct != NULL) *pct = '\0';
    if (parse_ipv4(check, octets)) return ipv4_blocked(octets) ? 0 : 1;
    if (inet_pton(AF_INET6, check, &addr6) == 1)
        return ipv6_blocked(&addr6) ? 0 : 1;
    return 1;
}

static void extract_host(const char *url, char *host, size_t cap) {
    const char *start, *end, *p, *at;
    if (host == NULL || cap == 0) return;
    host[0] = '\0';
    if (url == NULL) return;
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
    at = NULL;
    for (p = start; *p != '\0' && *p != '/' && *p != '?' && *p != '#'; ++p) {
        if (*p == '@') at = p;
    }
    if (at != NULL) start = at + 1;
    if (*start == '[') {
        ++start;
        end = start;
        while (*end != '\0' && *end != ']') ++end;
    } else {
        end = start;
        while (*end != '\0' && *end != '/' && *end != ':' && *end != '?' &&
               *end != '#')
            ++end;
    }
    {
        size_t n = (size_t)(end - start);
        if (n >= cap) n = cap - 1u;
        memcpy(host, start, n);
        host[n] = '\0';
    }
    if (host[0] == '\0') copy_text(host, cap, "unknown");
}

int cnet_lookup_url_allowed(const char *url, unsigned flags) {
    char host[128];
    if (!scheme_allowed(url, flags)) return 0;
    if (strncmp(url, "file://", 7) == 0) return 1;
    extract_host(url, host, sizeof host);
    if (strcmp(host, "unknown") == 0 || host[0] == '\0') return 0;
    return cnet_lookup_host_allowed(host);
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

static int prerq_guard(void *clientp, char *conn_primary_ip, char *conn_local_ip,
                       int conn_primary_port, int conn_local_port) {
    LookupXfer *xfer = (LookupXfer *)clientp;
    (void)conn_local_ip;
    (void)conn_primary_port;
    (void)conn_local_port;
    if (conn_primary_ip == NULL || conn_primary_ip[0] == '\0')
        return CURL_PREREQFUNC_OK;
    if (!cnet_lookup_host_allowed(conn_primary_ip)) {
        if (xfer != NULL) xfer->blocked_ip = 1;
        return CURL_PREREQFUNC_ABORT;
    }
    return CURL_PREREQFUNC_OK;
}

static int header_is_location(const char *buf, size_t n) {
    static const char key[] = "location:";
    size_t i;
    if (buf == NULL || n < 9) return 0;
    for (i = 0; i < 9; ++i) {
        if (tolower((unsigned char)buf[i]) != (unsigned char)key[i]) return 0;
    }
    return 1;
}

static size_t on_header(char *buf, size_t size, size_t nitems, void *userdata) {
    LookupXfer *xfer = (LookupXfer *)userdata;
    size_t n = size * nitems;
    char loc[512];
    const char *p;
    size_t i;
    if (xfer == NULL || buf == NULL || n < 9) return n;
    if (!header_is_location(buf, n)) return n;
    p = buf + 9;
    while (*p == ' ' || *p == '\t') ++p;
    i = 0;
    while (p[i] != '\0' && p[i] != '\r' && p[i] != '\n' && i + 1u < sizeof loc)
        ++i;
    memcpy(loc, p, i);
    loc[i] = '\0';
    if (loc[0] == '\0') return n;
    /* Relative Location stays on the already-checked host. */
    if (strstr(loc, "://") == NULL) return n;
    if (!cnet_lookup_url_allowed(loc, 0)) {
        xfer->blocked_redirect = 1;
        return 0;
    }
    return n;
}

int cnet_lookup_execute_flags(const char *url, CnetLookupBind bind,
                              unsigned flags, CnetLookupReport *report) {
    LookupXfer xfer;
    CURL *curl;
    CURLcode rc;
    long status = 0;
    int bound = 0;
    const char *protocols;
    if (report == NULL) return -1;
    memset(report, 0, sizeof *report);
    memset(&xfer, 0, sizeof xfer);
    if (url == NULL || url[0] == '\0') return abstain(report, "empty_url");
    copy_text(report->url, sizeof report->url, url);
    extract_host(url, report->host, sizeof report->host);
    if (!scheme_allowed(url, flags)) return abstain(report, "scheme");
    if (strncmp(url, "file://", 7) != 0 &&
        !cnet_lookup_host_allowed(report->host))
        return abstain(report, "host");
    curl = curl_easy_init();
    if (curl == NULL) return -1;
    protocols = (flags & CNET_LOOKUP_F_ALLOW_FILE) ? "http,https,file"
                                                   : "http,https";
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &xfer);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, on_header);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &xfer);
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
    if (strncmp(url, "file://", 7) != 0) {
        curl_easy_setopt(curl, CURLOPT_PREREQFUNCTION, prerq_guard);
        curl_easy_setopt(curl, CURLOPT_PREREQDATA, &xfer);
    }
    if (curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, protocols) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https") !=
            CURLE_OK) {
        curl_easy_cleanup(curl);
        return abstain(report, "scheme");
    }
    rc = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    report->http_status = status;
    if (xfer.blocked_ip || xfer.blocked_redirect) {
        free(xfer.data);
        return abstain(report, "host");
    }
    if (rc != CURLE_OK || xfer.data == NULL) {
        free(xfer.data);
        return abstain(report, "fetch");
    }
    report->bytes = xfer.length;
    if (strncmp(url, "file://", 7) != 0 && status != 200) {
        free(xfer.data);
        return abstain(report, "http_status");
    }
    if (cce_sha256_bytes_hex(xfer.data, xfer.length, report->sha256) != 0) {
        free(xfer.data);
        return -1;
    }
    fill_snippet(xfer.data, report->snippet, sizeof report->snippet);
    if (bind == CNET_LOOKUP_BIND_INTEGER)
        bound = bind_integer(xfer.data, report->value, sizeof report->value);
    else if (bind == CNET_LOOKUP_BIND_TOKEN)
        bound = bind_token(xfer.data, report->value, sizeof report->value);
    else if (bind == CNET_LOOKUP_BIND_LINE)
        bound = bind_line(xfer.data, report->value, sizeof report->value);
    else if (bind == CNET_LOOKUP_BIND_YEAR)
        bound = bind_year(xfer.data, report->value, sizeof report->value);
    free(xfer.data);
    if (!bound) return abstain(report, "unbindable");
    report->bound = 1;
    report->refusal[0] = '\0';
    return 0;
}

int cnet_lookup_execute(const char *url, CnetLookupBind bind,
                        CnetLookupReport *report) {
    return cnet_lookup_execute_flags(url, bind, 0, report);
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
