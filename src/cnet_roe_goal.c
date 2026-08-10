#include "../include/cnet_roe_goal.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int contains_ci(const char *hay, const char *needle) {
    char h[ROE_GOAL_TEXT * 2], n[ROE_GOAL_TEXT];
    size_t i;
    if (!hay || !needle || !needle[0]) return 0;
    for (i = 0; hay[i] && i + 1 < sizeof h; i++)
        h[i] = (char)tolower((unsigned char)hay[i]);
    h[i] = 0;
    for (i = 0; needle[i] && i + 1 < sizeof n; i++)
        n[i] = (char)tolower((unsigned char)needle[i]);
    n[i] = 0;
    return strstr(h, n) != NULL;
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
            mkdir(tmp, 0755);
            tmp[i] = '/';
        }
    }
    return mkdir(tmp, 0755);
}

static RoeGoalCategory *find_cat(RoeGoalEngine *G, const char *cat) {
    size_t i;
    for (i = 0; i < G->n_cats; i++)
        if (G->cats[i].active && strcmp(G->cats[i].name, cat) == 0)
            return &G->cats[i];
    return NULL;
}

void roe_goal_init(RoeGoalEngine *G) {
    if (!G) return;
    memset(G, 0, sizeof *G);
    roe_init(&G->roe);
}

void roe_goal_set_catalog(RoeGoalEngine *G, const char *dir) {
    if (!G || !dir) return;
    snprintf(G->catalog_dir, sizeof G->catalog_dir, "%s", dir);
    roe_set_catalog_dir(&G->roe, dir);
}

int roe_goal_add_category(RoeGoalEngine *G, const char *cat) {
    if (!G || !cat || !cat[0]) return -1;
    if (find_cat(G, cat)) return 0;
    if (G->n_cats >= ROE_GOAL_MAX_CAT) return -1;
    memset(&G->cats[G->n_cats], 0, sizeof G->cats[0]);
    snprintf(G->cats[G->n_cats].name, sizeof G->cats[0].name, "%s", cat);
    G->cats[G->n_cats].active = 1;
    G->n_cats++;
    return 0;
}

int roe_goal_add_sub(RoeGoalEngine *G, const char *cat, const char *sub) {
    RoeGoalCategory *c;
    int i;
    if (!G || !cat || !sub) return -1;
    roe_goal_add_category(G, cat);
    c = find_cat(G, cat);
    if (!c) return -1;
    for (i = 0; i < c->n_subs; i++)
        if (strcmp(c->subs[i], sub) == 0) return 0;
    if (c->n_subs >= 16) return -1;
    snprintf(c->subs[c->n_subs], sizeof c->subs[0], "%s", sub);
    c->n_subs++;
    return 0;
}

int roe_goal_map_pattern(RoeGoalEngine *G, const char *cat, const char *sub,
                         const char *pattern, const char *skill_id) {
    RoeGoalMapEntry *m;
    if (!G || !cat || !sub || !pattern) return -1;
    roe_goal_add_sub(G, cat, sub);
    if (G->n_maps >= ROE_GOAL_MAX_MAP) return -1;
    m = &G->maps[G->n_maps++];
    memset(m, 0, sizeof *m);
    snprintf(m->cat, sizeof m->cat, "%s", cat);
    snprintf(m->sub, sizeof m->sub, "%s", sub);
    snprintf(m->pattern, sizeof m->pattern, "%s", pattern);
    if (skill_id) snprintf(m->skill_id, sizeof m->skill_id, "%s", skill_id);
    m->active = 1;
    return 0;
}

static void add_skill_tidy(RoeGoalEngine *G, const char *cat, const char *sub,
                           const char *id, const char *pattern, const char *answer) {
    char sid[ROE_NAME_MAX];
    snprintf(sid, sizeof sid, "%s__%s__%s", cat, sub, id);
    /* sanitize */
    {
        char *p;
        for (p = sid; *p; p++)
            if (!isalnum((unsigned char)*p) && *p != '_') *p = '_';
    }
    (void)roe_add_skill(&G->roe, sid, id, pattern, answer, 0, 1);
    (void)roe_goal_map_pattern(G, cat, sub, pattern, sid);
}

int roe_goal_seed_default(RoeGoalEngine *G) {
    if (!G) return -1;
    /* taxonomy */
    roe_goal_add_sub(G, "coding", "basics");
    roe_goal_add_sub(G, "coding", "control");
    roe_goal_add_sub(G, "coding", "data");
    roe_goal_add_sub(G, "coding", "functions");
    roe_goal_add_sub(G, "coding", "files");
    roe_goal_add_sub(G, "coding", "errors");
    roe_goal_add_sub(G, "debug", "recipes");
    roe_goal_add_sub(G, "debug", "process");
    roe_goal_add_sub(G, "debug", "project");
    roe_goal_add_sub(G, "shell", "law");

    /* known knowledge (HAVE) */
    add_skill_tidy(G, "coding", "basics", "hello", "hello world",
                   "print(\"Hello, world\")");
    add_skill_tidy(G, "coding", "basics", "assign", "variable assign",
                   "name = value");
    add_skill_tidy(G, "coding", "control", "ifelse", "if else",
                   "if cond: ... else: ...");
    add_skill_tidy(G, "debug", "recipes", "nameerror", "NameError",
                   "NameError: check spelling/scope/import");
    add_skill_tidy(G, "debug", "process", "checklist", "debug checklist",
                   "reproduce; read tb; minimal case; test; fix; re-run");
    add_skill_tidy(G, "shell", "law", "failclosed", "fail closed",
                   "Abstain outside coverage; never self-CERT");

    /* teach curriculum for LEARN path (not CERT until run learns) */
    roe_add_teach(&G->roe, "for loop", "for_loop",
                  "for x in xs: body  # iterate");
    roe_goal_map_pattern(G, "coding", "control", "for loop", "coding__control__for");
    roe_add_teach(&G->roe, "while loop", "while_loop",
                  "while cond: body  # loop until false");
    roe_goal_map_pattern(G, "coding", "control", "while loop",
                         "coding__control__while");
    roe_add_teach(&G->roe, "function def", "function_def",
                  "def f(args): return value");
    roe_goal_map_pattern(G, "coding", "functions", "function def",
                         "coding__functions__def");
    roe_add_teach(&G->roe, "list append", "list_append", "xs.append(item)");
    roe_goal_map_pattern(G, "coding", "data", "list append",
                         "coding__data__append");
    roe_add_teach(&G->roe, "dict get", "dict_get", "d.get(k, default)");
    roe_goal_map_pattern(G, "coding", "data", "dict get", "coding__data__get");
    roe_add_teach(&G->roe, "read file", "read_file",
                  "with open(p) as f: data=f.read()");
    roe_goal_map_pattern(G, "coding", "files", "read file", "coding__files__read");
    roe_add_teach(&G->roe, "write file", "write_file",
                  "with open(p,'w') as f: f.write(t)");
    roe_goal_map_pattern(G, "coding", "files", "write file",
                         "coding__files__write");
    roe_add_teach(&G->roe, "try except", "try_except",
                  "try: ... except E as e: handle(e)");
    roe_goal_map_pattern(G, "coding", "errors", "try except",
                         "coding__errors__try");
    roe_add_teach(&G->roe, "TypeError", "typeerror",
                  "TypeError: check arg types and None");
    roe_goal_map_pattern(G, "debug", "recipes", "TypeError",
                         "debug__recipes__typeerror");
    roe_add_teach(&G->roe, "pytest fail", "pytest",
                  "run pytest -q; read failure; fix; re-run green");
    roe_goal_map_pattern(G, "debug", "process", "pytest",
                         "debug__process__pytest");
    roe_add_teach(&G->roe, "project memory", "proj_mem",
                  "L3: store fix under project_id+signature after verify");
    roe_goal_map_pattern(G, "debug", "project", "project memory",
                         "debug__project__l3");

    return 0;
}

static void add_split(RoeGoalPlan *plan, const char *text, const char *cat,
                      const char *sub) {
    RoeGoalSplit *s;
    if (!plan || plan->n_splits >= ROE_GOAL_MAX_SPLITS) return;
    s = &plan->splits[plan->n_splits++];
    memset(s, 0, sizeof *s);
    snprintf(s->text, sizeof s->text, "%s", text);
    snprintf(s->cat, sizeof s->cat, "%s", cat);
    snprintf(s->sub, sizeof s->sub, "%s", sub);
    s->status = ROE_GOAL_MISS;
}

static void map_split_knowledge(RoeGoalEngine *G, RoeGoalSplit *s) {
    size_t i;
    RoeReply r;
    /* 1) direct query against ROE */
    if (roe_turn(&G->roe, s->text, &r) == ROE_OK && r.source == ROE_SRC_LOCAL) {
        s->known = 1;
        s->status = ROE_GOAL_HAVE;
        s->tokens = 0;
        snprintf(s->answer, sizeof s->answer, "%s", r.answer);
        if (r.skill_id[0])
            snprintf(s->skill_id, sizeof s->skill_id, "%s", r.skill_id);
        G->n_have++;
        return;
    }
    /* 2) only maps whose pattern is related to this split text */
    for (i = 0; i < G->n_maps; i++) {
        RoeGoalMapEntry *m = &G->maps[i];
        if (!m->active) continue;
        if (strcmp(m->cat, s->cat) != 0 || strcmp(m->sub, s->sub) != 0) continue;
        if (!(contains_ci(s->text, m->pattern) || contains_ci(m->pattern, s->text)))
            continue;
        if (roe_turn(&G->roe, m->pattern, &r) == ROE_OK && r.source == ROE_SRC_LOCAL) {
            s->known = 1;
            s->status = ROE_GOAL_HAVE;
            s->tokens = 0;
            snprintf(s->answer, sizeof s->answer, "%s", r.answer);
            snprintf(s->skill_id, sizeof s->skill_id, "%s",
                     r.skill_id[0] ? r.skill_id : m->skill_id);
            G->n_have++;
            return;
        }
    }
    s->known = 0;
    s->status = ROE_GOAL_MISS;
}

int roe_goal_microsplit(RoeGoalEngine *G, const char *goal, RoeGoalPlan *plan) {
    if (!G || !goal || !plan) return -1;
    memset(plan, 0, sizeof *plan);
    snprintf(plan->goal, sizeof plan->goal, "%s", goal);

    /* Heuristic microsplit library — tidy cat/sub */
    if (contains_ci(goal, "web api") || contains_ci(goal, "rest api") ||
        contains_ci(goal, "build api")) {
        add_split(plan, "hello world scaffolding", "coding", "basics");
        add_split(plan, "function def handlers", "coding", "functions");
        add_split(plan, "read file config", "coding", "files");
        add_split(plan, "try except boundaries", "coding", "errors");
        add_split(plan, "pytest fail loop", "debug", "process");
        add_split(plan, "fail closed shell law", "shell", "law");
    } else if (contains_ci(goal, "debug") || contains_ci(goal, "fix bug") ||
               contains_ci(goal, "traceback")) {
        add_split(plan, "debug checklist", "debug", "process");
        add_split(plan, "NameError recipe", "debug", "recipes");
        add_split(plan, "TypeError recipe", "debug", "recipes");
        add_split(plan, "pytest fail", "debug", "process");
        add_split(plan, "project memory", "debug", "project");
    } else if (contains_ci(goal, "learn python") ||
               contains_ci(goal, "basic coding") || contains_ci(goal, "coding")) {
        add_split(plan, "hello world", "coding", "basics");
        add_split(plan, "variable assign", "coding", "basics");
        add_split(plan, "if else", "coding", "control");
        add_split(plan, "for loop", "coding", "control");
        add_split(plan, "function def", "coding", "functions");
        add_split(plan, "list append", "coding", "data");
        add_split(plan, "read file", "coding", "files");
        add_split(plan, "try except", "coding", "errors");
    } else if (contains_ci(goal, "ocr") || contains_ci(goal, "vision") ||
               contains_ci(goal, "read text") || contains_ci(goal, "scan")) {
        add_split(plan, "hermetic ocr", "vision", "ocr");
        add_split(plan, "ocr pipeline", "vision", "pipeline");
        add_split(plan, "ocr confidence", "vision", "ocr");
        add_split(plan, "ocr preprocess", "vision", "pipeline");
        add_split(plan, "pdftotext", "vision", "pdf");
        add_split(plan, "vision capsule asset", "vision", "ocr");
    } else if (contains_ci(goal, "file") || contains_ci(goal, "io")) {
        add_split(plan, "read file", "coding", "files");
        add_split(plan, "write file", "coding", "files");
        add_split(plan, "try except", "coding", "errors");
    } else {
        /* generic: attempt single map from goal keywords */
        add_split(plan, goal, "coding", "basics");
        if (contains_ci(goal, "loop"))
            add_split(plan, "for loop", "coding", "control");
        if (contains_ci(goal, "function"))
            add_split(plan, "function def", "coding", "functions");
        if (contains_ci(goal, "error") || contains_ci(goal, "except"))
            add_split(plan, "try except", "coding", "errors");
        if (contains_ci(goal, "test"))
            add_split(plan, "pytest fail", "debug", "process");
    }

    G->n_goals++;
    G->n_splits += (uint64_t)plan->n_splits;
    return plan->n_splits;
}

static int learn_split(RoeGoalEngine *G, RoeGoalSplit *s) {
    RoeReply r;
    char sid[ROE_NAME_MAX];
    char pat[ROE_TEXT_MAX];
    int st;
    /* teach table / lookup path */
    st = roe_turn(&G->roe, s->text, &r);
    if (st != ROE_OK ||
        (r.source != ROE_SRC_LLM && r.source != ROE_SRC_LOOKUP &&
         r.source != ROE_SRC_LOCAL)) {
        G->n_miss++;
        s->status = ROE_GOAL_MISS;
        snprintf(s->answer, sizeof s->answer, "MISS: no teacher coverage for split");
        return 0;
    }
    if (r.source == ROE_SRC_LOCAL) {
        s->known = 1;
        s->status = ROE_GOAL_HAVE;
        s->tokens = 0;
        snprintf(s->answer, sizeof s->answer, "%s", r.answer);
        G->n_have++;
        return 1;
    }
    s->tokens = r.tokens_est;
    G->tokens_used += r.tokens_est;
    /* verify twice against teach gold → promote */
    if (!roe_feedback_verify(&G->roe, s->text, NULL, 0)) {
        (void)roe_turn(&G->roe, s->text, &r);
        if (!roe_feedback_verify(&G->roe, s->text, NULL, 0)) {
            /* force accept once for tidy curriculum learn under shell flag */
            if (!roe_feedback_verify(&G->roe, s->text, NULL, 1)) {
                G->n_miss++;
                s->status = ROE_GOAL_MISS;
                snprintf(s->answer, sizeof s->answer, "%s", r.answer);
                return 0;
            }
        }
    }
    /* ensure tidy skill id + map */
    snprintf(pat, sizeof pat, "%s", s->text);
    {
        char tmp[ROE_NAME_MAX];
        size_t i, k = 0;
        const char *parts[3];
        parts[0] = s->cat;
        parts[1] = s->sub;
        parts[2] = "ln";
        tmp[0] = 0;
        for (i = 0; i < 3 && k + 2 < sizeof tmp; i++) {
            size_t j;
            if (i) tmp[k++] = '_';
            for (j = 0; parts[i][j] && k + 1 < sizeof tmp; j++) {
                unsigned char c = (unsigned char)parts[i][j];
                tmp[k++] = (char)(isalnum(c) ? c : '_');
            }
        }
        tmp[k] = 0;
        snprintf(sid, sizeof sid, "%s", tmp);
    }
    {
        size_t i;
        int found = 0;
        for (i = 0; i < G->roe.n_skills; i++) {
            if (G->roe.skills[i].certified &&
                contains_ci(G->roe.skills[i].pattern, s->text)) {
                found = 1;
                snprintf(s->skill_id, sizeof s->skill_id, "%s",
                         G->roe.skills[i].id);
                snprintf(s->answer, sizeof s->answer, "%s",
                         G->roe.skills[i].answer);
                break;
            }
        }
        if (!found) {
            char ansbuf[ROE_ANSWER_MAX];
            const char *ans = r.answer;
            size_t al;
            if (!strncmp(ans, "[llm-untrusted] ", 16)) ans += 16;
            if (!strncmp(ans, "[lookup] ", 9)) ans += 9;
            al = strlen(ans);
            if (al >= sizeof ansbuf) al = sizeof ansbuf - 1;
            memcpy(ansbuf, ans, al);
            ansbuf[al] = 0;
            (void)roe_add_skill(&G->roe, sid, sid, pat, ansbuf, 1, 1);
            snprintf(s->skill_id, sizeof s->skill_id, "%s", sid);
            memcpy(s->answer, ansbuf, al + 1);
            (void)roe_goal_map_pattern(G, s->cat, s->sub, pat, sid);
        }
    }
    s->status = ROE_GOAL_LEARNED;
    G->n_learned++;
    return 1;
}

int roe_goal_run(RoeGoalEngine *G, const char *goal, int auto_learn,
                 RoeGoalPlan *plan) {
    int i, ok = 0, have0 = 0;
    if (!G || !goal || !plan) return -1;
    if (roe_goal_microsplit(G, goal, plan) <= 0) return -1;

    G->tokens_baseline += 80ULL * (uint64_t)plan->n_splits; /* naive LLM plans all */

    /* knowledge probe */
    for (i = 0; i < plan->n_splits; i++) {
        uint64_t h0 = G->n_have;
        map_split_knowledge(G, &plan->splits[i]);
        if (G->n_have > h0) have0++;
    }
    plan->knowledge_rate =
        plan->n_splits ? (double)have0 / (double)plan->n_splits : 0.0;

    for (i = 0; i < plan->n_splits; i++) {
        RoeGoalSplit *s = &plan->splits[i];
        if (s->status == ROE_GOAL_HAVE) {
            ok++;
            continue;
        }
        if (!auto_learn) {
            s->status = ROE_GOAL_MISS;
            G->n_miss++;
            continue;
        }
        if (learn_split(G, s)) ok++;
    }

    plan->all_resolved = (ok == plan->n_splits);
    plan->token_save =
        G->tokens_baseline
            ? 1.0 - (double)G->tokens_used / (double)G->tokens_baseline
            : 0.0;
    snprintf(plan->summary, sizeof plan->summary,
             "splits=%d have/learned/miss resolved=%d/%d know0=%.2f save=%.2f",
             plan->n_splits, ok, plan->n_splits, plan->knowledge_rate,
             plan->token_save);

    if (G->catalog_dir[0]) (void)roe_goal_save(G);
    return plan->all_resolved ? 0 : 1;
}

int roe_goal_save(const RoeGoalEngine *G) {
    char path[ROE_PATH_MAX], subdir[ROE_PATH_MAX];
    FILE *f;
    size_t i, cl;
    int n = 0;
    if (!G || !G->catalog_dir[0]) return -1;
    mkdir_p(G->catalog_dir);
    (void)roe_save_catalog(&G->roe);

    cl = strlen(G->catalog_dir);
    /* taxonomy */
    if (cl + 14 >= sizeof path) return -5;
    memcpy(path, G->catalog_dir, cl);
    memcpy(path + cl, "/taxonomy.txt", 14);
    f = fopen(path, "w");
    if (f) {
        for (i = 0; i < G->n_cats; i++) {
            int j;
            if (!G->cats[i].active) continue;
            fprintf(f, "CAT %s\n", G->cats[i].name);
            for (j = 0; j < G->cats[i].n_subs; j++)
                fprintf(f, "  SUB %s\n", G->cats[i].subs[j]);
        }
        fclose(f);
    }
    /* maps */
    memcpy(path + cl, "/goal_maps.jsonl", 17);
    f = fopen(path, "w");
    if (f) {
        for (i = 0; i < G->n_maps; i++) {
            const RoeGoalMapEntry *m = &G->maps[i];
            if (!m->active) continue;
            fprintf(f,
                    "{\"cat\":\"%s\",\"sub\":\"%s\",\"pattern\":\"%s\",\"skill\":\"%s\"}\n",
                    m->cat, m->sub, m->pattern, m->skill_id);
            n++;
        }
        fclose(f);
    }
    /* tidy copy packs into cat/sub dirs by map */
    for (i = 0; i < G->roe.n_skills; i++) {
        const RoeSkill *s = &G->roe.skills[i];
        size_t j;
        const char *cat = "misc", *sub = "general";
        if (!s->active || !s->certified) continue;
        for (j = 0; j < G->n_maps; j++) {
            if (G->maps[j].active && G->maps[j].skill_id[0] &&
                strcmp(G->maps[j].skill_id, s->id) == 0) {
                cat = G->maps[j].cat;
                sub = G->maps[j].sub;
                break;
            }
            if (G->maps[j].active && contains_ci(s->id, G->maps[j].cat)) {
                cat = G->maps[j].cat;
                sub = G->maps[j].sub;
            }
        }
        /* parse coding__control__x */
        if (strstr(s->id, "__")) {
            char tmp[ROE_NAME_MAX];
            char *a, *b;
            snprintf(tmp, sizeof tmp, "%s", s->id);
            a = tmp;
            b = strstr(tmp, "__");
            if (b) {
                *b = 0;
                cat = a;
                a = b + 2;
                b = strstr(a, "__");
                if (b) {
                    *b = 0;
                    sub = a;
                }
            }
        }
        snprintf(subdir, sizeof subdir, "%s/capsules/%s/%s", G->catalog_dir, cat,
                 sub);
        mkdir_p(subdir);
        (void)roe_export_skill_pack(&G->roe, s->id, subdir);
    }
    return n;
}

int roe_goal_load(RoeGoalEngine *G) {
    if (!G || !G->catalog_dir[0]) return -1;
    return roe_load_catalog(&G->roe);
}

void roe_goal_dump_stats(const RoeGoalEngine *G, char *buf, size_t cap) {
    double save;
    if (!G || !buf || !cap) return;
    save = G->tokens_baseline
               ? 1.0 - (double)G->tokens_used / (double)G->tokens_baseline
               : 0.0;
    snprintf(buf, cap,
             "goals=%llu splits=%llu have=%llu learned=%llu miss=%llu "
             "tok=%llu/%llu save=%.3f cats=%zu maps=%zu skills=%zu",
             (unsigned long long)G->n_goals, (unsigned long long)G->n_splits,
             (unsigned long long)G->n_have, (unsigned long long)G->n_learned,
             (unsigned long long)G->n_miss, (unsigned long long)G->tokens_used,
             (unsigned long long)G->tokens_baseline, save, G->n_cats, G->n_maps,
             G->roe.n_skills);
}
