/* v4 extensions: goal graph scoring, stable evolve, novel curriculum (core).
 *
 * Usage:
 *   governor_v4_ext --test
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir((p), 0755)
#endif

#define MAX_NODES 64
#define MAX_GOALS 8
#define ID_LEN 64

typedef struct {
    char id[ID_LEN];
    char metric[ID_LEN];
    char kind[32];
    char from[ID_LEN];
    char to[ID_LEN];
    double target;
    double weight;
    int prefer_lower;
    char goals[MAX_GOALS][ID_LEN];
    int n_goals;
    char deps[MAX_GOALS][ID_LEN];
    int n_deps;
} GraphNode;

typedef struct {
    char id[ID_LEN];
    double urgency;
    char metric[ID_LEN];
    double value;
    double target;
    char transfer_from[ID_LEN];
} ScoredNode;

static double sb_get(const char *metric, double teacher_uptime, double backlog,
                     double hermes_fail, double hours_proc, double web_notes,
                     double eval_jtc) {
    if (!metric) return 0.0;
    if (strcmp(metric, "teacher_uptime") == 0) return teacher_uptime;
    if (strcmp(metric, "backlog_pressure") == 0) return backlog;
    if (strcmp(metric, "hermes_task_fail_rate") == 0) return hermes_fail;
    if (strcmp(metric, "hours_since_procedure") == 0) return hours_proc;
    if (strcmp(metric, "web_notes") == 0) return web_notes;
    if (strcmp(metric, "eval_jtc_delta") == 0) return eval_jtc;
    return 0.0;
}

/* Minimal JSON extractors for the known goal-graph schema. */
static const char *skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

static int json_str_field(const char *obj, const char *key, char *out, size_t cap) {
    char pat[96];
    const char *p, *q;
    size_t n;
    snprintf(pat, sizeof pat, "\"%s\"", key);
    p = strstr(obj, pat);
    if (!p) { if (cap) out[0] = 0; return 0; }
    p = strchr(p + strlen(pat), ':');
    if (!p) return 0;
    p = skip_ws(p + 1);
    if (*p != '"') return 0;
    p++;
    q = strchr(p, '"');
    if (!q) return 0;
    n = (size_t)(q - p);
    if (n >= cap) n = cap - 1;
    memcpy(out, p, n);
    out[n] = 0;
    return 1;
}

static double json_num_field(const char *obj, const char *key, double def) {
    char pat[96];
    const char *p;
    snprintf(pat, sizeof pat, "\"%s\"", key);
    p = strstr(obj, pat);
    if (!p) return def;
    p = strchr(p + strlen(pat), ':');
    if (!p) return def;
    return atof(skip_ws(p + 1));
}

static int json_bool_field(const char *obj, const char *key) {
    char pat[96];
    const char *p;
    snprintf(pat, sizeof pat, "\"%s\"", key);
    p = strstr(obj, pat);
    if (!p) return 0;
    p = strchr(p + strlen(pat), ':');
    if (!p) return 0;
    p = skip_ws(p + 1);
    return (strncmp(p, "true", 4) == 0);
}

static int parse_string_array(const char *obj, const char *key,
                              char arr[][ID_LEN], int maxn) {
    char pat[96];
    const char *p, *end;
    int n = 0;
    snprintf(pat, sizeof pat, "\"%s\"", key);
    p = strstr(obj, pat);
    if (!p) return 0;
    p = strchr(p + strlen(pat), '[');
    if (!p) return 0;
    end = strchr(p, ']');
    if (!end) return 0;
    p++;
    while (p < end && n < maxn) {
        const char *q;
        size_t L;
        p = skip_ws(p);
        if (p >= end || *p == ']') break;
        if (*p != '"') { p++; continue; }
        p++;
        q = strchr(p, '"');
        if (!q || q >= end) break;
        L = (size_t)(q - p);
        if (L >= ID_LEN) L = ID_LEN - 1;
        memcpy(arr[n], p, L);
        arr[n][L] = 0;
        n++;
        p = q + 1;
        if (*p == ',') p++;
    }
    return n;
}

static int load_graph(const char *path, GraphNode *nodes, int maxn) {
    FILE *f;
    long sz;
    char *buf;
    const char *p;
    int n = 0;
    f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 2 * 1024 * 1024) { fclose(f); return 0; }
    buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return 0; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { free(buf); fclose(f); return 0; }
    buf[sz] = 0;
    fclose(f);
    p = strstr(buf, "\"nodes\"");
    if (!p) { free(buf); return 0; }
    p = strchr(p, '[');
    if (!p) { free(buf); return 0; }
    p++;
    while (*p && n < maxn) {
        const char *start, *end;
        char obj[2048];
        size_t L;
        p = skip_ws(p);
        if (*p == ']') break;
        if (*p != '{') { p++; continue; }
        start = p;
        end = strchr(start, '}');
        if (!end) break;
        L = (size_t)(end - start + 1);
        if (L >= sizeof obj) L = sizeof obj - 1;
        memcpy(obj, start, L);
        obj[L] = 0;
        memset(&nodes[n], 0, sizeof nodes[n]);
        json_str_field(obj, "id", nodes[n].id, sizeof nodes[n].id);
        json_str_field(obj, "metric", nodes[n].metric, sizeof nodes[n].metric);
        json_str_field(obj, "kind", nodes[n].kind, sizeof nodes[n].kind);
        json_str_field(obj, "from", nodes[n].from, sizeof nodes[n].from);
        json_str_field(obj, "to", nodes[n].to, sizeof nodes[n].to);
        nodes[n].target = json_num_field(obj, "target", 0.0);
        nodes[n].weight = json_num_field(obj, "weight", 1.0);
        nodes[n].prefer_lower = json_bool_field(obj, "prefer_lower");
        nodes[n].n_goals = parse_string_array(obj, "goals", nodes[n].goals, MAX_GOALS);
        nodes[n].n_deps = parse_string_array(obj, "deps", nodes[n].deps, MAX_GOALS);
        if (nodes[n].id[0]) n++;
        p = end + 1;
        if (*p == ',') p++;
    }
    free(buf);
    return n;
}

int score_goal_graph(const char *graph_path,
                     double teacher_uptime, double backlog, double hermes_fail,
                     double hours_proc, double web_notes, double eval_jtc,
                     double w_eval, double transfer_credit,
                     ScoredNode *out, int max_out) {
    GraphNode nodes[MAX_NODES];
    ScoredNode scored[MAX_NODES];
    int nn, i, j, ns = 0;
    nn = load_graph(graph_path, nodes, MAX_NODES);
    if (nn <= 0) return 0;
    for (i = 0; i < nn; i++) {
        double val, gap, weight, dep_pen = 0.0, urgency;
        if (strcmp(nodes[i].kind, "transfer") == 0) continue;
        val = sb_get(nodes[i].metric, teacher_uptime, backlog, hermes_fail,
                     hours_proc, web_notes, eval_jtc);
        weight = nodes[i].weight;
        if (strstr(nodes[i].metric, "eval") || strstr(nodes[i].metric, "jtc"))
            weight *= w_eval;
        if (nodes[i].prefer_lower)
            gap = fmax(0.0, val - nodes[i].target) / fmax(fabs(nodes[i].target), 1e-6);
        else
            gap = fmax(0.0, nodes[i].target - val) / fmax(fabs(nodes[i].target), 1e-6);
        for (j = 0; j < nodes[i].n_deps; j++) {
            int k;
            for (k = 0; k < ns; k++) {
                if (strcmp(scored[k].id, nodes[i].deps[j]) == 0 && scored[k].urgency > 0.5)
                    dep_pen += 0.15;
            }
        }
        urgency = gap * weight + dep_pen;
        memset(&scored[ns], 0, sizeof scored[ns]);
        snprintf(scored[ns].id, sizeof scored[ns].id, "%s", nodes[i].id);
        snprintf(scored[ns].metric, sizeof scored[ns].metric, "%s", nodes[i].metric);
        scored[ns].urgency = round(urgency * 10000.0) / 10000.0;
        scored[ns].value = val;
        scored[ns].target = nodes[i].target;
        ns++;
    }
    for (i = 0; i < nn; i++) {
        int ifrom = -1, ito = -1;
        if (strcmp(nodes[i].kind, "transfer") != 0) continue;
        for (j = 0; j < ns; j++) {
            if (strcmp(scored[j].id, nodes[i].from) == 0) ifrom = j;
            if (strcmp(scored[j].id, nodes[i].to) == 0) ito = j;
        }
        if (ifrom >= 0 && ito >= 0 && scored[ifrom].urgency < 0.3 && scored[ito].urgency > 0.4) {
            scored[ito].urgency =
                round((scored[ito].urgency + transfer_credit * nodes[i].weight) * 10000.0) /
                10000.0;
            snprintf(scored[ito].transfer_from, sizeof scored[ito].transfer_from, "%s",
                     nodes[i].from);
        }
    }
    /* sort by urgency desc */
    for (i = 0; i < ns; i++) {
        for (j = i + 1; j < ns; j++) {
            if (scored[j].urgency > scored[i].urgency) {
                ScoredNode t = scored[i];
                scored[i] = scored[j];
                scored[j] = t;
            }
        }
    }
    if (ns > max_out) ns = max_out;
    memcpy(out, scored, (size_t)ns * sizeof(ScoredNode));
    return ns;
}

typedef struct {
    double w_eval, w_real_miss, w_hermes_err, w_backlog, w_velocity;
    double threshold_backlog, inject_n, evolve_rate, evolve_clamp;
    double transfer_credit, min_dt_h;
    int evolve_min_cycles;
} Meta;

static void meta_defaults(Meta *m) {
    memset(m, 0, sizeof *m);
    m->w_eval = 3.0;
    m->w_real_miss = 3.0;
    m->w_hermes_err = 2.5;
    m->w_backlog = 2.5;
    m->w_velocity = 1.0;
    m->threshold_backlog = 8.0;
    m->inject_n = 4;
    m->evolve_rate = 0.04;
    m->evolve_clamp = 0.15;
    m->transfer_credit = 0.25;
    m->min_dt_h = 0.05;
    m->evolve_min_cycles = 4;
}

static void adj(Meta *meta, const Meta *defaults, const char *key, double delta) {
    double base, cur, nxt, lo, hi;
    if (strcmp(key, "w_eval") == 0) {
        base = defaults->w_eval; cur = meta->w_eval; lo = 1.0; hi = 5.0;
        nxt = cur + delta; meta->w_eval = fmax(lo, fmin(hi, nxt));
    } else if (strcmp(key, "w_real_miss") == 0) {
        base = defaults->w_real_miss; cur = meta->w_real_miss; lo = 1.0; hi = 5.0;
        nxt = cur + delta; meta->w_real_miss = fmax(lo, fmin(hi, nxt));
        (void)base;
    } else if (strcmp(key, "w_hermes_err") == 0) {
        cur = meta->w_hermes_err; lo = 1.0; hi = 5.0;
        nxt = cur + delta; meta->w_hermes_err = fmax(lo, fmin(hi, nxt));
    } else if (strcmp(key, "w_backlog") == 0) {
        cur = meta->w_backlog; lo = 1.0; hi = 4.0;
        nxt = cur + delta; meta->w_backlog = fmax(lo, fmin(hi, nxt));
    } else if (strcmp(key, "threshold_backlog") == 0) {
        cur = meta->threshold_backlog; lo = 4.0; hi = 16.0;
        nxt = cur + delta; meta->threshold_backlog = fmax(lo, fmin(hi, nxt));
    } else if (strcmp(key, "inject_n") == 0) {
        cur = meta->inject_n; lo = 2; hi = 6;
        nxt = cur + delta; meta->inject_n = (int)round(fmax(lo, fmin(hi, nxt)));
    } else if (strcmp(key, "w_velocity") == 0) {
        cur = meta->w_velocity;
        nxt = cur + delta; meta->w_velocity = fmax(0.5, fmin(3.0, nxt));
    } else if (strcmp(key, "transfer_credit") == 0) {
        cur = meta->transfer_credit;
        nxt = cur + delta; meta->transfer_credit = fmax(0.05, fmin(1.0, nxt));
    }
}

void stable_evolve(Meta *meta, const Meta *defaults, int cycles, int drain_streak,
                   double d_backlog, double d_eval_jtc, double hermes_fail,
                   int hermes_noisy, int plateau, double dt_h, char *note, size_t nnote) {
    double rate = meta->evolve_rate;
    double thresh_veto = 0.05;
    note[0] = 0;
    if (cycles < meta->evolve_min_cycles) {
        snprintf(note, nnote, "warmup_%d/%d", cycles, meta->evolve_min_cycles);
        return;
    }
    if (dt_h < meta->min_dt_h) {
        snprintf(note, nnote, "skip_evolve_small_dt");
        return;
    }
    if (d_backlog >= 0 && drain_streak >= 3) {
        adj(meta, defaults, "w_real_miss", rate);
        adj(meta, defaults, "threshold_backlog", -0.15);
        if (meta->inject_n > 2) adj(meta, defaults, "inject_n", -1);
        strncat(note, "backlog_stuck", nnote - strlen(note) - 1);
    }
    if (d_eval_jtc < -thresh_veto) {
        adj(meta, defaults, "w_eval", rate * 1.5);
        if (note[0]) strncat(note, "+", nnote - strlen(note) - 1);
        strncat(note, "eval_veto", nnote - strlen(note) - 1);
    } else if (d_eval_jtc > 0.02) {
        adj(meta, defaults, "w_eval", -rate * 0.25);
        if (note[0]) strncat(note, "+", nnote - strlen(note) - 1);
        strncat(note, "eval_ok", nnote - strlen(note) - 1);
    }
    if (hermes_fail > 0.2 && !hermes_noisy) {
        adj(meta, defaults, "w_hermes_err", rate);
        if (note[0]) strncat(note, "+", nnote - strlen(note) - 1);
        strncat(note, "hermes_struct_fail", nnote - strlen(note) - 1);
    } else if (hermes_fail < 0.08) {
        adj(meta, defaults, "w_hermes_err", -rate * 0.2);
    }
    if (plateau) {
        adj(meta, defaults, "w_velocity", rate * 0.5);
        if (note[0]) strncat(note, "+", nnote - strlen(note) - 1);
        strncat(note, "plateau", nnote - strlen(note) - 1);
    }
    if (!note[0]) snprintf(note, nnote, "stable");
}

static int self_test(void) {
    Meta m, d;
    ScoredNode scored[MAX_NODES];
    char note[128];
    const char *root = getenv("CNET_ROOT");
    char gpath[512];
    int n;
    meta_defaults(&d);
    m = d;
    stable_evolve(&m, &d, 10, 5, 1.0, -0.1, 0.5, 0, 1, 0.2, note, sizeof note);
    if (m.w_eval < d.w_eval - 0.01) {
        fprintf(stderr, "stable_evolve failed w_eval\n");
        return 1;
    }
    snprintf(gpath, sizeof gpath, "%s/config/governor_goal_graph.json",
             root && root[0] ? root : ".");
    n = score_goal_graph(gpath, 1.0, 20.0, 0.3, 10.0, 1.0, 0.1, d.w_eval,
                         d.transfer_credit, scored, MAX_NODES);
    if (n <= 0 || scored[0].urgency < 0) {
        fprintf(stderr, "score_goal_graph failed n=%d\n", n);
        return 1;
    }
    printf("GOVERNOR_V4_EXT_PASS checks=2\n");
    return 0;
}

int main(int argc, char **argv) {
    int i;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--test") == 0) return self_test();
    }
    printf("usage: governor_v4_ext --test\n");
    return 0;
}
