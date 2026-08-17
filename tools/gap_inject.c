/* Pick genuinely-teachable window tokens and queue them as NO_PLAN gaps.
 *
 * Usage:
 *   gap_inject [--n N] [--k K] [--base PATH] [--window PATH] [--dry-run]
 *
 * Env defaults: CNET_ROOT, CNET_AUTOTEACH_INJECT, CNET_CURIOSITY_K,
 *               CNET_BASE_PATH, CNET_WINDOW_FILE / CNET_RESIDUAL_WINDOW,
 *               CNET_GAP_INBOX
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <time.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#define MKDIR(p) mkdir((p), 0755)
#endif

#define MAX_IDS 65536

static int env_int(const char *k, int d) {
    const char *v = getenv(k);
    return (v && v[0]) ? atoi(v) : d;
}

static const char *env_str(const char *k, const char *d) {
    const char *v = getenv(k);
    return (v && v[0]) ? v : d;
}

static int ensure_parent(const char *path) {
    char tmp[1024];
    size_t i, len;
    snprintf(tmp, sizeof tmp, "%s", path);
    len = strlen(tmp);
    for (i = 1; i < len; i++) {
        if (tmp[i] == '/' || tmp[i] == '\\') {
            char c = tmp[i];
            tmp[i] = 0;
            MKDIR(tmp);
            tmp[i] = c;
        }
    }
    return 0;
}

static int load_window(const char *path, int *ids, int maxn) {
    FILE *f;
    char tok[64];
    int n = 0;
    f = fopen(path, "r");
    if (!f) return 0;
    while (n < maxn && fscanf(f, "%63s", tok) == 1) {
        int all_digit = 1, i;
        for (i = 0; tok[i]; i++)
            if (!isdigit((unsigned char)tok[i])) { all_digit = 0; break; }
        if (all_digit && tok[0]) ids[n++] = atoi(tok);
    }
    fclose(f);
    return n;
}

/* Match tk(\d+)q\1 — token id where both sides equal. */
static int extract_tk_same(const char *line, int *out_id) {
    const char *p = line;
    while ((p = strstr(p, "tk")) != NULL) {
        const char *q = p + 2;
        int a = 0, b = 0;
        if (!isdigit((unsigned char)*q)) { p++; continue; }
        while (isdigit((unsigned char)*q)) a = a * 10 + (*q++ - '0');
        if (*q != 'q') { p++; continue; }
        q++;
        if (!isdigit((unsigned char)*q)) { p++; continue; }
        while (isdigit((unsigned char)*q)) b = b * 10 + (*q++ - '0');
        if (a == b && a >= 0) {
            *out_id = a;
            return 1;
        }
        p++;
    }
    return 0;
}

static int load_covered(const char *gaps_path, unsigned char *set, int max_id) {
    FILE *f;
    char line[4096];
    int lineno = 0, n = 0;
    memset(set, 0, (size_t)max_id + 1);
    f = fopen(gaps_path, "r");
    if (!f) return 0;
    while (fgets(line, sizeof line, f)) {
        /* Parse status without mutating `line` before tk extract — older code
           null-terminated fields in-place, so extract_tk_same only saw "0". */
        const char *p = line;
        char status[8];
        int si = 0, field = 0, tid;
        lineno++;
        if (lineno < 3) continue;
        status[0] = 0;
        while (*p && *p != '\n') {
            while (*p == ' ' || *p == '\t') p++;
            if (!*p || *p == '\n') break;
            if (field == 1) {
                si = 0;
                while (*p && *p != ' ' && *p != '\t' && *p != '\n' && si < 7)
                    status[si++] = *p++;
                status[si] = 0;
            } else {
                while (*p && *p != ' ' && *p != '\t' && *p != '\n') p++;
            }
            field++;
            if (field > 1) break;
        }
        if (status[0] != '2' || status[1] != '\0') continue;
        if (extract_tk_same(line, &tid) && tid >= 0 && tid <= max_id && !set[tid]) {
            set[tid] = 1;
            n++;
        }
    }
    fclose(f);
    return n;
}

static int load_pending(const char *inbox, unsigned char *set, int max_id) {
    FILE *f;
    char line[4096];
    int n = 0;
    f = fopen(inbox, "r");
    if (!f) return 0;
    while (fgets(line, sizeof line, f)) {
        int tid;
        if (extract_tk_same(line, &tid) && tid >= 0 && tid <= max_id) {
            if (!set[tid]) { set[tid] = 1; n++; }
            else set[tid] = 1;
        }
    }
    fclose(f);
    return n;
}

/* Fisher-Yates sample of first `take` from fresh[0..nfresh). */
static void sample_ids(int *fresh, int nfresh, int take) {
    int i;
    for (i = nfresh - 1; i > 0; i--) {
        int j = (int)(rand() % (unsigned)(i + 1));
        int t = fresh[i];
        fresh[i] = fresh[j];
        fresh[j] = t;
    }
    (void)take;
}

int main(int argc, char **argv) {
    const char *root = env_str("CNET_ROOT", ".");
    char default_base[1024], default_window[1024];
    const char *base_path, *window_path, *inbox_path;
    int n_inject = env_int("CNET_AUTOTEACH_INJECT", 4);
    int k = env_int("CNET_CURIOSITY_K", 3);
    int dry = 0, ai;
    int ids[MAX_IDS], fresh[MAX_IDS];
    int n_ids, n_fresh = 0, i, covered_n = 0;
    unsigned char *skip;
    int max_id = 0;
    char gaps_path[1100], inbox_buf[1100];
    FILE *out;
    int wrote = 0;
    const char *reason = "ok";
    int remaining = 0;

    snprintf(default_base, sizeof default_base, "%s/soul_gemma4v2_final.cnb", root);
    snprintf(default_window, sizeof default_window, "%s/english_window_256_bonsai_v2.txt", root);
    base_path = env_str("CNET_BASE_PATH", default_base);
    window_path = getenv("CNET_WINDOW_FILE");
    if (!window_path || !window_path[0])
        window_path = getenv("CNET_RESIDUAL_WINDOW");
    if (!window_path || !window_path[0])
        window_path = default_window;

    for (ai = 1; ai < argc; ai++) {
        if (strcmp(argv[ai], "--n") == 0 && ai + 1 < argc) n_inject = atoi(argv[++ai]);
        else if (strcmp(argv[ai], "--k") == 0 && ai + 1 < argc) k = atoi(argv[++ai]);
        else if (strcmp(argv[ai], "--base") == 0 && ai + 1 < argc) base_path = argv[++ai];
        else if (strcmp(argv[ai], "--window") == 0 && ai + 1 < argc) window_path = argv[++ai];
        else if (strcmp(argv[ai], "--dry-run") == 0) dry = 1;
        else {
            fprintf(stderr, "usage: %s [--n N] [--k K] [--base P] [--window P] [--dry-run]\n",
                    argv[0]);
            return 2;
        }
    }

    snprintf(gaps_path, sizeof gaps_path, "%s.gaps.txt", base_path);
    {
        const char *env_inbox = getenv("CNET_GAP_INBOX");
        if (env_inbox && env_inbox[0]) inbox_path = env_inbox;
        else {
            snprintf(inbox_buf, sizeof inbox_buf, "%s.inbox", base_path);
            inbox_path = inbox_buf;
        }
    }

    n_ids = load_window(window_path, ids, MAX_IDS);
    if (n_ids <= 0) {
        printf("AUTOTEACH_INJECT n=0 reason=empty_window covered=0 remaining=0 inbox=%s\n",
               inbox_path);
        return 0;
    }
    for (i = 0; i < n_ids; i++)
        if (ids[i] > max_id) max_id = ids[i];
    if (max_id < 1) max_id = 1;

    skip = (unsigned char *)calloc((size_t)max_id + 1, 1);
    if (!skip) return 1;
    load_covered(gaps_path, skip, max_id);
    load_pending(inbox_path, skip, max_id);
    for (i = 0; i <= max_id; i++) if (skip[i]) covered_n++;

    for (i = 0; i < n_ids; i++) {
        if (ids[i] >= 0 && ids[i] <= max_id && !skip[ids[i]])
            fresh[n_fresh++] = ids[i];
    }

    if (n_fresh == 0) {
        printf("AUTOTEACH_INJECT n=0 reason=window_exhausted covered=%d remaining=0 inbox=%s\n",
               covered_n, inbox_path);
        free(skip);
        return 0;
    }

    srand((unsigned)time(NULL) ^ (unsigned)(uintptr_t)&n_fresh);
    {
        int take = n_inject < n_fresh ? n_inject : n_fresh;
        sample_ids(fresh, n_fresh, take);
        remaining = n_fresh - take;
        if (!dry) {
            ensure_parent(inbox_path);
            out = fopen(inbox_path, "a");
            if (!out) {
                fprintf(stderr, "cannot open inbox %s\n", inbox_path);
                free(skip);
                return 1;
            }
        } else {
            out = NULL;
        }
        for (i = 0; i < take; i++) {
            char line[256];
            int t = fresh[i];
            snprintf(line, sizeof line, "NO_PLAN 1 %d 1 w_cur 1 %d %d tk%dq%d\n",
                     n_ids, n_ids, k, t, t);
            if (out) fputs(line, out);
            wrote++;
        }
        if (out) fclose(out);
    }

    (void)reason;
    printf("AUTOTEACH_INJECT n=%d reason=ok covered=%d remaining=%d inbox=%s\n",
           wrote, covered_n, remaining, inbox_path);
    free(skip);
    return 0;
}
