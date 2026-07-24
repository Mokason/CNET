/* CNET library janitor (governance G1+G2).
 *
 * Maintenance pass over a personal CNB under a human policy file:
 *   - load config/governance_policy.env (CNET_GOV_* env overlays)
 *   - soul_health_tick
 *   - low-reliability scan (+ optional LOW_RELIABILITY ledger notes)
 *   - dim-bucket near-duplicate census (report only; no auto-delete)
 *   - gap ledger census
 *   - optional rollback pin (rotated snapshots)
 *   - markdown + JSON report under artifacts/janitor/
 *
 * Safety:
 *   - Refuses if <base>.stop exists (when policy.refuse_if_stop)
 *   - Single-flight lock
 *   - Does not delete units; does not rewrite policy
 *
 * Usage: ./bin/cnet_janitor [base.cnb]
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
#include "../include/cnet_governance.h"

static int env_flag(const char *k) {
    const char *v = getenv(k);
    return v && v[0] == '1' && v[1] == '\0';
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
    ssize_t w;
    if (!path || !path[0]) return -1;
    fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0644);
    if (fd < 0) return -1;
    snprintf(pidbuf, sizeof pidbuf, "%d\n", (int)getpid());
    w = write(fd, pidbuf, strlen(pidbuf));
    (void)w;
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
        if (sscanf(line, "%d %d", &kind, &status) != 2) {
            c->other_n++;
            continue;
        }
        if (status == 0) c->open_n++;
        else if (status == 1) c->deferred_n++;
        else if (status == 2) c->closed_n++;
        else c->other_n++;
        {
            char *tok, *save = NULL, *copy = strdup(line);
            if (copy) {
                for (tok = strtok_r(copy, " \t\n", &save); tok;
                     tok = strtok_r(NULL, " \t\n", &save)) {
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

/* Dim-bucket duplicate census (report-only). */
typedef struct {
    int in_d, out_d;
    int count;
    char sample[3][128];
} DimBucket;

static void dedupe_scan(SoulHost *host, int nunits, FILE *md, int *out_buckets,
                        int *out_flagged) {
    DimBucket *bk = NULL;
    size_t nb = 0, cap = 0;
    int ui, flagged = 0, big = 0;
    fprintf(md, "## Near-duplicate dim census (report only)

");
    fprintf(md, "Groups units by (in_dim, out_dim). No deletion.\n\n");
    for (ui = 0; ui < nunits; ui++) {
        char name[128];
        int idim = 0, odim = 0;
        size_t j;
        int found = -1;
        if (soul_unit_name(host, ui, name, (int)sizeof name) != 0) continue;
        if (soul_unit_dims(host, name, &idim, &odim) != 0) continue;
        for (j = 0; j < nb; j++) {
            if (bk[j].in_d == idim && bk[j].out_d == odim) {
                found = (int)j;
                break;
            }
        }
        if (found < 0) {
            if (nb == cap) {
                size_t ncap = cap ? cap * 2 : 64;
                DimBucket *nbk = realloc(bk, ncap * sizeof *bk);
                if (!nbk) break;
                bk = nbk;
                cap = ncap;
            }
            bk[nb].in_d = idim;
            bk[nb].out_d = odim;
            bk[nb].count = 0;
            bk[nb].sample[0][0] = bk[nb].sample[1][0] = bk[nb].sample[2][0] =
                '\0';
            found = (int)nb;
            nb++;
        }
        {
            DimBucket *b = &bk[found];
            if (b->count < 3)
                snprintf(b->sample[b->count], sizeof b->sample[0], "%s", name);
            b->count++;
        }
    }
    fprintf(md, "| in | out | count | samples |\n|---|---|---|---|\n");
    {
        size_t j;
        for (j = 0; j < nb; j++) {
            if (bk[j].count < 5) continue;
            flagged++;
            big += bk[j].count;
            fprintf(md, "| %d | %d | %d | `%s` `%s` `%s` |\n", bk[j].in_d,
                    bk[j].out_d, bk[j].count, bk[j].sample[0],
                    bk[j].sample[1][0] ? bk[j].sample[1] : "",
                    bk[j].sample[2][0] ? bk[j].sample[2] : "");
        }
    }
    if (flagged == 0)
        fprintf(md, "_(no dim buckets with count ≥ 5)_\n");
    fprintf(md, "\nFlagged buckets: %d  Units in those buckets: %d\n\n", flagged,
            big);
    free(bk);
    if (out_buckets) *out_buckets = flagged;
    if (out_flagged) *out_flagged = big;
}

int main(int argc, char **argv) {
    const char *base;
    const char *report_dir;
    const char *policy_path;
    char ledger_path[768], stop_path[768], lock_path[800];
    char report_md[900], report_json[900], stamp[64];
    char pin_path[CNET_GOV_PATH_MAX];
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
    int dedupe_buckets = 0, dedupe_units = 0;
    int warn_units = 0, warn_oracle = 0, refuse_cap = 0;
    CnetGovernancePolicy gov;
    char pin_ok[8] = "no";

    base = (argc >= 2 && argv[1][0]) ? argv[1] : getenv("CNET_BASE_PATH");
    if (!base || !base[0]) base = "soul_gemma4v2_final.cnb";
    report_dir = getenv("CNET_JANITOR_REPORT_DIR");
    if (!report_dir || !report_dir[0]) report_dir = "artifacts/janitor";
    policy_path = getenv("CNET_GOV_POLICY");
    if (!policy_path || !policy_path[0])
        policy_path = "config/governance_policy.env";

    (void)cnet_gov_policy_load(&gov, policy_path);
    /* Legacy janitor env still works as overlay */
    if (getenv("CNET_JANITOR_LOW_REL"))
        gov.low_rel_floor = atof(getenv("CNET_JANITOR_LOW_REL"));
    if (getenv("CNET_JANITOR_LOW_REL_MIN_EV"))
        gov.low_rel_min_evidence =
            (size_t)atol(getenv("CNET_JANITOR_LOW_REL_MIN_EV"));
    if (env_flag("CNET_JANITOR_NOTE_LOW_REL")) gov.note_low_rel = 1;
    if (env_flag("CNET_JANITOR_SNAPSHOT")) gov.snapshot_on_run = 1;
    if (getenv("CNET_JANITOR_SNAPSHOT") &&
        getenv("CNET_JANITOR_SNAPSHOT")[0] == '0')
        gov.snapshot_on_run = 0;

    low_floor = gov.low_rel_floor;
    low_min_ev = (long)gov.low_rel_min_evidence;
    note_low = gov.note_low_rel;
    do_snap = gov.snapshot_on_run;

    if (!file_exists(base)) {
        fprintf(stderr, "cnet_janitor: base missing: %s\n", base);
        return 2;
    }
    snprintf(stop_path, sizeof stop_path, "%s.stop", base);
    if (gov.refuse_if_stop && file_exists(stop_path)) {
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
    pin_path[0] = '\0';

    printf("cnet_janitor: base=%s policy=%s report=%s\n", base, policy_path,
           report_md);
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
    if ((size_t)nunits >= gov.warn_units) warn_units = 1;
    if (gov.max_units && (size_t)nunits > gov.max_units) {
        warn_units = 1;
        if (gov.refuse_over_cap) refuse_cap = 1;
    }

    memset(&ledger, 0, sizeof ledger);
    if (note_low && file_exists(ledger_path))
        (void)acquire_ledger_load(&ledger, ledger_path);

    md = fopen(report_md, "w");
    js = fopen(report_json, "w");
    if (!md || !js) {
        fprintf(stderr, "cnet_janitor: cannot write reports\n");
        goto done;
    }

    fprintf(md, "# CNET Janitor Report (G1+G2)\n\n");
    fprintf(md, "- **When:** %s\n", stamp);
    fprintf(md, "- **Base:** `%s`\n", base);
    fprintf(md, "- **Units:** %d\n", nunits);
    fprintf(md, "- **Policy:** `%s`\n\n", policy_path);

    fprintf(md, "## Policy bounds\n\n");
    fprintf(md, "| key | value |\n|---|---|\n");
    fprintf(md, "| max_units | %zu |\n", gov.max_units);
    fprintf(md, "| warn_units | %zu |\n", gov.warn_units);
    fprintf(md, "| low_rel_floor | %.3f |\n", gov.low_rel_floor);
    fprintf(md, "| low_rel_min_evidence | %zu |\n", gov.low_rel_min_evidence);
    fprintf(md, "| note_low_rel | %d |\n", gov.note_low_rel);
    fprintf(md, "| snapshot_on_run | %d |\n", gov.snapshot_on_run);
    fprintf(md, "| max_snapshots | %zu |\n", gov.max_snapshots);
    fprintf(md, "| pin_dir | `%s` |\n", gov.pin_dir);
    fprintf(md, "| refuse_over_cap | %d |\n\n", gov.refuse_over_cap);
    if (warn_units)
        fprintf(md, "**WARN:** unit count %d crosses warn/max bounds.\n\n",
                nunits);

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
    fprintf(md,
            "| trust uncertified/evidenced/certified/demoted | "
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
            if (rel < low_floor) {
                char hl[512];
                int has_ev = 0;
                hl[0] = '\0';
                if (soul_unit_health_layers(host, name, hl, (int)sizeof hl) ==
                    0) {
                    const char *p = strstr(hl, "\"evidence\"");
                    if (p) {
                        p = strchr(p, ':');
                        if (p) ev = strtoul(p + 1, NULL, 10);
                        has_ev = 1;
                    }
                }
                if (!has_ev) {
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


    /* Janitor v2: gh_ skill noise census (report-only; prune opt-in later) */
    {
        size_t gh_n = 0;
        int vi;
        fprintf(md, "## Janitor v2 skill quality\n\n");
        fprintf(md, "| class | count |\n|---|---:|\n");
        for (vi = 0; vi < nunits; vi++) {
            char name[128];
            if (soul_unit_name(host, vi, name, (int)sizeof name) != 0) continue;
            if (strncmp(name, "acq_skill_gh_", 13) == 0 || strstr(name, "_gh_"))
                gh_n++;
        }
        fprintf(md, "| gh_ noise names | %zu |\n", gh_n);
        fprintf(md, "| units total | %d |\n\n", nunits);
        fprintf(md,
                "_Prune apply is opt-in via CNET_JANITOR_PRUNE_GH=1 "
                "(default report-only)._\\n\\n");
        if (getenv("CNET_JANITOR_PRUNE_GH") &&
            getenv("CNET_JANITOR_PRUNE_GH")[0] == '1') {
            FILE *pf = NULL;
            const char *outdir = getenv("CNET_JANITOR_OUT");
            char ppath[512];
            int vj;
            if (!outdir || !outdir[0]) outdir = "artifacts/janitor";
            snprintf(ppath, sizeof ppath, "%s/gh_prune_candidates.txt", outdir);
            {
                char cmd[640];
                snprintf(cmd, sizeof cmd, "mkdir -p '%s'", outdir);
                (void)system(cmd);
            }
            pf = fopen(ppath, "w");
            fprintf(md,
                    "PRUNE_GH: candidates → `%s` (no CNB rewrite; pin-aware "
                    "list for operator/janitor apply).\\n\\n",
                    ppath);
            if (pf) {
                for (vj = 0; vj < nunits; vj++) {
                    char nm[128];
                    if (soul_unit_name(host, vj, nm, (int)sizeof nm) != 0)
                        continue;
                    if (strncmp(nm, "acq_skill_gh_", 13) == 0 ||
                        strstr(nm, "_gh_"))
                        fprintf(pf, "%s\n", nm);
                }
                fclose(pf);
            }
        }
    }

    dedupe_scan(host, nunits, md, &dedupe_buckets, &dedupe_units);

    census_ledger(ledger_path, &census);
    if (census.waiting_oracle >= gov.max_waiting_oracle_warn) warn_oracle = 1;
    fprintf(md, "## Gap ledger census\n\n");
    fprintf(md, "| status | count |\n|---|---|\n");
    fprintf(md, "| open | %zu |\n", census.open_n);
    fprintf(md, "| deferred | %zu |\n", census.deferred_n);
    fprintf(md, "| closed | %zu |\n", census.closed_n);
    fprintf(md, "| waiting_oracle (token) | %zu |\n", census.waiting_oracle);
    fprintf(md, "| low_reliability (token) | %zu |\n\n", census.low_rel_gaps);
    if (warn_oracle)
        fprintf(md, "**WARN:** waiting_oracle ≥ policy warn (%zu).\n\n",
                gov.max_waiting_oracle_warn);

    fprintf(md, "## Serve stats\n\n");
    fprintf(md, "See `%s.state/soul_serve.stats` after close.\n\n", base);

    fprintf(md, "## Rollback pin\n\n");
    if (do_snap) {
        if (cnet_gov_pin_snapshot(&gov, base, pin_path, sizeof pin_path) == 0) {
            fprintf(md, "Pinned: `%s`\n", pin_path);
            snprintf(pin_ok, sizeof pin_ok, "yes");
        } else {
            fprintf(md, "Pin: **FAILED**\n");
            snprintf(pin_ok, sizeof pin_ok, "fail");
        }
    } else {
        fprintf(md, "Pin: skipped (policy snapshot_on_run=0)\n");
    }
    fprintf(md, "\nRestore: `bash scripts/cnet_janitor_restore.sh [pin.cnb]`\n\n");

    fprintf(md, "## Actions taken\n\n");
    fprintf(md, "- health_tick: yes (hn=%d)\n", hn);
    fprintf(md, "- low_rel_notes: %zu\n", low_noted);
    fprintf(md, "- dedupe_buckets_flagged: %d\n", dedupe_buckets);
    fprintf(md, "- pin: %s\n", pin_ok);
    fprintf(md, "\n## Governance\n\n");
    fprintf(md,
            "G1+G2 janitor. Policy bounds enforced as warnings"
            "%s. No unit deletion. Human charter remains external.\n",
            refuse_cap ? " (**REFUSE_OVER_CAP**)" : "");

    fprintf(js,
            "{\n"
            "  \"stamp\": \"%s\",\n"
            "  \"base\": \"%s\",\n"
            "  \"units\": %d,\n"
            "  \"policy\": {\n"
            "    \"path\": \"%s\",\n"
            "    \"max_units\": %zu,\n"
            "    \"warn_units\": %zu,\n"
            "    \"low_rel_floor\": %.4f,\n"
            "    \"snapshot_on_run\": %d,\n"
            "    \"max_snapshots\": %zu,\n"
            "    \"pin_dir\": \"%s\"\n"
            "  },\n"
            "  \"warnings\": {\n"
            "    \"units\": %s,\n"
            "    \"waiting_oracle\": %s,\n"
            "    \"refuse_over_cap\": %s\n"
            "  },\n"
            "  \"health\": {\n"
            "    \"entries\": %lld,\n"
            "    \"demoted_by_audit\": %lld,\n"
            "    \"healed\": %lld,\n"
            "    \"trust\": {\"uncertified\": %lld, \"evidenced\": %lld, "
            "\"certified\": %lld, \"demoted\": %lld}\n"
            "  },\n"
            "  \"low_rel\": {\"floor\": %.4f, \"min_ev\": %ld, \"hits\": %zu, "
            "\"noted\": %zu},\n"
            "  \"dedupe\": {\"buckets\": %d, \"units_in_buckets\": %d},\n"
            "  \"ledger\": {\"open\": %zu, \"deferred\": %zu, \"closed\": %zu, "
            "\"waiting_oracle\": %zu, \"low_rel_gaps\": %zu},\n"
            "  \"pin\": {\"ok\": \"%s\", \"path\": \"%s\"}\n"
            "}\n",
            stamp, base, nunits, policy_path, gov.max_units, gov.warn_units,
            gov.low_rel_floor, gov.snapshot_on_run, gov.max_snapshots,
            gov.pin_dir, warn_units ? "true" : "false",
            warn_oracle ? "true" : "false", refuse_cap ? "true" : "false",
            health[0], health[1], health[5], health[9], health[10], health[11],
            health[12], low_floor, low_min_ev, low_hit, low_noted,
            dedupe_buckets, dedupe_units, census.open_n, census.deferred_n,
            census.closed_n, census.waiting_oracle, census.low_rel_gaps, pin_ok,
            pin_path[0] ? pin_path : "");

    {
        char latest_md[900], latest_js[900];
        snprintf(latest_md, sizeof latest_md, "%s/LATEST.md", report_dir);
        snprintf(latest_js, sizeof latest_js, "%s/LATEST.json", report_dir);
        unlink(latest_md);
        unlink(latest_js);
        (void)symlink(strrchr(report_md, '/') ? strrchr(report_md, '/') + 1
                                              : report_md,
                      latest_md);
        (void)symlink(strrchr(report_json, '/') ? strrchr(report_json, '/') + 1
                                                : report_json,
                      latest_js);
    }

    printf("JANITOR_OK units=%d demoted=%lld healed=%lld low_hits=%zu "
           "low_noted=%zu ledger_closed=%zu deferred=%zu dedupe_buckets=%d "
           "pin=%s warn_units=%d\n",
           nunits, health[1], health[5], low_hit, low_noted, census.closed_n,
           census.deferred_n, dedupe_buckets, pin_ok, warn_units);
    printf("JANITOR_REPORT %s\n", report_md);
    rc = refuse_cap ? 8 : 0;

done:
    if (md) fclose(md);
    if (js) fclose(js);
    acquire_ledger_free(&ledger);
    if (host) soul_close(host);
    release_lock(lock_path);
    return rc;
}
