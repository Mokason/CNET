#include "../../include/cnet_mcp_client.h"
#include "../../include/cnet_json_escape.h"
#include "../../include/cnet_platform.h"
#include "../../include/cnet_mcp_evidence_internal.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#if CNET_HAVE_CURL
#include <curl/curl.h>
#endif

static int ci_has(const char *hay, const char *needle) {
    size_t n, m, i, j;
    if (!hay || !needle || !needle[0]) return 0;
    n = strlen(hay);
    m = strlen(needle);
    if (m > n) return 0;
    for (i = 0; i + m <= n; i++) {
        for (j = 0; j < m; j++) {
            if (tolower((unsigned char)hay[i + j]) != tolower((unsigned char)needle[j]))
                break;
        }
        if (j == m) return 1;
    }
    return 0;
}

static const char *after_ci(const char *hay, const char *prefix) {
    size_t n, m, i, j;
    if (!hay || !prefix || !prefix[0]) return NULL;
    n = strlen(hay);
    m = strlen(prefix);
    if (m > n) return NULL;
    for (i = 0; i + m <= n; i++) {
        for (j = 0; j < m; j++) {
            if (tolower((unsigned char)hay[i + j]) != tolower((unsigned char)prefix[j]))
                break;
        }
        if (j == m) {
            const char *p = hay + i + m;
            while (*p == ' ' || *p == '\t' || *p == ':') p++;
            return *p ? p : NULL;
        }
    }
    return NULL;
}

int cnet_mcp_client_enabled(void) {
    const char *e = getenv("CNET_MCP_CLIENT");
    if (e && (e[0] == '0' || e[0] == 'n' || e[0] == 'N' || e[0] == 'f' || e[0] == 'F'))
        return 0;
    return 1;
}

int cnet_mcp_sock_path(char *out, size_t cap) {
    const char *e, *xd;
    if (!out || cap < 8) return -1;
    e = getenv("CNET_MCP_SHARED_SOCK");
    if (e && e[0]) {
        snprintf(out, cap, "%s", e);
        return 0;
    }
    xd = getenv("XDG_RUNTIME_DIR");
    if (xd && xd[0]) {
        snprintf(out, cap, "%s/cnet/mcp-shared.sock", xd);
        return 0;
    }
    snprintf(out, cap, "/run/user/%d/cnet/mcp-shared.sock", (int)getuid());
    return 0;
}

static int json_unescape_into(const char *src, size_t n, char *dst, size_t cap) {
    size_t i = 0, o = 0;
    if (!dst || cap == 0) return 0;
    while (i < n && o + 1 < cap) {
        if (src[i] == '\\' && i + 1 < n) {
            char c = src[i + 1];
            if (c == 'n') dst[o++] = '\n';
            else if (c == 't') dst[o++] = '\t';
            else if (c == 'r') dst[o++] = '\r';
            else dst[o++] = c;
            i += 2;
            continue;
        }
        dst[o++] = src[i++];
    }
    dst[o] = 0;
    return (int)o;
}

static int extract_mcp_text(const char *resp, char *out, size_t cap) {
    const char *p, *end;
    if (!resp || !out || cap < 8) return 0;
    p = strstr(resp, "\"text\":\"");
    if (!p) p = strstr(resp, "\"text\": \"");
    if (!p) {
        snprintf(out, cap, "%.700s", resp);
        return (int)strlen(out);
    }
    p = strchr(p, ':');
    if (!p) return 0;
    p++;
    while (*p == ' ') p++;
    if (*p != '"') return 0;
    p++;
    end = p;
    while (*end) {
        if (*end == '\\' && end[1]) {
            end += 2;
            continue;
        }
        if (*end == '"') break;
        end++;
    }
    return json_unescape_into(p, (size_t)(end - p), out, cap);
}

#define CNET_MCP_TIMEOUT_DEFAULT_MS 2000
#define CNET_MCP_TIMEOUT_MIN_MS 50
#define CNET_MCP_TIMEOUT_MAX_MS 60000

static int mcp_timeout_ms(void) {
    const char *configured = getenv("CNET_MCP_TIMEOUT_MS");
    char *end = NULL;
    long value;
    if (!configured || !configured[0]) return CNET_MCP_TIMEOUT_DEFAULT_MS;
    errno = 0;
    value = strtol(configured, &end, 10);
    if (errno || !end || *end != '\0') return CNET_MCP_TIMEOUT_DEFAULT_MS;
    if (value < CNET_MCP_TIMEOUT_MIN_MS) return CNET_MCP_TIMEOUT_MIN_MS;
    if (value > CNET_MCP_TIMEOUT_MAX_MS) return CNET_MCP_TIMEOUT_MAX_MS;
    return (int)value;
}

static int64_t mcp_monotonic_ms(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return -1;
    return (int64_t)now.tv_sec * 1000 + (int64_t)now.tv_nsec / 1000000;
}

static int mcp_wait(int fd, short events, int64_t deadline) {
    struct pollfd descriptor;
    int64_t now = mcp_monotonic_ms();
    int64_t remaining;
    int timeout;
    int ready;
    if (now < 0) return 0;
    remaining = deadline - now;
    if (remaining <= 0) return 0;
    timeout = remaining > INT_MAX ? INT_MAX : (int)remaining;
    descriptor.fd = fd;
    descriptor.events = events;
    descriptor.revents = 0;
    ready = poll(&descriptor, 1, timeout);
    if (ready <= 0 || (descriptor.revents & (POLLERR | POLLNVAL))) return 0;
    if (descriptor.revents & events) return 1;
    return events == POLLIN && (descriptor.revents & POLLHUP);
}

static int mcp_connect(int fd, const struct sockaddr_un *address,
                       int64_t deadline) {
    int result = connect(fd, (const struct sockaddr *)address, sizeof *address);
    if (result == 0) return 1;
    if (errno != EINPROGRESS || !mcp_wait(fd, POLLOUT, deadline)) return 0;
    {
        int socket_error = 0;
        socklen_t size = sizeof socket_error;
        return getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &size) == 0 &&
               socket_error == 0;
    }
}

static int mcp_write_all(int fd, const char *data, size_t size,
                         int64_t deadline) {
    size_t written = 0;
    while (written < size) {
        /* A closed peer is an ordinary refusal, never a process-wide SIGPIPE.
         * Keep the signal policy socket-local rather than changing handlers. */
        ssize_t count = send(fd, data + written, size - written, MSG_NOSIGNAL);
        if (count > 0) {
            written += (size_t)count;
            continue;
        }
        if (count < 0 && errno == EINTR) continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) &&
            mcp_wait(fd, POLLOUT, deadline))
            continue;
        return 0;
    }
    return 1;
}

static int mcp_read_line(int fd, char *out, size_t cap, int64_t deadline) {
    size_t used = 0;
    out[0] = '\0';
    while (used + 1 < cap) {
        ssize_t count = read(fd, out + used, cap - used - 1);
        if (count > 0) {
            char *newline;
            if (memchr(out + used, '\0', (size_t)count)) return 0;
            used += (size_t)count;
            out[used] = '\0';
            newline = memchr(out, '\n', used);
            if (newline) {
                newline[1] = '\0';
                return 1;
            }
            continue;
        }
        if (count < 0 && errno == EINTR) continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) &&
            mcp_wait(fd, POLLIN, deadline))
            continue;
        break;
    }
    out[0] = '\0';
    return 0;
}

static int mcp_rpc(const char *req, char *out, size_t cap) {
    char path[512];
    int fd;
    int flags;
    int ok = 0;
    int timeout;
    int64_t start;
    int64_t deadline;
    struct sockaddr_un addr;
    if (!req || !out || cap < 2 ||
        cnet_mcp_sock_path(path, sizeof path) != 0)
        return 0;
    out[0] = '\0';
    timeout = mcp_timeout_ms();
    start = mcp_monotonic_ms();
    if (start < 0 || start > INT64_MAX - timeout) return 0;
    deadline = start + timeout;
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return 0;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof addr.sun_path) {
        goto done;
    }
    memcpy(addr.sun_path, path, strlen(path) + 1);
    flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) goto done;
    if (!mcp_connect(fd, &addr, deadline)) goto done;
    if (!mcp_write_all(fd, req, strlen(req), deadline) ||
        !mcp_write_all(fd, "\n", 1, deadline))
        goto done;
    ok = mcp_read_line(fd, out, cap, deadline);
done:
    close(fd);
    if (!ok) out[0] = '\0';
    return ok;
}

int cnet_mcp_call(const char *tool, const char *args_json, char *out, size_t cap) {
    char req[1600], raw[4096], esc[96];
    const char *args = (args_json && args_json[0]) ? args_json : "{}";
    if (!cnet_mcp_client_enabled() || !tool || !tool[0] || !out || cap < 16)
        return 0;
    if (cnet_json_escape(tool, esc, sizeof esc) != 0) return 0;
    if (snprintf(req, sizeof req,
                 "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\","
                 "\"params\":{\"name\":\"%s\",\"arguments\":%s}}",
                 esc, args) >= (int)sizeof req)
        return 0;
    if (!mcp_rpc(req, raw, sizeof raw)) return 0;
    if (strstr(raw, "\"error\"")) return 0;
    return extract_mcp_text(raw, out, cap);
}

int cnet_mcp_read_call(const char *tool, const char *args_json, char *out, size_t cap) {
    char req[16384];
    char *raw;
    const char *enabled = getenv("CNET_MCP_READ_ENABLED");
    if (out && cap) out[0] = 0;
    if (!out || cap < 2 || !enabled || strcmp(enabled, "1") ||
        !cnet_mcp_client_enabled() || !read_arguments(tool, args_json)) return 0;
    int n = snprintf(req, sizeof req,
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"%s\",\"arguments\":%s}}", tool, args_json);
    if (n < 0 || (size_t)n >= sizeof req) return 0;
    raw = calloc(262144, 1);
    if (!raw) return 0;
    int ok = mcp_rpc(req, raw, 262144) && read_response(raw, tool, out, cap);
    free(raw);
    if (!ok) out[0] = 0;
    return ok ? (int)strlen(out) : 0;
}

int cnet_mcp_read_summary(const char *evidence, char *out, size_t cap) {
    ReadSource source;
    if (out && cap) out[0] = 0;
    if (!out || cap < 128 || !read_evidence(evidence, NULL, &source)) return 0;
    /* Escape/control-free display; data is never routed back through the tool
     * selector. UTF-8 is cut only at a character boundary. */
    for (size_t i = 0; source.text[i]; i++) {
        if ((unsigned char)source.text[i] < 32 || source.text[i] == 127)
            source.text[i] = ' ';
        if ((unsigned char)source.text[i] == 0xc2 &&
            (unsigned char)source.text[i+1] >= 0x80 && (unsigned char)source.text[i+1] <= 0x9f)
            source.text[i] = source.text[i+1] = ' ';
        if ((unsigned char)source.text[i] == 0xe2 && (unsigned char)source.text[i+1] == 0x80 &&
            ((unsigned char)source.text[i+2] == 0xa8 || (unsigned char)source.text[i+2] == 0xa9))
            source.text[i] = source.text[i+1] = source.text[i+2] = ' ';
    }
    /* One line even when used inside cnetd's line-oriented text protocol. */
    int n = snprintf(out, cap, "[Untrusted web evidence; not CERT] %s | ", source.url);
    if (n < 0 || (size_t)n + 16 >= cap) { out[0] = 0; return 0; }
    size_t room = cap - (size_t)n - 1, len = strlen(source.text);
    int clipped = len > room;
    if (clipped) {
        len = room - 4;
        while (len && ((unsigned char)source.text[len] & 0xc0) == 0x80) len--;
    }
    memcpy(out + n, source.text, len);
    out[n + len] = 0;
    if (clipped) strcat(out, "...");
    return (int)strlen(out);
}

static void clip_speak(const char *prefix, const char *body, char *out, size_t cap) {
    char flat[900];
    size_t i, o = 0;
    for (i = 0; body && body[i] && o + 1 < sizeof flat; i++) {
        char c = body[i];
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
        if (c == ' ' && o && flat[o - 1] == ' ') continue;
        flat[o++] = c;
    }
    flat[o] = 0;
    if (strlen(flat) > 520) {
        flat[517] = '.';
        flat[518] = '.';
        flat[519] = '.';
        flat[520] = 0;
    }
    snprintf(out, cap, "%s%s", prefix, flat);
}

#if CNET_HAVE_CURL
struct LiveBuf { char *data; size_t len; };

static size_t live_write(char *ptr, size_t size, size_t nmemb, void *ud) {
    struct LiveBuf *m = (struct LiveBuf *)ud;
    size_t want = size * nmemb;
    size_t n = want;
    char *p;
    if (!m || !ptr || want == 0) return 0;
    if (m->len >= 1000000) return want;
    if (m->len + n > 1000000) n = 1000000 - m->len;
    if (n == 0) return want;
    p = (char *)realloc(m->data, m->len + n + 1);
    if (!p) return 0;
    m->data = p;
    memcpy(m->data + m->len, ptr, n);
    m->len += n;
    m->data[m->len] = 0;
    return want;
}

static int live_http_get(const char *url, char **out) {
    CURL *curl;
    CURLcode rc;
    long code = 0;
    struct LiveBuf m;
    memset(&m, 0, sizeof m);
    *out = NULL;
    curl = curl_easy_init();
    if (!curl) return -1;
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, live_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &m);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 12L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "cnetd-live-lookup/1.0");
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    rc = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK || code >= 400 || !m.data) {
        free(m.data);
        return -1;
    }
    *out = m.data;
    return 0;
}

static int json_quoted_after(const char *j, const char *key, char *out, size_t cap) {
    const char *p, *end;
    char pat[80];
    if (!j || !key || !out || cap < 2) return 0;
    snprintf(pat, sizeof pat, "\"%s\"", key);
    p = strstr(j, pat);
    if (!p) return 0;
    p = strchr(p + strlen(pat), ':');
    if (!p) return 0;
    p++;
    while (*p == ' ') p++;
    if (*p != '"') return 0;
    p++;
    end = p;
    while (*end && *end != '"') {
        if (*end == '\\' && end[1]) end += 2;
        else end++;
    }
    return json_unescape_into(p, (size_t)(end - p), out, cap) > 0;
}

static void weather_place(const char *q, char *out, size_t cap) {
    const char *p = after_ci(q, " in ");
    size_t o = 0;
    if (!p) {
        /* last non-stop word */
        const char *w = q + strlen(q);
        while (w > q && !isalnum((unsigned char)w[-1])) w--;
        p = w;
        while (p > q && (isalnum((unsigned char)p[-1]) || p[-1] == '-' || p[-1] == ' '))
            p--;
        while (*p == ' ') p++;
    }
    while (*p && o + 1 < cap) {
        unsigned char c = (unsigned char)*p++;
        if (c == '?' || c == '.' || c == ',') break;
        if (!(isalnum(c) || c == ' ' || c == '-' || c == '+')) continue;
        if (c == ' ' && o && out[o - 1] == ' ') continue;
        out[o++] = (char)c;
    }
    while (o && out[o - 1] == ' ') o--;
    out[o] = 0;
    /* drop trailing filler words */
    if (ci_has(out, "tomorrow") || ci_has(out, "today") || ci_has(out, "weather") ||
        ci_has(out, "please") || ci_has(out, "google")) {
        /* if the whole thing is filler, keep last token only */
        char *sp = strrchr(out, ' ');
        if (sp && sp[1] && !ci_has(sp + 1, "tomorrow") && !ci_has(sp + 1, "today"))
            memmove(out, sp + 1, strlen(sp + 1) + 1);
    }
}

static int url_enc(const char *in, char *out, size_t cap) {
    static const char *hex = "0123456789ABCDEF";
    size_t o = 0;
    for (; in && *in; in++) {
        unsigned char c = (unsigned char)*in;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            if (o + 1 >= cap) return -1;
            out[o++] = (char)c;
        } else if (c == ' ') {
            if (o + 1 >= cap) return -1;
            out[o++] = '+';
        } else {
            if (o + 3 >= cap) return -1;
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 15];
        }
    }
    if (o >= cap) return -1;
    out[o] = 0;
    return 0;
}

static int live_weather_ask(const char *q, char *out, size_t cap) {
    char place[80], enc[160], url[320], nowt[16], nowd[80], d0[24], d1[24];
    char mn0[8], mx0[8], mn1[8], mx1[8], noon0[80], noon1[80];
    char *raw = NULL;
    const char *blk;
    if (!ci_has(q, "weather") && !ci_has(q, "forecast") && !ci_has(q, "wttr"))
        return 0;
    weather_place(q, place, sizeof place);
    if (strlen(place) < 2) snprintf(place, sizeof place, "Riga");
    if (url_enc(place, enc, sizeof enc) != 0) return 0;
    snprintf(url, sizeof url, "https://wttr.in/%s?format=j1", enc);
    if (live_http_get(url, &raw) != 0 || !raw) {
        snprintf(url, sizeof url, "https://wttr.in/%s?format=%%l:+%%C+%%t+%%w", enc);
        if (live_http_get(url, &raw) != 0 || !raw) {
            snprintf(out, cap,
                     "Live weather fetch failed for %s (not CERT). Try the city spelling.",
                     place);
            return (int)strlen(out);
        }
        clip_speak("Live weather (not CERT, wttr.in): ", raw, out, cap);
        free(raw);
        return (int)strlen(out);
    }
    json_quoted_after(raw, "temp_C", nowt, sizeof nowt);
    {
        const char *desc = strstr(raw, "\"weatherDesc\"");
        if (desc) json_quoted_after(desc, "value", nowd, sizeof nowd);
        else nowd[0] = 0;
    }
    blk = strstr(raw, "\"weather\"");
    d0[0] = d1[0] = mn0[0] = mx0[0] = mn1[0] = mx1[0] = noon0[0] = noon1[0] = 0;
    if (blk) {
        const char *day0 = strstr(blk, "\"date\"");
        const char *day1 = day0 ? strstr(day0 + 6, "\"date\"") : NULL;
        if (day0) {
            json_quoted_after(day0, "date", d0, sizeof d0);
            json_quoted_after(day0, "mintempC", mn0, sizeof mn0);
            json_quoted_after(day0, "maxtempC", mx0, sizeof mx0);
            {
                const char *h = strstr(day0, "\"hourly\"");
                const char *noon = h ? strstr(h, "\"time\":\"1200\"") : NULL;
                if (!noon && h) noon = strstr(h, "\"time\": \"1200\"");
                if (noon) json_quoted_after(noon, "value", noon0, sizeof noon0);
                else if (h) json_quoted_after(h, "value", noon0, sizeof noon0);
            }
        }
        if (day1) {
            json_quoted_after(day1, "date", d1, sizeof d1);
            json_quoted_after(day1, "mintempC", mn1, sizeof mn1);
            json_quoted_after(day1, "maxtempC", mx1, sizeof mx1);
            {
                const char *h = strstr(day1, "\"hourly\"");
                const char *noon = h ? strstr(h, "\"time\":\"1200\"") : NULL;
                if (!noon && h) noon = strstr(h, "\"time\": \"1200\"");
                if (noon) json_quoted_after(noon, "value", noon1, sizeof noon1);
                else if (h) json_quoted_after(h, "value", noon1, sizeof noon1);
            }
        }
    }
    free(raw);
    snprintf(out, cap,
             "Live weather (not CERT, wttr.in): %s now %sC %s. "
             "%s: %s, %s–%sC. %s: %s, %s–%sC.",
             place,
             nowt[0] ? nowt : "?",
             nowd[0] ? nowd : "",
             d0[0] ? d0 : "today",
             noon0[0] ? noon0 : "—",
             mn0[0] ? mn0 : "?",
             mx0[0] ? mx0 : "?",
             d1[0] ? d1 : "tomorrow",
             noon1[0] ? noon1 : "—",
             mn1[0] ? mn1 : "?",
             mx1[0] ? mx1 : "?");
    return (int)strlen(out);
}
#else
static int live_weather_ask(const char *q, char *out, size_t cap) {
    (void)q;
    (void)out;
    (void)cap;
    return 0;
}
#endif

int cnet_mcp_factory_ask(const char *q, char *out, size_t cap) {
    char body[2048], args[400], esc[240];
    const char *p;
    if (!cnet_mcp_client_enabled() || !q || !q[0] || !out || cap < 32)
        return 0;
    out[0] = 0;

    /* Weather/forecast: live wttr.in — Instant Answer only has the city wiki. */
    if (live_weather_ask(q, out, cap) > 0)
        return (int)strlen(out);

    if (ci_has(q, "list skills") || ci_has(q, "sealed skills") ||
        ci_has(q, "mcp skills") || ci_has(q, "what skills")) {
        if (cnet_mcp_call("cnet_list_skills", "{}", body, sizeof body) <= 0)
            return 0;
        clip_speak("Factory MCP (not CERT): ", body, out, cap);
        return (int)strlen(out);
    }
    if (ci_has(q, "list units") || ci_has(q, "mcp units")) {
        if (cnet_mcp_call("cnet_list_units", "{}", body, sizeof body) <= 0)
            return 0;
        clip_speak("Factory MCP units (not CERT): ", body, out, cap);
        return (int)strlen(out);
    }
    if (ci_has(q, "mcp health") || ci_has(q, "health tick")) {
        if (cnet_mcp_call("cnet_health_tick", "{}", body, sizeof body) <= 0)
            return 0;
        clip_speak("Factory MCP health (not CERT): ", body, out, cap);
        return (int)strlen(out);
    }

    p = after_ci(q, "wiki ") ? after_ci(q, "wiki ") : after_ci(q, "wikipedia ");
    if (p && cnet_json_escape(p, esc, sizeof esc) == 0) {
        snprintf(args, sizeof args, "{\"query\":\"%s\"}", esc);
        if (cnet_mcp_call("cnet_wiki_lookup", args, body, sizeof body) <= 0)
            return 0;
        clip_speak("Factory MCP wiki (not CERT): ", body, out, cap);
        return (int)strlen(out);
    }

    /* Discord/PEER: "search online …" / "look up …" — factory web, never CERT. */
    p = after_ci(q, "search online");
    if (!p) p = after_ci(q, "search the web");
    if (!p) p = after_ci(q, "search web");
    if (!p) p = after_ci(q, "web search");
    if (!p) p = after_ci(q, "look up online");
    if (!p) p = after_ci(q, "look this up");
    if (!p) p = after_ci(q, "look up ");
    if (!p) p = after_ci(q, "google ");
    if (!p) p = after_ci(q, "search for ");
    if (p && cnet_json_escape(p, esc, sizeof esc) == 0) {
        snprintf(args, sizeof args, "{\"query\":\"%s\"}", esc);
        if (cnet_mcp_call("cnet_web_search", args, body, sizeof body) <= 0)
            return 0;
        clip_speak("Web (not CERT): ", body, out, cap);
        return (int)strlen(out);
    }

    p = after_ci(q, "use skill ");
    if (!p) p = after_ci(q, "run skill ");
    if (p && cnet_json_escape(p, esc, sizeof esc) == 0) {
        snprintf(args, sizeof args, "{\"skill\":\"%s\"}", esc);
        if (cnet_mcp_call("cnet_use_skill", args, body, sizeof body) <= 0)
            return 0;
        clip_speak("Factory MCP skill (not CERT): ", body, out, cap);
        return (int)strlen(out);
    }

    p = after_ci(q, "verify claim");
    if (p && cnet_json_escape(p, esc, sizeof esc) == 0) {
        snprintf(args, sizeof args, "{\"claim\":\"%s\"}", esc);
        if (cnet_mcp_call("cnet_verify_claim", args, body, sizeof body) <= 0)
            return 0;
        clip_speak("Factory MCP verify (not CERT): ", body, out, cap);
        return (int)strlen(out);
    }
    return 0;
}
