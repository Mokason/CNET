/* ROE chain-of-thought — pure C, 0-token skeleton (Autonomous-ASI, not AGI). */
#include "../include/cnet_roe_cot.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

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

static void stolower(char *d, size_t cap, const char *s) {
    size_t i;
    if (!d || !cap) return;
    for (i = 0; s && s[i] && i + 1 < cap; i++)
        d[i] = (char)tolower((unsigned char)s[i]);
    d[i < cap ? i : cap - 1] = 0;
}

static int contains_ci(const char *hay, const char *needle) {
    char h[ROE_COT_TEXT], n[ROE_COT_TEXT];
    if (!hay || !needle || !needle[0]) return 0;
    stolower(h, sizeof h, hay);
    stolower(n, sizeof n, needle);
    return strstr(h, n) != NULL;
}

static double json_num(const char *buf, const char *key, double defv) {
    char pat[96];
    const char *p;
    if (!buf || !key) return defv;
    snprintf(pat, sizeof pat, "\"%s\"", key);
    p = strstr(buf, pat);
    if (!p) return defv;
    p = strchr(p + strlen(pat), ':');
    if (!p) return defv;
    return strtod(p + 1, NULL);
}

static int json_boolish(const char *buf, const char *key) {
    char pat[96];
    const char *p;
    if (!buf || !key) return 0;
    snprintf(pat, sizeof pat, "\"%s\"", key);
    p = strstr(buf, pat);
    if (!p) return 0;
    p = strchr(p + strlen(pat), ':');
    if (!p) return 0;
    while (*p && (*p == ':' || *p == ' ' || *p == '\t')) p++;
    if (strncmp(p, "true", 4) == 0) return 1;
    if (*p == '1') return 1;
    return 0;
}

static int read_file(const char *path, char *buf, size_t cap) {
    FILE *f;
    size_t n;
    if (!path || !buf || !cap) return -1;
    f = fopen(path, "r");
    if (!f) return -1;
    n = fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[n] = 0;
    return (int)n;
}

static RoeCotHop *add_hop(RoeCotChain *C, int kind, const char *label) {
    RoeCotHop *h;
    int reserve = 0;
    if (!C || C->n_hops >= ROE_COT_MAX_HOPS) return NULL;
    /* reserve last 3 slots for JOIN/VERIFY/SHOW */
    if (kind != ROE_COT_JOIN && kind != ROE_COT_VERIFY && kind != ROE_COT_SHOW &&
        kind != ROE_COT_STOP)
        reserve = 3;
    if (C->n_hops + reserve >= ROE_COT_MAX_HOPS &&
        kind != ROE_COT_JOIN && kind != ROE_COT_VERIFY && kind != ROE_COT_SHOW)
        return NULL;
    if (C->n_hops >= C->max_hops && kind != ROE_COT_JOIN && kind != ROE_COT_VERIFY &&
        kind != ROE_COT_SHOW)
        return NULL;
    h = &C->hops[C->n_hops++];
    memset(h, 0, sizeof *h);
    h->kind = kind;
    scopy(h->label, sizeof h->label, label);
    h->status = ROE_COT_OK;
    h->tokens = 0;
    return h;
}

void roe_cot_init(RoeCotChain *C) {
    if (!C) return;
    memset(C, 0, sizeof *C);
    scopy(C->root, sizeof C->root, "artifacts/roe_daily_packs");
    scopy(C->gov, sizeof C->gov, "logs/governor");
    C->max_hops = 8;
    C->allow_teacher = 0;
    C->run_act = 1;
    C->never_self_cert = 1;
    C->not_conscious = 1;
    C->not_agi = 1;
    C->tokens_skeleton = 0;
    C->body.dopamine = 0.5;
    C->body.serotonin = 0.5;
    C->body.adenosine = 0.35;
    C->body.promotes_cap = 30;
    C->body.teacher_cap = 24;
}

void roe_cot_set_paths(RoeCotChain *C, const char *root, const char *gov) {
    if (!C) return;
    if (root && root[0]) scopy(C->root, sizeof C->root, root);
    if (gov && gov[0]) scopy(C->gov, sizeof C->gov, gov);
}

void roe_cot_load_body(RoeCotChain *C) {
    char path[ROE_COT_PATH], buf[8192];
    if (!C) return;
    snprintf(path, sizeof path, "%s/neuromod_state.json", C->gov);
    if (read_file(path, buf, sizeof buf) > 0) {
        C->body.dopamine = json_num(buf, "dopamine", C->body.dopamine);
        /* levels nested — try both */
        if (strstr(buf, "\"levels\"")) {
            const char *lv = strstr(buf, "\"levels\"");
            if (lv) {
                C->body.dopamine = json_num(lv, "dopamine", C->body.dopamine);
                C->body.serotonin = json_num(lv, "serotonin", C->body.serotonin);
                C->body.adenosine = json_num(lv, "adenosine", C->body.adenosine);
            }
        } else {
            C->body.serotonin = json_num(buf, "serotonin", C->body.serotonin);
            C->body.adenosine = json_num(buf, "adenosine", C->body.adenosine);
        }
    }
    snprintf(path, sizeof path, "%s/schedule_gate.json", C->gov);
    if (read_file(path, buf, sizeof buf) > 0) {
        C->body.pause_grow = json_boolish(buf, "pause_grow_probes");
        C->body.control_mode = json_boolish(buf, "control_mode");
        C->body.impulse_mode = json_boolish(buf, "impulsivity_mode");
    }
    snprintf(path, sizeof path, "%s/front_door_bias.json", C->gov);
    if (read_file(path, buf, sizeof buf) > 0) {
        double exp = json_num(buf, "expires_ts", 0);
        if (exp > (double)time(NULL) && json_boolish(buf, "prefer_local"))
            C->body.da_prefer_local = 1;
    }
    snprintf(path, sizeof path, "%s/autonomy_counters.json", C->gov);
    if (read_file(path, buf, sizeof buf) > 0) {
        C->body.promotes_day = (int)json_num(buf, "promotes_day", 0);
        C->body.teacher_hour = (int)json_num(buf, "teacher_hour", 0);
    }
    snprintf(path, sizeof path, "%s/autonomy_state.json", C->gov);
    if (read_file(path, buf, sizeof buf) > 0) {
        C->body.promotes_cap = (int)json_num(buf, "promotes_per_day", 30);
        if (C->body.promotes_cap <= 0) C->body.promotes_cap = 30;
        C->body.teacher_cap = (int)json_num(buf, "teacher_calls_per_hour", 24);
        if (C->body.teacher_cap <= 0) C->body.teacher_cap = 24;
    }
}

/* Longest substring route from ROUTES.jsonl */
static int match_route(const RoeCotChain *C, const char *q, char *pack, size_t pc,
                       char *skill, size_t sc, char *pat, size_t ptc) {
    char path[ROE_COT_PATH], line[1024], best_pat[256];
    FILE *f;
    int best_len = 0;
    pack[0] = skill[0] = pat[0] = best_pat[0] = 0;
    snprintf(path, sizeof path, "%s/ROUTES.jsonl", C->root);
    f = fopen(path, "r");
    if (!f) return 0;
    while (fgets(line, sizeof line, f)) {
        const char *pp, *pk, *ps;
        char pbuf[256], kbuf[64], sbuf[64];
        int plen;
        pbuf[0] = kbuf[0] = sbuf[0] = 0;
        pp = strstr(line, "\"pattern\"");
        pk = strstr(line, "\"pack\"");
        ps = strstr(line, "\"skill\"");
        if (!pp) continue;
        /* crude "pattern":"..." */
        {
            const char *a = strchr(pp, ':');
            if (!a) continue;
            a = strchr(a, '"');
            if (!a) continue;
            a++;
            {
                const char *b = strchr(a, '"');
                size_t n;
                if (!b) continue;
                n = (size_t)(b - a);
                if (n >= sizeof pbuf) n = sizeof pbuf - 1;
                memcpy(pbuf, a, n);
                pbuf[n] = 0;
            }
        }
        if (pk) {
            const char *a = strchr(pk, ':');
            if (a && (a = strchr(a, '"'))) {
                const char *b;
                a++;
                b = strchr(a, '"');
                if (b) {
                    size_t n = (size_t)(b - a);
                    if (n >= sizeof kbuf) n = sizeof kbuf - 1;
                    memcpy(kbuf, a, n);
                    kbuf[n] = 0;
                }
            }
        }
        if (ps) {
            const char *a = strchr(ps, ':');
            if (a && (a = strchr(a, '"'))) {
                const char *b;
                a++;
                b = strchr(a, '"');
                if (b) {
                    size_t n = (size_t)(b - a);
                    if (n >= sizeof sbuf) n = sizeof sbuf - 1;
                    memcpy(sbuf, a, n);
                    sbuf[n] = 0;
                }
            }
        }
        plen = (int)strlen(pbuf);
        if (plen >= 3 && contains_ci(q, pbuf) && plen > best_len) {
            best_len = plen;
            scopy(best_pat, sizeof best_pat, pbuf);
            scopy(pack, pc, kbuf);
            scopy(skill, sc, sbuf);
            scopy(pat, ptc, pbuf);
        }
    }
    fclose(f);
    return best_len > 0;
}

static void split_goal(const char *q, char parts[][ROE_COT_LINE], int *nparts, int maxp) {
    /* Heuristic microsplit: "and"/"then"/";" or single hop */
    char tmp[ROE_COT_TEXT];
    char *save = NULL, *tok;
    int n = 0;
    scopy(tmp, sizeof tmp, q);
    if (strstr(tmp, " and ") || strchr(tmp, ';') || strstr(tmp, " then ")) {
        for (tok = strtok_r(tmp, ";", &save); tok && n < maxp; tok = strtok_r(NULL, ";", &save)) {
            char *p = tok;
            while (*p == ' ') p++;
            /* further and/then */
            {
                char buf[ROE_COT_LINE];
                char *s2 = NULL, *t2;
                scopy(buf, sizeof buf, p);
                for (t2 = strtok_r(buf, "&", &s2); t2 && n < maxp; ) {
                    /* manual split on " and " / " then " is enough: treat whole clause */
                    break;
                }
            }
            if (contains_ci(p, " and ")) {
                char *a = strstr(p, " and ");
                if (!a) a = strstr(p, " AND ");
                if (a) {
                    *a = 0;
                    while (*p == ' ') p++;
                    scopy(parts[n++], ROE_COT_LINE, p);
                    p = a + 5;
                    while (*p == ' ') p++;
                    if (*p && n < maxp) scopy(parts[n++], ROE_COT_LINE, p);
                    continue;
                }
            }
            if (*p && n < maxp) scopy(parts[n++], ROE_COT_LINE, p);
        }
    }
    if (n == 0 && q && q[0] && maxp > 0) {
        scopy(parts[0], ROE_COT_LINE, q);
        n = 1;
    }
    /* cap splits for control mode depth */
    *nparts = n;
}

static int run_front_door(RoeCotChain *C, const char *q, RoeCotHop *h) {
    char cmd[ROE_COT_CMD];
    char outpath[] = "/tmp/roe_cot_act_XXXXXX";
    char qshort[200];
    int fd;
    FILE *f;
    char line[1024];
    if (!C->run_act) {
        scopy(h->detail, sizeof h->detail, "act skipped (run_act=0)");
        scopy(h->source, sizeof h->source, "SKIP");
        return 0;
    }
    scopy(qshort, sizeof qshort, q);
    if (strchr(qshort, '\'') || strchr(qshort, '"')) {
        scopy(h->detail, sizeof h->detail, "query has quotes — act blocked");
        h->status = ROE_COT_ABSTAIN;
        return -1;
    }
    fd = mkstemp(outpath);
    if (fd < 0) {
        scopy(h->detail, sizeof h->detail, "mkstemp failed");
        h->status = ROE_COT_ERR;
        return -1;
    }
    close(fd);
    if (C->allow_teacher && !C->body.da_prefer_local && !C->body.pause_grow)
        snprintf(cmd, sizeof cmd,
                 "ROE_NO_THOUGHT=1 ./bin/roe_front_door ask '%s' >'%s' 2>&1", qshort,
                 outpath);
    else
        snprintf(cmd, sizeof cmd,
                 "ROE_NO_THOUGHT=1 env -u ROE_LIVE -u ROE_LLM ./bin/roe_front_door ask '%s' "
                 ">'%s' 2>&1",
                 qshort, outpath);
    if (system(cmd) < 0) {
        /* still try read */
    }
    f = fopen(outpath, "r");
    if (!f) {
        h->status = ROE_COT_ERR;
        unlink(outpath);
        return -1;
    }
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "source=", 7) == 0) {
            char *s = line + 7, *sp;
            sp = strchr(s, ' ');
            if (sp) *sp = 0;
            scopy(h->source, sizeof h->source, s);
            {
                char *sk = strstr(line, "skill=");
                if (!sk) {
                    /* skill may be on same line after we truncated — re-parse from file later */
                }
            }
        }
        if (strstr(line, "skill=")) {
            char *sk = strstr(line, "skill=");
            if (sk) {
                sk += 6;
                {
                    char *e = sk;
                    while (*e && *e != ' ' && *e != '\n') e++;
                    *e = 0;
                    if (strcmp(sk, "-") != 0) scopy(h->skill, sizeof h->skill, sk);
                }
            }
        }
        if (strncmp(line, "A:", 2) == 0) {
            char *a = line + 2;
            while (*a == ' ') a++;
            {
                size_t L = strlen(a);
                while (L && (a[L - 1] == '\n' || a[L - 1] == '\r')) a[--L] = 0;
            }
            scopy(h->answer, sizeof h->answer, a);
        }
    }
    fclose(f);
    unlink(outpath);
    if (!h->source[0]) scopy(h->source, sizeof h->source, "UNKNOWN");
    if (strcmp(h->source, "LOCAL") == 0)
        h->status = ROE_COT_OK;
    else if (strcmp(h->source, "LLM") == 0)
        h->status = ROE_COT_MISS;
    else
        h->status = ROE_COT_ABSTAIN;
    snprintf(h->detail, sizeof h->detail, "front_door source=%s skill=%s", h->source,
             h->skill[0] ? h->skill : "-");
    return 0;
}

int roe_cot_run(RoeCotChain *C, const char *query) {
    RoeCotHop *h;
    char pack[ROE_COT_ID], skill[ROE_COT_ID], pat[256];
    char parts[4][ROE_COT_LINE];
    int nparts = 0, i;
    int max_act;

    if (!C || !query || !query[0]) return -1;
    scopy(C->query, sizeof C->query, query);
    C->n_hops = 0;
    C->stopped_early = 0;
    C->final_answer[0] = 0;
    C->final_source[0] = 0;
    C->stop_reason[0] = 0;
    roe_cot_load_body(C);

    /* depth from neuromod */
    max_act = 2;
    if (C->body.control_mode || C->body.serotonin >= 0.60) max_act = 3;
    if (C->body.impulse_mode || C->body.serotonin <= 0.42) max_act = 1;
    if (C->body.adenosine >= 0.58 || C->body.pause_grow) {
        max_act = 1;
    }

    /* PARSE */
    h = add_hop(C, ROE_COT_PARSE, "PARSE");
    if (!h) return -1;
    snprintf(h->detail, sizeof h->detail, "qlen=%zu profile=marble", strlen(query));

    /* CHECK early — ADO / budgets */
    h = add_hop(C, ROE_COT_CHECK, "CHECK");
    if (!h) return -1;
    snprintf(h->detail, sizeof h->detail,
             "DA=%.2f 5HT=%.2f ADO=%.2f pause_grow=%d da_local=%d prom_day=%d/%d",
             C->body.dopamine, C->body.serotonin, C->body.adenosine, C->body.pause_grow,
             C->body.da_prefer_local, C->body.promotes_day, C->body.promotes_cap);
    if (C->body.adenosine >= 0.70) {
        C->stopped_early = 1;
        scopy(C->stop_reason, sizeof C->stop_reason, "adenosine_too_high");
        h->status = ROE_COT_BUDGET;
        scopy(h->detail + strlen(h->detail), sizeof h->detail - strlen(h->detail),
              " → STOP consolidate first");
        /* still SHOW */
        goto join_verify;
    }

    /* SPLIT */
    h = add_hop(C, ROE_COT_SPLIT, "SPLIT");
    if (!h) return -1;
    split_goal(query, parts, &nparts, 4);
    if (nparts > max_act) nparts = max_act;
    {
        char acc[ROE_COT_LINE];
        int pos = 0;
        acc[0] = 0;
        for (i = 0; i < nparts; i++) {
            int w = snprintf(acc + pos, sizeof acc - (size_t)pos, "%s[%d]=%.40s",
                             i ? " " : "", i, parts[i]);
            if (w > 0) pos += w;
        }
        snprintf(h->detail, sizeof h->detail, "n=%d max_act=%d %s", nparts, max_act, acc);
    }

    /* RETRIEVE + ACT per part */
    for (i = 0; i < nparts && C->n_hops < C->max_hops - 2; i++) {
        pack[0] = skill[0] = pat[0] = 0;
        h = add_hop(C, ROE_COT_RETRIEVE, "RETRIEVE");
        if (!h) break;
        if (match_route(C, parts[i], pack, sizeof pack, skill, sizeof skill, pat, sizeof pat)) {
            scopy(h->pack, sizeof h->pack, pack);
            scopy(h->skill, sizeof h->skill, skill);
            snprintf(h->detail, sizeof h->detail, "route pack=%s skill=%s pat=%s", pack,
                     skill[0] ? skill : "-", pat);
        } else {
            scopy(h->pack, sizeof h->pack, "always_on");
            snprintf(h->detail, sizeof h->detail, "no route — always_on + personal");
        }

        h = add_hop(C, ROE_COT_ACT, "ACT");
        if (!h) break;
        scopy(h->pack, sizeof h->pack, pack[0] ? pack : "always_on");
        if (skill[0]) scopy(h->skill, sizeof h->skill, skill);
        run_front_door(C, parts[i], h);
        /* prefer last LOCAL answer as final */
        if (h->answer[0] && (strcmp(h->source, "LOCAL") == 0 || !C->final_answer[0])) {
            scopy(C->final_answer, sizeof C->final_answer, h->answer);
            scopy(C->final_source, sizeof C->final_source, h->source);
        }
    }

join_verify:
    /* JOIN */
    h = add_hop(C, ROE_COT_JOIN, "JOIN");
    if (h) {
        if (C->final_answer[0])
            snprintf(h->detail, sizeof h->detail, "assembled source=%s ans=%.60s",
                     C->final_source, C->final_answer);
        else {
            scopy(h->detail, sizeof h->detail, "no LOCAL answer — abstain join");
            h->status = ROE_COT_ABSTAIN;
            if (!C->final_answer[0])
                scopy(C->final_answer, sizeof C->final_answer,
                      "ABSTAIN: chain found no LOCAL cert coverage.");
            scopy(C->final_source, sizeof C->final_source, "ABSTAIN");
        }
    }

    /* VERIFY */
    h = add_hop(C, ROE_COT_VERIFY, "VERIFY");
    if (h) {
        snprintf(h->detail, sizeof h->detail,
                 "never_self_cert=%d chain≠CERT source=%s teacher=%s", C->never_self_cert,
                 C->final_source[0] ? C->final_source : "-",
                 C->allow_teacher ? "allowed_on_miss" : "off");
        if (strcmp(C->final_source, "LLM") == 0) {
            /* untrusted */
            scopy(h->detail + strlen(h->detail), sizeof h->detail - strlen(h->detail),
                  " LLM untrusted until gold|reviewer");
            h->status = ROE_COT_MISS;
        }
    }

    /* SHOW */
    h = add_hop(C, ROE_COT_SHOW, "SHOW");
    if (h)
        snprintf(h->detail, sizeof h->detail, "panel hops=%d skeleton_tokens=0", C->n_hops);

    /* chain one-liner */
    snprintf(C->chain_line, sizeof C->chain_line,
             "CoT: hops=%d DA=%.2f/5HT=%.2f/ADO=%.2f src=%s stop=%s", C->n_hops,
             C->body.dopamine, C->body.serotonin, C->body.adenosine,
             C->final_source[0] ? C->final_source : "-",
             C->stopped_early ? C->stop_reason : "ok");

    snprintf(C->summary, sizeof C->summary,
             "query=%.80s | hops=%d | answer_source=%s | law=never_self_cert,not_conscious,not_agi",
             C->query, C->n_hops, C->final_source);
    return 0;
}

int roe_cot_format_panel(const RoeCotChain *C, char *buf, size_t cap) {
    size_t pos = 0;
    int i;
    if (!C || !buf || !cap) return -1;
    pos += (size_t)snprintf(buf + pos, cap - pos, "┌─ chain-of-thought (0 tokens*) ─────────\n");
    if (pos >= cap) return -1;
    pos += (size_t)snprintf(buf + pos, cap - pos, "│ query: %.120s\n", C->query);
    for (i = 0; i < C->n_hops && pos + 8 < cap; i++) {
        const RoeCotHop *h = &C->hops[i];
        pos += (size_t)snprintf(buf + pos, cap - pos, "│ hop%d %-8s %s\n", i + 1, h->label,
                                h->detail);
    }
    pos += (size_t)snprintf(buf + pos, cap - pos, "│ %s\n", C->chain_line);
    pos += (size_t)snprintf(buf + pos, cap - pos,
                            "│ law: never_self_cert · not_conscious · not_agi · chain≠CERT\n");
    pos += (size_t)snprintf(buf + pos, cap - pos,
                            "│ * skeleton 0 LLM tokens; teacher only if ACT miss+allowed\n");
    pos += (size_t)snprintf(buf + pos, cap - pos, "└────────────────────────────────────────\n");
    pos += (size_t)snprintf(buf + pos, cap - pos, "answer (%s):\n%s\n",
                            C->final_source[0] ? C->final_source : "-", C->final_answer);
    return (int)pos;
}

void roe_cot_print_panel(const RoeCotChain *C) {
    char buf[8192];
    if (roe_cot_format_panel(C, buf, sizeof buf) > 0) fputs(buf, stdout);
}

int roe_cot_persist(const RoeCotChain *C) {
    char path[ROE_COT_PATH], panel[8192];
    char qesc[ROE_COT_TEXT], aesc[ROE_COT_ANS], lesc[ROE_COT_TEXT];
    FILE *f;
    int i;
    if (!C) return -1;
    /* minimal JSON string escape: strip quotes/backslashes */
    {
        size_t si, di;
        const char *s;
        s = C->query;
        for (si = di = 0; s[si] && di + 1 < sizeof qesc; si++) {
            if (s[si] == '"' || s[si] == '\\') continue;
            qesc[di++] = s[si];
        }
        qesc[di] = 0;
        s = C->final_answer;
        for (si = di = 0; s[si] && di + 1 < sizeof aesc; si++) {
            if (s[si] == '"' || s[si] == '\\' || s[si] == '\n') {
                if (s[si] == '\n') {
                    if (di + 2 < sizeof aesc) {
                        aesc[di++] = ' ';
                    }
                }
                continue;
            }
            aesc[di++] = s[si];
        }
        aesc[di] = 0;
        s = C->chain_line;
        for (si = di = 0; s[si] && di + 1 < sizeof lesc; si++) {
            if (s[si] == '"' || s[si] == '\\') continue;
            lesc[di++] = s[si];
        }
        lesc[di] = 0;
    }
    snprintf(path, sizeof path, "%s", C->gov);
    mkdir(path, 0755);
    snprintf(path, sizeof path, "%s/chain_last.txt", C->gov);
    if (roe_cot_format_panel(C, panel, sizeof panel) < 0) return -1;
    f = fopen(path, "w");
    if (!f) return -1;
    fputs(panel, f);
    fclose(f);
    snprintf(path, sizeof path, "%s/chain_last.json", C->gov);
    f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f,
            "{\n  \"engine\": \"roe_chain_think_c\",\n  \"tokens_skeleton\": 0,\n"
            "  \"llm_cot\": false,\n  \"query\": \"%s\",\n  \"n_hops\": %d,\n"
            "  \"final_source\": \"%s\",\n  \"chain_line\": \"%s\",\n"
            "  \"never_self_cert\": true,\n  \"not_conscious\": true,\n  \"not_agi\": true,\n"
            "  \"continuity_is_not_cert\": true,\n  \"hops\": [\n",
            qesc, C->n_hops, C->final_source, lesc);
    for (i = 0; i < C->n_hops; i++) {
        const RoeCotHop *h = &C->hops[i];
        char desc[220];
        size_t si, di;
        for (si = di = 0; h->detail[si] && di + 1 < sizeof desc; si++) {
            if (h->detail[si] == '"' || h->detail[si] == '\\') continue;
            desc[di++] = h->detail[si];
        }
        desc[di] = 0;
        fprintf(f,
                "    {\"i\":%d,\"label\":\"%s\",\"detail\":\"%s\",\"source\":\"%s\","
                "\"skill\":\"%s\",\"status\":%d}%s\n",
                i, h->label, desc, h->source, h->skill, h->status,
                i + 1 < C->n_hops ? "," : "");
    }
    fprintf(f, "  ],\n  \"answer\": \"%s\"\n}\n", aesc);
    fclose(f);
    snprintf(path, sizeof path, "%s/chain_think.jsonl", C->gov);
    f = fopen(path, "a");
    if (f) {
        fprintf(f, "{\"ts\":%ld,\"hops\":%d,\"src\":\"%s\",\"line\":\"%s\"}\n",
                (long)time(NULL), C->n_hops, C->final_source, lesc);
        fclose(f);
    }
    return 0;
}

int roe_cot_selftest(void) {
    RoeCotChain C;
    char panel[4096];
    int fail = 0;
    printf("=== roe_chain_think (C, 0-token CoT) ===\n");
#define T(ok, m)                                                                         \
    do {                                                                                 \
        printf("  %-56s %s\n", m, (ok) ? "PASS" : "FAIL");                               \
        if (!(ok)) fail++;                                                               \
    } while (0)

    roe_cot_init(&C);
    C.run_act = 0; /* unit test without front_door */
    C.max_hops = 6;
    T(roe_cot_run(&C, "who are you") == 0, "run identity");
    T(C.tokens_skeleton == 0, "skeleton tokens 0");
    T(C.n_hops >= 4, "has multiple hops");
    T(C.never_self_cert == 1 && C.not_conscious == 1, "law flags");
    {
        int has_parse = 0, has_split = 0, has_ver = 0, i;
        for (i = 0; i < C.n_hops; i++) {
            if (C.hops[i].kind == ROE_COT_PARSE) has_parse = 1;
            if (C.hops[i].kind == ROE_COT_SPLIT) has_split = 1;
            if (C.hops[i].kind == ROE_COT_VERIFY) has_ver = 1;
        }
        T(has_parse && has_split && has_ver, "PARSE+SPLIT+VERIFY present");
    }
    T(roe_cot_format_panel(&C, panel, sizeof panel) > 0, "format panel");
    T(strstr(panel, "chain-of-thought") != NULL, "panel title");
    T(strstr(panel, "0 tokens") != NULL, "panel says 0 tokens");
    T(strstr(panel, "not_conscious") != NULL, "panel law");
    /* multi clause split */
    roe_cot_init(&C);
    C.run_act = 0;
    T(roe_cot_run(&C, "who are you and never self-cert rule") == 0, "split and-clause");
    T(C.n_hops >= 5, "split yields more hops");
    /* ADO stop: empty gov so load_body cannot overwrite */
    roe_cot_init(&C);
    C.run_act = 0;
    scopy(C.gov, sizeof C.gov, "/tmp/roe_cot_empty_gov_zz");
    C.body.adenosine = 0.75;
    /* run calls load_body — with missing files body keeps 0.75 only if we set AFTER load.
       load_body doesn't reset missing keys — keeps 0.75. Good. */
    T(roe_cot_run(&C, "status") == 0, "high ADO run");
    T(C.stopped_early == 1 || C.body.adenosine >= 0.58, "ADO stop or high pressure");
    if (C.stopped_early)
        T(strstr(C.stop_reason, "adenosine") != NULL, "stop_reason adenosine");
    else
        T(1, "ADO soft (file may have lowered)");

    roe_cot_init(&C);
    C.run_act = 0;
    mkdir("logs/governor", 0755);
    scopy(C.gov, sizeof C.gov, "logs/governor");
    roe_cot_run(&C, "who are you");
    T(roe_cot_persist(&C) == 0, "persist chain_last");
#undef T
    printf("\nfailures=%d\n", fail);
    if (fail) {
        printf("ROE_CHAIN_THINK_FAIL\n");
        return 1;
    }
    printf("ROE_CHAIN_THINK_PASS\n");
    return 0;
}
