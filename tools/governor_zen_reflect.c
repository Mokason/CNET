/* Zen reflect-before-commit gate for CNET governor (core).
 *
 * Usage:
 *   governor_zen_reflect --test
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir((p), 0755)
#endif

#define MAX_GOALS 16
#define ID_LEN 64

typedef struct {
    char id[ID_LEN];
    char actions[256];
} Goal;

typedef struct {
    char mode[32];
    char principles[8][160];
    int n_principles;
    char notes[8][128];
    int n_notes;
    char goals_after[MAX_GOALS][ID_LEN];
    int n_goals;
} ZenLog;

static void ensure_dir(const char *path) {
    char tmp[512];
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
    MKDIR(tmp);
}

static void add_prin(ZenLog *log, const char *s) {
    if (log->n_principles < 8)
        snprintf(log->principles[log->n_principles++], 160, "%s", s);
}

static void add_note(ZenLog *log, const char *s) {
    if (log->n_notes < 8)
        snprintf(log->notes[log->n_notes++], 128, "%s", s);
}

static int has_id(Goal *g, int n, const char *id) {
    int i;
    for (i = 0; i < n; i++)
        if (strcmp(g[i].id, id) == 0) return 1;
    return 0;
}

static void write_log(const ZenLog *log) {
    const char *gov = getenv("CNET_GOVERNOR_DIR");
    char path[512];
    FILE *f;
    int i;
    if (!gov || !gov[0]) gov = "logs/governor";
    ensure_dir(gov);
    snprintf(path, sizeof path, "%s/zen_reflect.json", gov);
    f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "{\n  \"mode\": \"%s\",\n  \"engine\": \"zen_reflect_v1\",\n  \"principles\": [",
            log->mode);
    for (i = 0; i < log->n_principles; i++)
        fprintf(f, "%s\"%s\"", i ? ", " : "", log->principles[i]);
    fprintf(f, "],\n  \"goals_after\": [");
    for (i = 0; i < log->n_goals; i++)
        fprintf(f, "%s\"%s\"", i ? ", " : "", log->goals_after[i]);
    fprintf(f, "]\n}\n");
    fclose(f);
}

static int reflect(Goal *picked, int n_picked, int bonsai_ok, int lane_ok,
                   int eval_veto, int freeze_seals, int busy, double d_backlog,
                   double vigilance, double frustration, double caution,
                   double patience, char last_goals[][ID_LEN], int n_last,
                   int *same_streak, Goal *out, int *n_out, ZenLog *log) {
    Goal revised[MAX_GOALS];
    int n = 0, i;
    memset(log, 0, sizeof *log);
    for (i = 0; i < n_picked && i < MAX_GOALS; i++) revised[n++] = picked[i];

    if (bonsai_ok == 0 || lane_ok == 0) {
        add_prin(log, "engaged_action: teacher/lane down — commit keep_alive immediately");
        if (!has_id(revised, n, "keep_teacher_alive")) {
            Goal g;
            memset(&g, 0, sizeof g);
            snprintf(g.id, sizeof g.id, "keep_teacher_alive");
            snprintf(g.actions, sizeof g.actions, "ensure_bonsai,ensure_lane");
            if (n < MAX_GOALS) {
                memmove(&revised[1], &revised[0], (size_t)n * sizeof(Goal));
                revised[0] = g;
                n++;
            }
        }
        snprintf(log->mode, sizeof log->mode, "commit_infra");
        *n_out = n;
        memcpy(out, revised, (size_t)n * sizeof(Goal));
        for (i = 0; i < n; i++)
            snprintf(log->goals_after[log->n_goals++], ID_LEN, "%s", revised[i].id);
        write_log(log);
        return 0;
    }

    if (eval_veto || freeze_seals) {
        Goal filtered[MAX_GOALS];
        int nf = 0;
        add_prin(log, "non_attachment: eval_veto/freeze — drop seal-heavy goals");
        for (i = 0; i < n; i++) {
            if (strcmp(revised[i].id, "peft_jtc") == 0 ||
                strcmp(revised[i].id, "structure_mine") == 0 ||
                strcmp(revised[i].id, "break_plateau") == 0)
                continue;
            filtered[nf++] = revised[i];
        }
        n = nf;
        memcpy(revised, filtered, (size_t)n * sizeof(Goal));
        if (n == 0) {
            memset(&revised[0], 0, sizeof revised[0]);
            snprintf(revised[0].id, sizeof revised[0].id, "outcome_review");
            snprintf(revised[0].actions, sizeof revised[0].actions,
                     "close_outcome,run_eval_probe");
            n = 1;
        }
        add_note(log, "dropped_seals");
    }

    {
        int match = (n == n_last);
        for (i = 0; match && i < n; i++)
            if (strcmp(revised[i].id, last_goals[i]) != 0) match = 0;
        if (match)
            (*same_streak)++;
        else
            *same_streak = 1;
    }
    if (*same_streak >= 4 && d_backlog >= 0 && !has_id(revised, n, "outcome_review")) {
        add_prin(log, "beginners_mind: attached to same goals without backlog improvement — force review");
        memset(revised, 0, sizeof revised);
        snprintf(revised[0].id, sizeof revised[0].id, "outcome_review");
        snprintf(revised[0].actions, sizeof revised[0].actions,
                 "close_outcome,run_eval_probe");
        snprintf(revised[1].id, sizeof revised[1].id, "health_hold");
        snprintf(revised[1].actions, sizeof revised[1].actions, "log_hold");
        n = 2;
        add_note(log, "attachment_break");
    }

    if ((vigilance >= 0.72 || caution >= 0.78) && busy) {
        Goal filtered[MAX_GOALS];
        int nf = 0;
        add_prin(log, "zazen: high vigilance+busy — demote heavy explore/break");
        for (i = 0; i < n; i++) {
            if (strcmp(revised[i].id, "break_plateau") == 0 ||
                strcmp(revised[i].id, "coverage_curiosity") == 0 ||
                strcmp(revised[i].id, "verified_web") == 0)
                continue;
            filtered[nf++] = revised[i];
        }
        n = nf;
        memcpy(revised, filtered, (size_t)n * sizeof(Goal));
        if (n == 0) {
            snprintf(revised[0].id, sizeof revised[0].id, "health_hold");
            n = 1;
        }
        add_note(log, "vigilance_hold");
    }

    if (frustration >= 0.65 && patience >= 0.55 && has_id(revised, n, "drain_open_gaps") &&
        *same_streak >= 3) {
        add_prin(log, "simplicity: inject thrash — insert outcome_review before more gaps");
        if (!has_id(revised, n, "outcome_review") && n < MAX_GOALS) {
            Goal g;
            memset(&g, 0, sizeof g);
            snprintf(g.id, sizeof g.id, "outcome_review");
            memmove(&revised[1], &revised[0], (size_t)n * sizeof(Goal));
            revised[0] = g;
            n++;
            if (n > 3) n = 3;
        }
        add_note(log, "frustration_review");
    }

    if (n == 0) {
        add_prin(log, "present_moment: nothing left — health_hold");
        snprintf(revised[0].id, sizeof revised[0].id, "health_hold");
        n = 1;
    }
    if (log->n_principles == 0)
        add_prin(log, "clear_see: agenda accepted — move without hesitation");

    snprintf(log->mode, sizeof log->mode, "ok");
    *n_out = n;
    memcpy(out, revised, (size_t)n * sizeof(Goal));
    for (i = 0; i < n; i++)
        snprintf(log->goals_after[log->n_goals++], ID_LEN, "%s", revised[i].id);
    write_log(log);
    (void)caution;
    return 0;
}

static int self_test(void) {
    Goal picked[1], out[MAX_GOALS];
    ZenLog log;
    char last[1][ID_LEN];
    int streak = 3, n_out = 0, i, found;
    snprintf(picked[0].id, sizeof picked[0].id, "drain_open_gaps");
    snprintf(picked[0].actions, sizeof picked[0].actions, "inject_window_gaps");
    snprintf(last[0], ID_LEN, "drain_open_gaps");
    reflect(picked, 1, 1, 1, 0, 0, 0, 1.0, 0.4, 0.7, 0.6, 0.7, last, 1, &streak, out,
            &n_out, &log);
    found = has_id(out, n_out, "outcome_review") || log.n_notes > 0;
    if (!found) {
        fprintf(stderr, "attachment reflect failed\n");
        return 1;
    }
    reflect(NULL, 0, 0, 1, 0, 0, 0, 0, 0.5, 0.5, 0.5, 0.5, NULL, 0, &streak, out, &n_out,
            &log);
    if (strcmp(out[0].id, "keep_teacher_alive") != 0 || strcmp(log.mode, "commit_infra") != 0) {
        fprintf(stderr, "infra fire failed\n");
        return 1;
    }
    snprintf(picked[0].id, sizeof picked[0].id, "peft_jtc");
    streak = 1;
    reflect(picked, 1, 1, 1, 1, 0, 0, 0, 0.5, 0.5, 0.5, 0.5, NULL, 0, &streak, out, &n_out,
            &log);
    for (i = 0; i < n_out; i++) {
        if (strcmp(out[i].id, "peft_jtc") == 0) {
            fprintf(stderr, "veto drop failed\n");
            return 1;
        }
    }
    printf("ZEN_REFLECT_SELFTEST_PASS checks=3\n");
    return 0;
}

int main(int argc, char **argv) {
    int i;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--test") == 0) return self_test();
    }
    printf("usage: import reflect() from governor / governor_zen_reflect --test\n");
    return 0;
}
