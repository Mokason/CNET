/* CNET dense-bucket consolidate (G3).
 *
 * Plan (default) or apply a subset export that keeps:
 *   - all protected units (non acq_tk*, json_toolcall*, policy protect prefixes)
 *   - top K acq_tk* per (in_dim,out_dim) bucket by reliability score
 *
 * Always pins before apply. Writes plan JSON under artifacts/janitor/.
 *
 * Usage:
 *   ./bin/cnet_consolidate [base.cnb]           # dry-run plan
 *   CNET_CONSOLIDATE_APPLY=1 ./bin/cnet_consolidate [base.cnb]
 *
 * Env / policy:
 *   CNET_GOV_KEEP_PER_BUCKET   default 48
 *   CNET_GOV_BUCKET_MIN        only shrink buckets larger than this (default 32)
 *   CNET_CONSOLIDATE_OUT       output path (default <base>.consolidated.cnb)
 *   CNET_CONSOLIDATE_REPLACE=1 after apply, replace base with out (needs pin)
 *   CNET_CONSOLIDATE_DROP_GH=1 also drop acq_skill_gh_* noise (never keep)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

#include "../include/base.h"
#include "../include/soul_host.h"
#include "../include/cnet_governance.h"
#include "../include/nn.h"

typedef struct {
    char name[CNB_NAME_MAX];
    int in_d, out_d;
    int score; /* reliability milli + small tiebreak */
    int keep;
    int protected;
    int unmeasured;  /* no recorded execution outcomes -> never rank/drop it */
} UnitRow;

typedef struct {
    char **names;
    size_t n, cap;
} KeepSet;

static int env_flag(const char *k) {
    const char *v = getenv(k);
    return v && v[0] == '1' && !v[1];
}

static long env_l(const char *k, long d) {
    const char *v = getenv(k);
    return (v && v[0]) ? atol(v) : d;
}

/* Ghost/freeform skill noise minted as acq_skill_gh_* — opt-in drop. */
static int is_gh_noise_name(const char *nm) {
    if (!nm || !nm[0]) return 0;
    if (strncmp(nm, "acq_skill_gh_", 13) == 0) return 1;
    /* near-miss clones like acq_skill_gh_*x / *y / *z suffixes still match prefix */
    return 0;
}

static int is_protected_name(const char *nm) {
    if (!nm || !nm[0]) return 1;
    if (strncmp(nm, "json_toolcall", 13) == 0) return 1;
    if (strncmp(nm, "le8_", 4) == 0) return 1;
    /* Explicit gh_ drop overrides the non-token protect rule. */
    if (env_flag("CNET_CONSOLIDATE_DROP_GH") && is_gh_noise_name(nm)) return 0;
    if (strncmp(nm, "acq_tk", 6) != 0) return 1; /* non-token units kept */
    return 0;
}

static int keep_cb(const char *name, void *ctx) {
    KeepSet *ks = ctx;
    size_t i;
    for (i = 0; i < ks->n; i++)
        if (strcmp(ks->names[i], name) == 0) return 1;
    return 0;
}


static int ensure_dir(const char *path) {
    char tmp[1024];
    size_t i, len;
    if (!path || !path[0]) return -1;
    snprintf(tmp, sizeof tmp, "%s", path);
    len = strlen(tmp);
    for (i = 1; i < len; i++) {
        if (tmp[i] == '/') {
            tmp[i] = '\0';
            mkdir(tmp, 0755);
            tmp[i] = '/';
        }
    }
    return mkdir(tmp, 0755) == 0 || errno == EEXIST ? 0 : -1;
}

static void keep_add(KeepSet *ks, const char *nm) {
    size_t i;
    char *c;
    for (i = 0; i < ks->n; i++)
        if (strcmp(ks->names[i], nm) == 0) return;
    if (ks->n == ks->cap) {
        size_t nc = ks->cap ? ks->cap * 2 : 64;
        char **nn = realloc(ks->names, nc * sizeof *nn);
        if (!nn) return;
        ks->names = nn;
        ks->cap = nc;
    }
    c = strdup(nm);
    if (!c) return;
    ks->names[ks->n++] = c;
}

int main(int argc, char **argv) {
    const char *base;
    const char *report_dir = "artifacts/janitor";
    char out_path[1024], plan_path[1024], pin_path[CNET_GOV_PATH_MAX];
    char stamp[32];
    CnetGovernancePolicy gov;
    SoulHost *host = NULL;
    UnitRow *rows = NULL;
    size_t nrows = 0, rcap = 0;
    int nunits, ui;
    long keep_k, bucket_min;
    int apply, replace;
    KeepSet keep;
    size_t kept = 0, dropped = 0;
    size_t unmeasured_spared = 0;
    time_t now = time(NULL);
    struct tm tm_now;
    FILE *plan;
    int rc = 1;

    base = (argc >= 2 && argv[1][0]) ? argv[1] : getenv("CNET_BASE_PATH");
    if (!base || !base[0]) base = "soul_gemma4v2_final.cnb";
    (void)cnet_gov_policy_load(&gov, getenv("CNET_GOV_POLICY")
                                         ? getenv("CNET_GOV_POLICY")
                                         : "config/governance_policy.env");
    keep_k = env_l("CNET_GOV_KEEP_PER_BUCKET", 48);
    bucket_min = env_l("CNET_GOV_BUCKET_MIN", 32);
    apply = env_flag("CNET_CONSOLIDATE_APPLY");
    replace = env_flag("CNET_CONSOLIDATE_REPLACE");

    if (access(base, R_OK) != 0) {
        fprintf(stderr, "cnet_consolidate: missing base %s\n", base);
        return 2;
    }
    {
        char stop[800];
        snprintf(stop, sizeof stop, "%s.stop", base);
        if (gov.refuse_if_stop && access(stop, F_OK) == 0) {
            fprintf(stderr, "cnet_consolidate: refuse stop file\n");
            return 3;
        }
    }

    localtime_r(&now, &tm_now);
    strftime(stamp, sizeof stamp, "%Y%m%d_%H%M%S", &tm_now);
    ensure_dir(report_dir);
    snprintf(plan_path, sizeof plan_path, "%s/consolidate_%s.json", report_dir,
             stamp);
    {
        const char *o = getenv("CNET_CONSOLIDATE_OUT");
        if (o && o[0])
            snprintf(out_path, sizeof out_path, "%s", o);
        else
            snprintf(out_path, sizeof out_path, "%s.consolidated.cnb", base);
    }

    if (soul_open(base, NULL, &host) != 0 || !host) {
        fprintf(stderr, "cnet_consolidate: soul_open failed\n");
        return 4;
    }
    nunits = soul_unit_count(host);
    if (nunits < 0) nunits = 0;

    for (ui = 0; ui < nunits; ui++) {
        UnitRow row;
        int idim = 0, odim = 0, reli;
        memset(&row, 0, sizeof row);
        if (soul_unit_name(host, ui, row.name, (int)sizeof row.name) != 0)
            continue;
        if (soul_unit_dims(host, row.name, &idim, &odim) != 0) continue;
        row.in_d = idim;
        row.out_d = odim;
        reli = soul_unit_reliability_milli(host, row.name);
        if (reli < 0) reli = 500;
        row.score = reli;
        /* Evidence gate.
         *
         * Nothing persisted reliability counters until gap_lane_persist_stats
         * existed, so soul_unit_reliability_milli returned the Laplace prior
         * (500) for EVERY unit and this bucket sort ranked a constant. The
         * 2026-07-25 16:14 apply dropped 108 certified acq_tk* units as
         * "lowest reliability" on the strength of that constant. Units with no
         * recorded outcomes are now unmeasured, not low-scoring, and are never
         * dropped. */
        row.unmeasured = soul_unit_evidence_count(host, row.name) <= 0;
        row.protected = is_protected_name(row.name);
        if (nrows == rcap) {
            size_t nc = rcap ? rcap * 2 : 128;
            UnitRow *nr = realloc(rows, nc * sizeof *nr);
            if (!nr) break;
            rows = nr;
            rcap = nc;
        }
        rows[nrows++] = row;
    }
    soul_close(host);
    host = NULL;

    memset(&keep, 0, sizeof keep);
    /* Always keep protected */
    {
        size_t i;
        for (i = 0; i < nrows; i++) {
            if (rows[i].protected) {
                rows[i].keep = 1;
                keep_add(&keep, rows[i].name);
            }
        }
    }
    /* Per dim bucket: rank non-protected, keep top K if bucket large */
    {
        size_t i, j;
        for (i = 0; i < nrows; i++) {
            int in_d, out_d, count = 0;
            UnitRow **bucket;
            int bcount = 0;
            if (rows[i].protected) continue;
            /* skip if already processed — mark by keep==-1 temp? use score scan */
            in_d = rows[i].in_d;
            out_d = rows[i].out_d;
            /* count bucket */
            for (j = 0; j < nrows; j++)
                if (!rows[j].protected && rows[j].in_d == in_d &&
                    rows[j].out_d == out_d)
                    count++;
            if (count == 0) continue;
            /* only process once per bucket: when i is first member */
            {
                size_t first = nrows;
                for (j = 0; j < nrows; j++)
                    if (!rows[j].protected && rows[j].in_d == in_d &&
                        rows[j].out_d == out_d) {
                        first = j;
                        break;
                    }
                if (first != i) continue;
            }
            bucket = calloc((size_t)count, sizeof *bucket);
            if (!bucket) continue;
            for (j = 0; j < nrows; j++)
                if (!rows[j].protected && rows[j].in_d == in_d &&
                    rows[j].out_d == out_d)
                    bucket[bcount++] = &rows[j];
            {
                int a, b;
                for (a = 0; a < bcount; a++)
                    for (b = a + 1; b < bcount; b++)
                        if (bucket[b]->score > bucket[a]->score) {
                            UnitRow *t = bucket[a];
                            bucket[a] = bucket[b];
                            bucket[b] = t;
                        }
            }
            if (count <= bucket_min) {
                /* keep all in small buckets */
                int k;
                for (k = 0; k < bcount; k++) {
                    bucket[k]->keep = 1;
                    keep_add(&keep, bucket[k]->name);
                }
            } else {
                int k;
                int take = (int)keep_k;
                if (take > bcount) take = bcount;
                for (k = 0; k < take; k++) {
                    bucket[k]->keep = 1;
                    keep_add(&keep, bucket[k]->name);
                }
                /* Below the cut: drop only what we actually measured. An
                   unmeasured unit has no score to lose a ranking on. */
                for (k = take; k < bcount; k++) {
                    if (bucket[k]->unmeasured) {
                        bucket[k]->keep = 1;
                        keep_add(&keep, bucket[k]->name);
                        unmeasured_spared++;
                    } else {
                        bucket[k]->keep = 0;
                    }
                }
            }
            free(bucket);
        }
    }

    /* Force-drop gh_ noise even if a small dim-bucket would have kept them. */
    if (env_flag("CNET_CONSOLIDATE_DROP_GH")) {
        size_t i, j;
        for (i = 0; i < nrows; i++) {
            if (!is_gh_noise_name(rows[i].name)) continue;
            rows[i].keep = 0;
            rows[i].protected = 0;
        }
        /* Rebuild keep set from rows with keep==1 */
        for (j = 0; j < keep.n; j++) free(keep.names[j]);
        free(keep.names);
        memset(&keep, 0, sizeof keep);
        for (i = 0; i < nrows; i++)
            if (rows[i].keep) keep_add(&keep, rows[i].name);
    }

    kept = 0;
    dropped = 0;
    for (size_t i = 0; i < nrows; i++) {
        if (rows[i].keep) kept++;
        else dropped++;
    }

    plan = fopen(plan_path, "w");
    if (!plan) {
        fprintf(stderr, "cannot write plan\n");
        goto done;
    }
    fprintf(plan,
            "{\n  \"stamp\": \"%s\",\n  \"base\": \"%s\",\n  \"units_in\": %zu,\n"
            "  \"keep\": %zu,\n  \"drop\": %zu,\n  \"keep_per_bucket\": %ld,\n"
            "  \"bucket_min\": %ld,\n  \"apply\": %s,\n  \"out\": \"%s\",\n"
            "  \"dropped_samples\": [\n",
            stamp, base, nrows, kept, dropped, keep_k, bucket_min,
            apply ? "true" : "false", out_path);
    {
        size_t i, n = 0;
        for (i = 0; i < nrows && n < 40; i++) {
            if (rows[i].keep) continue;
            fprintf(plan, "    %s\"%s\"", n ? ",\n" : "", rows[i].name);
            n++;
        }
        fprintf(plan, "\n  ]\n}\n");
    }
    fclose(plan);
    plan = NULL;

    printf("CONSOLIDATE_PLAN in=%zu keep=%zu drop=%zu plan=%s%s\n", nrows, kept,
           dropped, plan_path,
           env_flag("CNET_CONSOLIDATE_DROP_GH") ? " drop_gh=1" : "");
    if (unmeasured_spared)
        printf("CONSOLIDATE_SPARED_UNMEASURED %zu (no execution outcomes "
               "recorded — not rankable, not dropped)\n",
               unmeasured_spared);

    if (!apply) {
        printf("CONSOLIDATE_DRY_RUN (set CNET_CONSOLIDATE_APPLY=1 to export "
               "subset)\n");
        rc = 0;
        goto done;
    }

    if (dropped == 0) {
        printf("CONSOLIDATE_NOTHING_TO_DROP\n");
        rc = 0;
        goto done;
    }

    /* Pin before apply */
    pin_path[0] = '\0';
    gov.snapshot_on_run = 1;
    if (cnet_gov_pin_snapshot(&gov, base, pin_path, sizeof pin_path) != 0) {
        fprintf(stderr, "cnet_consolidate: pin failed — refuse apply\n");
        rc = 5;
        goto done;
    }
    printf("CONSOLIDATE_PIN %s\n", pin_path);

    {
        CnetBase src, dst;
        cnb_init(&src);
        cnb_init(&dst);
        if (cnb_load(&src, base) != 0) {
            fprintf(stderr, "cnb_load failed\n");
            cnb_free(&src);
            rc = 6;
            goto done;
        }
        if (cnb_export_subset(&src, &dst, keep_cb, &keep) != 0) {
            fprintf(stderr, "export_subset failed\n");
            cnb_free(&src);
            cnb_free(&dst);
            rc = 7;
            goto done;
        }
        if (cnb_save(&dst, out_path) != 0) {
            fprintf(stderr, "cnb_save failed %s\n", out_path);
            cnb_free(&src);
            cnb_free(&dst);
            rc = 8;
            goto done;
        }
        printf("CONSOLIDATE_WROTE %s units=%zu\n", out_path, dst.unit_count);
        cnb_free(&src);
        cnb_free(&dst);
    }

    if (replace) {
        char cmd[2200];
        snprintf(cmd, sizeof cmd, "cp -f -- \"%s\" \"%s\"", out_path, base);
        if (system(cmd) != 0) {
            fprintf(stderr, "replace base failed\n");
            rc = 9;
            goto done;
        }
        printf("CONSOLIDATE_REPLACED_BASE %s\n", base);
    }

    /* Durable audit record.
     *
     * An apply on 2026-07-25 16:14 took the live base from 228 units to 101 and
     * left no trace: logs/consolidate.log still held a DRY_RUN line from four
     * days earlier because that run's stdout was never captured. A prune of
     * certified work must be reconstructible regardless of who invoked the tool
     * or where its stdout went, so write the record here rather than relying on
     * the caller to redirect. */
    {
        const char *audit_path = getenv("CNET_CONSOLIDATE_AUDIT");
        char def[1200];
        FILE *af;
        if (!audit_path || !audit_path[0]) {
            snprintf(def, sizeof def, "%s/consolidate_audit.jsonl", report_dir);
            audit_path = def;
        }
        af = fopen(audit_path, "a");
        if (af) {
            fprintf(af,
                    "{\"ts\":%lld,\"base\":\"%s\",\"pin\":\"%s\",\"out\":\"%s\","
                    "\"plan\":\"%s\",\"in\":%zu,\"kept\":%zu,\"dropped\":%zu,"
                    "\"replaced_base\":%d}\n",
                    (long long)time(NULL), base, pin_path, out_path, plan_path,
                    nrows, kept, dropped, replace ? 1 : 0);
            fclose(af);
            printf("CONSOLIDATE_AUDIT %s\n", audit_path);
        } else {
            fprintf(stderr,
                    "cnet_consolidate: WARNING could not write audit %s\n",
                    audit_path);
        }
    }

    printf("CONSOLIDATE_APPLY_OK keep=%zu drop=%zu\n", kept, dropped);
    rc = 0;

done:
    if (rows) free(rows);
    if (keep.names) {
        size_t i;
        for (i = 0; i < keep.n; i++) free(keep.names[i]);
        free(keep.names);
    }
    return rc;
}
