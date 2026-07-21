/* CNET library janitor (governance G1).
 *
 * One maintenance pass over a personal CNB:
 *   - soul_health_tick (audit / label / heal / promote / structure-mine)
 *   - low-reliability scan (+ optional LOW_RELIABILITY ledger notes)
 *   - gap ledger census (open / deferred / closed)
 *   - serve-stats snapshot
 *   - markdown + JSON report under artifacts/janitor/
 *   - optional CNB file snapshot (copy) when CNET_JANITOR_SNAPSHOT=1
 *
 * Safety:
 *   - Refuses if <base>.stop exists (learner stop convention)
 *   - Refuses if CNET_JANITOR_LOCK cannot be acquired (single flight)
 *   - Does not delete units; does not rewrite policy
 *   - Default is report + health + persist runtime state via soul_close
 *
 * Usage:
 *   ./bin/cnet_janitor [base.cnb]
 * Env:
 *   CNET_BASE_PATH              default base
 *   CNET_JANITOR_REPORT_DIR     default artifacts/janitor
 *   CNET_JANITOR_LOW_REL        floor (default 0.55); 0 = skip scan notes
 *   CNET_JANITOR_LOW_REL_MIN_EV evidence needed (default 64)
 *   CNET_JANITOR_NOTE_LOW_REL=1 write LOW_RELIABILITY into <base>.gaps.txt
 *   CNET_JANITOR_SNAPSHOT=1     copy CNB to report dir/snapshots/
 *   CNET_JANITOR_LOCK           lock file path (default <report>/.janitor.lock)
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "../include/soul_host.h"
#include "../include/acquire.h"
#include "../include/nn.h"
#include "../include/router.h"

static int env_flag(const char *k) {
    const char *v = getenv(k);
    return v && v[0] == '1' && v[1] == '\0';
}

static double env_d(const char *k, double d) {
    const char *v = getenv(k);
    return (v && v[0]) ? atof(v) : d;
}

static long env_l(const char *k, long d) {
    const char *v = getenv(k);
    return (v && v[0]) ? atol(v) : d;
}

static int ensure_dir(const char *path) {
    char tmp[1024];
    size_t len, i;
    if (!path || !path[0]) return -1;
    snprintf(tmp, sizeof tmp, "%s", path);
    len = strlen(tmp);
    for (i = 1; i < len; i++) {
        if (tmp[i] == '/') {
            tmp[i] = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
            tmp[i] = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

static int file_exists(const char *p) {
    struct stat st;
    return p && p[0] && stat(p, &st) == 0;
}

static int acquire_lock(const char *path) {
    int fd;
    char pidbuf[32];
    if (!path || !path[0]) return -1;
    fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0644);
    if (fd < 0) return -1;
    snprintf(pidbuf, sizeof pidbuf, "%d\n", (int)getpid());
    (void)write(fd, pidbuf, strlen(pidbuf));
    close(fd);
    return 0;
}

static void release_lock(const char *path) {
    if (path && path[0]) unlink(path);
}

typedef struct {
    size_t open_n, deferred_n, closed_n, other_n;
    size_t waiting_oracle;
    size_t low_rel_gaps;
} LedgerCensus;

static void census_ledger(const char *path, LedgerCensus *c) {
    FILE *f;
    char line[1024];
    memset(c, 0, sizeof *c);
    if (!path || !file_exists(path)) return;
    f = fopen(path, "r");
    if (!f) return;
    if (!fgets(line, sizeof line, f)) { fclose(f); return; }
    if (!fgets(line, sizeof line, f)) { fclose(f); return; }
    while (fgets(line, sizeof line, f)) {
        int kind, status;
        char reason[64];
        if (sscanf(line, "%d %d", &kind, &status) != 2) {
            c->other_n++;
            continue;
        }
        if (status == 0) c->open_n++;
        else if (status == 1) c->deferred_n++;
        else if (status == 2) c->closed_n++;
        else c->other_n++;
        reason[0] = '\0';
        /* defer_reason often field 14 (0-based varies); scan tokens */
        {
            char *tok, *save = NULL, *copy = strdup(line);
            int i = 0;
            if (copy) {
                for (tok = strtok_r(copy, " \t\n", &save); tok;
                     tok = strtok_r(NULL, " \t\n", &save), i++) {
                    if (strcmp(tok, "waiting_oracle") == 0) c->waiting_oracle++;
                    if (strcmp(tok, "low_reliability") == 0 ||
                        strcmp(tok, "LOW_RELIABILITY") == 0)
                        c->low_rel_gaps++;
                }
                free(copy);
            }
        }
        (void)kind;
    }
    fclose(f);
}

int main(int argc, char **argv) {
    const char *base;
    const char *report_dir;
    char ledger_path[768], stop_path[768], lock_path[800];
    char report_md[900], report_json[900], stamp[64];
    char snap_dir[900], snap_path[960];
    SoulHost *host = NULL;
    long long health[16];
    int hn, ui, nunits;
    double low_floor;
    long low_min_ev;
    size_t low_hit = 0, low_noted = 0;
    LedgerCensus census;
    FILE *md = NULL, *js = NULL;
    time_t now = time(NULL);
    struct tm tm_now;
    AcquireLedger ledger;
    int note_low, do_snap;
    int rc = 1;

    base = (argc >= 2 && argv[1][0]) ? argv[1] : getenv("CNET_BASE_PATH");
    if (!base || !base[0])
        base = "soul_gemma4v2_final.cnb";
    report_dir = getenv("CNET_JANITOR_REPORT_DIR");
    if (!report_dir || !report_dir[0]) report_dir = "artifacts/janitor";

    low_floor = env_d("CNET_JANITOR_LOW_REL", 0.55);
    low_min_ev = env_l("CNET_JANITOR_LOW_REL_MIN_EV", 64);
    note_low = env_flag("CNET_JANITOR_NOTE_LOW_REL");
    do_snap = env_flag("CNET_JANITOR_SNAPSHOT");

    if (!file_exists(base)) {
        fprintf(stderr, "cnet_janitor: base missing: %s\n", base);
        return 2;
    }
    snprintf(stop_path, sizeof stop_path, "%s.stop", base);
    if (file_exists(stop_path)) {
        fprintf(stderr, "cnet_janitor: refuse — stop file present: %s\n",
                stop_path);
        return 3;
    }

    if (ensure_dir(report_dir) != 0) {
        fprintf(stderr, "cnet_janitor: cannot create report dir %s\n",
                report_dir);
        return 4;
    }
    {
        const char *lk = getenv("CNET_JANITOR_LOCK");
        if (lk && lk[0])
            snprintf(lock_path, sizeof lock_path, "%s", lk);
        else
            snprintf(lock_path, sizeof lock_path, "%s/.janitor.lock",
                     report_dir);
    }
    if (acquire_lock(lock_path) != 0) {
        fprintf(stderr, "cnet_janitor: another janitor holds %s\n", lock_path);
        return 5;
    }

    localtime_r(&now, &tm_now);
    strftime(stamp, sizeof stamp, "%Y%m%d_%H%M%S", &tm_now);
    snprintf(report_md, sizeof report_md, "%s/janitor_%s.md", report_dir,
             stamp);
    snprintf(report_json, sizeof report_json, "%s/janitor_%s.json", report_dir,
             stamp);
    snprintf(ledger_path, sizeof ledger_path, "%s.gaps.txt", base);

    printf("cnet_janitor: base=%s report=%s\n", base, report_md);
    fflush(stdout);

    host = NULL;
    if (soul_open(base, NULL, &host) != 0 || !host) {
        fprintf(stderr, "cnet_janitor: soul_open failed\n");
        release_lock(lock_path);
        return 6;
    }

    memset(health, 0, sizeof health);
    hn = soul_health_tick(host, health, 16);
    if (hn < 0) {
        fprintf(stderr, "cnet_janitor: soul_health_tick failed rc=%d\n", hn);
        soul_close(host);
        release_lock(lock_path);
        return 7;
    }

    nunits = soul_unit_count(host);
    if (nunits < 0) nunits = 0;

    /* Low-rel scan */
    memset(&ledger, 0, sizeof ledger);
    if (note_low && file_exists(ledger_path))
        (void)acquire_ledger_load(&ledger, ledger_path);

    md = fopen(report_md, "w");
    js = fopen(report_json, "w");
    if (!md || !js) {
        fprintf(stderr, "cnet_janitor: cannot write reports\n");
        goto done;
    }

    fprintf(md, "# CNET Janitor Report\n\n");
    fprintf(md, "- **When:** %s\n", stamp);
    fprintf(md, "- **Base:** `%s`\n", base);
    fprintf(md, "- **Units:** %d\n\n", nunits);

    fprintf(md, "## Health tick\n\n");
    fprintf(md, "| field | value |\n|---|---|\n");
    fprintf(md, "| entries | %lld |\n", health[0]);
    fprintf(md, "| demoted_by_audit | %lld |\n", health[1]);
    fprintf(md, "| labeled_from_contract | %lld |\n", health[2]);
    fprintf(md, "| labeled_via_teacher | %lld |\n", health[3]);
    fprintf(md, "| heal_attempted | %lld |\n", health[4]);
    fprintf(md, "| healed | %lld |\n", health[5]);
    fprintf(md, "| promoted_provisional | %lld |\n", health[6]);
    fprintf(md, "| shadows_promoted | %lld |\n", health[7]);
    fprintf(md, "| reset_remaining | %lld |\n", health[8]);
    fprintf(md, "| trust uncertified/evidenced/certified/demoted | "
                "%lld/%lld/%lld/%lld |\n\n",
            health[9], health[10], health[11], health[12]);

    fprintf(md, "## Low-reliability scan\n\n");
    fprintf(md, "Floor=%.3f min_evidence=%ld note_to_ledger=%s\n\n", low_floor,
            low_min_ev, note_low ? "yes" : "no");
    if (low_floor > 0.0) {
        fprintf(md, "| unit | reliability | evidence |\n|---|---|---|\n");
        for (ui = 0; ui < nunits; ui++) {
            char name[128];
            int reli;
            unsigned long ev = 0;
            double rel = 0.5;
            if (soul_unit_name(host, ui, name, (int)sizeof name) != 0)
                continue;
            reli = soul_unit_reliability_milli(host, name);
            if (reli >= 0) rel = reli / 1000.0;
            /* evidence: use health layers JSON snippet or skip count */
            {
                /* Approximate evidence from reliability API only; detailed
                   evidence is in registry state — milli is enough for janitor. */
            }
            if (rel < low_floor) {
                /* Without evidence count API on soul, treat milli-known units
                   with rel under floor as candidates when min_ev==0, else we
                   only list when we can get evidence from serve path. */
                char hl[512];
                int has_ev = 0;
                hl[0] = '\0';
                if (soul_unit_health_layers(host, name, hl, (int)sizeof hl) ==
                    0) {
                    /* parse "evidence":N if present */
                    const char *p = strstr(hl, "\"evidence\"");
                    if (p) {
                        p = strchr(p, ':');
                        if (p) ev = strtoul(p + 1, NULL, 10);
                        has_ev = 1;
                    }
                }
                if (!has_ev) {
                    /* Fallback: if reliability left Laplace 0.5 exactly and
                       milli==500, skip; else require min_ev==0 or ev. */
                    if (reli == 500) continue;
                    if (low_min_ev > 0 && ev < (unsigned long)low_min_ev)
                        continue;
                } else if (ev < (unsigned long)low_min_ev) {
                    continue;
                }
                low_hit++;
                fprintf(md, "| `%s` | %.3f | %lu |\n", name, rel,
                        (unsigned long)ev);
                if (note_low) {
                    if (acquire_note_low_reliability(&ledger, name, rel,
                                                     low_floor) >= 0)
                        low_noted++;
                }
            }
        }
        if (low_hit == 0) fprintf(md, "_(none under floor with evidence)_\n");
        fprintf(md, "\nCandidates: %zu  Noted to ledger: %zu\n\n", low_hit,
                low_noted);
    } else {
        fprintf(md, "_(scan disabled)_\n\n");
    }

    if (note_low && low_noted > 0)
        (void)acquire_ledger_save(&ledger, ledger_path);

    census_ledger(ledger_path, &census);
    fprintf(md, "## Gap ledger census\n\n");
    fprintf(md, "| status | count |\n|---|---|\n");
    fprintf(md, "| open | %zu |\n", census.open_n);
    fprintf(md, "| deferred | %zu |\n", census.deferred_n);
    fprintf(md, "| closed | %zu |\n", census.closed_n);
    fprintf(md, "| waiting_oracle (token) | %zu |\n", census.waiting_oracle);
    fprintf(md, "| low_reliability (token) | %zu |\n\n", census.low_rel_gaps);

    fprintf(md, "## Serve stats (host after tick)\n\n");
    {
        unsigned long long cs = 0, rs = 0, gn = 0, sm = 0, ss = 0;
        /* read back via soul if getters exist — use state file after close;
           for now print from health path note */
        fprintf(md, "See `%s.state/soul_serve.stats` after close.\n\n", base);
        (void)cs; (void)rs; (void)gn; (void)sm; (void)ss;
    }

    fprintf(md, "## Actions taken\n\n");
    fprintf(md, "- health_tick: yes (hn=%d)\n", hn);
    fprintf(md, "- low_rel_notes: %zu\n", low_noted);
    fprintf(md, "- snapshot: %s\n", do_snap ? "requested" : "no");
    fprintf(md, "\n## Governance\n\n");
    fprintf(md, "G1 janitor only. No unit deletion. No policy rewrite. "
                "Human charter remains external.\n");

    /* JSON */
    fprintf(js,
            "{\n"
            "  \"stamp\": \"%s\",\n"
            "  \"base\": \"%s\",\n"
            "  \"units\": %d,\n"
            "  \"health\": {\n"
            "    \"entries\": %lld,\n"
            "    \"demoted_by_audit\": %lld,\n"
            "    \"labeled_from_contract\": %lld,\n"
            "    \"labeled_via_teacher\": %lld,\n"
            "    \"heal_attempted\": %lld,\n"
            "    \"healed\": %lld,\n"
            "    \"promoted_provisional\": %lld,\n"
            "    \"shadows_promoted\": %lld,\n"
            "    \"reset_remaining\": %lld,\n"
            "    \"trust\": {\"uncertified\": %lld, \"evidenced\": %lld, "
            "\"certified\": %lld, \"demoted\": %lld}\n"
            "  },\n"
            "  \"low_rel\": {\"floor\": %.4f, \"min_ev\": %ld, \"hits\": %zu, "
            "\"noted\": %zu},\n"
            "  \"ledger\": {\"open\": %zu, \"deferred\": %zu, \"closed\": %zu, "
            "\"waiting_oracle\": %zu, \"low_rel_gaps\": %zu}\n"
            "}\n",
            stamp, base, nunits, health[0], health[1], health[2], health[3],
            health[4], health[5], health[6], health[7], health[8], health[9],
            health[10], health[11], health[12], low_floor, low_min_ev, low_hit,
            low_noted, census.open_n, census.deferred_n, census.closed_n,
            census.waiting_oracle, census.low_rel_gaps);

    if (do_snap) {
        snprintf(snap_dir, sizeof snap_dir, "%s/snapshots", report_dir);
        if (ensure_dir(snap_dir) == 0) {
            char cmd[2048];
            snprintf(snap_path, sizeof snap_path, "%s/%s_%s", snap_dir, stamp,
                     strrchr(base, '/') ? strrchr(base, '/') + 1 : base);
            snprintf(cmd, sizeof cmd, "cp -f -- \"%s\" \"%s\"", base, snap_path);
            if (system(cmd) == 0)
                fprintf(md, "\nSnapshot: `%s`\n", snap_path);
            else
                fprintf(md, "\nSnapshot: FAILED\n");
        }
    }

    /* latest pointers */
    {
        char latest_md[900], latest_js[900];
        snprintf(latest_md, sizeof latest_md, "%s/LATEST.md", report_dir);
        snprintf(latest_js, sizeof latest_js, "%s/LATEST.json", report_dir);
        unlink(latest_md);
        unlink(latest_js);
        symlink(strrchr(report_md, '/') ? strrchr(report_md, '/') + 1
                                        : report_md,
                latest_md);
        symlink(strrchr(report_json, '/') ? strrchr(report_json, '/') + 1
                                          : report_json,
                latest_js);
    }

    printf("JANITOR_OK units=%d demoted=%lld healed=%lld low_hits=%zu "
           "low_noted=%zu ledger_closed=%zu deferred=%zu\n",
           nunits, health[1], health[5], low_hit, low_noted, census.closed_n,
           census.deferred_n);
    printf("JANITOR_REPORT %s\n", report_md);
    rc = 0;

done:
    if (md) fclose(md);
    if (js) fclose(js);
    acquire_ledger_free(&ledger);
    if (host) soul_close(host);
    release_lock(lock_path);
    return rc;
}
