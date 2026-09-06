/* Operator gold curate — write evolve gold_file, never seal. */
#include "cnet_roe_gold.h"
#include "cnet_roe_gold_id.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static void scopy(char *d, size_t cap, const char *s) {
    size_t i;
    if (!d || !cap) return;
    if (!s) {
        d[0] = 0;
        return;
    }
    for (i = 0; s[i] && i + 1 < cap; i++) d[i] = s[i];
    d[i] = 0;
}

static void trim_copy(const char *in, char *out, size_t cap) {
    size_t n;
    char tmp[CNET_GOLD_A];
    if (!out || !cap) return;
    if (!in) {
        out[0] = 0;
        return;
    }
    while (*in && isspace((unsigned char)*in)) in++;
    scopy(tmp, sizeof tmp, in);
    n = strlen(tmp);
    while (n > 0 && isspace((unsigned char)tmp[n - 1])) tmp[--n] = 0;
    scopy(out, cap, tmp);
}

static int mkdir_p(const char *path) {
    char tmp[ROE_PATHMAX];
    char *p;
    if (!path || !path[0]) return -1;
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

static int starts_ci(const char *s, const char *pfx) {
    size_t i;
    if (!s || !pfx) return 0;
    for (i = 0; pfx[i]; i++) {
        if (tolower((unsigned char)s[i]) != tolower((unsigned char)pfx[i]))
            return 0;
    }
    return 1;
}

static int answer_is_uint(const char *a) {
    const char *p;
    int any = 0;
    if (!a) return 0;
    p = a;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p == '+' || *p == '-') p++;
    while (isdigit((unsigned char)*p)) {
        any = 1;
        p++;
    }
    while (*p && isspace((unsigned char)*p)) p++;
    return any && *p == '\0';
}

int cnet_gold_operator_not_faq(const char *query, const char *answer) {
    char nq[ROE_NORMMAX];
    size_t i;
    if (!query) return 0;
    roe_norm_q(query, nq);
    if (strstr(nq, "what time is it") || strstr(nq, "current time") ||
        strcmp(nq, "now") == 0 || strstr(nq, "clamp ") ||
        strstr(nq, "compose ") || strstr(nq, "load average") ||
        strstr(nq, "host load") || strstr(nq, "uptime") ||
        strstr(nq, "disk free") || strstr(nq, "disk space") ||
        strstr(nq, " xor") || strstr(nq, "shl") || strstr(nq, "days between") ||
        strstr(nq, "day of week") || strstr(nq, "how much ram") ||
        strstr(nq, "minutes in seconds") || strstr(nq, "crc8") ||
        strstr(nq, " then min") || strstr(nq, " then max"))
        return 1;
    if (!answer_is_uint(answer)) return 0;
    if (strstr(nq, " plus") || strstr(nq, "plus ") || strstr(nq, " minus") ||
        strstr(nq, "minus ") || strstr(nq, " times") || strstr(nq, "times ") ||
        strstr(nq, " multiplied") || strstr(nq, " divided") ||
        strstr(nq, "divided ") || strstr(nq, " modulo") || strstr(nq, " mod "))
        return 1;
    for (i = 0; nq[i]; i++) {
        const char *p;
        if (!isdigit((unsigned char)nq[i])) continue;
        p = nq + i;
        while (isdigit((unsigned char)*p)) p++;
        while (*p == ' ') p++;
        if (*p == '+' || *p == '*' || *p == '-' || *p == '/' || *p == '%') {
            p++;
            while (*p == ' ') p++;
            if (isdigit((unsigned char)*p)) return 1;
        }
    }
    return 0;
}

int cnet_gold_parse(const char *q, CnetGoldReq *out) {
    const char *p, *bar;
    if (out) memset(out, 0, sizeof *out);
    if (!q || !out) return 0;
    p = q;
    while (*p && isspace((unsigned char)*p)) p++;
    if (!starts_ci(p, "gold")) return 0;
    p += 4;
    if (*p && !isspace((unsigned char)*p)) return 0;
    while (*p && isspace((unsigned char)*p)) p++;
    if (starts_ci(p, "last") && (!p[4] || isspace((unsigned char)p[4]))) {
        out->cmd = CNET_GOLD_CMD_LAST;
        p += 4;
        while (*p && isspace((unsigned char)*p)) p++;
        trim_copy(p, out->answer, sizeof out->answer);
        return 1;
    }
    bar = strstr(p, " | ");
    if (!bar) return 0;
    out->cmd = CNET_GOLD_CMD_QA;
    {
        size_t n = (size_t)(bar - p);
        if (n >= sizeof out->query) n = sizeof out->query - 1;
        memcpy(out->query, p, n);
        out->query[n] = 0;
        trim_copy(out->query, out->query, sizeof out->query);
    }
    trim_copy(bar + 3, out->answer, sizeof out->answer);
    if (!out->query[0]) return 0;
    return 1;
}

const char *cnet_gold_refusal(const char *answer) {
    char a[4096];
    lower_copy(answer ? answer : "", a, sizeof a);
    while (*a == ' ') memmove(a, a + 1, strlen(a));
    if (!a[0]) return "empty answer";
    if (!strncmp(a, "abstain", 7)) return "ABSTAIN answer";
    if (!strncmp(a, "i don't know", 12)) return "\"i don't know\" answer";
    if (!strncmp(a, "i do not know", 13)) return "\"i do not know\" answer";
    if (!strncmp(a, "no local skill", 14)) return "\"no local skill\" answer";
    if (strstr(a, "no local skill") && strstr(a, "ask user"))
        return "no_local_skill/ask_user answer";
    return NULL;
}

static int json_query(const char *line, char *out, size_t cap) {
    const char *p = strstr(line, "\"query\"");
    size_t o = 0;
    if (!p) p = strstr(line, "\"q\"");
    if (!p || !out || !cap) return -1;
    p = strchr(p, ':');
    if (!p) return -1;
    p++;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '"') return -1;
    p++;
    while (*p && *p != '"' && o + 1 < cap) {
        if (*p == '\\' && p[1]) {
            p++;
            out[o++] = *p++;
        } else {
            out[o++] = *p++;
        }
    }
    out[o] = 0;
    return out[0] ? 0 : -1;
}

static int skip_miss_query(const char *line, const char *q) {
    if (strstr(line, "\"shortcircuit\":true")) return 1;
    if (starts_ci(q, "gold ") || starts_ci(q, "ffi ")) return 1;
    if (starts_ci(q, "gold\t") || starts_ci(q, "ffi\t")) return 1;
    return 0;
}

int cnet_gold_last_miss(const char *miss_log, char *query, size_t cap) {
    FILE *f;
    char *line = NULL;
    size_t lc = 0;
    char last[CNET_GOLD_Q];
    int hit = 0;
    if (query && cap) query[0] = 0;
    if (!miss_log || !miss_log[0] || !query || !cap) return -1;
    last[0] = 0;
    f = fopen(miss_log, "r");
    if (!f) return -1;
    while (getline(&line, &lc, f) > 0) {
        char q[CNET_GOLD_Q];
        if (json_query(line, q, sizeof q) != 0) continue;
        if (skip_miss_query(line, q)) continue;
        scopy(last, sizeof last, q);
        hit = 1;
    }
    free(line);
    fclose(f);
    if (!hit || !last[0]) return -1;
    scopy(query, cap, last);
    return 0;
}

static int query_blocklisted(const char *blocklist_path, const char *query,
                             char *rule, size_t rcap) {
    char line[512], nq[ROE_NORMMAX], v[256];
    FILE *f;
    int hit = 0;
    if (rule && rcap) rule[0] = 0;
    if (!blocklist_path || !blocklist_path[0] || !query) return 0;
    roe_norm_q(query, nq);
    f = fopen(blocklist_path, "r");
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
        for (e = k + strlen(k); e > k && isspace((unsigned char)e[-1]); e--)
            e[-1] = 0;
        while (*val && isspace((unsigned char)*val)) val++;
        for (e = val + strlen(val); e > val && isspace((unsigned char)e[-1]); e--)
            e[-1] = 0;
        if (strcmp(k, "substr") || !*val) continue;
        lower_copy(val, v, sizeof v);
        if (v[0] && strstr(nq, v)) {
            if (rule && rcap) scopy(rule, rcap, val);
            hit = 1;
            break;
        }
    }
    fclose(f);
    return hit;
}

int cnet_gold_write(const char *packs, const char *query, const char *answer,
                    const char *blocklist_path, int force, char *path_out,
                    size_t cap, char *reason, size_t rcap) {
    char gpath[ROE_PATHMAX], gdir[ROE_PATHMAX];
    const char *why;
    FILE *f;
    if (path_out && cap) path_out[0] = 0;
    if (reason && rcap) reason[0] = 0;
    if (!packs || !packs[0] || !query || !query[0]) {
        if (reason && rcap) scopy(reason, rcap, "missing query");
        return -1;
    }
    why = cnet_gold_refusal(answer);
    if (why) {
        if (reason && rcap) scopy(reason, rcap, why);
        return -1;
    }
    {
        char nq[ROE_NORMMAX];
        roe_norm_q(query, nq);
        if (strstr(nq, "who are you") || strstr(nq, "who is roe") ||
            strstr(nq, "who is marble") || strstr(nq, "who is your operator") ||
            strstr(nq, "what do you call me") || strstr(nq, "still here") ||
            strstr(nq, "how are you") || strstr(nq, "how do you feel") ||
            strstr(nq, "yeah i know") || strcmp(nq, "hey") == 0 ||
            strcmp(nq, "hello") == 0 || strcmp(nq, "hi") == 0 ||
            strcmp(nq, "why") == 0 || strstr(nq, "why do you feel") ||
            strstr(nq, "what would you like to do") ||
            strstr(nq, "what do you want to do") ||
            strstr(nq, "what can you do") ||
            strstr(nq, "not bored") || strstr(nq, "still bored")) {
            if (reason && rcap) scopy(reason, rcap, "persona_guard");
            return -1;
        }
        if (cnet_gold_operator_not_faq(query, answer)) {
            if (reason && rcap) scopy(reason, rcap, "operator_not_faq");
            return -1;
        }
    }
    if (!force && query_blocklisted(blocklist_path, query, reason, rcap)) {
        return -1;
    }
    roe_gold_path(gpath, sizeof gpath, packs, query);
    if (snprintf(gdir, sizeof gdir, "%s/gold", packs) >= (int)sizeof gdir) {
        if (reason && rcap) scopy(reason, rcap, "path too long");
        return -1;
    }
    if (mkdir_p(gdir) != 0) {
        if (reason && rcap) scopy(reason, rcap, "mkdir gold");
        return -1;
    }
    f = fopen(gpath, "w");
    if (!f) {
        if (reason && rcap) scopy(reason, rcap, "write failed");
        return -1;
    }
    fprintf(f, "%s\n", answer);
    fclose(f);
    if (path_out && cap) scopy(path_out, cap, gpath);
    return 0;
}

int cnet_gold_selftest(void) {
    int fails = 0, checks = 0;
    CnetGoldReq r;
#define CHK(c, n)                                                              \
    do {                                                                       \
        checks++;                                                              \
        printf("  %-52s %s\n", (n), (c) ? "PASS" : "FAIL");                    \
        if (!(c)) fails++;                                                     \
    } while (0)

    printf("=== cnet_roe_gold selftest ===\n");

    CHK(cnet_gold_parse("who are you", &r) == 0 && r.cmd == CNET_GOLD_CMD_NONE,
        "who are you is not gold");
    CHK(cnet_gold_parse("ffi gold", &r) == 0, "ffi gold is not this verb");
    CHK(cnet_gold_parse("gold last flew", &r) == 1 &&
            r.cmd == CNET_GOLD_CMD_LAST && !strcmp(r.answer, "flew"),
        "gold last <answer>");
    CHK(cnet_gold_parse("gold past tense of fly | flew", &r) == 1 &&
            r.cmd == CNET_GOLD_CMD_QA && !strcmp(r.query, "past tense of fly") &&
            !strcmp(r.answer, "flew"),
        "gold query | answer");
    CHK(cnet_gold_parse("gold last", &r) == 1 && r.cmd == CNET_GOLD_CMD_LAST &&
            !r.answer[0],
        "gold last with empty answer still parses");

    CHK(cnet_gold_refusal("ABSTAIN: no local skill") != NULL, "abstain refused");
    CHK(cnet_gold_refusal("I don't know") != NULL, "i-dont-know refused");
    CHK(cnet_gold_refusal("") != NULL, "empty refused");
    CHK(cnet_gold_refusal("fly -> flew.") == NULL, "real form allowed");

    {
        const char *path = "/tmp/cnet_gold_last_miss.jsonl";
        FILE *f = fopen(path, "w");
        CHK(f != NULL, "temp miss_log");
        if (f) {
            fputs("{\"query\":\"autonomous cycle probe zz\",\"shortcircuit\":true}\n",
                  f);
            fputs("{\"query\":\"ffi note site=x sent=go\",\"shortcircuit\":false}\n",
                  f);
            fputs("{\"query\":\"gold last junk\",\"shortcircuit\":false}\n", f);
            fputs("{\"query\":\"past tense of fly\",\"shortcircuit\":false,"
                  "\"learnable\":true}\n",
                  f);
            fclose(f);
        }
        {
            char q[CNET_GOLD_Q];
            CHK(cnet_gold_last_miss(path, q, sizeof q) == 0 &&
                    !strcmp(q, "past tense of fly"),
                "last miss skips probe/ffi/gold verbs");
        }
    }

    {
        char gp[CNET_GOLD_PATH], why[CNET_GOLD_REASON];
        CHK(cnet_gold_write("/tmp/cnet_gold_packs", "past tense of fly",
                            "ABSTAIN: no", NULL, 0, gp, sizeof gp, why,
                            sizeof why) != 0,
            "write refuses ABSTAIN");
        CHK(cnet_gold_write("/tmp/cnet_gold_packs", "who are you", "I am Marble.",
                            NULL, 0, gp, sizeof gp, why, sizeof why) != 0,
            "persona who-are-you gold refused");
        CHK(cnet_gold_write("/tmp/cnet_gold_packs", "who is your operator",
                            "Mokason", NULL, 0, gp, sizeof gp, why, sizeof why) != 0,
            "persona operator gold refused");
        CHK(cnet_gold_write("/tmp/cnet_gold_packs", "how are you", "Spark.",
                            NULL, 0, gp, sizeof gp, why, sizeof why) != 0,
            "persona mood gold refused");
        CHK(cnet_gold_write("/tmp/cnet_gold_packs", "hey", "Hey. Marble here.",
                            NULL, 0, gp, sizeof gp, why, sizeof why) != 0,
            "persona greet gold refused");
        CHK(cnet_gold_write("/tmp/cnet_gold_packs", "yeah i know", "Got it.",
                            NULL, 0, gp, sizeof gp, why, sizeof why) != 0,
            "persona ack gold refused");
        CHK(cnet_gold_write("/tmp/cnet_gold_packs", "past tense of fly",
                            "fly -> flew.", NULL, 0, gp, sizeof gp, why,
                            sizeof why) == 0 &&
                strstr(gp, "/gold/") != NULL,
            "write gold file for clean pair");
        CHK(cnet_gold_write("/tmp/cnet_gold_packs", "10+11", "21", NULL, 0, gp,
                            sizeof gp, why, sizeof why) != 0 &&
                strstr(why, "operator_not_faq") != NULL,
            "arith instance gold refused");
        CHK(cnet_gold_write("/tmp/cnet_gold_packs", "20/4", "5", NULL, 0, gp,
                            sizeof gp, why, sizeof why) != 0 &&
                strstr(why, "operator_not_faq") != NULL,
            "div instance gold refused");
        CHK(cnet_gold_write("/tmp/cnet_gold_packs", "clamp 10 0 5", "5", NULL, 0,
                            gp, sizeof gp, why, sizeof why) != 0 &&
                strstr(why, "operator_not_faq") != NULL,
            "clamp instance gold refused");
        CHK(cnet_gold_write("/tmp/cnet_gold_packs", "what time is it", "noon",
                            NULL, 0, gp, sizeof gp, why, sizeof why) != 0 &&
                strstr(why, "operator_not_faq") != NULL,
            "clock instance gold refused");
        CHK(cnet_gold_write("/tmp/cnet_gold_packs", "twelve plus five", "17",
                            NULL, 0, gp, sizeof gp, why, sizeof why) != 0,
            "number-word sum gold refused");
        CHK(cnet_gold_write("/tmp/cnet_gold_packs", "twelve", "12", NULL, 0, gp,
                            sizeof gp, why, sizeof why) == 0,
            "numeral overlay gold allowed");
    }

    printf("\nchecks=%d failures=%d\n", checks, fails);
    if (fails) {
        printf("CNET_GOLD_FAIL\n");
        return 1;
    }
    printf("CNET_GOLD_SELFTEST_PASS\n");
    return 0;
#undef CHK
}
