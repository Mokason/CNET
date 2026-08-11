/* Curriculum / soak probe short-circuit — fail-closed before Teacher.
 * Zero-alloc match on fixed pattern table + optional config file.
 * Never used as CERT content; only suppresses live LLM burn.
 */
#ifndef CNET_PROBE_SHORTCIRCUIT_H
#define CNET_PROBE_SHORTCIRCUIT_H

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CNET_PROBE_MAX 64
#define CNET_PROBE_PAT 96

typedef struct {
    char pat[CNET_PROBE_PAT];
    int active;
} CnetProbeRule;

typedef struct {
    CnetProbeRule rules[CNET_PROBE_MAX];
    int n;
    int loaded_file;
} CnetProbeTable;

/* Built-in soak/curriculum noise signatures */
static const char *const k_probe_builtin[] = {
    "novel fact",
    "mystic ooze",
    "zz99",
    "zzqq",
    "zz unknown",
    "zz_ood",
    "brand new teacher only",
    "say pong only",
    "say hello teacher",
    "autonomous cycle probe",
    "deploy probe unique",
    "quantum flute",
    "galactic overmind",
    "orbital printer",
    "alien haskell",
    "banjo merge",
    "soup kitchen narrative",
    "teacher only query",
    NULL,
};

static void cnet_probe_tolower(char *d, size_t cap, const char *s) {
    size_t i;
    if (!d || !cap) return;
    for (i = 0; s && s[i] && i + 1 < cap; i++)
        d[i] = (char)tolower((unsigned char)s[i]);
    d[i < cap ? i : cap - 1] = 0;
}

static void cnet_probe_table_init(CnetProbeTable *T) {
    int i;
    if (!T) return;
    memset(T, 0, sizeof *T);
    for (i = 0; k_probe_builtin[i] && T->n < CNET_PROBE_MAX; i++) {
        snprintf(T->rules[T->n].pat, sizeof T->rules[T->n].pat, "%s",
                 k_probe_builtin[i]);
        T->rules[T->n].active = 1;
        T->n++;
    }
}

/* Optional lines: one substring per line; # comments */
static int cnet_probe_table_load(CnetProbeTable *T, const char *path) {
    FILE *f;
    char line[128];
    if (!T || !path || !path[0]) return -1;
    if (T->n <= 0) cnet_probe_table_init(T);
    f = fopen(path, "r");
    if (!f) return -1;
    while (fgets(line, sizeof line, f) && T->n < CNET_PROBE_MAX) {
        char *p = line;
        size_t n;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == 0 || *p == '#' || *p == '\n') continue;
        n = strlen(p);
        while (n && (p[n - 1] == '\n' || p[n - 1] == '\r' || p[n - 1] == ' '))
            p[--n] = 0;
        if (n < 2) continue;
        snprintf(T->rules[T->n].pat, sizeof T->rules[T->n].pat, "%s", p);
        T->rules[T->n].active = 1;
        T->n++;
        T->loaded_file = 1;
    }
    fclose(f);
    return 0;
}

/* Returns matching pattern or NULL. out_pat may be NULL. */
static const char *cnet_probe_match(const CnetProbeTable *T, const char *query,
                                    char *out_pat, size_t out_cap) {
    char q[512];
    int i;
    if (!query || !query[0]) return NULL;
    cnet_probe_tolower(q, sizeof q, query);
    if (!T || T->n <= 0) {
        /* builtin-only fallback without table */
        for (i = 0; k_probe_builtin[i]; i++) {
            char p[CNET_PROBE_PAT];
            cnet_probe_tolower(p, sizeof p, k_probe_builtin[i]);
            if (strstr(q, p)) {
                if (out_pat && out_cap)
                    snprintf(out_pat, out_cap, "%s", k_probe_builtin[i]);
                return k_probe_builtin[i];
            }
        }
        return NULL;
    }
    for (i = 0; i < T->n; i++) {
        char p[CNET_PROBE_PAT];
        if (!T->rules[i].active || !T->rules[i].pat[0]) continue;
        cnet_probe_tolower(p, sizeof p, T->rules[i].pat);
        if (strstr(q, p)) {
            if (out_pat && out_cap)
                snprintf(out_pat, out_cap, "%s", T->rules[i].pat);
            return T->rules[i].pat;
        }
    }
    return NULL;
}

#ifdef __cplusplus
}
#endif

#endif /* CNET_PROBE_SHORTCIRCUIT_H */
