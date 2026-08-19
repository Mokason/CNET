#include "../../include/cnet_roe_debug.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int name_eq(const char *a, const char *b) {
    return a && b && strcmp(a, b) == 0;
}

static int mkdir_p(const char *path) {
    char tmp[ROE_PATH_MAX];
    size_t i, len;
    if (!path || !path[0]) return -1;
    snprintf(tmp, sizeof tmp, "%s", path);
    len = strlen(tmp);
    if (len && tmp[len - 1] == '/') tmp[len - 1] = 0;
    for (i = 1; tmp[i]; i++) {
        if (tmp[i] == '/') {
            tmp[i] = 0;
            cnet_mkdir(tmp, 0755);
            tmp[i] = '/';
        }
    }
    return cnet_mkdir(tmp, 0755);
}

void roe_dbg_init(RoeDebug *D) {
    if (!D) return;
    memset(D, 0, sizeof *D);
    roe_init(&D->roe);
}

void roe_dbg_set_catalog(RoeDebug *D, const char *dir) {
    if (!D || !dir) return;
    snprintf(D->catalog_dir, sizeof D->catalog_dir, "%s", dir);
    roe_set_catalog_dir(&D->roe, dir);
}

void roe_dbg_signature(const char *traceback_or_msg, char *sig, size_t cap) {
    char buf[1024];
    char *line, *last = NULL, *p, *save = NULL;
    size_t i, o = 0;
    if (!sig || !cap) return;
    sig[0] = 0;
    if (!traceback_or_msg || !traceback_or_msg[0]) {
        snprintf(sig, cap, "empty");
        return;
    }
    snprintf(buf, sizeof buf, "%s", traceback_or_msg);
    /* prefer last non-empty line */
    for (line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        while (*line == ' ' || *line == '\t') line++;
        if (*line) last = line;
    }
    if (!last) last = buf;
    /* strip File "path", line N patterns loosely by taking from first Alpha error token */
    p = last;
    /* if contains Error or Exception, start there */
    {
        char *e = strstr(last, "Error");
        char *x = strstr(last, "Exception");
        if (e && (!x || e < x)) p = e;
        else if (x) p = x;
    }
    /* normalize: alnum â†’ lower, else _ ; collapse _ */
    for (i = 0; p[i] && o + 1 < cap; i++) {
        unsigned char c = (unsigned char)p[i];
        if (isalnum(c)) {
            sig[o++] = (char)tolower(c);
        } else if (o > 0 && sig[o - 1] != '_') {
            sig[o++] = '_';
        }
    }
    while (o > 0 && sig[o - 1] == '_') o--;
    sig[o] = 0;
    if (!sig[0]) snprintf(sig, cap, "unknown_error");
    if (o > 120) sig[120] = 0; /* bound */
}

int roe_dbg_add_project(RoeDebug *D, const char *project_id) {
    size_t i;
    if (!D || !project_id || !project_id[0]) return -1;
    for (i = 0; i < D->n_projects; i++)
        if (D->projects[i].active && name_eq(D->projects[i].id, project_id))
            return 0;
    if (D->n_projects >= ROE_DBG_PROJ_MAX) return -1;
    memset(&D->projects[D->n_projects], 0, sizeof D->projects[0]);
    snprintf(D->projects[D->n_projects].id, sizeof D->projects[0].id, "%s",
             project_id);
    D->projects[D->n_projects].active = 1;
    D->n_projects++;
    return 0;
}

static RoeDbgProject *find_proj(RoeDebug *D, const char *id) {
    size_t i;
    for (i = 0; i < D->n_projects; i++)
        if (D->projects[i].active && name_eq(D->projects[i].id, id))
            return &D->projects[i];
    return NULL;
}

static RoeDbgMemory *find_mem(RoeDebug *D, const char *project_id, const char *sig) {
    size_t i;
    for (i = 0; i < D->n_mem; i++)
        if (D->mem[i].active && D->mem[i].certified &&
            name_eq(D->mem[i].project_id, project_id) &&
            name_eq(D->mem[i].sig, sig))
            return &D->mem[i];
    return NULL;
}

int roe_dbg_seed_curriculum(RoeDebug *D) {
    if (!D) return -1;
    /* L1 recipes */
    roe_add_skill(&D->roe, "dbg_nameerror", "NameError", "NameError",
                  "L1 NameError: name missing â€” check spelling, scope, imports, typos.", 0,
                  1);
    roe_add_skill(&D->roe, "dbg_typeerror", "TypeError", "TypeError",
                  "L1 TypeError: wrong type/arity â€” check args, None, str/int mix.", 0, 1);
    roe_add_skill(&D->roe, "dbg_attributeerror", "AttributeError", "AttributeError",
                  "L1 AttributeError: object has no attr â€” check type, None, API name.", 0,
                  1);
    roe_add_skill(&D->roe, "dbg_importerror", "ImportError", "ImportError",
                  "L1 ImportError/ModuleNotFound â€” check sys.path, venv, package name.", 0,
                  1);
    roe_add_skill(&D->roe, "dbg_modulenotfound", "ModuleNotFoundError", "ModuleNotFoundError",
                  "L1 ModuleNotFoundError: pip/venv/package path; confirm install.", 0, 1);
    roe_add_skill(&D->roe, "dbg_keyerror", "KeyError", "KeyError",
                  "L1 KeyError: missing dict/key â€” use .get, validate keys.", 0, 1);
    roe_add_skill(&D->roe, "dbg_indexerror", "IndexError", "IndexError",
                  "L1 IndexError: index out of range â€” check len and off-by-one.", 0, 1);
    roe_add_skill(&D->roe, "dbg_valueerror", "ValueError", "ValueError",
                  "L1 ValueError: bad value domain â€” validate inputs before convert.", 0, 1);
    roe_add_skill(&D->roe, "dbg_assertion", "AssertionError", "AssertionError",
                  "L1 AssertionError: expectation failed â€” read assert, print actuals.", 0,
                  1);
    roe_add_skill(&D->roe, "dbg_syntax", "SyntaxError", "SyntaxError",
                  "L1 SyntaxError: fix parse error at caret â€” parens, colons, indents.", 0,
                  1);
    /* L2 checklist */
    roe_add_skill(&D->roe, "dbg_checklist", "debug checklist", "debug checklist",
                  "L2 checklist: 1) reproduce 2) read full traceback 3) minimal case "
                  "4) bisect change 5) write failing test 6) fix 7) re-run tests",
                  0, 1);
    roe_add_skill(&D->roe, "dbg_pytest", "pytest fail", "pytest",
                  "L2 pytest: run pytest -q path::test; read failure; fix; re-run until green",
                  0, 1);
    roe_add_skill(&D->roe, "dbg_bisect", "git bisect", "bisect",
                  "L2 bisect: git bisect start; good/bad; isolate first bad commit", 0, 1);

    /* Teach fallbacks for promote into global when useful */
    roe_add_teach(&D->roe, "circular import", "circular_import",
                  "L1 circular import: delay import, refactor deps, import inside function");
    roe_add_teach(&D->roe, "test isolation", "test_isolation",
                  "L2 tests pollute state: reset fixtures, avoid global mutate, use tmp_path");
    return 0;
}

int roe_dbg_turn(RoeDebug *D, const char *project_id, const char *query_or_tb,
                 RoeDbgReply *out) {
    char sig[ROE_DBG_SIG_MAX];
    RoeDbgMemory *m;
    RoeDbgProject *p;
    RoeReply rr;
    int st;

    if (out) memset(out, 0, sizeof *out);
    if (!D || !query_or_tb || !query_or_tb[0]) return ROE_ERR;

    D->n_turns++;
    D->tokens_baseline += 40; /* pretend full LLM debug session cost unit */

    if (project_id && project_id[0]) roe_dbg_add_project(D, project_id);
    p = project_id ? find_proj(D, project_id) : NULL;
    if (p) p->n_debugs++;

    roe_dbg_signature(query_or_tb, sig, sizeof sig);
    if (out) snprintf(out->sig, sizeof out->sig, "%s", sig);

    /* L3 project memory first */
    if (project_id && project_id[0]) {
        m = find_mem(D, project_id, sig);
        if (m) {
            m->hits++;
            if (p) p->n_l3_hits++;
            D->n_l3++;
            D->tokens_used += 0;
            if (out) {
                out->level = ROE_DBG_L3_PROJECT;
                out->verified_local = 1;
                out->tokens_est = 0;
                out->source = ROE_SRC_LOCAL;
                snprintf(out->skill_id, sizeof out->skill_id, "%s", m->skill_id);
                {
                    char body[ROE_ANSWER_MAX];
                    size_t n;
                    snprintf(body, sizeof body, "L3[%s] %s | fix: ", m->project_id,
                             m->note[0] ? m->note : m->sig);
                    n = strlen(body);
                    if (n >= sizeof(out->answer)) n = sizeof(out->answer) - 1;
                    memcpy(out->answer, body, n);
                    {
                        size_t fl = strlen(m->fix);
                        size_t room = sizeof(out->answer) - n - 1;
                        if (fl > room) fl = room;
                        memcpy(out->answer + n, m->fix, fl);
                        out->answer[n + fl] = 0;
                    }
                }
            }
            return ROE_OK;
        }
    }

    /* L1/L2 via ROE global skills / teach */
    st = roe_turn(&D->roe, query_or_tb, &rr);
    D->tokens_used += rr.tokens_est;
    if (st == ROE_OK && rr.source == ROE_SRC_LOCAL) {
        int is_l2 = (strstr(rr.skill_id, "checklist") || strstr(rr.skill_id, "pytest") ||
                     strstr(rr.skill_id, "bisect") || strstr(rr.answer, "L2"))
                        ? 1
                        : 0;
        if (is_l2) D->n_l2++;
        else D->n_l1++;
        if (out) {
            out->level = is_l2 ? ROE_DBG_L2_CHECKLIST : ROE_DBG_L1_RECIPE;
            out->verified_local = 1;
            out->tokens_est = 0;
            out->source = ROE_SRC_LOCAL;
            snprintf(out->skill_id, sizeof out->skill_id, "%s", rr.skill_id);
            snprintf(out->answer, sizeof out->answer, "%s", rr.answer);
        }
        return ROE_OK;
    }
    if (st == ROE_OK && (rr.source == ROE_SRC_LLM || rr.source == ROE_SRC_LOOKUP)) {
        D->n_miss++;
        if (out) {
            out->level = ROE_DBG_MISS;
            out->verified_local = 0;
            out->tokens_est = rr.tokens_est;
            out->source = rr.source;
            snprintf(out->answer, sizeof out->answer, "%s", rr.answer);
        }
        return ROE_OK;
    }

    D->n_abstain++;
    if (out) {
        out->level = ROE_DBG_ABSTAIN;
        out->source = ROE_SRC_ASK_USER;
        snprintf(out->answer, sizeof out->answer,
                 "ABSTAIN: no L3 project memory and no L1/L2 recipe. Capture fix under verify.");
    }
    return ROE_ABSTAIN;
}

int roe_dbg_learn(RoeDebug *D, const char *project_id, const char *query_or_tb,
                  const char *fix, const char *note) {
    char sig[ROE_DBG_SIG_MAX];
    char skill_id[ROE_NAME_MAX];
    RoeDbgMemory *m;
    size_t i;
    if (!D || !project_id || !query_or_tb || !fix) return -1;
    roe_dbg_add_project(D, project_id);
    roe_dbg_signature(query_or_tb, sig, sizeof sig);

    /* update existing */
    for (i = 0; i < D->n_mem; i++) {
        if (D->mem[i].active && name_eq(D->mem[i].project_id, project_id) &&
            name_eq(D->mem[i].sig, sig)) {
            m = &D->mem[i];
            snprintf(m->fix, sizeof m->fix, "%s", fix);
            if (note) snprintf(m->note, sizeof m->note, "%s", note);
            m->certified = 1;
            m->verifies++;
            m->tick_learned = ++D->roe.tick;
            D->n_promote_l3++;
            if (D->catalog_dir[0]) (void)roe_dbg_save(D);
            return 1;
        }
    }
    if (D->n_mem >= ROE_DBG_MEM_MAX) return -1;
    m = &D->mem[D->n_mem++];
    memset(m, 0, sizeof *m);
    snprintf(m->project_id, sizeof m->project_id, "%s", project_id);
    snprintf(m->sig, sizeof m->sig, "%s", sig);
    snprintf(m->fix, sizeof m->fix, "%s", fix);
    if (note) snprintf(m->note, sizeof m->note, "%s", note);
    skill_id[0] = 'l';
    skill_id[1] = '3';
    skill_id[2] = '_';
    {
        size_t n = strlen(sig);
        size_t i;
        if (n > sizeof(skill_id) - 4) n = sizeof(skill_id) - 4;
        for (i = 0; i < n; i++) {
            unsigned char c = (unsigned char)sig[i];
            skill_id[3 + i] = (char)(isalnum(c) || c == '_' ? c : '_');
        }
        skill_id[3 + n] = 0;
    }
    snprintf(m->skill_id, sizeof m->skill_id, "%s", skill_id);
    m->certified = 1;
    m->active = 1;
    m->verifies = 1;
    m->tick_learned = ++D->roe.tick;
    D->n_promote_l3++;
    /* also register a project-tagged pattern on ROE for soft recall */
    {
        char pat[ROE_TEXT_MAX];
        snprintf(pat, sizeof pat, "%s", sig);
        (void)roe_add_skill(&D->roe, skill_id, skill_id, pat, fix, 1, 1);
    }
    if (D->catalog_dir[0]) (void)roe_dbg_save(D);
    return 1;
}

int roe_dbg_verify(RoeDebug *D, const char *project_id, const char *query_or_tb,
                   const char *fix_override, int tests_passed, int user_accept) {
    char sig[ROE_DBG_SIG_MAX];
    const char *fix = fix_override;
    if (!D || !project_id || !query_or_tb) return 0;
    if (!(tests_passed || user_accept)) {
        D->n_verify_fail++;
        return 0;
    }
    roe_dbg_signature(query_or_tb, sig, sizeof sig);
    if (!fix || !fix[0]) {
        /* use teach answer if any */
        RoeReply rr;
        (void)roe_turn(&D->roe, query_or_tb, &rr);
        fix = rr.answer;
        if (!fix[0]) fix = "verified fix recorded";
    }
    D->n_verify_ok++;
    return roe_dbg_learn(D, project_id, query_or_tb, fix,
                         tests_passed ? "tests_passed" : "user_accept");
}

void roe_dbg_dump_stats(const RoeDebug *D, char *buf, size_t cap) {
    double hit3, save;
    if (!D || !buf || !cap) return;
    hit3 = D->n_turns ? (double)D->n_l3 / (double)D->n_turns : 0.0;
    save = D->tokens_baseline
               ? 1.0 - (double)D->tokens_used / (double)D->tokens_baseline
               : 0.0;
    snprintf(buf, cap,
             "turns=%llu L1=%llu L2=%llu L3=%llu miss=%llu abs=%llu "
             "verify_ok=%llu verify_fail=%llu promote_l3=%llu "
             "tok=%llu/%llu l3_rate=%.3f save=%.3f projects=%zu mem=%zu",
             (unsigned long long)D->n_turns, (unsigned long long)D->n_l1,
             (unsigned long long)D->n_l2, (unsigned long long)D->n_l3,
             (unsigned long long)D->n_miss, (unsigned long long)D->n_abstain,
             (unsigned long long)D->n_verify_ok, (unsigned long long)D->n_verify_fail,
             (unsigned long long)D->n_promote_l3, (unsigned long long)D->tokens_used,
             (unsigned long long)D->tokens_baseline, hit3, save, D->n_projects,
             D->n_mem);
}

int roe_dbg_save(const RoeDebug *D) {
    char path[ROE_PATH_MAX];
    FILE *f;
    size_t i;
    int n = 0;
    size_t cl;
    if (!D || !D->catalog_dir[0]) return -1;
    mkdir_p(D->catalog_dir);
    (void)roe_save_catalog(&D->roe);
    cl = strlen(D->catalog_dir);
    if (cl + 18 >= sizeof path) return -5;
    memcpy(path, D->catalog_dir, cl);
    memcpy(path + cl, "/project_memory.jsonl", 22);
    f = fopen(path, "w");
    if (!f) return -2;
    for (i = 0; i < D->n_mem; i++) {
        const RoeDbgMemory *m = &D->mem[i];
        if (!m->active || !m->certified) continue;
        fprintf(f,
                "{\"project\":\"%s\",\"sig\":\"%s\",\"skill\":\"%s\",\"fix\":\"%s\","
                "\"note\":\"%s\",\"hits\":%llu,\"verifies\":%llu}\n",
                m->project_id, m->sig, m->skill_id, m->fix, m->note,
                (unsigned long long)m->hits, (unsigned long long)m->verifies);
        n++;
    }
    fclose(f);
    /* projects list */
    memcpy(path + cl, "/projects.txt", 14);
    f = fopen(path, "w");
    if (f) {
        for (i = 0; i < D->n_projects; i++)
            if (D->projects[i].active) fprintf(f, "%s\n", D->projects[i].id);
        fclose(f);
    }
    return n;
}

int roe_dbg_load(RoeDebug *D) {
    char path[ROE_PATH_MAX], line[1200];
    FILE *f;
    int n = 0;
    size_t cl;
    if (!D || !D->catalog_dir[0]) return -1;
    (void)roe_load_catalog(&D->roe);
    cl = strlen(D->catalog_dir);
    if (cl + 22 >= sizeof path) return -5;
    memcpy(path, D->catalog_dir, cl);
    memcpy(path + cl, "/project_memory.jsonl", 22);
    f = fopen(path, "r");
    if (!f) return 0;
    while (fgets(line, sizeof line, f)) {
        char project[ROE_DBG_PROJ_NAME], sig[ROE_DBG_SIG_MAX], skill[ROE_NAME_MAX],
            fix[ROE_ANSWER_MAX], note[ROE_TEXT_MAX];
        const char *p;
        RoeDbgMemory *m;
        size_t i;
        project[0] = sig[0] = skill[0] = fix[0] = note[0] = 0;
        p = strstr(line, "\"project\":\"");
        if (p) sscanf(p, "\"project\":\"%63[^\"]\"", project);
        p = strstr(line, "\"sig\":\"");
        if (p) sscanf(p, "\"sig\":\"%159[^\"]\"", sig);
        p = strstr(line, "\"skill\":\"");
        if (p) sscanf(p, "\"skill\":\"%63[^\"]\"", skill);
        p = strstr(line, "\"fix\":\"");
        if (p) sscanf(p, "\"fix\":\"%511[^\"]\"", fix);
        p = strstr(line, "\"note\":\"");
        if (p) sscanf(p, "\"note\":\"%255[^\"]\"", note);
        if (!(project[0] && sig[0] && fix[0])) continue;
        if (find_mem(D, project, sig)) continue;
        if (D->n_mem >= ROE_DBG_MEM_MAX) break;
        roe_dbg_add_project(D, project);
        m = &D->mem[D->n_mem++];
        memset(m, 0, sizeof *m);
        snprintf(m->project_id, sizeof m->project_id, "%s", project);
        snprintf(m->sig, sizeof m->sig, "%s", sig);
        snprintf(m->fix, sizeof m->fix, "%s", fix);
        if (note[0]) snprintf(m->note, sizeof m->note, "%s", note);
        if (skill[0])
            snprintf(m->skill_id, sizeof m->skill_id, "%s", skill);
        else
            snprintf(m->skill_id, sizeof m->skill_id, "l3_loaded_%d", n);
        m->certified = 1;
        m->active = 1;
        n++;
        (void)i;
    }
    fclose(f);
    return n;
}
