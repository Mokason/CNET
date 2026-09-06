/* roe_gold_put — mint a curated gold answer for the ROE promote path.
 *
 *   bin/roe_gold_put <query> <answer>
 *   bin/roe_gold_put --stdin <query>          # answer on stdin (long text)
 *   bin/roe_gold_put --path <query>           # print the gold path, write nothing
 *   bin/roe_gold_put --dry-run <query> <answer>
 *
 * Writes <packs>/gold/<sha1_16>.txt, where the digest comes from
 * include/cnet_roe_gold_id.h — the SAME normalisation and SHA-1 that
 * bin/roe_evolve_tick uses to look gold up. Sharing the header is the point:
 * a private copy that drifts would write the file where nothing reads it, and
 * the promote would silently never happen.
 *
 * Gold is the external-verify leg of the promote policy: an answer with gold
 * skips the reviewer. That makes this a CERT-adjacent tool, so it refuses to
 * mint gold that the tick would then refuse anyway:
 *   - refusal/ABSTAIN answers are rejected outright (hard law, --force cannot
 *     override: caching "I don't know" as CERT is the self-CERT failure)
 *   - queries hitting a substr| blocklist rule are rejected unless --force
 * It never writes pack_personal, never seals, never promotes. Promotion still
 * happens only in the tick, under the unchanged policy.
 *
 * Packs root: CNET_PACKS_ROOT > CNET_MINIMAL_ROOT/data/roe_daily_packs >
 *             <repo>/artifacts/roe_daily_packs   (identical to the tick)
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cnet_roe_gold_id.h"
#include "cnet_roe_gold.h"

static int mkdir_p(const char *path) {
    char tmp[ROE_PATHMAX];
    char *p;
    if (snprintf(tmp, sizeof tmp, "%s", path) >= (int)sizeof tmp) return -1;
    for (p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        if (mkdir(tmp, 0755) != 0 && !roe_is_dir(tmp)) return -1;
        *p = '/';
    }
    if (mkdir(tmp, 0755) != 0 && !roe_is_dir(tmp)) return -1;
    return 0;
}

static void lower_copy(const char *in, char *out, size_t cap) {
    size_t i;
    for (i = 0; in && in[i] && i + 1 < cap; i++)
        out[i] = (char)tolower((unsigned char)in[i]);
    out[i] = 0;
}

/* Hard law, independent of any file: a refusal must never become CERT. */
static const char *refusal_reason(const char *answer) {
    char a[4096];
    lower_copy(answer ? answer : "", a, sizeof a);
    while (*a == ' ') memmove(a, a + 1, strlen(a));
    if (!a[0])                                        return "empty answer";
    if (!strncmp(a, "abstain", 7))                    return "ABSTAIN answer";
    if (!strncmp(a, "i don't know", 12))              return "\"i don't know\" answer";
    if (!strncmp(a, "i do not know", 13))             return "\"i do not know\" answer";
    if (!strncmp(a, "no local skill", 14))            return "\"no local skill\" answer";
    if (strstr(a, "no local skill") && strstr(a, "ask user"))
        return "no_local_skill/ask_user answer";
    return NULL;
}

/* Advisory mirror of the tick's substr| rules. The tick remains authoritative;
 * this only stops you planting gold that the tick will refuse at promote time. */
static int query_blocklisted(const char *repo_root, const char *query,
                             char *rule, size_t rcap) {
    char path[ROE_PATHMAX], line[512], nq[ROE_NORMMAX], v[256];
    const char *env = getenv("CNET_BLOCKLIST");
    FILE *f;
    int hit = 0;
    roe_norm_q(query, nq);
    if (env && env[0]) snprintf(path, sizeof path, "%s", env);
    else snprintf(path, sizeof path, "%s/config/promote_blocklist.txt", repo_root);
    f = fopen(path, "r");
    if (!f) return 0;
    while (fgets(line, sizeof line, f)) {
        char *bar, *k = line, *val, *e;
        line[strcspn(line, "\r\n")] = 0;
        while (*k && isspace((unsigned char)*k)) k++;
        if (!*k || *k == '#') continue;
        bar = strchr(k, '|');
        if (!bar) continue;
        *bar = 0;
        val = bar + 1;
        for (e = k + strlen(k); e > k && isspace((unsigned char)e[-1]); e--) e[-1] = 0;
        while (*val && isspace((unsigned char)*val)) val++;
        for (e = val + strlen(val); e > val && isspace((unsigned char)e[-1]); e--) e[-1] = 0;
        if (strcmp(k, "substr") || !*val) continue;
        lower_copy(val, v, sizeof v);
        if (v[0] && strstr(nq, v)) {
            snprintf(rule, rcap, "%s", val);
            hit = 1;
            break;
        }
    }
    fclose(f);
    return hit;
}

static void resolve_root(char *out, size_t cap, const char *argv0) {
    char buf[ROE_PATHMAX];
    char *slash;
    const char *env = getenv("CNET_ROOT");
    if (env && env[0] && roe_is_dir(env)) { snprintf(out, cap, "%s", env); return; }
    if (roe_is_dir("tools") && roe_is_dir("include")) { snprintf(out, cap, "."); return; }
    if (argv0 && snprintf(buf, sizeof buf, "%s", argv0) < (int)sizeof buf) {
        slash = strrchr(buf, '/');
        if (slash) {
            *slash = 0;
            slash = strrchr(buf, '/');
            if (slash) { *slash = 0; snprintf(out, cap, "%s", buf); return; }
        }
    }
    snprintf(out, cap, ".");
}

static int selftest(const char *argv0) {
    char root[ROE_PATHMAX], packs[ROE_PATHMAX], gp[ROE_PATHMAX], h[17], sid[32], rule[256];
    int fails = 0, checks = 0;
#define CHK(c, n) do { checks++; printf("  %-52s %s\n", (n), (c) ? "PASS" : "FAIL"); \
                       if (!(c)) fails++; } while (0)
    printf("=== roe_gold_put selftest ===\n");
    resolve_root(root, sizeof root, argv0);
    roe_packs_root(packs, sizeof packs, root);

    /* identity must equal the evolve tick's, which equals sha1sum's */
    roe_q_hash16("roe evolve tick demo query alpha", h);
    CHK(!strcmp(h, "60e86e983af32d6e"), "gold id matches evolve tick / sha1sum");
    roe_skill_id_for("roe evolve tick demo query alpha", sid);
    CHK(!strcmp(sid, "auto_60e86e983af32d6e"), "skill id matches evolve tick");
    /* normalisation: casing and spacing must not change the identity */
    {
        char h2[17];
        roe_q_hash16("  ROE   Evolve Tick  DEMO query   Alpha ", h2);
        CHK(!strcmp(h, h2), "identity stable under case/space noise");
    }
    roe_gold_path(gp, sizeof gp, packs, "roe evolve tick demo query alpha");
    CHK(strstr(gp, "/gold/60e86e983af32d6e.txt") != NULL, "gold path shape");

    CHK(refusal_reason("ABSTAIN: no local skill") != NULL, "abstain refused");
    CHK(refusal_reason("I don't know") != NULL, "i-dont-know refused");
    CHK(refusal_reason("") != NULL, "empty refused");
    CHK(refusal_reason("A certified unit is a verified capsule.") == NULL,
        "real answer allowed");
    CHK(query_blocklisted(root, "zz mystic ooze 99", rule, sizeof rule),
        "blocklisted probe query refused");
    CHK(!query_blocklisted(root, "what is a certified unit", rule, sizeof rule),
        "clean query allowed");

    printf("\nchecks=%d failures=%d\n", checks, fails);
    if (fails) { printf("ROE_GOLD_PUT_FAIL\n"); return 1; }
    if (cnet_gold_selftest() != 0) return 1;
    printf("ROE_GOLD_PUT_SELFTEST_PASS\n");
    return 0;
#undef CHK
}

static void usage(void) {
    fprintf(stderr,
        "usage: roe_gold_put [--dry-run] [--force] [--kick] <query> <answer>\n"
        "       roe_gold_put [--dry-run] [--force] [--kick] --last <answer>\n"
        "       roe_gold_put [--dry-run] [--force] --stdin <query>   # answer on stdin\n"
        "       roe_gold_put --path <query>                          # print path only\n"
        "       roe_gold_put --selftest\n");
}

int main(int argc, char **argv) {
    char root[ROE_PATHMAX], packs[ROE_PATHMAX], gpath[ROE_PATHMAX], gdir[ROE_PATHMAX];
    char answer[65536], rule[256];
    const char *query = NULL, *ans = NULL, *why;
    int dry = 0, force = 0, use_stdin = 0, path_only = 0, use_last = 0, kick = 0, i;
    FILE *f;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--dry-run"))       dry = 1;
        else if (!strcmp(argv[i], "--force"))    force = 1;
        else if (!strcmp(argv[i], "--stdin"))    use_stdin = 1;
        else if (!strcmp(argv[i], "--path"))     path_only = 1;
        else if (!strcmp(argv[i], "--last"))     use_last = 1;
        else if (!strcmp(argv[i], "--kick"))     kick = 1;
        else if (!strcmp(argv[i], "--selftest")) return selftest(argv[0]);
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { usage(); return 0; }
        else if (argv[i][0] == '-' && argv[i][1]) { usage(); return 1; }
        else if (!query) query = argv[i];
        else if (!ans)   ans = argv[i];
        else { usage(); return 1; }
    }

    resolve_root(root, sizeof root, argv[0]);
    roe_packs_root(packs, sizeof packs, root);

    if (use_last) {
        static char lastq[CNET_GOLD_Q];
        char miss[ROE_PATHMAX];
        const char *ml = getenv("CNET_MISS_LOG");
        if (ans && ans[0]) { usage(); return 1; }
        ans = query;
        query = NULL;
        if (ml && ml[0]) {
            if (snprintf(miss, sizeof miss, "%s", ml) >= (int)sizeof miss) {
                fprintf(stderr, "roe_gold_put: miss_log path too long\n");
                return 2;
            }
        } else if (snprintf(miss, sizeof miss, "%s/miss_log.jsonl", packs) >=
                   (int)sizeof miss) {
            fprintf(stderr, "roe_gold_put: miss_log path too long\n");
            return 2;
        }
        if (cnet_gold_last_miss(miss, lastq, sizeof lastq) != 0) {
            fprintf(stderr, "roe_gold_put: no last organic miss in %s\n", miss);
            return 3;
        }
        query = lastq;
        printf("LAST_MISS %s\n", query);
    }
    if (!query || !query[0]) { usage(); return 1; }
    roe_gold_path(gpath, sizeof gpath, packs, query);

    if (path_only) { printf("%s\n", gpath); return 0; }

    if (use_stdin) {
        size_t n = fread(answer, 1, sizeof answer - 1, stdin);
        answer[n] = 0;
        ans = answer;
    }
    if (!ans || !ans[0]) {
        fprintf(stderr, "roe_gold_put: no answer given\n");
        usage();
        return 1;
    }
    {   /* trim trailing whitespace/newlines */
        size_t e = strlen(ans);
        static char trimmed[65536];
        snprintf(trimmed, sizeof trimmed, "%s", ans);
        for (e = strlen(trimmed); e > 0 && isspace((unsigned char)trimmed[e - 1]); e--)
            trimmed[e - 1] = 0;
        ans = trimmed;
    }

    /* Hard law: --force cannot mint a refusal as gold. */
    why = refusal_reason(ans);
    if (why) {
        fprintf(stderr, "roe_gold_put: REFUSED (%s) — a refusal must never become CERT\n", why);
        return 3;
    }
    if (!force && query_blocklisted(root, query, rule, sizeof rule)) {
        fprintf(stderr,
                "roe_gold_put: REFUSED — query hits blocklist rule substr|%s\n"
                "  the evolve tick would skip this at promote time; use --force to override\n",
                rule);
        return 3;
    }

    if (dry) {
        printf("DRY %s\n", gpath);
        return 0;
    }
    if (snprintf(gdir, sizeof gdir, "%.*s/gold", (int)(sizeof gdir - 8), packs) >= (int)sizeof gdir) {
        fprintf(stderr, "roe_gold_put: packs path too long\n");
        return 2;
    }
    if (mkdir_p(gdir) != 0) {
        fprintf(stderr, "roe_gold_put: cannot create %s\n", gdir);
        return 2;
    }
    f = fopen(gpath, "w");
    if (!f) {
        fprintf(stderr, "roe_gold_put: cannot write %s\n", gpath);
        return 2;
    }
    fprintf(f, "%s\n", ans);
    fclose(f);
    printf("GOLD %s\n", gpath);
    if (kick) {
        int krc = system("./bin/roe_evolve_tick --gold-only --max-promotes 1");
        printf("KICK evolve --gold-only rc=%d (still not admit)\n", krc);
    }
    return 0;
}
