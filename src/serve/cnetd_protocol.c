#define _POSIX_C_SOURCE 200809L

#include "cnetd_protocol.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "cnet_json_internal.h"

static void set_error(char *error, size_t cap, const char *message) {
    if (!error || cap == 0) return;
    (void)snprintf(error, cap, "%s", message ? message : "invalid request");
}

static int64_t monotonic_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return -1;
    return (int64_t)ts.tv_sec * 1000 + (int64_t)ts.tv_nsec / 1000000;
}

CnetdReadResult cnetd_read_request(int fd, char *out, size_t cap,
                                   int timeout_ms) {
    int64_t start;
    int64_t deadline;
    size_t used = 0;

    if (fd < 0 || !out || cap < 2 || timeout_ms <= 0)
        return CNETD_READ_ERROR;
    out[0] = '\0';
    start = monotonic_ms();
    if (start < 0 || start > INT64_MAX - timeout_ms)
        return CNETD_READ_ERROR;
    deadline = start + timeout_ms;

    for (;;) {
        struct pollfd pfd;
        int64_t now = monotonic_ms();
        int64_t remaining;
        int wait_ms;
        int ready;
        ssize_t got;
        char *newline;

        if (now < 0) return CNETD_READ_ERROR;
        remaining = deadline - now;
        if (remaining <= 0) return CNETD_READ_TIMEOUT;
        wait_ms = remaining > INT_MAX ? INT_MAX : (int)remaining;

        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        ready = poll(&pfd, 1, wait_ms);
        if (ready == 0) return CNETD_READ_TIMEOUT;
        if (ready < 0 || (pfd.revents & (POLLERR | POLLNVAL)))
            return CNETD_READ_ERROR;

        if (used == cap - 1) return CNETD_READ_TOO_LONG;
        got = read(fd, out + used, cap - 1 - used);
        if (got == 0)
            return used == 0 ? CNETD_READ_EOF : CNETD_READ_INCOMPLETE;
        if (got < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            return CNETD_READ_ERROR;
        }
        if (memchr(out + used, '\0', (size_t)got) != NULL)
            return CNETD_READ_ERROR;
        used += (size_t)got;
        out[used] = '\0';
        newline = memchr(out, '\n', used);
        if (newline) {
            size_t end = (size_t)(newline - out);
            if (end > 0 && out[end - 1] == '\r') end--;
            out[end] = '\0';
            return CNETD_READ_OK;
        }
        if (used == cap - 1) return CNETD_READ_TOO_LONG;
    }
}


static int parse_json_request(const unsigned char *text, CnetdRequest *out,
                              char *error, size_t error_cap) {
    JsonCursor cursor;
    int seen_q = 0;
    int seen_op = 0;
    int seen_peer = 0;
    char op[16] = "ask";

    cursor.p = text;
    json_ws(&cursor);
    if (*cursor.p++ != '{') goto invalid_json;
    json_ws(&cursor);
    if (*cursor.p == '}') goto missing_q;
    for (;;) {
        char key[64];
        if (json_string(&cursor, key, sizeof key) != 0) goto invalid_json;
        json_ws(&cursor);
        if (*cursor.p++ != ':') goto invalid_json;
        json_ws(&cursor);
        if (strcmp(key, "q") == 0) {
            if (seen_q) {
                set_error(error, error_cap, "duplicate q field");
                return -1;
            }
            if (json_string(&cursor, out->query, sizeof out->query) != 0)
                goto invalid_json;
            seen_q = 1;
        } else if (strcmp(key, "peer") == 0) {
            if (seen_peer || json_string(&cursor, out->peer, sizeof out->peer) != 0)
                goto invalid_json;
            for (const unsigned char *p = (const unsigned char *)out->peer; *p; p++)
                if (*p <= 32 || *p == 127) goto invalid_json;
            seen_peer = 1;
        } else if (strcmp(key, "op") == 0) {
            if (seen_op) {
                set_error(error, error_cap, "duplicate op field");
                return -1;
            }
            if (json_string(&cursor, op, sizeof op) != 0) goto invalid_json;
            seen_op = 1;
        } else if (json_value(&cursor, 1) != 0) {
            goto invalid_json;
        }
        json_ws(&cursor);
        if (*cursor.p == '}') {
            cursor.p++;
            break;
        }
        if (*cursor.p++ != ',') goto invalid_json;
        json_ws(&cursor);
    }
    json_ws(&cursor);
    if (*cursor.p != '\0') {
        set_error(error, error_cap, "trailing JSON data");
        return -1;
    }
    if (!seen_q) goto missing_q;
    if (out->query[0] == '\0') {
        set_error(error, error_cap, "empty q field");
        return -1;
    }
    if (strcmp(op, "ask") != 0) {
        set_error(error, error_cap, "unsupported op");
        return -1;
    }
    out->kind = CNETD_REQUEST_ASK;
    out->json = 1;
    return 0;

missing_q:
    set_error(error, error_cap, "missing q field");
    return -1;
invalid_json:
    set_error(error, error_cap, "invalid JSON request");
    return -1;
}

static int copy_query(char *dest, size_t cap, const char *source,
                      char *error, size_t error_cap) {
    size_t length = strlen(source);
    if (length == 0) {
        set_error(error, error_cap, "empty query");
        return -1;
    }
    if (length >= cap) {
        set_error(error, error_cap, "query too long");
        return -1;
    }
    memcpy(dest, source, length + 1);
    return 0;
}

int cnetd_parse_request(const char *line, CnetdRequest *out,
                        char *error, size_t error_cap) {
    const char *start;
    const char *query;
    const char *peer_end;
    size_t peer_len;

    if (!line || !out) {
        set_error(error, error_cap, "invalid parser arguments");
        return -1;
    }
    memset(out, 0, sizeof *out);
    if (error && error_cap > 0) error[0] = '\0';

    start = line;
    while (*start && isspace((unsigned char)*start)) start++;
    if (*start == '{')
        return parse_json_request((const unsigned char *)start, out,
                                  error, error_cap);
    if (*line == '\0') {
        set_error(error, error_cap, "empty request");
        return -1;
    }
    if (strcmp(line, "PING") == 0) {
        out->kind = CNETD_REQUEST_PING;
        return 0;
    }
    if (strcmp(line, "STATUS") == 0) {
        out->kind = CNETD_REQUEST_STATUS;
        return 0;
    }
    if (strcmp(line, "QUIT") == 0) {
        out->kind = CNETD_REQUEST_QUIT;
        return 0;
    }
    if (strncmp(line, "ASK ", 4) == 0) {
        out->kind = CNETD_REQUEST_ASK;
        return copy_query(out->query, sizeof out->query, line + 4,
                          error, error_cap);
    }
    if (strncmp(line, "PEER ", 5) == 0) {
        peer_end = line + 5;
        while (*peer_end && !isspace((unsigned char)*peer_end)) peer_end++;
        peer_len = (size_t)(peer_end - (line + 5));
        query = peer_end;
        while (*query && isspace((unsigned char)*query)) query++;
        if (peer_len == 0 || peer_len >= sizeof out->peer) {
            set_error(error, error_cap, "invalid peer identity");
            return -1;
        }
        memcpy(out->peer, line + 5, peer_len);
        out->peer[peer_len] = '\0';
        out->kind = CNETD_REQUEST_ASK;
        return copy_query(out->query, sizeof out->query, query,
                          error, error_cap);
    }
    if (strcmp(line, "ASK") == 0 || strcmp(line, "PEER") == 0) {
        set_error(error, error_cap, "missing query");
        return -1;
    }
    out->kind = CNETD_REQUEST_ASK;
    return copy_query(out->query, sizeof out->query, line,
                      error, error_cap);
}
