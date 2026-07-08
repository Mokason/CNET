#include "../../include/contract/mcp_utils.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

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

#ifdef _WIN32
static int url_host_allowed(const char *url) {
    return (strncmp(url, "https://api.duckduckgo.com/", 27) == 0 ||
            strncmp(url, "https://en.wikipedia.org/", 25) == 0);
}
#endif

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
        INTERNET_FLAG_NO_COOKIES | INTERNET_FLAG_NO_UI,
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
    /* Linux HTTP fallback using curl + popen */
    char cmd[2048];
    FILE *pipe;
    size_t got = 0;
    char buf[4096];

    /* Basic safety: only allow https to known hosts */
    if (strncmp(url, "https://api.duckduckgo.com/", 27) != 0 &&
        strncmp(url, "https://en.wikipedia.org/", 25) != 0 &&
        strncmp(url, "https://", 8) != 0) {
        snprintf(out, out_cap, "HTTP fetch blocked for unsafe URL.");
        return -1;
    }

    snprintf(cmd, sizeof(cmd), "curl -s -L --max-time 10 --connect-timeout 5 \"%s\"", url);

    pipe = popen(cmd, "r");
    if (!pipe) {
        snprintf(out, out_cap, "HTTP fetch failed (no curl/popen).");
        return -1;
    }

    while (fgets(buf, sizeof(buf), pipe) != NULL) {
        size_t len = strlen(buf);
        if (got + len >= out_cap - 1) {
            memcpy(out + got, buf, (out_cap - 1) - got);
            got = out_cap - 1;
            break;
        }
        memcpy(out + got, buf, len);
        got += len;
    }
    out[got] = '\0';
    pclose(pipe);
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
            } else if (esc == '\"') {
                out[j++] = '\"';
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
    FILE *f;
    size_t written;

    if (!path || !content) return -1;
    if (snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path) >= (int)sizeof(tmp_path)) {
        return -1;
    }

    f = fopen(tmp_path, "wb");
    if (!f) return -1;
    len = strlen(content);
    written = fwrite(content, 1, len, f);
    if (ferror(f) || written != len) {
        fclose(f);
        remove(tmp_path);
        return -1;
    }
    if (fclose(f) != 0) {
        remove(tmp_path);
        return -1;
    }
    if (rename(tmp_path, path) != 0) {
        if (remove(path) != 0 && errno != ENOENT) {
            remove(tmp_path);
            return -1;
        }
        if (rename(tmp_path, path) != 0) {
            remove(tmp_path);
            return -1;
        }
    }
    return 0;
}


