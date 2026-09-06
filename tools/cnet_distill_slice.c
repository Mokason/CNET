/* cnet_distill_slice — propose a domain slice from teacher traces (NOT seal).
 *
 * usage:
 *   cnet_distill_slice --domain NAME [--query "q"]... [--out DIR] [--dry-run]
 *   cnet_distill_slice --domain NAME --from-file queries.txt [--teacher]
 *
 * Writes:
 *   <out>/<domain>-<ts>/PROPOSE.json
 *   <out>/<domain>-<ts>/rows.jsonl     teacher/dry rows, claimed_cert=0
 *
 * Law: propose only. Teacher drafts never CERT. Anti-collapse: no LOCAL
 * CNET answers used as student labels.
 *
 * Anti-collapse is ENFORCED here, not merely asserted in the JSON: every query
 * is first put to the live front door with the residual disabled, and any query
 * CNET already answers LOCAL (Tier-A) is dropped with reason
 * anti_collapse_local_tier_a. Distilling our own certified answers back into a
 * student is the collapse the doctrine forbids, so it has to be a code gate.
 * Disable only for offline tests with --no-collapse-check.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MAX_Q 32
#define QLEN 512
#define ALEN 768

static int is_dir(const char *p) {
    struct stat st;
    return p && p[0] && stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

/* mkdir -p: the inbox lives under $CNET_MINIMAL_ROOT/var/... whose parents may
 * not exist on a fresh deploy; a single mkdir() silently loses the slice. */
static int mymkdir(const char *path) {
    char tmp[512];
    char *q;
    if (!path || !path[0]) return -1;
    if ((size_t)snprintf(tmp, sizeof tmp, "%s", path) >= sizeof tmp) return -1;
    for (q = tmp + 1; *q; q++) {
        if (*q != '/') continue;
        *q = 0;
        if (mkdir(tmp, 0755) != 0 && !is_dir(tmp)) return -1;
        *q = '/';
    }
    if (mkdir(tmp, 0755) != 0 && !is_dir(tmp)) return -1;
    return 0;
}

/* Shell single-quote. Queries can come from a miss_log cluster, i.e. from
 * arbitrary user text, and they reach curl through a shell. */
static int sh_quote(const char *in, char *out, size_t cap) {
    size_t o = 0, i;
    if (!in || cap < 3) return -1;
    out[o++] = 0x27;
    for (i = 0; in[i]; i++) {
        if (in[i] == 0x27) {
            if (o + 4 >= cap) return -1;
            memcpy(out + o, "\'\\\'\'", 4);
            o += 4;
        } else {
            if (o + 2 >= cap) return -1;
            out[o++] = in[i];
        }
    }
    out[o++] = 0x27;
    out[o] = 0;
    return 0;
}

/* JSON-escape into a buffer (the fputc version below writes to a stream). */
static void json_esc_buf(const char *in, char *out, size_t cap) {
    size_t o = 0, i;
    for (i = 0; in && in[i] && o + 8 < cap; i++) {
        unsigned char c = (unsigned char)in[i];
        switch (c) {
        case '"':  out[o++] = 0x5c; out[o++] = '"';  break;
        case 0x5c: out[o++] = 0x5c; out[o++] = 0x5c; break;
        case '\n': out[o++] = 0x5c; out[o++] = 'n';  break;
        case '\r': out[o++] = 0x5c; out[o++] = 'r';  break;
        case '\t': out[o++] = 0x5c; out[o++] = 't';  break;
        default:
            if (c < 0x20) { o += (size_t)snprintf(out + o, 8, "\\u%04x", c); }
            else out[o++] = (char)c;
        }
    }
    out[o] = 0;
}

static int json_esc(FILE *f, const char *s) {
    if (!s) return 0;
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') fputc('\\', f);
        if (*s != '\n' && *s != '\r') fputc(*s, f);
    }
    return 0;
}

static int teacher_ask(const char *q, char *out, size_t n) {
    const char *http = getenv("CNET_STAGE_HTTP");
    const char *model = getenv("CNET_STAGE_MODEL");
    char cmd[4096];
    FILE *fp;
    char resp[8192];
    size_t got;
    const char *p;
    if (!http || !http[0]) http = "http://127.0.0.1:8081";
    if (!model || !model[0]) model = "held";
    /* curl one-shot; fail closed on empty.
     * The query is JSON-escaped and then the whole payload is shell-quoted:
     * a bare interpolation let a query containing a quote break out of both
     * the JSON string and the shell word. Queries can come from a miss_log
     * cluster, so treat them as untrusted. */
    {
        char esc[QLEN * 2], body[QLEN * 3], bodyq[QLEN * 4], urlq[512];
        json_esc_buf(q, esc, sizeof esc);
        snprintf(body, sizeof body,
                 "{\"model\":\"%s\",\"messages\":[{\"role\":\"user\","
                 "\"content\":\"%s\"}],\"max_tokens\":128,\"temperature\":0.2}",
                 model, esc);
        snprintf(urlq, sizeof urlq, "%s/v1/chat/completions", http);
        if (sh_quote(body, bodyq, sizeof bodyq) != 0) return -1;
        if ((size_t)snprintf(cmd, sizeof cmd,
                             "curl -sS --max-time 25 -H 'Content-Type: application/json' "
                             "-d %s '%s' 2>/dev/null",
                             bodyq, urlq) >= sizeof cmd)
            return -1;
    }
    fp = popen(cmd, "r");
    if (!fp) return -1;
    got = fread(resp, 1, sizeof resp - 1, fp);
    resp[got] = 0;
    pclose(fp);
    p = strstr(resp, "\"content\"");
    if (!p) return -1;
    p = strchr(p, ':');
    if (!p) return -1;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return -1;
    p++;
    {
        size_t i = 0;
        while (*p && *p != '"' && i + 1 < n) {
            if (*p == '\\' && p[1]) {
                p++;
                out[i++] = (*p == 'n') ? ' ' : *p;
                p++;
            } else {
                out[i++] = *p++;
            }
        }
        out[i] = 0;
    }
    return out[0] ? 0 : -1;
}

/* ENFORCED anti-collapse.
 *
 * Returns 1 when CNET already answers this query LOCAL (Tier-A). Such a query
 * must never become a distill row: labelling a student with our own certified
 * output is the collapse loop the doctrine forbids. The residual is disabled
 * for the probe so only sealed coverage can answer.
 *
 * Fails OPEN (returns 0) when the front door is unavailable -- an offline box
 * would otherwise silently drop every row. The PROPOSE.json records whether the
 * check actually ran, so a reviewer can tell "checked" from "could not check".
 */
static int collapse_probe_available(void) {
    const char *fd = getenv("CNET_FRONT_DOOR_BIN");
    if (fd && fd[0]) return access(fd, X_OK) == 0;
    return access("bin/roe_front_door", X_OK) == 0 ||
           access("bin/cnet_peer", X_OK) == 0;
}

static int already_local_tier_a(const char *q) {
    const char *fd = getenv("CNET_FRONT_DOOR_BIN");
    char qq[QLEN * 4], cmd[QLEN * 6], out[8192];
    FILE *p;
    size_t n;
    if (!collapse_probe_available()) return 0;
    if (sh_quote(q, qq, sizeof qq) != 0) return 0;
    if (fd && fd[0] && access(fd, X_OK) == 0) {
        const char *root = getenv("CNET_PACKS_ROOT");
        if ((size_t)snprintf(cmd, sizeof cmd,
                             "ROE_LIVE=0 ROE_LLM=0 '%s' ask %s%s%s%s 2>&1",
                             fd, qq, root && root[0] ? " --root '" : "",
                             root && root[0] ? root : "",
                             root && root[0] ? "'" : "") >= sizeof cmd)
            return 0;
    } else if (access("bin/cnet_peer", X_OK) == 0) {
        if ((size_t)snprintf(cmd, sizeof cmd, "./bin/cnet_peer %s 2>&1", qq) >= sizeof cmd)
            return 0;
    } else {
        return 0;
    }
    p = popen(cmd, "r");
    if (!p) return 0;
    n = fread(out, 1, sizeof out - 1, p);
    out[n] = 0;
    pclose(p);
    return strstr(out, "source=LOCAL") != NULL || strstr(out, "SOURCE LOCAL") != NULL;
}

/* One query per line; '#' comments and blanks skipped. */
static int load_queries_file(const char *path, char qs[][QLEN], int max_q, int nq) {
    FILE *f = fopen(path, "r");
    char line[QLEN];
    if (!f) return nq;
    while (nq < max_q && fgets(line, sizeof line, f)) {
        char *e;
        line[strcspn(line, "\r\n")] = 0;
        e = line;
        while (*e == ' ' || *e == '\t') e++;
        if (!*e || *e == '#') continue;
        snprintf(qs[nq], QLEN, "%s", e);
        nq++;
    }
    fclose(f);
    return nq;
}

/* Organic demand: distinct "query" values from a miss_log.jsonl. This is the
 * input that matters -- a curriculum someone typed is a guess, a miss cluster
 * is evidence the coverage gap is real. */
static int load_queries_misslog(const char *path, char qs[][QLEN], int max_q, int nq) {
    FILE *f = fopen(path, "r");
    char *line = NULL;
    size_t cap = 0;
    if (!f) return nq;
    while (nq < max_q && getline(&line, &cap, f) > 0) {
        const char *p = strstr(line, "\"query\":\"");
        char val[QLEN];
        size_t o = 0;
        int dup = 0, i;
        if (!p) continue;
        p += 9;
        while (*p && *p != '"' && o + 2 < sizeof val) {
            if (*p == 0x5c && p[1]) { p++; val[o++] = (*p == 'n') ? ' ' : *p; p++; }
            else val[o++] = *p++;
        }
        val[o] = 0;
        if (!val[0]) continue;
        for (i = 0; i < nq; i++)
            if (!strcmp(qs[i], val)) { dup = 1; break; }
        if (dup) continue;
        snprintf(qs[nq], QLEN, "%s", val);
        nq++;
    }
    free(line);
    fclose(f);
    return nq;
}

/* Offline checks for the parts that carry law or take untrusted input. */
static int selftest(void) {
    char b[2048];
    int fail = 0, n = 0;
#define T(ok, m) do { n++; printf("  %-52s %s\n", (m), (ok) ? "PASS" : "FAIL"); \
                      if (!(ok)) fail++; } while (0)
    printf("=== cnet_distill_slice selftest ===\n");

    /* untrusted query text must not escape the shell word or the JSON string */
    T(sh_quote("a'; rm -rf /", b, sizeof b) == 0 && b[0] == 0x27 &&
          strstr(b, "'\\''") != NULL,
      "shell quoting neutralises an embedded quote");
    json_esc_buf("say \"hi\"\nnow", b, sizeof b);
    T(strstr(b, "\\\"hi\\\"") != NULL && strstr(b, "\\n") != NULL,
      "json escaping of quote and newline");
    json_esc_buf("back\\slash", b, sizeof b);
    T(strstr(b, "\\\\") != NULL, "json escaping of backslash");

    /* miss_log parsing: organic demand is the input that matters */
    {
        char qs[MAX_Q][QLEN];
        FILE *f = fopen("/tmp/cnet_distill_ml.jsonl", "w");
        int got;
        if (f) {
            fprintf(f, "{\"ts\":1,\"query\":\"what is a certified unit\"}\n");
            fprintf(f, "{\"ts\":2,\"query\":\"what is a certified unit\"}\n");
            fprintf(f, "{\"ts\":3,\"query\":\"how do bricks load\"}\n");
            fprintf(f, "not json at all\n");
            fclose(f);
        }
        got = load_queries_misslog("/tmp/cnet_distill_ml.jsonl", qs, MAX_Q, 0);
        T(got == 2, "miss_log: distinct queries only (dupes collapsed)");
        T(got == 2 && !strcmp(qs[0], "what is a certified unit"),
          "miss_log: query text extracted");
    }
    {
        char qs[MAX_Q][QLEN];
        FILE *f = fopen("/tmp/cnet_distill_q.txt", "w");
        int got;
        if (f) {
            fprintf(f, "# comment\n\n  first query\nsecond query\n");
            fclose(f);
        }
        got = load_queries_file("/tmp/cnet_distill_q.txt", qs, MAX_Q, 0);
        T(got == 2 && !strcmp(qs[0], "first query"),
          "from-file: comments and blanks skipped");
    }
    T(load_queries_file("/tmp/definitely_missing_xyz", (char (*)[QLEN])b, 1, 0) == 0,
      "from-file: missing file adds nothing");

    /* mkdir -p must create intermediate dirs or the slice is silently lost */
    T(mymkdir("/tmp/cnet_distill_selftest/a/b/c") == 0 &&
          is_dir("/tmp/cnet_distill_selftest/a/b/c"),
      "mkdir -p creates intermediate directories");

    printf("\nchecks=%d failures=%d\n", n, fail);
    if (fail) { printf("DISTILL_SLICE_FAIL\n"); return 1; }
    printf("DISTILL_SLICE_SELFTEST_PASS\n");
    return 0;
#undef T
}

static void usage(void) {
    fprintf(stderr,
            "usage: cnet_distill_slice --domain NAME [--query Q]... "
            "[--out DIR] [--dry-run|--teacher]\n"
            "       cnet_distill_slice --domain NAME --from-file queries.txt\n"
            "       cnet_distill_slice --domain NAME --from-miss-log miss_log.jsonl\n"
            "       cnet_distill_slice --selftest\n"
            "  --no-collapse-check   skip the Tier-A anti-collapse probe (offline only)\n"
            "Propose a distill slice. Never self-CERTs.\n");
}

int main(int argc, char **argv) {
    const char *domain = NULL;
    const char *out_root = NULL;
    int dry = 1;
    char qs[MAX_Q][QLEN];
    int nq = 0;
    char outdir[512], prop[640], rows[640];
    time_t now = time(NULL);
    FILE *f;
    int i, teacher_ok = 0, n_rows = 0;
    int collapse_check = 1, n_collapse = 0, probe_ran = 0, n_gold = 0;
    const char *qfile = NULL, *misslog = NULL;
    char gold[640];

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--domain") && i + 1 < argc)
            domain = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc)
            out_root = argv[++i];
        else if (!strcmp(argv[i], "--query") && i + 1 < argc && nq < MAX_Q) {
            snprintf(qs[nq], sizeof qs[nq], "%s", argv[++i]);
            nq++;
        } else if (!strcmp(argv[i], "--dry-run"))
            dry = 1;
        else if (!strcmp(argv[i], "--teacher"))
            dry = 0;
        else if (!strcmp(argv[i], "--from-file") && i + 1 < argc)
            qfile = argv[++i];
        else if (!strcmp(argv[i], "--from-miss-log") && i + 1 < argc)
            misslog = argv[++i];
        else if (!strcmp(argv[i], "--no-collapse-check"))
            collapse_check = 0;
        else if (!strcmp(argv[i], "--selftest"))
            return selftest();
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage();
            return 0;
        }
    }
    if (!domain || !domain[0]) {
        usage();
        return 2;
    }
    if (qfile) nq = load_queries_file(qfile, qs, MAX_Q, nq);
    if (misslog) nq = load_queries_misslog(misslog, qs, MAX_Q, nq);
    /* default curriculum if none given — dry placeholders, not CERT */
    if (nq == 0) {
        snprintf(qs[nq++], QLEN, "what is this domain: %s", domain);
        snprintf(qs[nq++], QLEN, "%s example query one", domain);
        snprintf(qs[nq++], QLEN, "%s example query two", domain);
    }
    if (!out_root) {
        const char *min = getenv("CNET_MINIMAL_ROOT");
        if (min && min[0]) {
            static char def[512];
            snprintf(def, sizeof def, "%s/var/capsule_inbox", min);
            out_root = def;
        } else {
            out_root = "var/capsule_inbox";
        }
    }
    mymkdir(out_root);
    snprintf(outdir, sizeof outdir, "%.400s/%.64s-%ld", out_root, domain, (long)now);
    mymkdir(outdir);
    snprintf(rows, sizeof rows, "%s/rows.jsonl", outdir);
    f = fopen(rows, "w");
    if (!f) {
        fprintf(stderr, "cannot write %s\n", rows);
        return 1;
    }
    probe_ran = collapse_check && collapse_probe_available();
    snprintf(gold, sizeof gold, "%s/gold_rows.jsonl", outdir);
    for (i = 0; i < nq; i++) {
        char ans[ALEN];
        ans[0] = 0;
        /* ENFORCED anti-collapse: never label a student with our own Tier-A. */
        if (probe_ran && already_local_tier_a(qs[i])) {
            fprintf(f, "{\"domain\":\"");
            json_esc(f, domain);
            fprintf(f, "\",\"q\":\"");
            json_esc(f, qs[i]);
            fprintf(f, "\",\"draft\":\"\",\"source\":\"skipped\","
                       "\"claimed_cert\":0,\"auto_cert\":false,"
                       "\"anti_collapse\":true,\"kind\":\"distill_row\","
                       "\"skip_reason\":\"anti_collapse_local_tier_a\"}\n");
            n_collapse++;
            n_rows++;
            continue;
        }
        if (!dry) {
            if (teacher_ask(qs[i], ans, sizeof ans) == 0)
                teacher_ok++;
            else
                snprintf(ans, sizeof ans, "(teacher_miss)");
        } else {
            snprintf(ans, sizeof ans, "(dry-run placeholder — not a label)");
        }
        fprintf(f, "{\"domain\":\"");
        json_esc(f, domain);
        fprintf(f, "\",\"q\":\"");
        json_esc(f, qs[i]);
        fprintf(f, "\",\"draft\":\"");
        json_esc(f, ans);
        fprintf(f, "\",\"source\":\"%s\",\"claimed_cert\":0,\"auto_cert\":false,"
                   "\"anti_collapse\":true,\"kind\":\"distill_row\"}\n",
                dry ? "dry" : "teacher");
        n_rows++;
    }
    fclose(f);

    /* Gold-shaped rows: the same drafts in the shape the existing ROE gold path
     * already understands (query/answer), so a reviewer can promote a verified
     * slice with the tooling that exists instead of a new importer. Still
     * auto_cert=false -- these are candidates for gold, not gold. */
    {
        FILE *g = fopen(gold, "w");
        FILE *r = fopen(rows, "r");
        char *line = NULL;
        size_t lc = 0;
        if (g && r) {
            while (getline(&line, &lc, r) > 0) {
                if (strstr(line, "\"skip_reason\"")) continue;
                if (strstr(line, "\"draft\":\"\"")) continue;
                {
                    const char *qp = strstr(line, "\"q\":\"");
                    const char *dp = strstr(line, "\"draft\":\"");
                    char qv[QLEN], dv[ALEN];
                    size_t o = 0;
                    if (!qp || !dp) continue;
                    qp += 5;
                    while (*qp && *qp != '"' && o + 2 < sizeof qv) {
                        if (*qp == 0x5c && qp[1]) { qv[o++] = *qp++; }
                        qv[o++] = *qp++;
                    }
                    qv[o] = 0;
                    o = 0;
                    dp += 9;
                    while (*dp && *dp != '"' && o + 2 < sizeof dv) {
                        if (*dp == 0x5c && dp[1]) { dv[o++] = *dp++; }
                        dv[o++] = *dp++;
                    }
                    dv[o] = 0;
                    if (!qv[0] || !dv[0]) continue;
                    fprintf(g, "{\"query\":\"%s\",\"answer\":\"%s\","
                               "\"auto_cert\":false,\"status\":\"pending_verify\","
                               "\"provenance\":\"distill_slice_teacher\"}\n",
                            qv, dv);
                    n_gold++;
                }
            }
            free(line);
        }
        if (r) fclose(r);
        if (g) fclose(g);
    }

    snprintf(prop, sizeof prop, "%s/PROPOSE.json", outdir);
    f = fopen(prop, "w");
    if (!f) return 1;
    fprintf(f,
            "{\n"
            "  \"kind\": \"distill_slice\",\n"
            "  \"domain\": \"%s\",\n"
            "  \"out\": \"%s\",\n"
            "  \"n_rows\": %d,\n"
            "  \"teacher_ok\": %d,\n"
            "  \"gold_rows\": %d,\n"
            "  \"skipped_anti_collapse\": %d,\n"
            "  \"anti_collapse_checked\": %s,\n"
            "  \"dry_run\": %s,\n"
            "  \"auto_cert\": false,\n"
            "  \"status\": \"pending_verify\",\n"
            "  \"student\": \"none_until_verify\",\n"
            "  \"law\": \"teacher_proposes_never_self_cert\",\n"
            "  \"ts\": %ld\n"
            "}\n",
            domain, outdir, n_rows, teacher_ok, n_gold, n_collapse,
            probe_ran ? "true" : "false", dry ? "true" : "false", (long)now);
    fclose(f);

    printf("DISTILL_SLICE domain=%s rows=%d gold=%d skipped_collapse=%d "
           "collapse_checked=%d teacher_ok=%d dry=%d out=%s auto_cert=0\n",
           domain, n_rows, n_gold, n_collapse, probe_ran, teacher_ok, dry, outdir);
    printf("DISTILL_SLICE_PASS\n");
    return 0;
}
