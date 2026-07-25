/* CNET native self-direction governor (C).
 *
 * Autonomy loop: charter + evolving scoreboard → pick goals → hire muscles.
 * Not a queue poller.
 *
 *   ./bin/cnet_governor              one cycle
 *   ./bin/cnet_governor --daemon 900 loop every 900s
 *   ./bin/cnet_governor --dry-run
 *   ./bin/cnet_governor --test
 *   make governor
 *
 * Env:
 *   CNET_ROOT, CNET_BASE_PATH, CNET_RESIDUAL_HTTP, CNET_RESIDUAL_WINDOW,
 *   CNET_GOVERNOR_CHARTER, CNET_GOVERNOR_DIR (logs/governor)
 */
#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MAX_GOALS 32
#define MAX_ACTIONS 8
#define MAX_ID 64
#define MAX_WHEN 256
#define MAX_PATH 512
#define HIST_CAP 64

typedef struct {
    char id[MAX_ID];
    int priority;
    char when[MAX_WHEN];
    char actions[MAX_ACTIONS][MAX_ID];
    int n_actions;
} Goal;

typedef struct {
    int version;
    int max_tasks;
    int budget_sec;
    Goal goals[MAX_GOALS];
    int n_goals;
} Charter;

typedef struct {
    /* live */
    int bonsai_ok, lane_ok;
    int open_gaps, deferred_oracle, closed_gaps;
    long fault_lines, units_proxy, base_size;
    int lora_files, hyb_struct;
    double hours_since_peft, hours_since_mine, hours_since_procedure;
    /* evolved */
    int cycle, plateau;
    double d_open_gaps, d_closed_gaps, d_fault_lines, d_units_proxy;
    double rate_closed_per_h, rate_units_per_h, rate_fault_per_h;
    double ewma_open, ewma_deferred, ewma_fault, ewma_units;
    double backlog_pressure, learning_velocity, teacher_uptime;
    double goal_health_drain, goal_health_peft, goal_health_mine, goal_health_coverage;
    double dt_h;
    time_t ts_unix;
} Scoreboard;

typedef struct {
    char path_root[MAX_PATH];
    char path_base[MAX_PATH];
    char path_charter[MAX_PATH];
    char path_govdir[MAX_PATH];
    char path_window[MAX_PATH];
    char path_fault[MAX_PATH];
    char path_lora[MAX_PATH];
    char path_inbox[MAX_PATH];
    char path_gaps[MAX_PATH];
    char http[128];
    int inject_n;
    /* state */
    int cycle;
    double last_peft, last_mine, last_procedure, last_cycle;
    double gh_drain, gh_peft, gh_mine, gh_coverage, gh_break;
    double ewma_open, ewma_deferred, ewma_fault, ewma_units;
    double ewma_vel, ewma_uptime;
    /* last core for deltas */
    int has_prev;
    Scoreboard prev;
} GovCtx;

/* ---------- utils ---------- */
static const char *env_or(const char *k, const char *d) {
    const char *v = getenv(k);
    return (v && v[0]) ? v : d;
}

static int ensure_dir(const char *path) {
    char tmp[MAX_PATH];
    size_t len = strlen(path);
    if (len >= sizeof tmp) return -1;
    memcpy(tmp, path, len + 1);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    return mkdir(tmp, 0755);
}

static int file_exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0;
}

static long file_size(const char *p) {
    struct stat st;
    if (stat(p, &st) != 0) return 0;
    return (long)st.st_size;
}

static long count_lines(const char *p) {
    FILE *f = fopen(p, "r");
    long n = 0;
    int c;
    if (!f) return 0;
    while ((c = fgetc(f)) != EOF)
        if (c == '\n') n++;
    fclose(f);
    return n;
}

static int systemctl_active(const char *unit) {
    char cmd[256];
    snprintf(cmd, sizeof cmd, "systemctl --user is-active --quiet %s", unit);
    return system(cmd) == 0 ? 1 : 0;
}

static int http_ok(const char *base) {
    char cmd[512];
    snprintf(cmd, sizeof cmd,
             "curl -sf --max-time 4 '%s/v1/models' >/dev/null 2>&1", base);
    return system(cmd) == 0 ? 1 : 0;
}

static double now_unix(void) {
    return (double)time(NULL);
}

static double hours_since(double t) {
    if (t <= 0) return 99.0;
    double h = (now_unix() - t) / 3600.0;
    return h < 0 ? 99.0 : h;
}

static double ewma(double prev, double x, double a) {
    if (prev == 0.0 && x != 0.0) return x;
    return a * x + (1.0 - a) * prev;
}

static int count_bytes(const char *path, const char *needle) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 400L * 1024 * 1024) {
        fclose(f);
        return 0;
    }
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return 0;
    }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf);
        fclose(f);
        return 0;
    }
    fclose(f);
    buf[sz] = 0;
    int n = 0;
    size_t nl = strlen(needle);
    for (char *p = buf; (p = strstr(p, needle)) != NULL; p += nl) n++;
    free(buf);
    return n;
}

static void gap_counts(const char *path, int *open_n, int *def_n, int *closed_n) {
    *open_n = *def_n = *closed_n = 0;
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[1024];
    int lineno = 0;
    while (fgets(line, sizeof line, f)) {
        lineno++;
        if (lineno <= 2) continue;
        if (line[0] == '#' || strncmp(line, "CNET_", 5) == 0) continue;
        int waiting = (strstr(line, "waiting_oracle") != NULL ||
                       strstr(line, "waiting_charter") != NULL);
        char copy[1024];
        snprintf(copy, sizeof copy, "%s", line);
        char *toks[16];
        int nt = 0;
        char *s = copy;
        while (nt < 16) {
            while (*s && isspace((unsigned char)*s)) s++;
            if (!*s || *s == '\n') break;
            toks[nt++] = s;
            while (*s && !isspace((unsigned char)*s)) s++;
            if (*s) *s++ = 0;
        }
        if (nt < 2) continue;
        if (waiting) {
            (*def_n)++;
        } else if (strcmp(toks[1], "2") == 0) {
            (*closed_n)++;
        } else if (strcmp(toks[1], "1") == 0) {
            (*open_n)++;
        } else if (isdigit((unsigned char)toks[0][0])) {
            (*open_n)++;
        }
    }
    fclose(f);
}

static int count_dir_files(const char *dir) {
    char cmd[MAX_PATH + 64];
    snprintf(cmd, sizeof cmd, "ls -1 '%s' 2>/dev/null | wc -l", dir);
    FILE *p = popen(cmd, "r");
    int n = 0;
    if (!p) return 0;
    if (fscanf(p, "%d", &n) != 1) n = 0;
    pclose(p);
    return n;
}

/* ---------- expression eval (safe subset) ---------- */
typedef struct {
    const char *s;
    const Scoreboard *sb;
    const char *err;
} Expr;

static void skip_ws(Expr *e) {
    while (*e->s && isspace((unsigned char)*e->s)) e->s++;
}

static double eval_or(Expr *e);

static int match(Expr *e, const char *op) {
    skip_ws(e);
    size_t n = strlen(op);
    if (strncmp(e->s, op, n) == 0) {
        e->s += n;
        return 1;
    }
    return 0;
}

static double sb_get(const Scoreboard *sb, const char *name) {
    if (strcmp(name, "bonsai_ok") == 0) return sb->bonsai_ok;
    if (strcmp(name, "lane_ok") == 0) return sb->lane_ok;
    if (strcmp(name, "open_gaps") == 0) return sb->open_gaps;
    if (strcmp(name, "deferred_oracle") == 0) return sb->deferred_oracle;
    if (strcmp(name, "closed_gaps") == 0) return sb->closed_gaps;
    if (strcmp(name, "fault_lines") == 0) return (double)sb->fault_lines;
    if (strcmp(name, "lora_files") == 0) return sb->lora_files;
    if (strcmp(name, "units_proxy") == 0) return (double)sb->units_proxy;
    if (strcmp(name, "hours_since_peft") == 0) return sb->hours_since_peft;
    if (strcmp(name, "hours_since_mine") == 0) return sb->hours_since_mine;
    if (strcmp(name, "hours_since_procedure") == 0) return sb->hours_since_procedure;
    if (strcmp(name, "backlog_pressure") == 0) return sb->backlog_pressure;
    if (strcmp(name, "learning_velocity") == 0) return sb->learning_velocity;
    if (strcmp(name, "plateau") == 0) return sb->plateau;
    if (strcmp(name, "teacher_uptime") == 0) return sb->teacher_uptime;
    if (strcmp(name, "goal_health_drain") == 0) return sb->goal_health_drain;
    if (strcmp(name, "goal_health_peft") == 0) return sb->goal_health_peft;
    if (strcmp(name, "true") == 0) return 1.0;
    if (strcmp(name, "false") == 0) return 0.0;
    return 0.0;
}

static double eval_primary(Expr *e) {
    skip_ws(e);
    if (match(e, "(")) {
        double v = eval_or(e);
        match(e, ")");
        return v;
    }
    if (isdigit((unsigned char)*e->s) ||
        (*e->s == '.' && isdigit((unsigned char)e->s[1]))) {
        char *end = NULL;
        double v = strtod(e->s, &end);
        e->s = end;
        return v;
    }
    if (isalpha((unsigned char)*e->s) || *e->s == '_') {
        char name[64];
        int i = 0;
        while ((isalnum((unsigned char)*e->s) || *e->s == '_') && i < 63)
            name[i++] = *e->s++;
        name[i] = 0;
        return sb_get(e->sb, name);
    }
    e->err = "bad primary";
    return 0;
}

static double eval_unary(Expr *e) {
    skip_ws(e);
    if (match(e, "-")) return -eval_unary(e);
    if (match(e, "!")) return eval_unary(e) == 0.0 ? 1.0 : 0.0;
    return eval_primary(e);
}

static double eval_mul(Expr *e) {
    double v = eval_unary(e);
    for (;;) {
        if (match(e, "*")) v *= eval_unary(e);
        else if (match(e, "/")) {
            double r = eval_unary(e);
            v = (r == 0) ? 0 : v / r;
        } else break;
    }
    return v;
}

static double eval_add(Expr *e) {
    double v = eval_mul(e);
    for (;;) {
        if (match(e, "+")) v += eval_mul(e);
        else if (match(e, "-")) v -= eval_mul(e);
        else break;
    }
    return v;
}

static double eval_cmp(Expr *e) {
    double v = eval_add(e);
    for (;;) {
        if (match(e, "==")) v = (fabs(v - eval_add(e)) < 1e-12) ? 1.0 : 0.0;
        else if (match(e, "!=")) v = (fabs(v - eval_add(e)) >= 1e-12) ? 1.0 : 0.0;
        else if (match(e, "<=")) v = (v <= eval_add(e)) ? 1.0 : 0.0;
        else if (match(e, ">=")) v = (v >= eval_add(e)) ? 1.0 : 0.0;
        else if (match(e, "<")) v = (v < eval_add(e)) ? 1.0 : 0.0;
        else if (match(e, ">")) v = (v > eval_add(e)) ? 1.0 : 0.0;
        else break;
    }
    return v;
}

static double eval_and(Expr *e) {
    double v = eval_cmp(e);
    while (match(e, "and") || match(e, "&&")) {
        double r = eval_cmp(e);
        v = (v != 0.0 && r != 0.0) ? 1.0 : 0.0;
    }
    return v;
}

static double eval_or(Expr *e) {
    double v = eval_and(e);
    while (match(e, "or") || match(e, "||")) {
        double r = eval_and(e);
        v = (v != 0.0 || r != 0.0) ? 1.0 : 0.0;
    }
    return v;
}

static int eval_when(const char *expr, const Scoreboard *sb) {
    if (!expr || !expr[0]) return 0;
    if (strcmp(expr, "true") == 0) return 1;
    if (strcmp(expr, "false") == 0) return 0;
    Expr e = {.s = expr, .sb = sb, .err = NULL};
    double v = eval_or(&e);
    skip_ws(&e);
    if (e.err || *e.s) return 0;
    return v != 0.0;
}

/* ---------- charter parse ---------- */
static void trim(char *s) {
    char *a = s;
    while (*a && isspace((unsigned char)*a)) a++;
    if (a != s) memmove(s, a, strlen(a) + 1);
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) s[--n] = 0;
}

static int parse_charter(const char *path, Charter *c) {
    memset(c, 0, sizeof *c);
    c->max_tasks = 3;
    c->budget_sec = 900;
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[512];
    Goal *g = NULL;
    while (fgets(line, sizeof line, f)) {
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        trim(line);
        if (!line[0]) continue;
        if (strncmp(line, "version:", 8) == 0) {
            c->version = atoi(line + 8);
        } else if (strncmp(line, "max_tasks_per_cycle:", 20) == 0) {
            c->max_tasks = atoi(line + 20);
        } else if (strncmp(line, "budget_sec:", 11) == 0) {
            c->budget_sec = atoi(line + 11);
        } else if (strncmp(line, "- id:", 5) == 0 ||
                   strncmp(line, "- id: ", 6) == 0) {
            if (c->n_goals >= MAX_GOALS) break;
            g = &c->goals[c->n_goals++];
            memset(g, 0, sizeof *g);
            const char *id = strstr(line, "id:");
            if (id) {
                id += 3;
                while (*id && isspace((unsigned char)*id)) id++;
                snprintf(g->id, sizeof g->id, "%s", id);
                trim(g->id);
            }
        } else if (g && strncmp(line, "priority:", 9) == 0) {
            g->priority = atoi(line + 9);
        } else if (g && strncmp(line, "when:", 5) == 0) {
            const char *w = line + 5;
            while (*w && isspace((unsigned char)*w)) w++;
            if (*w == '"') {
                w++;
                snprintf(g->when, sizeof g->when, "%s", w);
                char *q = strchr(g->when, '"');
                if (q) *q = 0;
            } else {
                snprintf(g->when, sizeof g->when, "%s", w);
            }
            trim(g->when);
        } else if (g && strncmp(line, "actions:", 8) == 0) {
            char *p = strchr(line, '[');
            if (!p) continue;
            p++;
            char *end = strchr(p, ']');
            if (end) *end = 0;
            while (*p && g->n_actions < MAX_ACTIONS) {
                while (*p && (isspace((unsigned char)*p) || *p == ',')) p++;
                if (!*p) break;
                char *start = p;
                while (*p && *p != ',' && !isspace((unsigned char)*p)) p++;
                char save = *p;
                *p = 0;
                snprintf(g->actions[g->n_actions++], MAX_ID, "%s", start);
                *p = save;
            }
        }
    }
    fclose(f);
    return c->n_goals > 0 ? 0 : -1;
}

/* ---------- state load/save ---------- */
static void load_state(GovCtx *ctx) {
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s/state.json", ctx->path_govdir);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char buf[4096];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    /* tiny key scrape */
    char *p;
    if ((p = strstr(buf, "\"cycle\""))) ctx->cycle = atoi(strchr(p, ':') + 1);
    if ((p = strstr(buf, "\"last_peft_unix\"")))
        ctx->last_peft = atof(strchr(p, ':') + 1);
    if ((p = strstr(buf, "\"last_mine_unix\"")))
        ctx->last_mine = atof(strchr(p, ':') + 1);
    if ((p = strstr(buf, "\"last_procedure_unix\"")))
        ctx->last_procedure = atof(strchr(p, ':') + 1);
    if ((p = strstr(buf, "\"last_cycle_unix\"")))
        ctx->last_cycle = atof(strchr(p, ':') + 1);
    if ((p = strstr(buf, "\"drain_open_gaps\"")))
        ctx->gh_drain = atof(strchr(p, ':') + 1);
    else ctx->gh_drain = 0.5;
    if ((p = strstr(buf, "\"peft_jtc\""))) ctx->gh_peft = atof(strchr(p, ':') + 1);
    else ctx->gh_peft = 0.5;
    if ((p = strstr(buf, "\"structure_mine\"")))
        ctx->gh_mine = atof(strchr(p, ':') + 1);
    else ctx->gh_mine = 0.5;
    if ((p = strstr(buf, "\"coverage_curiosity\"")))
        ctx->gh_coverage = atof(strchr(p, ':') + 1);
    else ctx->gh_coverage = 0.5;
    if ((p = strstr(buf, "\"break_plateau\"")))
        ctx->gh_break = atof(strchr(p, ':') + 1);
    else ctx->gh_break = 0.5;
}

static void load_ewma(GovCtx *ctx) {
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s/scoreboard_ewma.json", ctx->path_govdir);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char buf[2048];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    char *p;
    if ((p = strstr(buf, "\"ewma_open\""))) ctx->ewma_open = atof(strchr(p, ':') + 1);
    if ((p = strstr(buf, "\"ewma_deferred\"")))
        ctx->ewma_deferred = atof(strchr(p, ':') + 1);
    if ((p = strstr(buf, "\"ewma_fault\"")))
        ctx->ewma_fault = atof(strchr(p, ':') + 1);
    if ((p = strstr(buf, "\"ewma_units\"")))
        ctx->ewma_units = atof(strchr(p, ':') + 1);
    if ((p = strstr(buf, "\"learning_velocity\"")))
        ctx->ewma_vel = atof(strchr(p, ':') + 1);
    if ((p = strstr(buf, "\"teacher_uptime\"")))
        ctx->ewma_uptime = atof(strchr(p, ':') + 1);
}

static void load_prev_hist(GovCtx *ctx) {
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s/scoreboard_history.jsonl", ctx->path_govdir);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[2048], last[2048] = "";
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '{') snprintf(last, sizeof last, "%s", line);
    }
    fclose(f);
    if (!last[0]) return;
    ctx->has_prev = 1;
    memset(&ctx->prev, 0, sizeof ctx->prev);
    char *p;
    if ((p = strstr(last, "\"open_gaps\""))) ctx->prev.open_gaps = atoi(strchr(p, ':') + 1);
    if ((p = strstr(last, "\"deferred_oracle\"")))
        ctx->prev.deferred_oracle = atoi(strchr(p, ':') + 1);
    if ((p = strstr(last, "\"closed_gaps\"")))
        ctx->prev.closed_gaps = atoi(strchr(p, ':') + 1);
    if ((p = strstr(last, "\"fault_lines\"")))
        ctx->prev.fault_lines = atol(strchr(p, ':') + 1);
    if ((p = strstr(last, "\"units_proxy\"")))
        ctx->prev.units_proxy = atol(strchr(p, ':') + 1);
    if ((p = strstr(last, "\"ts_unix\"")))
        ctx->prev.ts_unix = (time_t)atol(strchr(p, ':') + 1);
    if ((p = strstr(last, "\"d_units_proxy\"")))
        ctx->prev.d_units_proxy = atof(strchr(p, ':') + 1);
    if ((p = strstr(last, "\"d_closed_gaps\"")))
        ctx->prev.d_closed_gaps = atof(strchr(p, ':') + 1);
}

/* ---------- collect + evolve ---------- */
static void collect_scoreboard(GovCtx *ctx, Scoreboard *sb) {
    memset(sb, 0, sizeof *sb);
    sb->ts_unix = time(NULL);
    sb->bonsai_ok =
        (systemctl_active("bonsai-server.service") && http_ok(ctx->http)) ? 1 : 0;
    sb->lane_ok = systemctl_active("cnet-personal-ai-lane.service");
    gap_counts(ctx->path_gaps, &sb->open_gaps, &sb->deferred_oracle, &sb->closed_gaps);
    sb->fault_lines = count_lines(ctx->path_fault);
    sb->lora_files = count_dir_files(ctx->path_lora);
    sb->units_proxy = count_bytes(ctx->path_base, "acq_") +
                      count_bytes(ctx->path_base, "json_toolcall") +
                      count_bytes(ctx->path_base, "hyb_struct");
    sb->hyb_struct = count_bytes(ctx->path_base, "hyb_struct");
    sb->base_size = file_size(ctx->path_base);
    sb->hours_since_peft = hours_since(ctx->last_peft);
    sb->hours_since_mine = hours_since(ctx->last_mine);
    sb->hours_since_procedure = hours_since(ctx->last_procedure);
    sb->goal_health_drain = ctx->gh_drain > 0 ? ctx->gh_drain : 0.5;
    sb->goal_health_peft = ctx->gh_peft > 0 ? ctx->gh_peft : 0.5;
    sb->goal_health_mine = ctx->gh_mine > 0 ? ctx->gh_mine : 0.5;
    sb->goal_health_coverage = ctx->gh_coverage > 0 ? ctx->gh_coverage : 0.5;
}

static void evolve_scoreboard(GovCtx *ctx, Scoreboard *sb, int dry) {
    sb->cycle = dry ? ctx->cycle : ctx->cycle + 1;

    if (ctx->has_prev) {
        sb->d_open_gaps = sb->open_gaps - ctx->prev.open_gaps;
        sb->d_closed_gaps = sb->closed_gaps - ctx->prev.closed_gaps;
        sb->d_fault_lines = (double)(sb->fault_lines - ctx->prev.fault_lines);
        sb->d_units_proxy = (double)(sb->units_proxy - ctx->prev.units_proxy);
        if (ctx->prev.ts_unix > 0)
            sb->dt_h = fmax(1e-3, (now_unix() - (double)ctx->prev.ts_unix) / 3600.0);
        else
            sb->dt_h = 0.25;
    } else {
        sb->dt_h = ctx->last_cycle > 0
                       ? fmax(1e-3, (now_unix() - ctx->last_cycle) / 3600.0)
                       : 0.25;
    }

    sb->rate_closed_per_h = sb->d_closed_gaps / sb->dt_h;
    sb->rate_fault_per_h = sb->d_fault_lines / sb->dt_h;
    sb->rate_units_per_h = sb->d_units_proxy / sb->dt_h;

    sb->ewma_open = ewma(ctx->ewma_open, (double)sb->open_gaps, 0.25);
    sb->ewma_deferred = ewma(ctx->ewma_deferred, (double)sb->deferred_oracle, 0.25);
    sb->ewma_fault = ewma(ctx->ewma_fault, (double)sb->fault_lines, 0.25);
    sb->ewma_units = ewma(ctx->ewma_units, (double)sb->units_proxy, 0.25);
    double up = (sb->bonsai_ok && sb->lane_ok) ? 1.0 : 0.0;
    sb->teacher_uptime = ewma(ctx->ewma_uptime, up, 0.15);

    sb->backlog_pressure = (double)(sb->open_gaps + sb->deferred_oracle);
    double inst =
        fmax(0.0, sb->rate_units_per_h) + 0.1 * fmax(0.0, sb->rate_closed_per_h);
    sb->learning_velocity = ewma(ctx->ewma_vel, inst, 0.3);

    /* plateau: if previous deltas were flat */
    if (ctx->has_prev &&
        ctx->prev.d_units_proxy <= 0 && ctx->prev.d_closed_gaps <= 2 &&
        sb->d_units_proxy <= 0 && sb->d_closed_gaps <= 2 && sb->bonsai_ok &&
        sb->lane_ok)
        sb->plateau = 1;
    else
        sb->plateau = 0;

    if (!dry) {
        ctx->cycle = sb->cycle;
        ctx->ewma_open = sb->ewma_open;
        ctx->ewma_deferred = sb->ewma_deferred;
        ctx->ewma_fault = sb->ewma_fault;
        ctx->ewma_units = sb->ewma_units;
        ctx->ewma_vel = sb->learning_velocity;
        ctx->ewma_uptime = sb->teacher_uptime;
        ctx->prev = *sb;
        ctx->has_prev = 1;
    }
}

/* ---------- pick goals ---------- */
typedef struct {
    int idx;
    int key_pri;
    double health;
} Rank;

static int rank_cmp(const void *a, const void *b) {
    const Rank *x = a, *y = b;
    if (x->key_pri != y->key_pri) return x->key_pri - y->key_pri;
    if (x->health < y->health) return 1;
    if (x->health > y->health) return -1;
    return 0;
}

static int pick_goals(const Charter *c, const Scoreboard *sb, int *out, int max_out) {
    Rank ranks[MAX_GOALS];
    int n = 0;
    for (int i = 0; i < c->n_goals; i++) {
        const Goal *g = &c->goals[i];
        int bias = 0;
        double health = 0.5;
        if (strcmp(g->id, "drain_open_gaps") == 0) health = sb->goal_health_drain;
        else if (strcmp(g->id, "peft_jtc") == 0) health = sb->goal_health_peft;
        else if (strcmp(g->id, "structure_mine") == 0) health = sb->goal_health_mine;
        else if (strcmp(g->id, "coverage_curiosity") == 0)
            health = sb->goal_health_coverage;
        if (sb->plateau && strcmp(g->id, "coverage_curiosity") == 0) bias = 2;
        if (sb->backlog_pressure >= 8 && strcmp(g->id, "drain_open_gaps") == 0)
            bias = -1;
        if (sb->plateau && strcmp(g->id, "structure_mine") == 0) bias = -1;
        if (sb->learning_velocity < 0.5 && strcmp(g->id, "peft_jtc") == 0 &&
            sb->fault_lines >= 64)
            bias = -1;
        ranks[n].idx = i;
        ranks[n].key_pri = g->priority + bias;
        ranks[n].health = health;
        n++;
    }
    qsort(ranks, (size_t)n, sizeof ranks[0], rank_cmp);
    int picked = 0;
    int max_n = c->max_tasks > 0 ? c->max_tasks : 3;
    if (max_n > max_out) max_n = max_out;
    for (int r = 0; r < n && picked < max_n; r++) {
        const Goal *g = &c->goals[ranks[r].idx];
        if (!eval_when(g->when, sb)) continue;
        if (strcmp(g->id, "health_hold") == 0 && picked > 0) continue;
        out[picked++] = ranks[r].idx;
        if (strcmp(g->id, "health_hold") == 0) break;
    }
    if (picked == 0) {
        for (int i = 0; i < c->n_goals; i++) {
            if (strcmp(c->goals[i].id, "health_hold") == 0) {
                out[0] = i;
                return 1;
            }
        }
    }
    return picked;
}

/* ---------- actions ---------- */
static int run_cmd(const char *cmd, int dry) {
    if (dry) {
        fprintf(stderr, "dry: %s\n", cmd);
        return 0;
    }
    int rc = system(cmd);
    return rc == 0 ? 0 : -1;
}

static int act_ensure_bonsai(GovCtx *ctx, int dry) {
    (void)ctx;
    if (dry) return 0;
    system("systemctl --user start bonsai-server.service >/dev/null 2>&1");
    sleep(1);
    return http_ok(ctx->http) ? 0 : -1;
}

static int act_ensure_lane(GovCtx *ctx, int dry) {
    (void)ctx;
    if (dry) return 0;
    system("systemctl --user start cnet-personal-ai-lane.service >/dev/null 2>&1");
    sleep(1);
    return systemctl_active("cnet-personal-ai-lane.service") ? 0 : -1;
}

static int act_inject_window_gaps(GovCtx *ctx, int dry) {
    if (!file_exists(ctx->path_window)) return -1;
    FILE *f = fopen(ctx->path_window, "r");
    if (!f) return -1;
    int ids[512];
    int nids = 0;
    while (nids < 512 && fscanf(f, "%d", &ids[nids]) == 1) nids++;
    fclose(f);
    if (nids <= 0) return -1;
    int W = nids;
    int k = 3;
    int n = ctx->inject_n < nids ? ctx->inject_n : nids;
    if (n < 1) n = 1;
    if (dry) return 0;
    /* simple LCG pick */
    unsigned seed = (unsigned)time(NULL) / 600u;
    FILE *out = fopen(ctx->path_inbox, "a");
    if (!out) return -1;
    for (int i = 0; i < n; i++) {
        seed = seed * 1103515245u + 12345u;
        int tid = ids[seed % (unsigned)nids];
        fprintf(out, "NO_PLAN 1 %d 1 w_cur 1 %d %d tk%dq%d\n", W, W, k, tid, tid);
    }
    fclose(out);
    return 0;
}

static int act_run_bin(GovCtx *ctx, const char *rel, double *stamp, int dry) {
    char bin[MAX_PATH];
    snprintf(bin, sizeof bin, "%s/bin/%s", ctx->path_root, rel);
    if (!file_exists(bin)) return -1;
    if (dry) return 0;
    char cmd[MAX_PATH * 2];
    snprintf(cmd, sizeof cmd,
             "cd '%s' && CNET_BASE_PATH='%s' CNET_FAULT_LOG='%s' "
             "CNET_LORA_STORE_DIR='%s' CNET_FAULT_DEDUPE=0 "
             "CNET_RESIDUAL_HTTP='%s' CNET_RESIDUAL_WINDOW='%s' "
             "CNET_STRUCTURE_EXPAND_N=32 CNET_SOUL_RESIDUAL_PREFER_HERMETIC=0 "
             "timeout 900 '%s' >>'%s/muscle.log' 2>&1",
             ctx->path_root, ctx->path_base, ctx->path_fault, ctx->path_lora,
             ctx->http, ctx->path_window, bin, ctx->path_govdir);
    int rc = run_cmd(cmd, 0);
    if (rc == 0 && stamp) *stamp = now_unix();
    return rc;
}

static int act_seed_procedures(GovCtx *ctx, int dry) {
    char script[MAX_PATH];
    snprintf(script, sizeof script, "%s/scripts/procedure_chunk_seal.sh",
             ctx->path_root);
    if (!file_exists(script)) return -1;
    if (dry) return 0;
    char cmd[MAX_PATH + 64];
    snprintf(cmd, sizeof cmd, "bash '%s' >>'%s/muscle.log' 2>&1", script,
             ctx->path_govdir);
    int rc = run_cmd(cmd, 0);
    if (rc == 0) ctx->last_procedure = now_unix();
    return rc;
}

static int dispatch_action(GovCtx *ctx, const char *name, int dry) {
    if (strcmp(name, "ensure_bonsai") == 0) return act_ensure_bonsai(ctx, dry);
    if (strcmp(name, "ensure_lane") == 0) return act_ensure_lane(ctx, dry);
    if (strcmp(name, "inject_window_gaps") == 0)
        return act_inject_window_gaps(ctx, dry);
    if (strcmp(name, "nudge_lane_tick") == 0) return 0;
    if (strcmp(name, "run_cert_learn") == 0)
        return act_run_bin(ctx, "cnet_cert_learn_tick", &ctx->last_peft, dry);
    if (strcmp(name, "run_structure_mine") == 0)
        return act_run_bin(ctx, "struct_mine_persist", &ctx->last_mine, dry);
    if (strcmp(name, "seed_procedures") == 0) return act_seed_procedures(ctx, dry);
    if (strcmp(name, "ensure_curiosity") == 0) return 0;
    if (strcmp(name, "log_hold") == 0) return 0;
    return -1;
}

static void bump_gh(double *h, double d) {
    *h += d;
    if (*h < 0) *h = 0;
    if (*h > 1) *h = 1;
}

static void update_goal_health(GovCtx *ctx, const Charter *c, const int *picked,
                               int np, const int *act_ok, int nact,
                               const Scoreboard *sb) {
    double ok_ratio = 0;
    if (nact > 0) {
        int ok = 0;
        for (int i = 0; i < nact; i++)
            if (act_ok[i]) ok++;
        ok_ratio = (double)ok / (double)nact;
    }
    for (int i = 0; i < np; i++) {
        const char *id = c->goals[picked[i]].id;
        double delta = 0.08 * ok_ratio - 0.05 * (1.0 - ok_ratio);
        if (strcmp(id, "drain_open_gaps") == 0) {
            bump_gh(&ctx->gh_drain, delta);
            if (sb->d_closed_gaps > 0) bump_gh(&ctx->gh_drain, 0.05);
        } else if (strcmp(id, "peft_jtc") == 0)
            bump_gh(&ctx->gh_peft, delta + 0.06 * ok_ratio);
        else if (strcmp(id, "structure_mine") == 0)
            bump_gh(&ctx->gh_mine, delta + 0.06 * ok_ratio);
        else if (strcmp(id, "coverage_curiosity") == 0) {
            bump_gh(&ctx->gh_coverage, delta);
            if (sb->plateau) bump_gh(&ctx->gh_coverage, -0.04);
        } else if (strcmp(id, "break_plateau") == 0)
            bump_gh(&ctx->gh_break, delta);
    }
    if (sb->plateau) {
        bump_gh(&ctx->gh_coverage, -0.03);
        bump_gh(&ctx->gh_drain, 0.02);
    }
}

/* ---------- persist ---------- */
static void write_json_outputs(GovCtx *ctx, const Scoreboard *sb, const Charter *c,
                               const int *picked, int np, const char **act_names,
                               const int *act_ok, int nact, int dry) {
    ensure_dir(ctx->path_govdir);
    char path[MAX_PATH];
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    char ts[64];
    strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%S%z", &tm);

    snprintf(path, sizeof path, "%s/scoreboard.json", ctx->path_govdir);
    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f,
                "{\n  \"ts\": \"%s\",\n  \"cycle\": %d,\n  \"bonsai_ok\": %d,\n  "
                "\"lane_ok\": %d,\n  \"open_gaps\": %d,\n  \"deferred_oracle\": %d,\n  "
                "\"closed_gaps\": %d,\n  \"fault_lines\": %ld,\n  \"lora_files\": %d,\n  "
                "\"units_proxy\": %ld,\n  \"hyb_struct\": %d,\n  \"base_size\": %ld,\n  "
                "\"hours_since_peft\": %.6f,\n  \"hours_since_mine\": %.6f,\n  "
                "\"hours_since_procedure\": %.6f,\n  \"d_open_gaps\": %.3f,\n  "
                "\"d_closed_gaps\": %.3f,\n  \"d_fault_lines\": %.3f,\n  "
                "\"d_units_proxy\": %.3f,\n  \"rate_closed_per_h\": %.4f,\n  "
                "\"rate_units_per_h\": %.4f,\n  \"ewma_open\": %.4f,\n  "
                "\"ewma_units\": %.4f,\n  \"backlog_pressure\": %.3f,\n  "
                "\"learning_velocity\": %.4f,\n  \"plateau\": %d,\n  "
                "\"teacher_uptime\": %.4f,\n  \"goal_health_drain\": %.4f,\n  "
                "\"goal_health_peft\": %.4f,\n  \"goal_health_mine\": %.4f,\n  "
                "\"goal_health_coverage\": %.4f,\n  \"engine\": \"cnet_governor_c\"\n}\n",
                ts, sb->cycle, sb->bonsai_ok, sb->lane_ok, sb->open_gaps,
                sb->deferred_oracle, sb->closed_gaps, sb->fault_lines, sb->lora_files,
                sb->units_proxy, sb->hyb_struct, sb->base_size, sb->hours_since_peft,
                sb->hours_since_mine, sb->hours_since_procedure, sb->d_open_gaps,
                sb->d_closed_gaps, sb->d_fault_lines, sb->d_units_proxy,
                sb->rate_closed_per_h, sb->rate_units_per_h, sb->ewma_open,
                sb->ewma_units, sb->backlog_pressure, sb->learning_velocity,
                sb->plateau, sb->teacher_uptime, sb->goal_health_drain,
                sb->goal_health_peft, sb->goal_health_mine, sb->goal_health_coverage);
        fclose(f);
    }

    if (!dry) {
        snprintf(path, sizeof path, "%s/scoreboard_history.jsonl", ctx->path_govdir);
        f = fopen(path, "a");
        if (f) {
            fprintf(f,
                    "{\"ts\":\"%s\",\"ts_unix\":%ld,\"cycle\":%d,\"open_gaps\":%d,"
                    "\"deferred_oracle\":%d,\"closed_gaps\":%d,\"fault_lines\":%ld,"
                    "\"units_proxy\":%ld,\"d_closed_gaps\":%.3f,\"d_units_proxy\":%.3f,"
                    "\"backlog_pressure\":%.3f,\"learning_velocity\":%.4f,\"plateau\":%d,"
                    "\"teacher_uptime\":%.4f}\n",
                    ts, (long)sb->ts_unix, sb->cycle, sb->open_gaps, sb->deferred_oracle,
                    sb->closed_gaps, sb->fault_lines, sb->units_proxy, sb->d_closed_gaps,
                    sb->d_units_proxy, sb->backlog_pressure, sb->learning_velocity,
                    sb->plateau, sb->teacher_uptime);
            fclose(f);
        }
        snprintf(path, sizeof path, "%s/scoreboard_ewma.json", ctx->path_govdir);
        f = fopen(path, "w");
        if (f) {
            fprintf(f,
                    "{\n  \"ewma_open\": %.6f,\n  \"ewma_deferred\": %.6f,\n  "
                    "\"ewma_fault\": %.6f,\n  \"ewma_units\": %.6f,\n  "
                    "\"teacher_uptime\": %.6f,\n  \"learning_velocity\": %.6f,\n  "
                    "\"updated_ts\": \"%s\",\n  \"engine\": \"c\"\n}\n",
                    sb->ewma_open, sb->ewma_deferred, sb->ewma_fault, sb->ewma_units,
                    sb->teacher_uptime, sb->learning_velocity, ts);
            fclose(f);
        }
        snprintf(path, sizeof path, "%s/state.json", ctx->path_govdir);
        f = fopen(path, "w");
        if (f) {
            fprintf(f,
                    "{\n  \"cycle\": %d,\n  \"last_peft_unix\": %.3f,\n  "
                    "\"last_mine_unix\": %.3f,\n  \"last_procedure_unix\": %.3f,\n  "
                    "\"last_cycle_unix\": %.3f,\n  \"goal_health\": {\n    "
                    "\"drain_open_gaps\": %.4f,\n    \"peft_jtc\": %.4f,\n    "
                    "\"structure_mine\": %.4f,\n    \"coverage_curiosity\": %.4f,\n    "
                    "\"break_plateau\": %.4f\n  },\n  \"last_goals\": [",
                    ctx->cycle, ctx->last_peft, ctx->last_mine, ctx->last_procedure,
                    now_unix(), ctx->gh_drain, ctx->gh_peft, ctx->gh_mine,
                    ctx->gh_coverage, ctx->gh_break);
            for (int i = 0; i < np; i++) {
                fprintf(f, "%s\"%s\"", i ? ", " : "", c->goals[picked[i]].id);
            }
            fprintf(f, "],\n  \"engine\": \"cnet_governor_c\"\n}\n");
            fclose(f);
        }
    }

    snprintf(path, sizeof path, "%s/last_decision.json", ctx->path_govdir);
    f = fopen(path, "w");
    if (f) {
        fprintf(f,
                "{\n  \"ts\": \"%s\",\n  \"dry_run\": %s,\n  \"engine\": "
                "\"cnet_governor_c\",\n  \"goals\": [",
                ts, dry ? "true" : "false");
        for (int i = 0; i < np; i++) {
            fprintf(f, "%s{\"id\": \"%s\", \"priority\": %d}", i ? ", " : "",
                    c->goals[picked[i]].id, c->goals[picked[i]].priority);
        }
        fprintf(f, "],\n  \"actions\": [");
        for (int i = 0; i < nact; i++) {
            fprintf(f, "%s{\"action\": \"%s\", \"ok\": %s}", i ? ", " : "",
                    act_names[i], act_ok[i] ? "true" : "false");
        }
        fprintf(f,
                "],\n  \"evolved\": {\"cycle\": %d, \"backlog_pressure\": %.3f, "
                "\"learning_velocity\": %.4f, \"plateau\": %d, \"teacher_uptime\": "
                "%.4f},\n  \"goal_health\": {\"drain_open_gaps\": %.4f, \"peft_jtc\": "
                "%.4f, \"structure_mine\": %.4f, \"coverage_curiosity\": %.4f}\n}\n",
                sb->cycle, sb->backlog_pressure, sb->learning_velocity, sb->plateau,
                sb->teacher_uptime, ctx->gh_drain, ctx->gh_peft, ctx->gh_mine,
                ctx->gh_coverage);
        fclose(f);
    }
}

/* ---------- cycle ---------- */
static int run_cycle(GovCtx *ctx, int dry) {
    Charter ch;
    if (parse_charter(ctx->path_charter, &ch) != 0) {
        fprintf(stderr, "charter parse fail: %s\n", ctx->path_charter);
        return 2;
    }
    load_state(ctx);
    load_ewma(ctx);
    load_prev_hist(ctx);

    Scoreboard sb;
    collect_scoreboard(ctx, &sb);
    evolve_scoreboard(ctx, &sb, dry);

    int picked[MAX_GOALS];
    int np = pick_goals(&ch, &sb, picked, MAX_GOALS);

    const char *act_names[64];
    int act_ok[64];
    int nact = 0;
    char seen[64][MAX_ID];
    int nseen = 0;

    for (int i = 0; i < np; i++) {
        Goal *g = &ch.goals[picked[i]];
        for (int a = 0; a < g->n_actions; a++) {
            int already = 0;
            for (int s = 0; s < nseen; s++)
                if (strcmp(seen[s], g->actions[a]) == 0) {
                    already = 1;
                    break;
                }
            if (already) continue;
            snprintf(seen[nseen++], MAX_ID, "%s", g->actions[a]);
            act_names[nact] = seen[nseen - 1];
            int rc = dispatch_action(ctx, g->actions[a], dry);
            act_ok[nact] = (rc == 0);
            nact++;
            if (nact >= 64) break;
        }
    }

    if (!dry) {
        update_goal_health(ctx, &ch, picked, np, act_ok, nact, &sb);
        sb.goal_health_drain = ctx->gh_drain;
        sb.goal_health_peft = ctx->gh_peft;
        sb.goal_health_mine = ctx->gh_mine;
        sb.goal_health_coverage = ctx->gh_coverage;
        ctx->last_cycle = now_unix();
    }

    write_json_outputs(ctx, &sb, &ch, picked, np, act_names, act_ok, nact, dry);

    printf("GOVERNOR_CYCLE_OK {\"engine\":\"c\",\"goals\":[");
    for (int i = 0; i < np; i++)
        printf("%s\"%s\"", i ? "," : "", ch.goals[picked[i]].id);
    printf("],\"actions\":[");
    for (int i = 0; i < nact; i++)
        printf("%s{\"a\":\"%s\",\"ok\":%s}", i ? "," : "", act_names[i],
               act_ok[i] ? "true" : "false");
    printf("],\"cycle\":%d,\"backlog\":%.0f,\"plateau\":%d,\"velocity\":%.3f}\n",
           sb.cycle, sb.backlog_pressure, sb.plateau, sb.learning_velocity);
    return 0;
}

/* ---------- self test ---------- */
static int self_test(void) {
    int fails = 0;
    Scoreboard sb;
    memset(&sb, 0, sizeof sb);
    sb.bonsai_ok = 0;
    sb.lane_ok = 1;
    if (eval_when("bonsai_ok == 0 or lane_ok == 0", &sb) != 1) {
        fprintf(stderr, "fail when bonsai down\n");
        fails++;
    }
    sb.bonsai_ok = 1;
    sb.open_gaps = 10;
    sb.deferred_oracle = 0;
    sb.backlog_pressure = 10;
    if (eval_when("open_gaps + deferred_oracle >= 8 or backlog_pressure >= 8", &sb) !=
        1) {
        fprintf(stderr, "fail backlog\n");
        fails++;
    }
    if (eval_when("true", &sb) != 1) {
        fprintf(stderr, "fail true\n");
        fails++;
    }
    if (eval_when("plateau == 1 and teacher_uptime >= 0.5", &sb) != 0) {
        fprintf(stderr, "fail plateau false\n");
        fails++;
    }
    sb.plateau = 1;
    sb.teacher_uptime = 1.0;
    if (eval_when("plateau == 1 and teacher_uptime >= 0.5", &sb) != 1) {
        fprintf(stderr, "fail plateau true\n");
        fails++;
    }

    /* charter parse if present */
    const char *root = env_or("CNET_ROOT", ".");
    char cp[MAX_PATH];
    snprintf(cp, sizeof cp, "%s/config/cnet_governor_charter.yaml", root);
    if (file_exists(cp)) {
        Charter ch;
        if (parse_charter(cp, &ch) != 0 || ch.n_goals < 3) {
            fprintf(stderr, "fail charter parse n=%d\n", ch.n_goals);
            fails++;
        } else {
            sb.bonsai_ok = 1;
            sb.lane_ok = 1;
            sb.backlog_pressure = 24;
            sb.open_gaps = 14;
            sb.deferred_oracle = 10;
            int picked[8];
            int np = pick_goals(&ch, &sb, picked, 8);
            if (np < 1 || strcmp(ch.goals[picked[0]].id, "drain_open_gaps") != 0) {
                fprintf(stderr, "fail pick drain got %s\n",
                        np ? ch.goals[picked[0]].id : "none");
                fails++;
            }
        }
    }

    if (fails) {
        printf("GOVERNOR_SELFTEST_FAIL fails=%d\n", fails);
        return 1;
    }
    printf("GOVERNOR_SELFTEST_PASS checks=6\n");
    return 0;
}

static void init_ctx(GovCtx *ctx) {
    memset(ctx, 0, sizeof *ctx);
    snprintf(ctx->path_root, sizeof ctx->path_root, "%s",
             env_or("CNET_ROOT", "/home/marble/AI/CNET"));
    snprintf(ctx->path_base, sizeof ctx->path_base, "%s",
             env_or("CNET_BASE_PATH",
                    "/home/marble/AI/CNET/soul_gemma4v2_final.cnb"));
    snprintf(ctx->path_charter, sizeof ctx->path_charter, "%s",
             env_or("CNET_GOVERNOR_CHARTER",
                    "/home/marble/AI/CNET/config/cnet_governor_charter.yaml"));
    snprintf(ctx->path_govdir, sizeof ctx->path_govdir, "%s",
             env_or("CNET_GOVERNOR_DIR", "/home/marble/AI/CNET/logs/governor"));
    snprintf(ctx->path_window, sizeof ctx->path_window, "%s",
             env_or("CNET_RESIDUAL_WINDOW",
                    "/home/marble/AI/CNET/english_window_256_bonsai.txt"));
    snprintf(ctx->path_fault, sizeof ctx->path_fault, "%s",
             env_or("CNET_FAULT_LOG", "/home/marble/AI/CNET/logs/cnet_faults.jsonl"));
    snprintf(ctx->path_lora, sizeof ctx->path_lora, "%s",
             env_or("CNET_LORA_STORE_DIR", "/home/marble/AI/CNET/logs/lora_store"));
    snprintf(ctx->http, sizeof ctx->http, "%s",
             env_or("CNET_RESIDUAL_HTTP", "http://127.0.0.1:8080"));
    snprintf(ctx->path_inbox, sizeof ctx->path_inbox, "%s.inbox", ctx->path_base);
    snprintf(ctx->path_gaps, sizeof ctx->path_gaps, "%s.gaps.txt", ctx->path_base);
    ctx->inject_n = atoi(env_or("CNET_GOVERNOR_INJECT", "4"));
    if (ctx->inject_n < 1) ctx->inject_n = 4;
    ctx->gh_drain = ctx->gh_peft = ctx->gh_mine = ctx->gh_coverage = ctx->gh_break =
        0.5;
}

int main(int argc, char **argv) {
    int dry = 0, test = 0, daemon = 0, interval = 900;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--dry-run") == 0) dry = 1;
        else if (strcmp(argv[i], "--test") == 0) test = 1;
        else if (strcmp(argv[i], "--daemon") == 0) {
            daemon = 1;
            if (i + 1 < argc && isdigit((unsigned char)argv[i + 1][0]))
                interval = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            fprintf(stderr,
                    "usage: cnet_governor [--test] [--dry-run] [--daemon [sec]]\n");
            return 0;
        }
    }

    if (test) return self_test();

    GovCtx ctx;
    init_ctx(&ctx);
    ensure_dir(ctx.path_govdir);

    if (daemon) {
        fprintf(stderr, "cnet_governor: daemon interval=%ds\n", interval);
        for (;;) {
            run_cycle(&ctx, 0);
            sleep((unsigned)(interval > 30 ? interval : 30));
        }
    }
    return run_cycle(&ctx, dry);
}
