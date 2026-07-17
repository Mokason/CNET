#include "../include/cnet_curiosity.h"
#include "../include/nn.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Write one NO_PLAN line (same format as gap_inbox_note_no_plan). */
static int curiosity_note(const char *inbox_path, Port in, Port goal) {
    FILE *f;
    if (!inbox_path || !inbox_path[0]) return -1;
    f = fopen(inbox_path, "a");
    if (!f) return -1;
    fprintf(f, "NO_PLAN %d %zu %zu %s %d %zu %zu %s\n",
            (int)in.family, in.field_width, in.field_count,
            in.tag[0] ? in.tag : "-", (int)goal.family, goal.field_width,
            goal.field_count, goal.tag[0] ? goal.tag : "-");
    fclose(f);
    return 0;
}

void cnet_curiosity_config_defaults(CnetCuriosityConfig *c) {
    if (!c) return;
    memset(c, 0, sizeof *c);
    c->enabled = 0;
    c->max_per_hour = 24;
    c->max_per_tick = 2;
    c->k = 3;
    c->yield_if_open = 4;
    snprintf(c->window_path, sizeof c->window_path, "english_window_256.txt");
    snprintf(c->state_path, sizeof c->state_path, "curiosity_state.txt");
}

static int env_size_clamped(const char *e, long lo, long hi, size_t *out) {
    long v;
    if (!e || !e[0] || !out) return 0;
    v = atol(e);
    if (v < lo || (hi >= 0 && v > hi)) return 0;
    *out = (size_t)v;
    return 1;
}

static void env_copy_path(char *dst, size_t cap, const char *e) {
    if (!dst || !cap || !e || !e[0]) return;
    snprintf(dst, cap, "%s", e);
}

void cnet_curiosity_config_from_env(CnetCuriosityConfig *c) {
    const char *e;
    size_t v;
    if (!c) return;
    cnet_curiosity_config_defaults(c);
    e = getenv("CNET_CURIOSITY");
    c->enabled = (e && e[0] == '1') ? 1 : 0;
    if (env_size_clamped(getenv("CNET_CURIOSITY_MAX_PER_HOUR"), 0, -1, &v))
        c->max_per_hour = v;
    if (env_size_clamped(getenv("CNET_CURIOSITY_MAX_PER_TICK"), 1, -1, &v))
        c->max_per_tick = v;
    if (env_size_clamped(getenv("CNET_CURIOSITY_K"), 1, 16, &v))
        c->k = (int)v;
    if (env_size_clamped(getenv("CNET_CURIOSITY_YIELD_OPEN"), 0, -1, &v))
        c->yield_if_open = v;
    e = getenv("CNET_WINDOW_FILE");
    if (!e || !e[0]) e = getenv("CNET_RESIDUAL_WINDOW");
    env_copy_path(c->window_path, sizeof c->window_path, e);
    env_copy_path(c->state_path, sizeof c->state_path,
                  getenv("CNET_CURIOSITY_STATE"));
    env_copy_path(c->inbox_path, sizeof c->inbox_path, getenv("CNET_GAP_INBOX"));
}

/* Parse tk{N}q{N} token id from a unit name (e.g. acq_tk12q12). Returns -1. */
static int parse_tk_token_id(const char *name) {
    const char *p, *q;
    long a, b;
    char *end = NULL;
    if (!name) return -1;
    p = strstr(name, "tk");
    if (!p) return -1;
    p += 2;
    a = strtol(p, &end, 10);
    if (end == p || *end != 'q') return -1;
    q = end + 1;
    b = strtol(q, &end, 10);
    if (end == q || a != b || a < 0 || a > 2147483647L) return -1;
    return (int)a;
}

static int int_cmp(const void *x, const void *y) {
    int a = *(const int *)x, b = *(const int *)y;
    return (a > b) - (a < b);
}

/* One registry scan → sorted unique token ids; O(log n) covered checks. */
static int *build_covered_tokens(const PrimitiveRegistry *reg, size_t *n_out) {
    int *ids = NULL;
    size_t n = 0, cap = 0, i, w;
    if (n_out) *n_out = 0;
    if (!reg) return NULL;
    for (i = 0; i < reg->count; i++) {
        int tid = parse_tk_token_id(reg->entries[i].name);
        if (tid < 0) continue;
        if (n == cap) {
            size_t ncap = cap ? cap * 2 : 64;
            int *ni = (int *)realloc(ids, ncap * sizeof *ni);
            if (!ni) {
                free(ids);
                return NULL;
            }
            ids = ni;
            cap = ncap;
        }
        ids[n++] = tid;
    }
    if (n == 0) return ids;
    qsort(ids, n, sizeof *ids, int_cmp);
    w = 1;
    for (i = 1; i < n; i++)
        if (ids[i] != ids[w - 1]) ids[w++] = ids[i];
    if (n_out) *n_out = w;
    return ids;
}

static int covered_has(const int *sorted, size_t n, int tid) {
    size_t lo = 0, hi = n;
    if (!sorted || n == 0 || tid < 0) return 0;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (sorted[mid] < tid) lo = mid + 1;
        else if (sorted[mid] > tid) hi = mid;
        else return 1;
    }
    return 0;
}

int cnet_curiosity_token_covered(const PrimitiveRegistry *reg, int token_id) {
    /* Public API: single-token probe (still linear). Prefer set inside tick. */
    char needle[48];
    size_t i;
    if (!reg || token_id < 0) return 0;
    snprintf(needle, sizeof needle, "tk%dq%d", token_id, token_id);
    for (i = 0; i < reg->count; i++) {
        const char *n = reg->entries[i].name;
        if (n && strstr(n, needle) != NULL) return 1;
    }
    return 0;
}

static uint64_t hour_bucket(void) {
    return (uint64_t)(time(NULL) / 3600);
}

static size_t budget_remaining(const CnetCuriosityConfig *cfg) {
    FILE *f;
    uint64_t bucket = 0, cur = hour_bucket();
    size_t count = 0;
    char line[128];
    if (!cfg || cfg->max_per_hour == 0) return 0;
    f = fopen(cfg->state_path, "r");
    if (f) {
        while (fgets(line, sizeof line, f)) {
            if (sscanf(line, "hour=%llu", (unsigned long long *)&bucket) == 1)
                continue;
            if (sscanf(line, "count=%zu", &count) == 1) continue;
        }
        fclose(f);
    }
    if (bucket != cur) count = 0;
    if (count >= cfg->max_per_hour) return 0;
    return cfg->max_per_hour - count;
}

static void budget_consume(const CnetCuriosityConfig *cfg, size_t n) {
    FILE *f;
    uint64_t cur = hour_bucket();
    size_t count = 0;
    uint64_t bucket = 0;
    char line[128];
    if (!cfg || n == 0) return;
    f = fopen(cfg->state_path, "r");
    if (f) {
        while (fgets(line, sizeof line, f)) {
            sscanf(line, "hour=%llu", (unsigned long long *)&bucket);
            sscanf(line, "count=%zu", &count);
        }
        fclose(f);
    }
    if (bucket != cur) count = 0;
    count += n;
    f = fopen(cfg->state_path, "w");
    if (!f) return;
    fprintf(f, "hour=%llu\ncount=%zu\n", (unsigned long long)cur, count);
    fclose(f);
}

static int load_window(const char *path, int *ids, int cap) {
    FILE *f;
    char line[64];
    int n = 0;
    if (!path || !path[0] || !ids || cap <= 0) return 0;
    f = fopen(path, "r");
    if (!f) return 0;
    while (n < cap && fgets(line, sizeof line, f)) {
        char *e;
        long v = strtol(line, &e, 10);
        if (e != line && v >= 0) ids[n++] = (int)v;
    }
    fclose(f);
    return n;
}

int cnet_curiosity_tick(const CnetCuriosityConfig *cfg,
                        const PrimitiveRegistry *reg, size_t open_gaps,
                        CnetCuriosityReport *rep) {
    int ids[512];
    int n_win, i;
    size_t remain, to_emit, emitted = 0;
    Port in, goal;
    CnetCuriosityReport local;

    memset(&local, 0, sizeof local);
    if (rep) memset(rep, 0, sizeof *rep);
    if (!cfg || !cfg->enabled) {
        if (rep) *rep = local;
        return 0;
    }
    local.enabled = 1;

    if (cfg->yield_if_open > 0 && open_gaps >= cfg->yield_if_open) {
        local.skipped_busy = 1;
        if (rep) *rep = local;
        return 0;
    }
    if (!cfg->inbox_path[0]) {
        if (rep) *rep = local;
        return 0;
    }

    remain = budget_remaining(cfg);
    if (remain == 0) {
        local.skipped_budget = 1;
        if (rep) *rep = local;
        return 0;
    }
    to_emit = cfg->max_per_tick;
    if (to_emit > remain) to_emit = remain;

    n_win = load_window(cfg->window_path, ids, 512);
    if (n_win <= 0) {
        if (rep) *rep = local;
        return 0;
    }

    memset(&in, 0, sizeof in);
    memset(&goal, 0, sizeof goal);
    in.family = PORT_ONEHOT;
    in.field_width = (size_t)n_win;
    in.field_count = 1;
    snprintf(in.tag, sizeof in.tag, "w_cur");
    goal.family = PORT_ONEHOT;
    goal.field_width = (size_t)n_win;
    goal.field_count = (size_t)(cfg->k > 0 ? cfg->k : 3);

    /* Round-robin start from time so successive hours explore different slices.
       Covered check uses one registry scan + binary search (not per-token scan). */
    {
        size_t n_cov = 0;
        int *covered = build_covered_tokens(reg, &n_cov);
        int start = (int)(hour_bucket() % (uint64_t)n_win);
        for (i = 0; i < n_win && emitted < to_emit; i++) {
            int idx = (start + i) % n_win;
            int tid = ids[idx];
            local.candidates++;
            if (covered_has(covered, n_cov, tid) ||
                (!covered && cnet_curiosity_token_covered(reg, tid))) {
                local.skipped_covered++;
                continue;
            }
            snprintf(goal.tag, sizeof goal.tag, "tk%dq%d", tid, tid);
            if (curiosity_note(cfg->inbox_path, in, goal) == 0) emitted++;
        }
        free(covered);
    }

    if (emitted > 0) budget_consume(cfg, emitted);
    local.proposed = emitted;
    if (rep) *rep = local;
    return 0;
}
