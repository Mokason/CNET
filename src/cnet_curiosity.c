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

void cnet_curiosity_config_from_env(CnetCuriosityConfig *c) {
    const char *e;
    if (!c) return;
    cnet_curiosity_config_defaults(c);
    e = getenv("CNET_CURIOSITY");
    c->enabled = (e && e[0] == '1') ? 1 : 0;
    e = getenv("CNET_CURIOSITY_MAX_PER_HOUR");
    if (e && e[0]) {
        long v = atol(e);
        if (v >= 0) c->max_per_hour = (size_t)v;
    }
    e = getenv("CNET_CURIOSITY_MAX_PER_TICK");
    if (e && e[0]) {
        long v = atol(e);
        if (v >= 1) c->max_per_tick = (size_t)v;
    }
    e = getenv("CNET_CURIOSITY_K");
    if (e && e[0]) {
        long v = atol(e);
        if (v >= 1 && v <= 16) c->k = (int)v;
    }
    e = getenv("CNET_CURIOSITY_YIELD_OPEN");
    if (e && e[0]) {
        long v = atol(e);
        if (v >= 0) c->yield_if_open = (size_t)v;
    }
    e = getenv("CNET_WINDOW_FILE");
    if (!e || !e[0]) e = getenv("CNET_RESIDUAL_WINDOW");
    if (e && e[0])
        snprintf(c->window_path, sizeof c->window_path, "%s", e);
    e = getenv("CNET_CURIOSITY_STATE");
    if (e && e[0])
        snprintf(c->state_path, sizeof c->state_path, "%s", e);
    e = getenv("CNET_GAP_INBOX");
    if (e && e[0])
        snprintf(c->inbox_path, sizeof c->inbox_path, "%s", e);
}

int cnet_curiosity_token_covered(const PrimitiveRegistry *reg, int token_id) {
    char needle[48];
    size_t i;
    if (!reg || token_id < 0) return 0;
    snprintf(needle, sizeof needle, "tk%dq%d", token_id, token_id);
    for (i = 0; i < reg->count; i++) {
        const char *n = reg->entries[i].name;
        if (!n) continue;
        if (strstr(n, needle) != NULL) return 1;
        /* also goal tag style acq_tk… */
        if (strncmp(n, "acq_", 4) == 0 && strstr(n + 4, needle) != NULL)
            return 1;
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

    /* Round-robin start from time so successive hours explore different slices. */
    {
        int start = (int)(hour_bucket() % (uint64_t)n_win);
        for (i = 0; i < n_win && emitted < to_emit; i++) {
            int idx = (start + i) % n_win;
            int tid = ids[idx];
            local.candidates++;
            if (cnet_curiosity_token_covered(reg, tid)) {
                local.skipped_covered++;
                continue;
            }
            snprintf(goal.tag, sizeof goal.tag, "tk%dq%d", tid, tid);
            if (curiosity_note(cfg->inbox_path, in, goal) == 0) emitted++;
        }
    }

    if (emitted > 0) budget_consume(cfg, emitted);
    local.proposed = emitted;
    if (rep) *rep = local;
    return 0;
}
