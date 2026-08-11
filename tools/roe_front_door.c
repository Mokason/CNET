/* ROE front door — selective pack load to cut residual-teacher tokens.
 *
 *   ./bin/roe_front_door ask "query" [--root DIR] [--no-always] [--miss-log PATH]
 *   ./bin/roe_front_door bench [--root DIR]
 *   ./bin/roe_front_door route "query"   # print pack match only
 *
 * Flow: ROUTES.jsonl → longest pattern match → load always-on trio (+ domain)
 *       → roe_turn → if not LOCAL, append miss_log.jsonl
 *
 * make roe_front_door → ROE_FRONT_DOOR_PASS
 */
#include <ctype.h>
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "../include/cnet_roe_asi.h"
#include "../include/cnet_domain_route.h"
#include "../include/cnet_probe_shortcircuit.h"

#define FD_ROOT_DEFAULT "artifacts/roe_daily_packs"
#define FD_MAX_ROUTES 256
#define FD_MAX_ALWAYS 8
#define FD_MAX_PACKS_LOAD 8
#define FD_PATH 1024
#define FD_BIAS_PATH_DEFAULT "logs/governor/front_door_bias.json"
#define FD_PAT 256
#define FD_ID 64

typedef struct {
    char pattern[FD_PAT];
    char pack[FD_ID];
    char cat[32];
    char sub[32];
    char skill[FD_ID];
} FdRoute;

typedef struct {
    char root[FD_PATH];
    char miss_log[FD_PATH];
    FdRoute routes[FD_MAX_ROUTES];
    int n_routes;
    char always_on[FD_MAX_ALWAYS][FD_ID];
    int n_always;
    int load_always;
} FdRouter;

static int failures, checks;
static void check(int ok, const char *m) {
    checks++;
    printf("  %-64s %s\n", m, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static int path_join2(char *out, size_t cap, const char *a, const char *b) {
    size_t na, nb;
    if (!out || !cap || !a || !b) return -1;
    na = strlen(a);
    nb = strlen(b);
    if (na + 1 + nb + 1 > cap) return -1;
    memcpy(out, a, na);
    out[na] = '/';
    memcpy(out + na + 1, b, nb);
    out[na + 1 + nb] = 0;
    return 0;
}

static void str_tolower_copy(char *dst, size_t cap, const char *src) {
    size_t i;
    if (!dst || !cap) return;
    for (i = 0; src && src[i] && i + 1 < cap; i++)
        dst[i] = (char)tolower((unsigned char)src[i]);
    dst[i < cap ? i : cap - 1] = 0;
}

static int contains_ci(const char *hay, const char *needle) {
    char h[ROE_TEXT_MAX], n[ROE_TEXT_MAX];
    if (!hay || !needle || !needle[0]) return 0;
    str_tolower_copy(h, sizeof h, hay);
    str_tolower_copy(n, sizeof n, needle);
    return strstr(h, n) != NULL;
}

static int json_get_str(const char *line, const char *key, char *out, size_t cap) {
    char pat[96];
    const char *p;
    size_t klen;
    if (!line || !key || !out || !cap) return -1;
    out[0] = 0;
    klen = strlen(key);
    if (klen + 4 >= sizeof pat) return -1;
    /* "key":"value" compact */
    snprintf(pat, sizeof pat, "\"%s\":\"", key);
    p = strstr(line, pat);
    if (!p) return -1;
    p += strlen(pat);
    {
        size_t i = 0;
        while (p[i] && p[i] != '"' && i + 1 < cap) {
            out[i] = p[i];
            i++;
        }
        out[i] = 0;
    }
    return out[0] ? 0 : -1;
}

static int fd_init(FdRouter *F, const char *root) {
    char path[FD_PATH], line[1024];
    FILE *f;
    if (!F) return -1;
    memset(F, 0, sizeof *F);
    snprintf(F->root, sizeof F->root, "%s", root && root[0] ? root : FD_ROOT_DEFAULT);
    if (path_join2(F->miss_log, sizeof F->miss_log, F->root, "miss_log.jsonl") != 0)
        return -1;
    F->load_always = 1;
    /* default always-on: persona first so identity beats shell boilerplate */
    snprintf(F->always_on[0], sizeof F->always_on[0], "%s", "pack_soul_marble");
    snprintf(F->always_on[1], sizeof F->always_on[1], "%s", "pack_roe_self");
    snprintf(F->always_on[2], sizeof F->always_on[2], "%s", "pack_goal_split");
    snprintf(F->always_on[3], sizeof F->always_on[3], "%s", "pack_toolcall_hermes");
    /* Garden growth — unattended promotes land here */
    snprintf(F->always_on[4], sizeof F->always_on[4], "%s", "pack_personal");
    F->n_always = 5;

    if (path_join2(path, sizeof path, F->root, "ROUTES.jsonl") != 0) return -1;
    f = fopen(path, "r");
    if (!f) return -2;
    while (fgets(line, sizeof line, f) && F->n_routes < FD_MAX_ROUTES) {
        FdRoute *r = &F->routes[F->n_routes];
        memset(r, 0, sizeof *r);
        if (json_get_str(line, "pattern", r->pattern, sizeof r->pattern) != 0)
            continue;
        if (json_get_str(line, "pack", r->pack, sizeof r->pack) != 0) continue;
        (void)json_get_str(line, "cat", r->cat, sizeof r->cat);
        (void)json_get_str(line, "sub", r->sub, sizeof r->sub);
        (void)json_get_str(line, "skill", r->skill, sizeof r->skill);
        F->n_routes++;
    }
    fclose(f);
    return F->n_routes > 0 ? 0 : -3;
}

/* Longest pattern match wins. Returns route index or -1. */
static int fd_match(const FdRouter *F, const char *query, FdRoute *out) {
    int best = -1;
    size_t best_len = 0;
    int i;
    if (!F || !query) return -1;
    for (i = 0; i < F->n_routes; i++) {
        size_t L = strlen(F->routes[i].pattern);
        if (L < 3) continue;
        if (contains_ci(query, F->routes[i].pattern) && L > best_len) {
            best_len = L;
            best = i;
        }
    }
    if (best >= 0 && out) *out = F->routes[best];
    return best;
}

static int pack_already(const char *const *list, int n, const char *id) {
    int i;
    for (i = 0; i < n; i++)
        if (list[i] && id && strcmp(list[i], id) == 0) return 1;
    return 0;
}

static int fd_load_pack(RoeAsi *R, const char *root, const char *pack_id) {
    char path[FD_PATH];
    int n;
    if (!R || !root || !pack_id) return -1;
    if (path_join2(path, sizeof path, root, pack_id) != 0) return -1;
    roe_set_catalog_dir(R, path);
    n = roe_load_catalog(R);
    return n;
}

typedef struct {
    char packs_loaded[FD_MAX_PACKS_LOAD][FD_ID];
    int n_packs;
    int n_skills_loaded;
    char route_pattern[FD_PAT];
    char route_pack[FD_ID];
    char route_skill[FD_ID];
    int matched;
    RoeReply reply;
    int is_miss; /* not LOCAL CERT */
} FdTurnResult;

static int fd_prepare(FdRouter *F, RoeAsi *R, const char *query, FdTurnResult *tr) {
    FdRoute rt;
    const char *plist[FD_MAX_PACKS_LOAD];
    int np = 0, i, nadd;
    if (!F || !R || !query || !tr) return -1;
    memset(tr, 0, sizeof *tr);
    roe_init(R);

    if (F->load_always) {
        for (i = 0; i < F->n_always && np < FD_MAX_PACKS_LOAD; i++) {
            plist[np++] = F->always_on[i];
        }
    }

    if (fd_match(F, query, &rt) >= 0) {
        tr->matched = 1;
        snprintf(tr->route_pattern, sizeof tr->route_pattern, "%s", rt.pattern);
        snprintf(tr->route_pack, sizeof tr->route_pack, "%s", rt.pack);
        snprintf(tr->route_skill, sizeof tr->route_skill, "%s", rt.skill);
        if (!pack_already(plist, np, rt.pack) && np < FD_MAX_PACKS_LOAD)
            plist[np++] = rt.pack;
    }

    /* If nothing matched and always-on disabled, still try always-on once */
    if (np == 0) {
        plist[np++] = "pack_roe_self";
    }

    for (i = 0; i < np; i++) {
        nadd = fd_load_pack(R, F->root, plist[i]);
        if (nadd < 0) nadd = 0;
        snprintf(tr->packs_loaded[tr->n_packs], sizeof tr->packs_loaded[0], "%s",
                 plist[i]);
        tr->n_packs++;
        tr->n_skills_loaded = (int)R->n_skills;
        (void)nadd;
    }
    return tr->n_packs > 0 ? 0 : -1;
}

static void iso_now(char *out, size_t cap) {
    time_t t = time(NULL);
    struct tm *tm = gmtime(&t);
    if (!out || !cap) return;
    if (tm)
        strftime(out, cap, "%Y-%m-%dT%H:%M:%SZ", tm);
    else
        snprintf(out, cap, "unknown");
}

static int fd_append_miss(const FdRouter *F, const char *query, const FdTurnResult *tr,
                          const char *shortcircuit_pat) {
    FILE *f;
    char ts[40];
    char qesc[ROE_TEXT_MAX];
    size_t i, j = 0;
    if (!F || !query || !tr) return -1;
    /* crude escape quotes */
    for (i = 0; query[i] && j + 2 < sizeof qesc; i++) {
        if (query[i] == '"' || query[i] == '\\') qesc[j++] = '\\';
        qesc[j++] = query[i];
    }
    qesc[j] = 0;
    iso_now(ts, sizeof ts);
    f = fopen(F->miss_log, "a");
    if (!f) return -2;
    /* escape answer */
    {
        char aesc[ROE_ANSWER_MAX];
        size_t ai, aj = 0;
        const char *ans = tr->reply.answer;
        for (ai = 0; ans[ai] && aj + 2 < sizeof aesc; ai++) {
            if (ans[ai] == '"' || ans[ai] == '\\') aesc[aj++] = '\\';
            if (ans[ai] == '\n' || ans[ai] == '\r') {
                aesc[aj++] = ' ';
                continue;
            }
            aesc[aj++] = ans[ai];
        }
        aesc[aj] = 0;
        if (shortcircuit_pat && shortcircuit_pat[0]) {
            fprintf(f,
                    "{\"ts\":\"%s\",\"pack_tried\":\"%s\",\"query\":\"%s\","
                    "\"route_pattern\":\"%s\",\"source\":\"%s\",\"tokens_est\":%llu,"
                    "\"skill\":\"%s\",\"n_packs_loaded\":%d,\"verified\":%s,"
                    "\"answer\":\"%s\",\"shortcircuit\":true,\"probe_pat\":\"%s\","
                    "\"teacher\":false}\n",
                    ts, tr->route_pack[0] ? tr->route_pack : "", qesc,
                    tr->route_pattern, tr->reply.source_name,
                    (unsigned long long)tr->reply.tokens_est,
                    tr->reply.skill_id[0] ? tr->reply.skill_id : "", tr->n_packs,
                    tr->reply.verified ? "true" : "false", aesc,
                    shortcircuit_pat);
        } else {
            fprintf(f,
                    "{\"ts\":\"%s\",\"pack_tried\":\"%s\",\"query\":\"%s\","
                    "\"route_pattern\":\"%s\",\"source\":\"%s\",\"tokens_est\":%llu,"
                    "\"skill\":\"%s\",\"n_packs_loaded\":%d,\"verified\":%s,"
                    "\"answer\":\"%s\"}\n",
                    ts, tr->route_pack[0] ? tr->route_pack : "", qesc,
                    tr->route_pattern, tr->reply.source_name,
                    (unsigned long long)tr->reply.tokens_est,
                    tr->reply.skill_id[0] ? tr->reply.skill_id : "", tr->n_packs,
                    tr->reply.verified ? "true" : "false", aesc);
        }
    }
    fclose(f);
    return 0;
}

static int fd_prefer_local_active(double *out_weight) {
    /* DA neuromod brief boost: logs/governor/front_door_bias.json */
    const char *path;
    FILE *f;
    char buf[2048];
    size_t n;
    double expires = 0, weight = 1.0;
    int prefer = 0, disable_teacher = 0;
    time_t now = time(NULL);
    path = getenv("ROE_FRONT_DOOR_BIAS");
    if (!path || !path[0]) path = FD_BIAS_PATH_DEFAULT;
    if (out_weight) *out_weight = 1.0;
    f = fopen(path, "r");
    if (!f) return 0;
    n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    if (n == 0) return 0;
    buf[n] = 0;
    {
        const char *p;
        p = strstr(buf, "\"expires_ts\"");
        if (p) {
            p = strchr(p, ':');
            if (p) expires = strtod(p + 1, NULL);
        }
        p = strstr(buf, "\"prefer_local_weight\"");
        if (p) {
            p = strchr(p, ':');
            if (p) weight = strtod(p + 1, NULL);
        }
        if (strstr(buf, "\"prefer_local\": true") || strstr(buf, "\"prefer_local\":true"))
            prefer = 1;
        if (strstr(buf, "\"disable_live_teacher\": true") ||
            strstr(buf, "\"disable_live_teacher\":true"))
            disable_teacher = 1;
    }
    if (expires > 0 && (double)now > expires) return 0;
    if (!prefer && !disable_teacher) return 0;
    if (out_weight) *out_weight = weight > 1.0 ? weight : 1.5;
    return disable_teacher || prefer ? 1 : 0;
}

static int fd_turn(FdRouter *F, const char *query, FdTurnResult *tr) {
    RoeAsi R;
    RoeNet net;
    static int curl_once;
    static CnetProbeTable PT;
    static int pt_ready;
    double local_w = 1.0;
    int prefer_local;
    char probe_pat[CNET_PROBE_PAT];
    const char *pm = NULL;
    if (fd_prepare(F, &R, query, tr) != 0) return -1;
    if (!curl_once) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        curl_once = 1;
    }
    if (!pt_ready) {
        cnet_probe_table_init(&PT);
        (void)cnet_probe_table_load(&PT, "config/probe_shortcircuit.txt");
        {
            const char *home = getenv("CNET_MINIMAL_ROOT");
            char path[512];
            if (home && home[0]) {
                snprintf(path, sizeof path, "%s/config/probe_shortcircuit.txt", home);
                (void)cnet_probe_table_load(&PT, path);
            }
        }
        pt_ready = 1;
    }
    probe_pat[0] = 0;
    pm = cnet_probe_match(&PT, query, probe_pat, sizeof probe_pat);
    roe_net_from_env(&net);
    prefer_local = fd_prefer_local_active(&local_w);
    /* DA brief boost: prefer LOCAL weight by suppressing live teacher */
    if (prefer_local) {
        net.enable_llm = 0;
        if (getenv("ROE_FD_DEBUG"))
            fprintf(stderr, "fd_debug DA prefer_local weight=%.2f teacher_off\n", local_w);
    }
    /* Probe short-circuit: never burn Teacher on curriculum noise */
    if (pm) {
        net.enable_llm = 0;
        net.enable_lookup = 0;
        printf("probe_shortcircuit=1 pat=%s teacher=off\n", probe_pat[0] ? probe_pat : pm);
    }
    if (net.enable_llm || net.enable_lookup) roe_set_net(&R, &net);
    roe_turn(&R, query, &tr->reply);
    tr->is_miss = (tr->reply.source != ROE_SRC_LOCAL);
    if (tr->is_miss) (void)fd_append_miss(F, query, tr, pm ? probe_pat : NULL);
    return 0;
}

static void print_turn(const FdTurnResult *tr) {
    int i;
    printf("matched=%d route_pack=%s pattern=%s\n", tr->matched,
           tr->route_pack[0] ? tr->route_pack : "-",
           tr->route_pattern[0] ? tr->route_pattern : "-");
    printf("packs_loaded=%d skills=%d [", tr->n_packs, tr->n_skills_loaded);
    for (i = 0; i < tr->n_packs; i++)
        printf("%s%s", i ? "," : "", tr->packs_loaded[i]);
    printf("]\n");
    printf("source=%s skill=%s verified=%d miss=%d tokens=%llu\n",
           tr->reply.source_name,
           tr->reply.skill_id[0] ? tr->reply.skill_id : "-", tr->reply.verified,
           tr->is_miss, (unsigned long long)tr->reply.tokens_est);
    printf("A: %s\n", tr->reply.answer);
    printf("inventory: %s\n", tr->reply.inventory_line);
}

/* Token-free thought + continuity sidecars (Python); best-effort. */
static void fd_emit_thought(const char *q, const FdTurnResult *tr) {
    char cmd[1600];
    const char *py;
    if (!q || !tr) return;
    if (getenv("ROE_NO_THOUGHT") && getenv("ROE_NO_THOUGHT")[0] == '1') return;
    py = getenv("ROE_THOUGHT_PY");
    if (!py || !py[0]) py = "python3 scripts/cnet_thought_process.py";
    if (strchr(q, '\'') || strchr(q, '"')) return;
    snprintf(cmd, sizeof cmd,
             "%s --query '%s' --source '%s' --skill '%s' >/dev/null 2>&1; "
             "python3 scripts/cnet_continuity.py --query '%s' --source '%s' --skill '%s' "
             "--no-thought --line-only 2>/dev/null; "
             "if [ -f logs/governor/thought_last.json ]; then "
             "python3 -c \"import json;d=json.load(open('logs/governor/thought_last.json'));"
             "print('thought:',d.get('chain',''))\" 2>/dev/null; fi",
             py, q, tr->reply.source_name[0] ? tr->reply.source_name : "-",
             tr->reply.skill_id[0] ? tr->reply.skill_id : "-", q,
             tr->reply.source_name[0] ? tr->reply.source_name : "-",
             tr->reply.skill_id[0] ? tr->reply.skill_id : "-");
    if (system(cmd) != 0) {
        /* best-effort */
    }
}

static void strip_untrusted_prefix(char *s) {
    static const char *pfxs[] = {"[llm-untrusted] ", "[llm-live] ", "[lookup] ",
                                 "[lookup-live] ", NULL};
    int i;
    if (!s) return;
    for (i = 0; pfxs[i]; i++) {
        size_t n = strlen(pfxs[i]);
        if (strncmp(s, pfxs[i], n) == 0) {
            memmove(s, s + n, strlen(s + n) + 1);
            return;
        }
    }
}

static int cmd_ask(FdRouter *F, const char *q, int accept, const char *gold,
                   const char *promote_pack) {
    FdTurnResult tr;
    RoeAsi *R;
    RoeNet net;
    static int curl_once;
    static CnetDomainRouter DR;
    static int dr_ready;
    static CnetProbeTable PT;
    static int pt_ready;
    CnetDomainDecision dd;
    char probe_pat[CNET_PROBE_PAT];
    const char *pm = NULL;
    int rc = 0;
    if (!q || !q[0]) return 2;

    /* CERT-first domain dispatch (no malloc on match) */
    if (!dr_ready) {
        cnet_domain_route_init(&DR);
        (void)cnet_domain_route_load_file(&DR, "config/domain_routes.tsv");
        {
            const char *home = getenv("CNET_MINIMAL_ROOT");
            char path[512];
            if (home && home[0]) {
                snprintf(path, sizeof path, "%s/config/domain_routes.tsv", home);
                (void)cnet_domain_route_load_file(&DR, path);
            }
        }
        dr_ready = 1;
    }
    if (!pt_ready) {
        cnet_probe_table_init(&PT);
        (void)cnet_probe_table_load(&PT, "config/probe_shortcircuit.txt");
        {
            const char *home = getenv("CNET_MINIMAL_ROOT");
            char path[512];
            if (home && home[0]) {
                snprintf(path, sizeof path, "%s/config/probe_shortcircuit.txt", home);
                (void)cnet_probe_table_load(&PT, path);
            }
        }
        pt_ready = 1;
    }
    probe_pat[0] = 0;
    pm = cnet_probe_match(&PT, q, probe_pat, sizeof probe_pat);

    cnet_domain_route_resolve(&DR, q, &dd);
    printf("domain_route=%s reason=%s pack=%s mtk=%s conf=%d\n", dd.kind_name,
           dd.reason ? dd.reason : "-",
           dd.pack_or_skill[0] ? dd.pack_or_skill : "-",
           dd.mtk_path[0] ? dd.mtk_path : "-", dd.conf_x1000);
    /* Fail-closed residual: never auto-apply MTK from front door.
     * CERT/ABSTAIN continue into LOCAL packs. BASE_GGUF is advisory only here. */
    if (dd.kind == CNET_ROUTE_MTK) {
        printf("domain_route_note=MTK_selected_but_front_door_stays_CERT_path_"
               "(no_auto_weight_swap)\n");
    }
    if (pm) {
        printf("probe_shortcircuit=1 pat=%s teacher=off\n",
               probe_pat[0] ? probe_pat : pm);
    }

    R = (RoeAsi *)calloc(1, sizeof *R);
    if (!R) return 1;
    if (fd_prepare(F, R, q, &tr) != 0) {
        fprintf(stderr, "front_door prepare failed\n");
        free(R);
        return 1;
    }
    if (!curl_once) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        curl_once = 1;
    }
    roe_net_from_env(&net);
    {
        double local_w = 1.0;
        if (fd_prefer_local_active(&local_w)) {
            net.enable_llm = 0;
            if (getenv("ROE_FD_DEBUG"))
                fprintf(stderr, "fd_debug cmd_ask DA prefer_local w=%.2f\n", local_w);
        }
    }
    if (pm) {
        net.enable_llm = 0;
        net.enable_lookup = 0;
    }
    if (net.enable_llm || net.enable_lookup) roe_set_net(R, &net);
    roe_turn(R, q, &tr.reply);
    tr.is_miss = (tr.reply.source != ROE_SRC_LOCAL);
    if (tr.is_miss) (void)fd_append_miss(F, q, &tr, pm ? probe_pat : NULL);
    print_turn(&tr);
    fd_emit_thought(q, &tr);

    /* Shell accept → promote into domain pack (or --promote-pack).
     * Target pack is loaded alone so save_catalog does not dump always-on. */
    if (accept && tr.is_miss) {
        RoeAsi *P = (RoeAsi *)calloc(1, sizeof *P);
        RoeReply r2;
        char path[FD_PATH];
        char ans[ROE_ANSWER_MAX];
        const char *pack = promote_pack && promote_pack[0] ? promote_pack : NULL;
        const char *use_gold = gold;
        int promoted;
        if (!P) {
            free(R);
            return 1;
        }
        if (!pack && tr.route_pack[0]) pack = tr.route_pack;
        if (!pack) pack = "pack_meta_gardener";
        if (path_join2(path, sizeof path, F->root, pack) != 0) {
            fprintf(stderr, "promote path failed\n");
            free(P);
            free(R);
            return 1;
        }
        snprintf(ans, sizeof ans, "%s", tr.reply.answer);
        strip_untrusted_prefix(ans);
        if (!use_gold || !use_gold[0]) use_gold = ans[0] ? ans : NULL;
        if (!use_gold) {
            fprintf(stderr, "promote needs --gold or a teacher/lookup answer\n");
            free(P);
            free(R);
            return 1;
        }
        roe_init(P);
        roe_set_catalog_dir(P, path);
        (void)roe_load_catalog(P);
        roe_add_teach(P, q, "ad_hoc", use_gold);
        roe_turn(P, q, &r2);
        if (r2.source == ROE_SRC_LOCAL) {
            printf("promote=skip already_LOCAL pack=%s\n", pack);
            free(P);
            free(R);
            return 0;
        }
        promoted = roe_feedback_verify(P, q, use_gold, 1);
        printf("promote=%s pack=%s skills=%zu\n", promoted ? "yes" : "no", pack,
               P->n_skills);
        free(P);
    }
    free(R);
    return rc;
}

static int cmd_route(FdRouter *F, const char *q) {
    FdRoute rt;
    int ix = fd_match(F, q, &rt);
    if (ix < 0) {
        printf("match=none always_on_only=1\n");
        return 0;
    }
    printf("match=1 pack=%s pattern=%s skill=%s cat=%s/%s\n", rt.pack, rt.pattern,
           rt.skill, rt.cat, rt.sub);
    return 0;
}

static int count_all_skills(const char *root) {
    /* sum skills if we loaded every pack_* (upper bound) */
    RoeAsi R;
    char path[FD_PATH];
    const char *packs[] = {
        "pack_toolcall_hermes", "pack_roe_self",      "pack_coding_cnet_c",
        "pack_debug_l3",        "pack_goal_split",    "pack_doc_l3_ocr",
        "pack_git_pr",          "pack_ops_hermes_systemd", "pack_pm_director_oracle",
        "pack_meta_gardener",   "pack_soul_marble",
    };
    size_t i;
    int total = 0;
    for (i = 0; i < sizeof packs / sizeof packs[0]; i++) {
        roe_init(&R);
        if (path_join2(path, sizeof path, root, packs[i]) != 0) continue;
        roe_set_catalog_dir(&R, path);
        total += roe_load_catalog(&R);
    }
    return total;
}

static int cmd_bench(FdRouter *F) {
    const char *qs[] = {
        "who are you",
        "never self-cert from one llm",
        "gcc format-truncation werror snprintf",
        "debug checklist steps",
        "local first ocr router order",
        "pull request body sections",
        "systemctl --user how",
        "CNET proposes Unity disposes",
        "how many certs three numbers",
        "microsplit goal into cat sub",
        "use tools not guess file state",
        "totally unknown zzqq mystic ooze",
    };
    int n = (int)(sizeof qs / sizeof qs[0]);
    int i, local = 0, miss = 0;
    int skills_sum = 0;
    int all_skills;
    uint64_t tok_base = 0, tok_used = 0;
    FILE *bf;

    printf("=== front door bench ===\n");
    all_skills = count_all_skills(F->root);
    printf("all_packs_skills_sum=%d (monobrain upper bound)\n", all_skills);

    for (i = 0; i < n; i++) {
        FdTurnResult tr;
        RoeAsi R;
        if (fd_prepare(F, &R, qs[i], &tr) != 0) {
            check(0, "prepare");
            continue;
        }
        roe_turn(&R, qs[i], &tr.reply);
        tr.is_miss = (tr.reply.source != ROE_SRC_LOCAL);
        if (tr.is_miss) {
            miss++;
            (void)fd_append_miss(F, qs[i], &tr, NULL);
        } else
            local++;
        skills_sum += tr.n_skills_loaded;
        tok_base += 40; /* flat baseline per turn if teacher */
        tok_used += tr.reply.tokens_est;
        printf("  Q%d packs=%d skills=%d src=%s miss=%d | %s\n", i, tr.n_packs,
               tr.n_skills_loaded, tr.reply.source_name, tr.is_miss, qs[i]);
        check(tr.n_skills_loaded > 0, "loaded some skills");
        check(tr.n_skills_loaded < all_skills || all_skills == 0,
              "selective load < all packs");
    }

    {
        double hit = n ? (double)local / (double)n : 0.0;
        double avg_sk = n ? (double)skills_sum / (double)n : 0.0;
        double save =
            tok_base ? 1.0 - (double)tok_used / (double)tok_base : 0.0;
        printf("local=%d miss=%d hit=%.1f%% avg_skills_loaded=%.1f all=%d "
               "tok_used=%llu tok_base=%llu save=%.1f%%\n",
               local, miss, 100.0 * hit, avg_sk, all_skills,
               (unsigned long long)tok_used, (unsigned long long)tok_base,
               100.0 * save);
        check(hit >= 0.50, "front-door hit >= 50% on bench queries");
        check(avg_sk + 1.0 < (double)all_skills, "avg skills << monobrain sum");
        check(local + miss == n, "accounted all queries");

        {
            char path[FD_PATH];
            if (path_join2(path, sizeof path, F->root, "FRONT_BENCH.json") == 0) {
                bf = fopen(path, "w");
                if (bf) {
                    fprintf(bf,
                            "{\n  \"n\": %d,\n  \"local\": %d,\n  \"miss\": %d,\n"
                            "  \"hit\": %.4f,\n  \"avg_skills_loaded\": %.2f,\n"
                            "  \"all_packs_skills_sum\": %d,\n"
                            "  \"tok_used\": %llu,\n  \"tok_base\": %llu,\n"
                            "  \"token_save_proxy\": %.4f,\n"
                            "  \"never_self_cert\": true,\n  \"load_policy\": "
                            "\"always_on+matched_domain\"\n}\n",
                            n, local, miss, hit, avg_sk, all_skills,
                            (unsigned long long)tok_used,
                            (unsigned long long)tok_base, save);
                    fclose(bf);
                }
            }
        }
    }
    return 0;
}

static int cmd_selftest(FdRouter *F) {
    FdRoute rt;
    FdTurnResult tr;
    failures = checks = 0;
    printf("=== ROE front door selftest ===\n");
    check(F->n_routes >= 20, "routes loaded");
    check(fd_match(F, "who are you today", &rt) >= 0, "match who are you");
    check(strcmp(rt.pack, "pack_soul_marble") == 0, "pack_soul_marble");
    check(fd_match(F, "format-truncation werror path join", &rt) >= 0,
          "match coding");
    check(strcmp(rt.pack, "pack_coding_cnet_c") == 0, "pack_coding_cnet_c");
    check(fd_match(F, "local first ocr router", &rt) >= 0, "match ocr");
    check(strcmp(rt.pack, "pack_doc_l3_ocr") == 0, "pack_doc_l3_ocr");

    check(fd_turn(F, "who are you", &tr) == 0, "turn identity");
    check(tr.reply.source == ROE_SRC_LOCAL, "LOCAL hit");
    check(strstr(tr.reply.answer, "Marble") != NULL, "answers as Marble");
    check(tr.n_packs >= 1 && tr.n_packs <= 5, "few packs loaded");
    check(tr.n_skills_loaded < 50, "skills footprint bounded");

    check(fd_turn(F, "zz unknown mystic ooze 99", &tr) == 0, "turn OOD");
    check(tr.is_miss == 1, "OOD is miss");
    {
        FILE *f = fopen(F->miss_log, "r");
        check(f != NULL, "miss_log created");
        if (f) fclose(f);
    }

    cmd_bench(F);

    printf("\nchecks=%d failures=%d\n", checks, failures);
    if (failures) {
        printf("ROE_FRONT_DOOR_FAIL\n");
        return 1;
    }
    printf("ROE_FRONT_DOOR_PASS\n");
    return 0;
}

static void usage(const char *a0) {
    fprintf(stderr,
            "usage:\n"
            "  %s ask \"query\" [--root DIR] [--no-always]\n"
            "      [--accept] [--gold TEXT] [--promote-pack PACK]\n"
            "  %s route \"query\" [--root DIR]\n"
            "  %s bench [--root DIR]\n"
            "  %s selftest [--root DIR]\n",
            a0, a0, a0, a0);
}

int main(int argc, char **argv) {
    const char *cmd = NULL;
    const char *q = NULL;
    const char *root = FD_ROOT_DEFAULT;
    const char *gold = NULL;
    const char *promote_pack = NULL;
    int no_always = 0, accept = 0, i;
    FdRouter F;

    for (i = 1; i < argc; i++) {
        if (!cmd && argv[i][0] != '-')
            cmd = argv[i];
        else if (!strcmp(argv[i], "--root") && i + 1 < argc)
            root = argv[++i];
        else if (!strcmp(argv[i], "--no-always"))
            no_always = 1;
        else if (!strcmp(argv[i], "--accept"))
            accept = 1;
        else if (!strcmp(argv[i], "--gold") && i + 1 < argc)
            gold = argv[++i];
        else if (!strcmp(argv[i], "--promote-pack") && i + 1 < argc)
            promote_pack = argv[++i];
        else if (cmd && argv[i][0] != '-' && !q &&
                 (!strcmp(cmd, "ask") || !strcmp(cmd, "route")))
            q = argv[i];
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(argv[0]);
            return 0;
        }
    }
    if (!cmd) {
        usage(argv[0]);
        return 2;
    }

    if (fd_init(&F, root) != 0) {
        fprintf(stderr, "fd_init failed root=%s (run make roe_daily_packs)\n", root);
        return 1;
    }
    if (no_always) F.load_always = 0;

    if (!strcmp(cmd, "ask"))
        return cmd_ask(&F, q, accept, gold, promote_pack);
    if (!strcmp(cmd, "route")) return cmd_route(&F, q);
    if (!strcmp(cmd, "bench")) {
        cmd_bench(&F);
        printf("ROE_FRONT_DOOR_BENCH_OK\n");
        return 0;
    }
    if (!strcmp(cmd, "selftest")) return cmd_selftest(&F);

    usage(argv[0]);
    return 2;
}
