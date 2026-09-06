/* Explicit owner-only resident capsule control. No ASK routing or implicit retry. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "cnet_capsule_snapshot.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#ifdef CNET_CAPSULECTL_TESTING
#define DEADLINE_MS 250
#else
#define DEADLINE_MS 60000
#endif

static int atom(const char *s, size_t limit, int dots) {
    size_t n = strnlen(s, limit + 1);
    if (!n || n > limit || (dots && s[0] == '.')) return 0;
    for (size_t i = 0; i < n; i++)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') ||
              (s[i] >= '0' && s[i] <= '9') || s[i] == '_' || s[i] == '-' ||
              (dots && s[i] == '.'))) return 0;
    return 1;
}

static int revision_ok(const char *s) {
    size_t n = strnlen(s, 21);
    uint64_t value = 0;
    if (!n || n > 20 || s[0] == '0') return 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9' ||
            value > (UINT64_MAX - (unsigned)(s[i] - '0')) / 10) return 0;
        value = value * 10 + (unsigned)(s[i] - '0');
    }
    return 1;
}

static int digest_ok(const char *s) {
    if (strnlen(s, 65) != 64) return 0;
    for (size_t i = 0; i < 64; i++)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return 0;
    return 1;
}

static int arguments_ok(int argc, char **argv) {
    if (argc < 3 || argc > 6 || strnlen(argv[2], 9) > 8) return 0;
    if (!strcmp(argv[2], "STATUS")) return argc == 3;
    if (argc < 5 || !revision_ok(argv[3])) return 0;
    if (!strcmp(argv[2], "STAGE"))
        return argc == 6 && (argv[4][0] == '0' || argv[4][0] == '1') && !argv[4][1] && atom(argv[5], 63, 0);
    if (!strcmp(argv[2], "ACTIVATE"))
        return argc == 6 && atom(argv[4], 63, 0) && digest_ok(argv[5]);
    if (!strcmp(argv[2], "UNLOAD") || !strcmp(argv[2], "ROLLBACK"))
        return argc == 5 && atom(argv[4], 63, 0);
    return !strcmp(argv[2], "DISCARD") && argc == 5 && digest_ok(argv[4]);
}

static int64_t milliseconds(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) return -1;
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

/* All network phases share one deadline; readable HUP still needs an EOF read. */
static int ready(int fd, short events, int64_t deadline) {
    for (;;) {
        int64_t now = milliseconds();
        if (now < 0 || now >= deadline) return -1;
        int64_t left = deadline - now;
        struct pollfd p = {.fd = fd, .events = events};
        int rc = poll(&p, 1, left > INT_MAX ? INT_MAX : (int)left);
        if (rc < 0 && errno == EINTR) continue;
        if (rc <= 0 || (p.revents & POLLNVAL)) return -1;
        if (p.revents & (events | POLLHUP | POLLERR)) return 0;
    }
}

static int socket_ok(const struct stat *st) {
    return S_ISSOCK(st->st_mode) && st->st_uid == geteuid() && !(st->st_mode & 077);
}

/* Validate the server's exact bounded canonical status, never arbitrary text. */
static int response_code(char *line, size_t length) {
    if (!length || line[length - 1] != '\n') return -1;
    for (size_t i = 0; i + 1 < length; i++)
        if ((unsigned char)line[i] < 32 || (unsigned char)line[i] > 126) return -1;
    line[length - 1] = 0;
    char *part[7] = {line};
    size_t n = 1;
    for (char *p = line; *p; p++) {
        if (*p != ' ') continue;
        if (p == line || p[-1] == 0 || !p[1] || n == 7) return -1;
        *p = 0;
        part[n++] = p + 1;
    }
    if (n != 7 || (strcmp(part[0], "OK") && strcmp(part[0], "ERR"))) return -1;
    const char *prefix[] = {"revision=", "active=", "rollback=", "staged=", "durable=", "reason="};
    const char *value[6];
    for (size_t i = 0; i < 6; i++) {
        size_t len = strlen(prefix[i]);
        if (strncmp(part[i + 1], prefix[i], len)) return -1;
        value[i] = part[i + 1] + len;
    }
    if (!revision_ok(value[0])) return -1;
    for (size_t i = 1; i <= 3; i++)
        if (strcmp(value[i], "-") && !digest_ok(value[i])) return -1;
    if (strcmp(value[4], "0") && strcmp(value[4], "1")) return -1;
    int rc = !strcmp(part[0], "OK") ? 0 : 1;
    if ((!rc && strcmp(value[5], "ok")) ||
        (rc && strcmp(value[5], "refused") && strcmp(value[5], "durability_uncertain")) ||
        (!strcmp(value[5], "durability_uncertain") && strcmp(value[4], "0"))) return -1;
    return rc;
}

int main(int argc, char **argv) {
    if (!arguments_ok(argc, argv)) {
        fputs("usage: cnet_capsulectl /absolute/private/control.sock STATUS\n"
              "  STAGE revision 0|1 named-set | ACTIVATE revision token digest\n"
              "  UNLOAD revision token | ROLLBACK revision token | DISCARD revision digest\n", stderr);
        return 2;
    }
    int fd = -1, parent = -1, result = 2;
    size_t sent = 0;
    const char *error = "invalid or untrusted control socket path";
    char directory[108], request[256], reply[400], parsed[400];
    if (argv[1][0] != '/' || strnlen(argv[1], sizeof directory) >= sizeof directory) goto done;
    strcpy(directory, argv[1]);
    char *leaf = strrchr(directory, '/');
    if (!leaf || !atom(leaf + 1, 80, 1)) goto done;
    *leaf++ = 0;
    parent = cnet_capsule_owner_directory(directory, 1);
    struct stat before, after;
    if (parent < 0 || fstatat(parent, leaf, &before, AT_SYMLINK_NOFOLLOW) || !socket_ok(&before)) goto done;

    struct sockaddr_un address = {.sun_family = AF_UNIX};
    int n = snprintf(address.sun_path, sizeof address.sun_path, "/proc/self/fd/%d/%s", parent, leaf);
    if (n < 0 || (size_t)n >= sizeof address.sun_path) goto done;
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) { error = "cannot open control connection"; goto done; }
    int64_t start = milliseconds();
    if (start < 0) { error = "monotonic clock unavailable"; goto done; }
    int64_t deadline = start + DEADLINE_MS;
    error = "control connection failed or deadline expired";
    if (connect(fd, (struct sockaddr *)&address, sizeof address)) {
        if (errno != EINPROGRESS || ready(fd, POLLOUT, deadline)) goto done;
        int pending = 0;
        socklen_t size = sizeof pending;
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &pending, &size) || size != sizeof pending || pending) goto done;
    }
    struct ucred peer;
    socklen_t size = sizeof peer;
    error = "control peer identity or socket path changed";
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &size) || size != sizeof peer ||
        peer.uid != geteuid() || fstatat(parent, leaf, &after, AT_SYMLINK_NOFOLLOW) ||
        !socket_ok(&after) || before.st_dev != after.st_dev || before.st_ino != after.st_ino) goto done;

    size_t length = 0;
    for (int i = 2; i < argc; i++) {
        n = snprintf(request + length, sizeof request - length, "%s%c", argv[i], i == argc - 1 ? '\n' : ' ');
        if (n < 0 || (size_t)n >= sizeof request - length) { error = "request exceeds bound"; goto done; }
        length += (size_t)n;
    }
    error = "control send failed or deadline expired";
    while (sent < length) {
        if (ready(fd, POLLOUT, deadline)) goto done;
        ssize_t count = send(fd, request + sent, length - sent, MSG_NOSIGNAL);
        if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (count <= 0) goto done;
        sent += (size_t)count;
    }
    error = "control response failed, exceeded bound, or deadline expired";
    length = 0;
    for (;;) {
        if (ready(fd, POLLIN, deadline)) goto done;
        ssize_t count = recv(fd, reply + length, sizeof reply - 1 - length, 0);
        if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (count < 0) goto done;
        if (!count) break;
        length += (size_t)count;
        if (length == sizeof reply - 1) goto done;
    }
    reply[length] = 0;
    memcpy(parsed, reply, length + 1);
    int code = response_code(parsed, length);
    error = "malformed control response";
    if (code < 0) goto done;
    error = "cannot write validated control response";
    if (fwrite(reply, 1, length, stdout) != length || fflush(stdout)) goto done;
    result = code;
done:
    if (fd >= 0) close(fd);
    if (parent >= 0) close(parent);
    if (result == 2) {
        fprintf(stderr, "cnet_capsulectl: %s\n", error);
        if (sent) fputs("request outcome unknown; inspect STATUS; retry a mutation only with its identical operation, token, and expected revision\n", stderr);
    }
    return result;
}
