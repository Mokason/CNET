#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "../../include/contract/mcp_utils.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#ifndef _WIN32
#include <sys/stat.h>
#endif

#ifdef _WIN32
#include <windows.h>
#include <wininet.h>
#endif

static int is_allowed_path_char(char c) {
    return isalnum((unsigned char)c) || c == '_' || c == '-' || c == '.' ||
           c == '/' || c == '\\';
}

static int path_has_unsafe_segment(const char *in) {
    const char *p = in;
    while (*p != '\0') {
        const char *seg = p;
        size_t seg_len = 0;
        while (*p != '\0' && *p != '/' && *p != '\\') {
            if (!is_allowed_path_char(*p)) return 1;
            ++seg_len;
            ++p;
        }
        if (seg_len == 0) return 1;                 /* "foo//bar" */
        if (seg_len == 1 && seg[0] == '.') return 1; /* "." segment */
        if (seg_len == 2 && seg[0] == '.' && seg[1] == '.') return 1; /* ".." */
        if (*p == '\0') break;
        ++p; /* skip delimiter */
    }
    return 0;
}

static int path_under_build_root(const char *in) {
    return (strncmp(in, "build/", 6) == 0 || strncmp(in, "build\\", 6) == 0);
}

static int path_under_read_root(const char *in) {
    if (path_under_build_root(in)) return 1;
    if (strchr(in, '/') || strchr(in, '\\')) return 0;
    return 1;
}

static int path_has_allowed_read_ext(const char *in) {
    const char *ext = strrchr(in, '.');
    if (!ext) return 0;
    return (strcmp(ext, ".md") == 0 || strcmp(ext, ".txt") == 0 ||
            strcmp(ext, ".json") == 0 || strcmp(ext, ".log") == 0 ||
            strcmp(ext, ".cfg") == 0 || strcmp(ext, ".csv") == 0);
}

/* Lexical checks do not stop fopen from following a symlinked file or parent
 * directory. Reject every existing link/reparse component; a missing final
 * leaf remains valid so callers can use this for both reads and creates. */
static int path_components_safe(const char *path) {
    char probe[1024];
    size_t len;
    size_t i;

    if (!path) return 0;
    len = strlen(path);
    if (len == 0 || len >= sizeof probe) return 0;
    memcpy(probe, path, len + 1);
    for (i = 0; i <= len; ++i) {
        int leaf;
        char saved;
        if (probe[i] != '/' && probe[i] != '\\' && probe[i] != '\0')
            continue;
        saved = probe[i];
        probe[i] = '\0';
        leaf = (i == len);
        if (probe[0] != '\0') {
#ifdef _WIN32
            DWORD attrs = GetFileAttributesA(probe);
            if (attrs == INVALID_FILE_ATTRIBUTES) {
                if (!leaf) {
                    probe[i] = saved;
                    return 0;
                }
            } else if ((attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
                       (!leaf && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0)) {
                probe[i] = saved;
                return 0;
            }
#else
            struct stat st;
            if (lstat(probe, &st) != 0) {
                if (!leaf || errno != ENOENT) {
                    probe[i] = saved;
                    return 0;
                }
            } else if (S_ISLNK(st.st_mode) ||
                       (!leaf && !S_ISDIR(st.st_mode))) {
                probe[i] = saved;
                return 0;
            }
#endif
        }
        probe[i] = saved;
    }
    return 1;
}

static int resolve_safe_relative_path(
    const char *in, char *out, size_t cap,
    int need_build_root, int require_ext
) {
    if (!in || !out || cap == 0) return -1;
    out[0] = '\0';

    if (strpbrk(in, "\r\n\t") != NULL) return -1;
    if (strstr(in, "../") != NULL || strstr(in, "..\\") != NULL) return -1;
    if (strpbrk(in, "<>|*?\"") != NULL) return -1;
    if (in[0] == '/' || in[0] == '\\') return -1;
    if (in[0] == '\0') return -1;
    if ((strlen(in) > 1 && in[1] == ':') || strchr(in, ':') != NULL) return -1;
    if (path_has_unsafe_segment(in)) return -1;
    if (need_build_root && !path_under_build_root(in)) return -1;
    if (!need_build_root && !path_under_read_root(in)) return -1;
    if (require_ext && !path_has_allowed_read_ext(in)) return -1;

    if (snprintf(out, cap, "%s", in) >= (int)cap) return -1;
    if (strncmp(out, "./", 2) == 0) {
        size_t len = strlen(out);
        if (len <= 2) return -1;
        memmove(out, out + 2, len - 2 + 1);
    }
    if (need_build_root && !path_under_build_root(out)) return -1;
    if (!need_build_root && !path_under_read_root(out)) return -1;
    if (!path_components_safe(out)) return -1;
    return 0;
}

int mcp_resolve_read_path(const char *in, char *out, size_t cap) {
    return resolve_safe_relative_path(in, out, cap, 0, 1);
}

int mcp_resolve_write_path(const char *in, char *out, size_t cap) {
    return resolve_safe_relative_path(in, out, cap, 1, 0);
}

void mcp_trim_ws(char *s) {
    size_t len;
    if (!s) return;

    len = strlen(s);
    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r' ||
                       s[len - 1] == ' ' || s[len - 1] == '\t')) {
        s[--len] = '\0';
    }
    while (*s && isspace((unsigned char)*s)) {
        memmove(s, s + 1, strlen(s));
        --len;
    }
}

void mcp_sanitize_cache_key(const char *in, char *out, size_t cap) {
    size_t i = 0, j = 0;
    if (!in || !out || cap == 0) return;
    while (in[i] != '\0' && j + 1 < cap) {
        char c = in[i++];
        if (c == '\r' || c == '\n' || c == '\t') {
            out[j++] = ' ';
            continue;
        }
        if (c == '|' || c == ':' ) {
            out[j++] = ' ';
            continue;
        }
        if ((unsigned char)c < 0x20 || (unsigned char)c > 0x7e) {
            out[j++] = '_';
            continue;
        }
        out[j++] = c;
    }
    out[j] = '\0';
}

void mcp_normalize_cache_key(const char *in, char *out, size_t cap) {
    size_t i = 0, j = 0;
    if (!in || !out || cap == 0) return;
    while (in[i] != '\0' && j + 1 < cap) {
        char c = in[i++];
        if (c == ' ' || c == '_') {
            out[j++] = ' ';
        } else if (isalnum((unsigned char)c) || c == '\'' || c == '-') {
            out[j++] = (char)tolower((unsigned char)c);
        }
    }
    out[j] = '\0';
    mcp_trim_ws(out);
}

int mcp_url_encode_component(const char *in, char *out, size_t cap) {
    size_t i, j = 0;
    if (!in || !out || cap == 0) return -1;
    for (i = 0; in[i] != '\0'; ++i) {
        unsigned char c = (unsigned char)in[i];
        if (j + 1 >= cap) return -1;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out[j++] = (char)c;
            continue;
        }
        if (c == ' ') {
            if (j + 3 >= cap) return -1;
            out[j++] = '%';
            out[j++] = '2';
            out[j++] = '0';
            continue;
        }
        if (j + 3 >= cap) return -1;
        snprintf(out + j, 4, "%%%02X", (unsigned int)c);
        j += 3;
    }
    out[j] = '\0';
    return 0;
}

/* Strict transport allowlist: only the two known MCP knowledge hosts.
 * The header contract promises a bounded, command-free fetch; we never
 * accept arbitrary https hosts and we never run a shell. Both Windows
 * and Linux paths use this same allowlist. */
static int url_host_allowed(const char *url) {
    if (!url) return 0;
    if (strncmp(url, "https://api.duckduckgo.com/", 27) == 0)
        return 1;
    if (strncmp(url, "https://en.wikipedia.org/", 25) == 0)
        return 1;
    return 0;
}

int mcp_http_get(const char *url, char *out, size_t out_cap) {
    if (!url || !out || out_cap == 0) return -1;
    out[0] = '\0';

#ifdef _WIN32
    unsigned char buf[4096];
    HINTERNET hSession = NULL;
    HINTERNET hRequest = NULL;
    DWORD read = 0;
    size_t got = 0;

    if (!url_host_allowed(url)) return -1;

    hSession = InternetOpenA("CNET-MCP", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (!hSession) return -1;

    hRequest = InternetOpenUrlA(
        hSession,
        url,
        NULL,
        0,
        INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE |
        INTERNET_FLAG_NO_COOKIES | INTERNET_FLAG_NO_UI |
        INTERNET_FLAG_NO_AUTO_REDIRECT,
        0
    );
    if (!hRequest) {
        InternetCloseHandle(hSession);
        return -1;
    }

    while (InternetReadFile(hRequest, buf, sizeof(buf) - 1, &read) && read > 0) {
        if (got + (size_t)read >= out_cap - 1) {
            memcpy(out + got, buf, (out_cap - 1) - got);
            got = out_cap - 1;
            break;
        }
        memcpy(out + got, buf, read);
        got += (size_t)read;
    }
    out[got] = '\0';
    InternetCloseHandle(hRequest);
    InternetCloseHandle(hSession);
    return (got > 0) ? 0 : -1;
#else
    /* Linux: argv-based curl child. No shell, no popen, no command string.
     * The strict allowlist above is the only way a URL reaches curl, so an
     * attacker-controlled URL can never smuggle argv flags even if it
     * reached this function. execve uses a fixed argv with the URL as the
     * final positional argument. */
    int pipefd[2];
    pid_t pid;
    size_t got = 0;
    ssize_t n;

    if (!url_host_allowed(url)) {
        snprintf(out, out_cap, "HTTP fetch blocked: host not on MCP allowlist.");
        return -1;
    }
    if (strlen(url) > 1024) {
        snprintf(out, out_cap, "HTTP fetch blocked: URL too long.");
        return -1;
    }
    if (pipe(pipefd) != 0) {
        snprintf(out, out_cap, "HTTP fetch failed (pipe).");
        return -1;
    }
    pid = fork();
    if (pid < 0) {
        close(pipefd[0]); close(pipefd[1]);
        snprintf(out, out_cap, "HTTP fetch failed (fork).");
        return -1;
    }
    if (pid == 0) {
        /* child: exec curl with a fixed argv — no shell interpretation. */
        extern char **environ;
        const char *argv[] = {
            "curl", "--silent", "--show-error", "--fail",
            "--proto", "=https",
            "--max-time", "10",
            "--connect-timeout", "5",
            "--max-filesize", "1048576",
            url, NULL
        };
        int devnull_fd;
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        if (pipefd[1] != STDOUT_FILENO) close(pipefd[1]);
        devnull_fd = open("/dev/null", O_WRONLY);
        if (devnull_fd >= 0) {
            dup2(devnull_fd, STDERR_FILENO);
            if (devnull_fd != STDERR_FILENO) close(devnull_fd);
        }
        execve("/usr/bin/curl", (char *const *)argv, environ);
        /* fall back to PATH lookup if /usr/bin/curl missing */
        execvp("curl", (char *const *)argv);
        _exit(127);
    }
    close(pipefd[1]);
    while (got + 1 < out_cap &&
           (n = read(pipefd[0], out + got, out_cap - 1 - got)) > 0) {
        got += (size_t)n;
    }
    out[got] = '\0';
    close(pipefd[0]);
    {
        int status = 0;
        pid_t w;
        do { w = waitpid(pid, &status, 0); } while (w < 0 && errno == EINTR);
        if (w < 0 || (!WIFEXITED(status) || WEXITSTATUS(status) != 0)) {
            return -1;
        }
    }
    return (got > 0) ? 0 : -1;
#endif
}

int mcp_extract_json_field(const char *json, const char *key, char *out, size_t cap) {
    const char *cursor;
    size_t j = 0;
    if (!json || !key || !out || cap == 0) return -1;
    out[0] = '\0';

    cursor = strstr(json, key);
    if (!cursor) return 0;

    cursor += strlen(key);
    cursor = strchr(cursor, ':');
    if (!cursor) return 0;
    ++cursor;
    while (*cursor != '\0' && isspace((unsigned char)*cursor)) ++cursor;
    if (*cursor != '"') return 0;
    ++cursor;

    while (*cursor != '\0' && j + 1 < cap) {
        if (*cursor == '\\' && cursor[1] != '\0') {
            char esc = cursor[1];
            if (esc == 'n') {
                out[j++] = ' ';
            } else if (esc == 'r') {
                out[j++] = ' ';
            } else if (esc == 't') {
                out[j++] = ' ';
            } else if (esc == '"') {
                out[j++] = '"';
            } else if (esc == '\\') {
                out[j++] = '\\';
            } else if (esc == '/' ) {
                out[j++] = '/';
            } else if (esc == 'u' && isxdigit((unsigned char)cursor[2]) &&
                       isxdigit((unsigned char)cursor[3]) &&
                       isxdigit((unsigned char)cursor[4]) &&
                       isxdigit((unsigned char)cursor[5])) {
                out[j++] = '?';
                cursor += 4;
            } else {
                out[j++] = esc;
            }
            cursor += 2;
            continue;
        }
        if (*cursor == '"') {
            break;
        }
        out[j++] = *cursor++;
    }
    out[j] = '\0';
    mcp_trim_ws(out);
    return (int)j;
}

int mcp_atomic_write_text(const char *path, const char *content) {
    char tmp_path[1024];
    size_t len;

    if (!path || !content || !path_components_safe(path)) return -1;
    len = strlen(content);
#ifdef _WIN32
    {
        static volatile LONG serial = 0;
        HANDLE h = INVALID_HANDLE_VALUE;
        size_t done = 0;
        int attempt;
        for (attempt = 0; attempt < 32; ++attempt) {
            LONG id = InterlockedIncrement(&serial);
            if (snprintf(tmp_path, sizeof tmp_path, "%s.tmp.%lu.%ld", path,
                         (unsigned long)GetCurrentProcessId(), (long)id) >=
                (int)sizeof tmp_path)
                return -1;
            h = CreateFileA(tmp_path, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                            FILE_ATTRIBUTE_NORMAL, NULL);
            if (h != INVALID_HANDLE_VALUE) break;
            if (GetLastError() != ERROR_FILE_EXISTS) return -1;
        }
        if (h == INVALID_HANDLE_VALUE) return -1;
        while (done < len) {
            DWORD wrote = 0;
            DWORD chunk = (DWORD)((len - done) > 0x7fffffffu
                                      ? 0x7fffffffu
                                      : (len - done));
            if (!WriteFile(h, content + done, chunk, &wrote, NULL) ||
                wrote == 0) {
                CloseHandle(h);
                DeleteFileA(tmp_path);
                return -1;
            }
            done += (size_t)wrote;
        }
        {
            int ok = FlushFileBuffers(h) ? 1 : 0;
            if (!CloseHandle(h)) ok = 0;
            if (!ok ||
                !MoveFileExA(tmp_path, path,
                             MOVEFILE_REPLACE_EXISTING |
                                 MOVEFILE_WRITE_THROUGH)) {
                DeleteFileA(tmp_path);
                return -1;
            }
        }
    }
#else
    {
        FILE *f;
        int fd;
        int ok = 0;
        int dirfd = -1;
        char parent[1024];
        char *slash;

        if (snprintf(tmp_path, sizeof tmp_path, "%s.tmp.XXXXXX", path) >=
            (int)sizeof tmp_path)
            return -1;
        fd = mkstemp(tmp_path);
        if (fd < 0) return -1;
        f = fdopen(fd, "wb");
        if (!f) {
            close(fd);
            remove(tmp_path);
            return -1;
        }
        if ((len > 0 && fwrite(content, 1, len, f) != len) ||
            fflush(f) != 0 || fsync(fd) != 0)
            ok = -1;
        if (fclose(f) != 0) ok = -1;
        if (ok == 0 && rename(tmp_path, path) != 0) ok = -1;
        if (ok == 0) {
            if (strlen(path) >= sizeof parent) {
                ok = -1;
            } else {
                memcpy(parent, path, strlen(path) + 1);
                slash = strrchr(parent, '/');
                if (!slash) {
                    memcpy(parent, ".", 2);
                } else if (slash == parent) {
                    slash[1] = '\0';
                } else {
                    *slash = '\0';
                }
#ifdef O_DIRECTORY
                dirfd = open(parent, O_RDONLY | O_DIRECTORY);
#else
                dirfd = open(parent, O_RDONLY);
#endif
                if (dirfd < 0 || fsync(dirfd) != 0) ok = -1;
                if (dirfd >= 0 && close(dirfd) != 0) ok = -1;
            }
        }
        if (ok != 0) {
            remove(tmp_path);
            return -1;
        }
    }
#endif
    return 0;
}