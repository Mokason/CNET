/* cnet_peer — pure C UNIX client to cnetd (Marble peer path, not MCP).
 *
 * Usage:
 *   cnet_peer PING
 *   cnet_peer STATUS
 *   cnet_peer "who are you"
 *   cnet_peer --peer hermes "how do you work with Hermes"
 *   cnet_peer --json "who are you"
 *
 * Socket: CNET_SOCK or $XDG_RUNTIME_DIR/cnet/cnet.sock or
 *         ~/.local/share/cnet-minimal/run/cnet.sock
 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define LINE 8192

static int full_write(int fd, const void *buf, size_t n) {
    const char *p = (const char *)buf;
    while (n) {
        ssize_t k = write(fd, p, n);
        if (k < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += (size_t)k;
        n -= (size_t)k;
    }
    return 0;
}

static int resolve_sock(char *out, size_t n) {
    const char *e = getenv("CNET_SOCK");
    const char *xdg = getenv("XDG_RUNTIME_DIR");
    const char *home = getenv("HOME");
    struct passwd *pw;
    if (e && e[0]) {
        snprintf(out, n, "%s", e);
        return 0;
    }
    if (xdg && xdg[0]) {
        snprintf(out, n, "%s/cnet/cnet.sock", xdg);
        if (access(out, F_OK) == 0) return 0;
    }
    if (!home || !home[0]) {
        pw = getpwuid(getuid());
        home = pw ? pw->pw_dir : NULL;
    }
    if (home && home[0]) {
        snprintf(out, n, "%s/.local/share/cnet-minimal/run/cnet.sock", home);
        return 0;
    }
    return -1;
}

static int ask_once(const char *sock_path, const char *msg, int want_json) {
    int fd;
    struct sockaddr_un addr;
    char buf[LINE];
    char req[LINE + 64];
    size_t total = 0;
    ssize_t k;

    (void)want_json;
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    if (strlen(sock_path) >= sizeof addr.sun_path) {
        fprintf(stderr, "cnet_peer: path too long\n");
        close(fd);
        return 1;
    }
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", sock_path);
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        fprintf(stderr, "cnet_peer: connect %s: %s\n", sock_path, strerror(errno));
        fprintf(stderr, "  start: systemctl --user start cnetd.service\n");
        close(fd);
        return 1;
    }
    snprintf(req, sizeof req, "%s\n", msg);
    if (full_write(fd, req, strlen(req)) != 0) {
        perror("write");
        close(fd);
        return 1;
    }
    for (;;) {
        k = read(fd, buf + total, sizeof buf - 1 - total);
        if (k < 0) {
            if (errno == EINTR) continue;
            perror("read");
            close(fd);
            return 1;
        }
        if (k == 0) break;
        total += (size_t)k;
        buf[total] = 0;
        if (strstr(buf, "\nEND\n") || (buf[0] == '{' && total > 0 && buf[total - 1] == '\n'))
            break;
        if (total >= sizeof buf - 1) break;
    }
    close(fd);
    if (total) fwrite(buf, 1, total, stdout);
    if (total && buf[total - 1] != '\n') fputc('\n', stdout);
    return 0;
}

int main(int argc, char **argv) {
    char sock[512];
    char msg[LINE];
    const char *peer = NULL;
    int json = 0;
    int i = 1;
    const char *q;

    if (resolve_sock(sock, sizeof sock) != 0) {
        fprintf(stderr, "cnet_peer: cannot resolve socket path\n");
        return 2;
    }
    while (i < argc && argv[i][0] == '-') {
        if (!strcmp(argv[i], "--peer") && i + 1 < argc) {
            peer = argv[++i];
            i++;
        } else if (!strcmp(argv[i], "--json")) {
            json = 1;
            i++;
        } else if (!strcmp(argv[i], "--sock") && i + 1 < argc) {
            snprintf(sock, sizeof sock, "%s", argv[++i]);
            i++;
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            fprintf(stderr,
                    "usage: cnet_peer [--peer NAME] [--json] [--sock PATH] <query|PING|STATUS>\n"
                    "Marble peer client for cnetd (not MCP).\n");
            return 0;
        } else {
            fprintf(stderr, "unknown flag %s\n", argv[i]);
            return 2;
        }
    }
    if (i >= argc) {
        fprintf(stderr, "usage: cnet_peer <query>\n");
        return 2;
    }
    q = argv[i];
    if (!strcmp(q, "PING") || !strcmp(q, "STATUS") || !strcmp(q, "QUIT")) {
        snprintf(msg, sizeof msg, "%s", q);
    } else if (json) {
        /* minimal JSON ask */
        snprintf(msg, sizeof msg, "{\"op\":\"ask\",\"q\":\"");
        {
            size_t n = strlen(msg);
            const char *p = q;
            while (*p && n + 2 < sizeof msg - 4) {
                if (*p == '"' || *p == '\\') msg[n++] = '\\';
                msg[n++] = *p++;
            }
            msg[n] = 0;
        }
        strncat(msg, "\"}", sizeof msg - strlen(msg) - 1);
    } else if (peer && peer[0]) {
        snprintf(msg, sizeof msg, "PEER %s %s", peer, q);
    } else {
        snprintf(msg, sizeof msg, "ASK %s", q);
    }
    return ask_once(sock, msg, json);
}
