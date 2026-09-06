#define _POSIX_C_SOURCE 200809L
#include "cnet_md_memory.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int is_dir(const char *p) {
    struct stat st;
    return p && p[0] && stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static int mkdir_p(const char *path) {
    char tmp[CNET_MD_MEM_PATH];
    char *p;
    if (!path || !path[0]) return -1;
    if ((size_t)snprintf(tmp, sizeof tmp, "%s", path) >= sizeof tmp) return -1;
    for (p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        if (mkdir(tmp, 0755) != 0 && !is_dir(tmp)) return -1;
        *p = '/';
    }
    if (mkdir(tmp, 0755) != 0 && !is_dir(tmp)) return -1;
    return 0;
}

static int contains_ci(const char *hay, const char *needle) {
    size_t n, h, i, j;
    if (!hay || !needle || !needle[0]) return 1; /* empty substr = last note */
    n = strlen(needle);
    h = strlen(hay);
    if (n > h) return 0;
    for (i = 0; i + n <= h; i++) {
        for (j = 0; j < n; j++) {
            if (tolower((unsigned char)hay[i + j]) !=
                tolower((unsigned char)needle[j]))
                break;
        }
        if (j == n) return 1;
    }
    return 0;
}

int cnet_md_mem_path(char *out, size_t cap) {
    const char *e, *obs, *min;
    char dir[CNET_MD_MEM_PATH];
    char *slash;
    if (!out || !cap) return -1;
    out[0] = 0;
    e = getenv("CNET_MEMORY_MD");
    obs = getenv("OBSIDIAN_VAULT_PATH");
    min = getenv("CNET_MINIMAL_ROOT");
    if (e && e[0]) {
        if ((size_t)snprintf(out, cap, "%s", e) >= cap) return -1;
    } else if (obs && obs[0]) {
        if ((size_t)snprintf(out, cap, "%s/CNET/Marble-memory.md", obs) >= cap)
            return -1;
    } else if (min && min[0]) {
        if ((size_t)snprintf(out, cap, "%s/var/marble_memory.md", min) >= cap)
            return -1;
    } else {
        if ((size_t)snprintf(out, cap, "var/marble_memory.md") >= cap) return -1;
    }
    if ((size_t)snprintf(dir, sizeof dir, "%s", out) >= sizeof dir) return -1;
    slash = strrchr(dir, '/');
    if (slash) {
        *slash = 0;
        if (dir[0] && mkdir_p(dir) != 0) return -1;
    }
    return 0;
}

int cnet_md_mem_store(const char *peer, const char *note) {
    char path[CNET_MD_MEM_PATH];
    FILE *f;
    time_t now;
    struct tm tm;
    char ts[40];
    const char *p;
    if (!note || !note[0]) return -1;
    while (*note == ' ' || *note == '\t') note++;
    if (!note[0]) return -1;
    if (cnet_md_mem_path(path, sizeof path) != 0) return -1;
    f = fopen(path, "a");
    if (!f) return -1;
    now = time(NULL);
    if (!gmtime_r(&now, &tm)) {
        fclose(f);
        return -1;
    }
    strftime(ts, sizeof ts, "%Y-%m-%dT%H:%MZ", &tm);
    if (ftell(f) == 0)
        fputs("# Marble memory\n\nNotes are not CERT. auto_cert=false.\n\n", f);
    fprintf(f, "## %s %s\n", ts, peer && peer[0] ? peer : "-");
    for (p = note; *p; p++) {
        if (*p == '\n' || *p == '\r')
            fputc(' ', f);
        else
            fputc(*p, f);
    }
    fputc('\n', f);
    fputc('\n', f);
    fclose(f);
    return 0;
}

int cnet_md_mem_recall(const char *substr, char *out, size_t cap) {
    char path[CNET_MD_MEM_PATH];
    char line[1024];
    char last[CNET_MD_MEM_NOTE];
    FILE *f;
    int hit = 0;
    if (!out || !cap) return -1;
    out[0] = 0;
    last[0] = 0;
    if (cnet_md_mem_path(path, sizeof path) != 0) return -1;
    f = fopen(path, "r");
    if (!f) return 1;
    while (fgets(line, sizeof line, f)) {
        size_t n;
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = 0;
        if (!n) continue;
        if (strncmp(line, "GAP:", 4) == 0) continue; /* gaps are not knowledge */
        if (!contains_ci(line, substr)) continue;
        snprintf(last, sizeof last, "%s", line);
        hit = 1;
    }
    fclose(f);
    if (!hit) return 1;
    snprintf(out, cap, "%s", last);
    return 0;
}

static int md_has_gap(const char *query) {
    char path[CNET_MD_MEM_PATH];
    char line[1024];
    char needle[220];
    FILE *f;
    if (!query || !query[0]) return 0;
    if (cnet_md_mem_path(path, sizeof path) != 0) return 0;
    snprintf(needle, sizeof needle, "GAP: %.180s", query);
    f = fopen(path, "r");
    if (!f) return 0;
    while (fgets(line, sizeof line, f)) {
        if (contains_ci(line, needle)) {
            fclose(f);
            return 1;
        }
    }
    fclose(f);
    return 0;
}

int cnet_md_mem_gap(const char *peer, const char *query, const char *draft) {
    char buf[CNET_MD_MEM_NOTE];
    const char *q = query ? query : "";
    (void)draft; /* lookup draft is this-turn mouth, never stored as a fact */
    while (*q == ' ' || *q == '\t') q++;
    if (!q[0] || strlen(q) < 4) return -1;
    if (md_has_gap(q)) return 0;
    snprintf(buf, sizeof buf, "GAP: %.200s | unsealed", q);
    return cnet_md_mem_store(peer, buf);
}

int cnet_md_mem_list_gaps(char *out, size_t cap) {
    char path[CNET_MD_MEM_PATH];
    char line[1024];
    char last[5][CNET_MD_MEM_NOTE];
    FILE *f;
    int n = 0, i;
    size_t o = 0;
    if (!out || !cap) return -1;
    out[0] = 0;
    if (cnet_md_mem_path(path, sizeof path) != 0) return -1;
    f = fopen(path, "r");
    if (!f) return 1;
    while (fgets(line, sizeof line, f)) {
        size_t L;
        if (strncmp(line, "GAP:", 4) != 0) continue;
        L = strlen(line);
        while (L > 0 && (line[L - 1] == '\n' || line[L - 1] == '\r'))
            line[--L] = 0;
        if (n < 5) {
            snprintf(last[n], sizeof last[n], "%s", line);
            n++;
        } else {
            memmove(last[0], last[1], sizeof last[0] * 4);
            snprintf(last[4], sizeof last[4], "%s", line);
        }
    }
    fclose(f);
    if (!n) return 1;
    for (i = 0; i < n && o + 8 < cap; i++)
        o += (size_t)snprintf(out + o, cap - o, "%s%s", i ? " ;; " : "", last[i]);
    return 0;
}

int cnet_md_mem_selftest(void) {
    char path[CNET_MD_MEM_PATH];
    char got[CNET_MD_MEM_NOTE];
    int fail = 0, n = 0;
    const char *dir = "/tmp/cnet_md_mem_selftest";
#define T(ok, m)                                                               \
    do {                                                                       \
        n++;                                                                   \
        printf("  %-56s %s\n", m, (ok) ? "PASS" : "FAIL");                     \
        if (!(ok)) fail++;                                                     \
    } while (0)

    printf("=== md memory (store/recall, never CERT) ===\n");
    mkdir(dir, 0755);
    snprintf(path, sizeof path, "%s/Marble-memory.md", dir);
    unlink(path);
    setenv("CNET_MEMORY_MD", path, 1);
    unsetenv("OBSIDIAN_VAULT_PATH");
    T(cnet_md_mem_path(got, sizeof got) == 0 && !strcmp(got, path),
      "explicit CNET_MEMORY_MD");
    T(cnet_md_mem_store("discord", "coffee in the morning") == 0, "store note");
    T(cnet_md_mem_recall("coffee", got, sizeof got) == 0 &&
          strstr(got, "coffee") != NULL,
      "recall hits stored note");
    T(cnet_md_mem_recall("xyzzy-no-such", got, sizeof got) == 1, "recall miss");
    T(cnet_md_mem_store(NULL, "") != 0, "empty note refused");
    T(cnet_md_mem_gap("discord", "what is json", "a data format") == 0, "gap store");
    T(cnet_md_mem_list_gaps(got, sizeof got) == 0 && strstr(got, "GAP:") &&
          strstr(got, "json") != NULL,
      "list open gaps");
    T(cnet_md_mem_recall("json", got, sizeof got) == 1,
      "gap is not knowledge recall");
    T(cnet_md_mem_gap("discord", "what is json", "again") == 0, "gap idempotent");
    T(cnet_md_mem_recall("coffee", got, sizeof got) == 0 &&
          strstr(got, "coffee") != NULL,
      "knowledge recall still hits notes");
    unsetenv("CNET_MEMORY_MD");
    printf("\nchecks=%d failures=%d\n", n, fail);
    if (fail) {
        printf("MD_MEMORY_FAIL\n");
        return 1;
    }
    printf("MD_MEMORY_PASS\n");
    return 0;
#undef T
}
