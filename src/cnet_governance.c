#include "../include/cnet_governance.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int ensure_dir(const char *path) {
    char tmp[CNET_GOV_PATH_MAX];
    size_t len, i;
    if (!path || !path[0]) return -1;
    snprintf(tmp, sizeof tmp, "%s", path);
    len = strlen(tmp);
    for (i = 1; i < len; i++) {
        if (tmp[i] == '/') {
            tmp[i] = '\0';
            if (cnet_mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
            tmp[i] = '/';
        }
    }
    if (cnet_mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

static long env_l(const char *k, long d) {
    const char *v = getenv(k);
    return (v && v[0]) ? atol(v) : d;
}

static double env_d(const char *k, double d) {
    const char *v = getenv(k);
    return (v && v[0]) ? atof(v) : d;
}

static int env_flag_def(const char *k, int d) {
    const char *v = getenv(k);
    if (!v || !v[0]) return d;
    if (v[0] == '1' && !v[1]) return 1;
    if (v[0] == '0' && !v[1]) return 0;
    return d;
}

void cnet_gov_policy_defaults(CnetGovernancePolicy *p) {
    if (!p) return;
    memset(p, 0, sizeof *p);
    p->version = CNET_GOV_VERSION;
    p->max_units = 2000;
    p->warn_units = 800;
    p->low_rel_floor = 0.55;
    p->low_rel_min_evidence = 64;
    p->note_low_rel = 1;
    p->snapshot_on_run = 1;
    p->max_snapshots = 3;
    p->refuse_if_stop = 1;
    p->refuse_over_cap = 0;
    p->max_waiting_oracle_warn = 64;
    snprintf(p->pin_dir, sizeof p->pin_dir, "artifacts/janitor/pins");
    p->policy_path[0] = '\0';
}

void cnet_gov_policy_apply_env(CnetGovernancePolicy *p) {
    const char *s;
    if (!p) return;
    p->max_units = (size_t)env_l("CNET_GOV_MAX_UNITS", (long)p->max_units);
    p->warn_units = (size_t)env_l("CNET_GOV_WARN_UNITS", (long)p->warn_units);
    p->low_rel_floor = env_d("CNET_GOV_LOW_REL", p->low_rel_floor);
    p->low_rel_min_evidence =
        (size_t)env_l("CNET_GOV_LOW_REL_MIN_EV", (long)p->low_rel_min_evidence);
    p->note_low_rel = env_flag_def("CNET_GOV_NOTE_LOW_REL", p->note_low_rel);
    p->snapshot_on_run =
        env_flag_def("CNET_GOV_SNAPSHOT", p->snapshot_on_run);
    p->max_snapshots =
        (size_t)env_l("CNET_GOV_MAX_SNAPSHOTS", (long)p->max_snapshots);
    p->refuse_if_stop =
        env_flag_def("CNET_GOV_REFUSE_IF_STOP", p->refuse_if_stop);
    p->refuse_over_cap =
        env_flag_def("CNET_GOV_REFUSE_OVER_CAP", p->refuse_over_cap);
    p->max_waiting_oracle_warn = (size_t)env_l(
        "CNET_GOV_MAX_WAITING_ORACLE_WARN", (long)p->max_waiting_oracle_warn);
    s = getenv("CNET_GOV_PIN_DIR");
    if (s && s[0]) snprintf(p->pin_dir, sizeof p->pin_dir, "%s", s);
}

static void trim(char *s) {
    char *e;
    while (*s == ' ' || *s == '\t') memmove(s, s + 1, strlen(s));
    e = s + strlen(s);
    while (e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' ||
                     e[-1] == '\t'))
        *--e = '\0';
}

int cnet_gov_policy_load(CnetGovernancePolicy *p, const char *path) {
    FILE *f;
    char line[1024];
    if (!p) return -1;
    cnet_gov_policy_defaults(p);
    if (path && path[0])
        snprintf(p->policy_path, sizeof p->policy_path, "%s", path);
    if (!path || !path[0]) {
        cnet_gov_policy_apply_env(p);
        return 0;
    }
    f = fopen(path, "r");
    if (!f) {
        cnet_gov_policy_apply_env(p);
        return 0; /* missing = defaults */
    }
    while (fgets(line, sizeof line, f)) {
        char *eq, *k, *v;
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        k = line;
        v = eq + 1;
        trim(k);
        trim(v);
        if (strcmp(k, "version") == 0) p->version = atoi(v);
        else if (strcmp(k, "max_units") == 0) p->max_units = (size_t)atol(v);
        else if (strcmp(k, "warn_units") == 0) p->warn_units = (size_t)atol(v);
        else if (strcmp(k, "low_rel_floor") == 0) p->low_rel_floor = atof(v);
        else if (strcmp(k, "low_rel_min_evidence") == 0)
            p->low_rel_min_evidence = (size_t)atol(v);
        else if (strcmp(k, "note_low_rel") == 0) p->note_low_rel = atoi(v) ? 1 : 0;
        else if (strcmp(k, "snapshot_on_run") == 0)
            p->snapshot_on_run = atoi(v) ? 1 : 0;
        else if (strcmp(k, "max_snapshots") == 0)
            p->max_snapshots = (size_t)atol(v);
        else if (strcmp(k, "refuse_if_stop") == 0)
            p->refuse_if_stop = atoi(v) ? 1 : 0;
        else if (strcmp(k, "refuse_over_cap") == 0)
            p->refuse_over_cap = atoi(v) ? 1 : 0;
        else if (strcmp(k, "max_waiting_oracle_warn") == 0)
            p->max_waiting_oracle_warn = (size_t)atol(v);
        else if (strcmp(k, "pin_dir") == 0)
            snprintf(p->pin_dir, sizeof p->pin_dir, "%s", v);
    }
    fclose(f);
    cnet_gov_policy_apply_env(p); /* env wins */
    return 0;
}

int cnet_gov_policy_save(const CnetGovernancePolicy *p, const char *path) {
    FILE *f;
    if (!p || !path || !path[0]) return -1;
    f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f,
            "# CNET governance policy (human-edited constitution)\n"
            "# Janitor/lane must stay inside these bounds.\n"
            "version=%d\n"
            "max_units=%zu\n"
            "warn_units=%zu\n"
            "low_rel_floor=%.4f\n"
            "low_rel_min_evidence=%zu\n"
            "note_low_rel=%d\n"
            "snapshot_on_run=%d\n"
            "max_snapshots=%zu\n"
            "refuse_if_stop=%d\n"
            "refuse_over_cap=%d\n"
            "max_waiting_oracle_warn=%zu\n"
            "pin_dir=%s\n",
            p->version, p->max_units, p->warn_units, p->low_rel_floor,
            p->low_rel_min_evidence, p->note_low_rel, p->snapshot_on_run,
            p->max_snapshots, p->refuse_if_stop, p->refuse_over_cap,
            p->max_waiting_oracle_warn, p->pin_dir);
    fclose(f);
    return 0;
}

typedef struct {
    char path[CNET_GOV_PATH_MAX];
    time_t mtime;
} PinEnt;

static int pin_cmp_mtime(const void *a, const void *b) {
    const PinEnt *pa = a, *pb = b;
    if (pa->mtime < pb->mtime) return -1;
    if (pa->mtime > pb->mtime) return 1;
    return 0;
}

static void rotate_pins(const char *dir, size_t keep) {
    DIR *d;
    struct dirent *de;
    PinEnt ents[64];
    size_t n = 0, i;
    if (!dir || keep == 0) return;
    d = opendir(dir);
    if (!d) return;
    while ((de = readdir(d)) != NULL && n < 64) {
        struct stat st;
        char full[CNET_GOV_PATH_MAX];
        size_t len = strlen(de->d_name);
        if (strncmp(de->d_name, "pin_", 4) != 0) continue;
        if (len < 5 || strcmp(de->d_name + len - 4, ".cnb") != 0) continue;
        snprintf(full, sizeof full, "%s/%s", dir, de->d_name);
        if (stat(full, &st) != 0) continue;
        snprintf(ents[n].path, sizeof ents[n].path, "%s", full);
        ents[n].mtime = st.st_mtime;
        n++;
    }
    closedir(d);
    if (n <= keep) return;
    qsort(ents, n, sizeof ents[0], pin_cmp_mtime);
    for (i = 0; i < n - keep; i++) unlink(ents[i].path);
}

static int copy_regular_file_atomic(const char *source, const char *dest) {
    unsigned char buffer[64 * 1024];
    char tmp[CNET_GOV_PATH_MAX];
    struct stat source_stat;
    FILE *in = NULL, *out = NULL;
    int out_fd = -1;
    int ok = 0;
    int n;

    if (!source || !dest || !source[0] || !dest[0]) return -1;
    n = snprintf(tmp, sizeof tmp, "%s.tmp.XXXXXX", dest);
    if (n < 0 || (size_t)n >= sizeof tmp) return -1;
    in = fopen(source, "rb");
    if (!in) return -1;
    if (fstat(fileno(in), &source_stat) != 0 ||
        !S_ISREG(source_stat.st_mode))
        goto done;
    out_fd = mkstemp(tmp);
    if (out_fd < 0) goto done;
    if (fchmod(out_fd, source_stat.st_mode & 0777) != 0) goto done;
    out = fdopen(out_fd, "wb");
    if (!out) goto done;
    out_fd = -1;

    for (;;) {
        size_t got = fread(buffer, 1, sizeof buffer, in);
        if (got && fwrite(buffer, 1, got, out) != got) goto done;
        if (got < sizeof buffer) {
            if (ferror(in)) goto done;
            break;
        }
    }
    if (fflush(out) != 0 || cnet_fsync(fileno(out)) != 0) goto done;
    if (fclose(out) != 0) {
        out = NULL;
        goto done;
    }
    out = NULL;
    if (rename(tmp, dest) != 0) goto done;
    ok = 1;

done:
    if (in) fclose(in);
    if (out) fclose(out);
    if (out_fd >= 0) close(out_fd);
    if (!ok) unlink(tmp);
    return ok ? 0 : -1;
}

int cnet_gov_pin_snapshot(const CnetGovernancePolicy *p, const char *base_path,
                          char *out_path, size_t out_cap) {
    char stamp[32], dest[CNET_GOV_PATH_MAX];
    char cur[CNET_GOV_PATH_MAX];
    const char *base_name;
    time_t now = time(NULL);
    struct tm tm_now;
    FILE *cf;
    if (!p || !base_path || !base_path[0]) return -1;
    if (ensure_dir(p->pin_dir) != 0) return -2;
    cnet_localtime_r(&now, &tm_now);
    strftime(stamp, sizeof stamp, "%Y%m%d_%H%M%S", &tm_now);
    base_name = strrchr(base_path, '/');
    base_name = base_name ? base_name + 1 : base_path;
    if (!base_name[0] ||
        snprintf(dest, sizeof dest, "%s/pin_%s_%s", p->pin_dir, stamp,
                 base_name) >= (int)sizeof dest)
        return -1;
    if (copy_regular_file_atomic(base_path, dest) != 0) return -3;
    if (snprintf(cur, sizeof cur, "%s/CURRENT", p->pin_dir) >=
        (int)sizeof cur)
        return -2;
    cf = fopen(cur, "w");
    if (cf) {
        fprintf(cf, "%s\n", dest);
        fclose(cf);
    }
    if (p->max_snapshots > 0) rotate_pins(p->pin_dir, p->max_snapshots);
    if (out_path && out_cap) snprintf(out_path, out_cap, "%s", dest);
    return 0;
}

int cnet_gov_current_pin(const CnetGovernancePolicy *p, char *out,
                         size_t out_cap) {
    char cur[CNET_GOV_PATH_MAX];
    FILE *f;
    if (!p || !out || out_cap == 0) return -1;
    out[0] = '\0';
    if (snprintf(cur, sizeof cur, "%s/CURRENT", p->pin_dir) >=
        (int)sizeof cur)
        return -1;
    f = fopen(cur, "r");
    if (!f) return -2;
    if (!fgets(out, (int)out_cap, f)) {
        fclose(f);
        return -3;
    }
    fclose(f);
    {
        size_t n = strlen(out);
        while (n && (out[n - 1] == '\n' || out[n - 1] == '\r')) out[--n] = '\0';
    }
    return out[0] ? 0 : -4;
}

int cnet_gov_restore_pin(const char *pin_path, const char *base_path) {
    char logp[CNET_GOV_PATH_MAX];
    FILE *lf;
    time_t now = time(NULL);
    if (!pin_path || !base_path || !pin_path[0] || !base_path[0]) return -1;
    if (access(pin_path, R_OK) != 0) return -2;
    if (copy_regular_file_atomic(pin_path, base_path) != 0) return -3;
    if (snprintf(logp, sizeof logp, "%s.restore.log", base_path) >=
        (int)sizeof logp)
        return -1;
    lf = fopen(logp, "a");
    if (lf) {
        fprintf(lf, "%ld restore_from %s\n", (long)now, pin_path);
        fclose(lf);
    }
    return 0;
}
