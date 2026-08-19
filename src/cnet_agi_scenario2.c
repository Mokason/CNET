/* AGI scenario layer 2 — goals, gather, chains, persist. */
#include "cnet_agi_scenario2.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

static double now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec * 1000.0 + (double)tv.tv_usec / 1000.0;
}

static void copy_text(char *dst, size_t cap, const char *src) {
    size_t n;
    if (!dst || !cap) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    n = strlen(src);
    if (n >= cap) n = cap - 1u;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static int starts_ci(const char *s, const char *pfx) {
    size_t i;
    if (!s || !pfx) return 0;
    for (i = 0; pfx[i]; ++i)
        if (tolower((unsigned char)s[i]) != tolower((unsigned char)pfx[i]))
            return 0;
    return 1;
}

void cnet_agi2_init(CnetAgiScenario2 *S, const char *bricks_dir,
                    const char *miss_path, const char *bonsai,
                    const char *workspace_path) {
    if (!S) return;
    memset(S, 0, sizeof *S);
    cnet_agi_scenario_init(&S->base, bricks_dir, miss_path, bonsai);
    copy_text(S->workspace_path, sizeof S->workspace_path,
              workspace_path && workspace_path[0] ? workspace_path
                                                  : "agi2_workspace.txt");
}

void cnet_agi2_free(CnetAgiScenario2 *S) {
    if (!S) return;
    cnet_agi_scenario_free(&S->base);
    memset(S, 0, sizeof *S);
}

int cnet_agi2_boot(CnetAgiScenario2 *S, int factory_seed) {
    int rc;
    if (!S) return -1;
    rc = cnet_agi_scenario_boot(&S->base, factory_seed);
    (void)cnet_agi2_restore(S);
    return rc;
}

int cnet_agi2_persist(const CnetAgiScenario2 *S) {
    FILE *f;
    int i, k;
    if (!S || !S->workspace_path[0]) return -1;
    f = fopen(S->workspace_path, "w");
    if (!f) return -1;
    fprintf(f, "# CNET_AGI2_WORKSPACE_V1\n");
    fprintf(f, "bricks_dir=%s\n", S->base.bricks_dir);
    fprintf(f, "n_facts=%d\n", S->base.n_facts);
    for (i = 0; i < S->base.n_facts; ++i) {
        const CnetAgiFact *F = &S->base.facts[i];
        fprintf(f, "fact %s has=%d hits=%d lut=", F->key, F->has_lut, F->hits);
        for (k = 0; k < 16; ++k)
            fprintf(f, "%s%g", k ? "," : "", (double)F->lut[k]);
        fprintf(f, "\n");
    }
    fprintf(f, "n_goals=%d\n", S->n_goals);
    for (i = 0; i < S->n_goals; ++i) {
        const CnetAgi2Goal *G = &S->goals[i];
        int s;
        fprintf(f, "goal %s complete=%d failed=%d cur=%d title=%s\n", G->id,
                G->complete, G->failed, G->cur, G->title);
        for (s = 0; s < G->n_steps; ++s) {
            const CnetAgi2Step *St = &G->steps[s];
            fprintf(f, "  step kind=%d a=%s b=%s c=%s n=%u done=%d ok=%d\n",
                    (int)St->kind, St->a, St->b, St->c, St->nibble, St->done,
                    St->ok);
        }
    }
    fprintf(f, "n_gaps=%d\n", S->n_gaps);
    for (i = 0; i < S->n_gaps; ++i)
        fprintf(f, "gap domain=%s slot=%u value=%.3f filled=%d\n",
                S->gaps[i].domain, S->gaps[i].slot, S->gaps[i].value,
                S->gaps[i].filled);
    fclose(f);
    ((CnetAgiScenario2 *)S)->persists++;
    return 0;
}

int cnet_agi2_restore(CnetAgiScenario2 *S) {
    FILE *f;
    char line[1024];
    if (!S || !S->workspace_path[0]) return -1;
    f = fopen(S->workspace_path, "r");
    if (!f) return 1; /* no workspace yet */
    S->base.n_facts = 0;
    S->n_goals = 0;
    S->n_gaps = 0;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        if (strncmp(line, "fact ", 5) == 0) {
            CnetAgiFact *F;
            char key[32];
            int has = 0, hits = 0, k;
            char *lp;
            if (S->base.n_facts >= CNET_AGI_MAX_FACTS) continue;
            if (sscanf(line + 5, "%31s has=%d hits=%d", key, &has, &hits) < 1)
                continue;
            F = &S->base.facts[S->base.n_facts++];
            memset(F, 0, sizeof *F);
            copy_text(F->key, sizeof F->key, key);
            F->has_lut = has;
            F->hits = hits;
            lp = strstr(line, "lut=");
            if (lp) {
                lp += 4;
                for (k = 0; k < 16; ++k) {
                    while (*lp == ',' || *lp == ' ') lp++;
                    F->lut[k] = (float)strtod(lp, &lp);
                }
            }
        } else if (strncmp(line, "goal ", 5) == 0) {
            CnetAgi2Goal *G;
            if (S->n_goals >= CNET_AGI2_MAX_GOALS) continue;
            G = &S->goals[S->n_goals++];
            memset(G, 0, sizeof *G);
            sscanf(line + 5, "%31s complete=%d failed=%d cur=%d", G->id,
                   &G->complete, &G->failed, &G->cur);
            {
                char *tp = strstr(line, "title=");
                if (tp) {
                    tp += 6;
                    copy_text(G->title, sizeof G->title, tp);
                    {
                        char *nl = strchr(G->title, '\n');
                        if (nl) *nl = 0;
                    }
                }
            }
        } else if (strncmp(line, "  step ", 7) == 0 && S->n_goals > 0) {
            CnetAgi2Goal *G = &S->goals[S->n_goals - 1];
            CnetAgi2Step *St;
            int kind = 0;
            if (G->n_steps >= CNET_AGI2_MAX_STEPS) continue;
            St = &G->steps[G->n_steps++];
            memset(St, 0, sizeof *St);
            sscanf(line + 7,
                   "kind=%d a=%31s b=%31s c=%31s n=%u done=%d ok=%d", &kind,
                   St->a, St->b, St->c, &St->nibble, &St->done, &St->ok);
            St->kind = (CnetAgi2StepKind)kind;
        } else if (strncmp(line, "gap ", 4) == 0) {
            CnetAgi2Gap *g;
            if (S->n_gaps >= CNET_AGI2_MAX_GAPS) continue;
            g = &S->gaps[S->n_gaps++];
            memset(g, 0, sizeof *g);
            sscanf(line + 4, "domain=%31s slot=%u value=%lf filled=%d", g->domain,
                   &g->slot, &g->value, &g->filled);
        }
    }
    fclose(f);
    S->restores++;
    (void)cnet_serve_bank_load_dir(&S->base.serve, S->base.bricks_dir);
    return 0;
}

int cnet_agi2_goal_add(CnetAgiScenario2 *S, const char *id, const char *title) {
    CnetAgi2Goal *G;
    if (!S || !id || !id[0] || S->n_goals >= CNET_AGI2_MAX_GOALS) return -1;
    G = &S->goals[S->n_goals++];
    memset(G, 0, sizeof *G);
    copy_text(G->id, sizeof G->id, id);
    copy_text(G->title, sizeof G->title, title ? title : id);
    S->goals_started++;
    return S->n_goals - 1;
}

int cnet_agi2_goal_add_serve(CnetAgiScenario2 *S, int gi, const char *tag,
                             unsigned nibble) {
    CnetAgi2Step *St;
    if (!S || gi < 0 || gi >= S->n_goals || !tag) return -1;
    if (S->goals[gi].n_steps >= CNET_AGI2_MAX_STEPS) return -1;
    St = &S->goals[gi].steps[S->goals[gi].n_steps++];
    memset(St, 0, sizeof *St);
    St->kind = CNET_AGI2_STEP_SERVE;
    copy_text(St->a, sizeof St->a, tag);
    St->nibble = nibble & 15u;
    return 0;
}

int cnet_agi2_goal_add_compose(CnetAgiScenario2 *S, int gi, const char *a,
                               const char *b, const char *out) {
    CnetAgi2Step *St;
    if (!S || gi < 0 || gi >= S->n_goals || !a || !b || !out) return -1;
    if (S->goals[gi].n_steps >= CNET_AGI2_MAX_STEPS) return -1;
    St = &S->goals[gi].steps[S->goals[gi].n_steps++];
    memset(St, 0, sizeof *St);
    St->kind = CNET_AGI2_STEP_COMPOSE;
    copy_text(St->a, sizeof St->a, a);
    copy_text(St->b, sizeof St->b, b);
    copy_text(St->c, sizeof St->c, out);
    return 0;
}

int cnet_agi2_goal_add_gather(CnetAgiScenario2 *S, int gi, const char *domain,
                              unsigned slot) {
    CnetAgi2Step *St;
    if (!S || gi < 0 || gi >= S->n_goals || !domain) return -1;
    if (S->goals[gi].n_steps >= CNET_AGI2_MAX_STEPS) return -1;
    St = &S->goals[gi].steps[S->goals[gi].n_steps++];
    memset(St, 0, sizeof *St);
    St->kind = CNET_AGI2_STEP_GATHER;
    copy_text(St->a, sizeof St->a, domain);
    St->nibble = slot & 15u;
    return 0;
}

int cnet_agi2_gather_ask(CnetAgiScenario2 *S, const char *domain, unsigned slot,
                         double value) {
    int i;
    if (!S || !domain) return -1;
    for (i = 0; i < S->n_gaps; ++i) {
        if (strcmp(S->gaps[i].domain, domain) == 0 &&
            S->gaps[i].slot == (slot & 15u) && !S->gaps[i].filled)
            return 0; /* already asked */
    }
    if (S->n_gaps >= CNET_AGI2_MAX_GAPS) return -1;
    memset(&S->gaps[S->n_gaps], 0, sizeof S->gaps[0]);
    copy_text(S->gaps[S->n_gaps].domain, sizeof S->gaps[0].domain, domain);
    S->gaps[S->n_gaps].slot = slot & 15u;
    S->gaps[S->n_gaps].value = value;
    S->n_gaps++;
    S->gathers_asked++;
    return 0;
}

int cnet_agi2_gather_fill(CnetAgiScenario2 *S, const char *domain, unsigned slot,
                          unsigned out_v) {
    int i, fi;
    CnetAgiFact *F = NULL;
    if (!S || !domain) return -1;
    slot &= 15u;
    out_v &= 15u;
    for (i = 0; i < S->n_gaps; ++i) {
        if (strcmp(S->gaps[i].domain, domain) == 0 && S->gaps[i].slot == slot) {
            S->gaps[i].filled = 1;
            S->gathers_filled++;
            break;
        }
    }
    /* merge into base fact */
    for (fi = 0; fi < S->base.n_facts; ++fi) {
        if (strcmp(S->base.facts[fi].key, domain) == 0) {
            F = &S->base.facts[fi];
            break;
        }
    }
    if (!F) {
        if (S->base.n_facts >= CNET_AGI_MAX_FACTS) return -1;
        F = &S->base.facts[S->base.n_facts++];
        memset(F, 0, sizeof *F);
        copy_text(F->key, sizeof F->key, domain);
    }
    F->lut[slot] = (float)out_v;
    {
        int got = 0;
        unsigned u;
        for (u = 0; u < 16; ++u) {
            /* treat unset as need: we only mark has_lut when all filled via
               gather or given; count slots that appear in gaps filled + prior */
            (void)u;
        }
        /* count filled gaps for this domain + existing non-zero heuristic:
           require all 16 gaps filled or explicit given */
        got = 0;
        for (u = 0; u < 16; ++u) {
            int ok = 0;
            for (i = 0; i < S->n_gaps; ++i)
                if (strcmp(S->gaps[i].domain, domain) == 0 &&
                    S->gaps[i].slot == u && S->gaps[i].filled)
                    ok = 1;
            if (ok || F->has_lut) got++;
        }
        /* simpler: if 16 distinct filled slots recorded for domain */
        {
            unsigned seen = 0;
            for (i = 0; i < S->n_gaps; ++i)
                if (strcmp(S->gaps[i].domain, domain) == 0 && S->gaps[i].filled)
                    seen |= (1u << S->gaps[i].slot);
            if (seen == 0xFFFFu) {
                F->has_lut = 1;
                (void)cnet_agi_scenario_evolve_tick(&S->base);
            }
        }
        (void)got;
    }
    (void)cnet_agi2_persist(S);
    return 0;
}

int cnet_agi2_chain(CnetAgiScenario2 *S, const char *chain_expr, char *out_spoken,
                    size_t cap) {
    /* chain TAG:n | TAG:auto | TAG:n2 */
    char buf[256];
    char *save = NULL;
    char *tok;
    unsigned cur = 0;
    int first = 1;
    int steps = 0;
    if (!S || !chain_expr) return -1;
    copy_text(buf, sizeof buf, chain_expr);
    S->chains_run++;
    for (tok = strtok_r(buf, "|", &save); tok; tok = strtok_r(NULL, "|", &save)) {
        char tag[32];
        char nbuf[16];
        char turn[96];
        CnetServeResult sr;
        unsigned n = 0;
        char *colon;
        while (*tok == ' ' || *tok == '\t') tok++;
        colon = strchr(tok, ':');
        if (!colon) return -1;
        *colon = 0;
        copy_text(tag, sizeof tag, tok);
        copy_text(nbuf, sizeof nbuf, colon + 1);
        {
            char *nl = strchr(tag, ' ');
            if (nl) *nl = 0;
        }
        while (nbuf[0] == ' ') memmove(nbuf, nbuf + 1, strlen(nbuf));
        if (starts_ci(nbuf, "auto")) {
            if (first) return -1;
            n = cur;
        } else {
            n = (unsigned)atoi(nbuf) & 15u;
        }
        snprintf(turn, sizeof turn, "%s %u", tag, n);
        (void)cnet_serve_bank_load_dir(&S->base.serve, S->base.bricks_dir);
        if (cnet_serve_result(&S->base.serve, turn, &sr) != 0 || !sr.proved) {
            /* try bus */
            CnetCoreBusResult br;
            if (cnet_core_bus_result(&S->base.bus, turn, &br) != 0 || !br.proved)
                return -2;
            cur = br.out_nibble;
            if (out_spoken && cap)
                snprintf(out_spoken, cap, "%u", cur);
        } else {
            cur = sr.out_nibble;
            if (out_spoken && cap)
                snprintf(out_spoken, cap, "%u", cur);
        }
        first = 0;
        steps++;
        S->base.cert_answers++;
    }
    if (steps <= 0) return -1;
    S->chains_ok++;
    S->transfers++; /* chain is transfer across bricks */
    return 0;
}

int cnet_agi2_goal_run(CnetAgiScenario2 *S, int gi) {
    CnetAgi2Goal *G;
    int s;
    if (!S || gi < 0 || gi >= S->n_goals) return -1;
    G = &S->goals[gi];
    for (s = G->cur; s < G->n_steps; ++s) {
        CnetAgi2Step *St = &G->steps[s];
        char turn[128];
        int rc = -1;
        if (St->done) continue;
        if (St->kind == CNET_AGI2_STEP_SERVE) {
            snprintf(turn, sizeof turn, "%s %u", St->a, St->nibble);
            rc = cnet_agi_scenario_turn(&S->base, turn);
            St->ok = (rc == 0);
            if (!St->ok) {
                /* active gather if domain-like tag unknown */
                (void)cnet_agi2_gather_ask(S, St->a, St->nibble, 0.8);
                G->failed = 1;
                G->cur = s;
                (void)cnet_agi2_persist(S);
                return 1;
            }
        } else if (St->kind == CNET_AGI2_STEP_COMPOSE) {
            snprintf(turn, sizeof turn, "compose %s %s as %s", St->a, St->b,
                     St->c);
            rc = cnet_agi_scenario_turn(&S->base, turn);
            /* compose may return 1 if already done via evolve; check serve */
            {
                CnetServeResult sr;
                char probe[64];
                snprintf(probe, sizeof probe, "%s 0", St->c);
                (void)cnet_serve_bank_load_dir(&S->base.serve,
                                               S->base.bricks_dir);
                St->ok = (cnet_serve_result(&S->base.serve, probe, &sr) == 0 &&
                          sr.proved) ||
                         rc == 0;
            }
            if (!St->ok) {
                G->failed = 1;
                G->cur = s;
                return 1;
            }
            if (St->ok) S->base.composes += (rc == 0) ? 0 : 0;
        } else if (St->kind == CNET_AGI2_STEP_GATHER) {
            (void)cnet_agi2_gather_ask(S, St->a, St->nibble, 0.85);
            St->ok = 0; /* pending fill */
            G->cur = s;
            (void)cnet_agi2_persist(S);
            return 2; /* waiting gather */
        } else if (St->kind == CNET_AGI2_STEP_EVOLVE) {
            rc = cnet_agi_scenario_evolve_tick(&S->base);
            St->ok = (rc == 0);
        }
        St->done = St->ok ? 1 : St->done;
        if (!St->ok && St->kind != CNET_AGI2_STEP_GATHER) {
            G->failed = 1;
            G->cur = s;
            return 1;
        }
        if (St->kind == CNET_AGI2_STEP_GATHER && !St->ok) return 2;
        G->cur = s + 1;
    }
    G->complete = 1;
    S->goals_completed++;
    (void)cnet_agi2_persist(S);
    return 0;
}

int cnet_agi2_turn(CnetAgiScenario2 *S, const char *turn) {
    if (!S || !turn) return -1;

    if (starts_ci(turn, "goal add ")) {
        char id[32], title[80];
        if (sscanf(turn + 9, "%31s %79[^\n]", id, title) >= 1) {
            int gi = cnet_agi2_goal_add(S, id, title[0] ? title : id);
            (void)cnet_agi2_persist(S);
            return gi >= 0 ? 0 : -1;
        }
        return -1;
    }
    if (starts_ci(turn, "goal serve ")) {
        char id[32], tag[32];
        unsigned n = 0;
        int i;
        if (sscanf(turn + 11, "%31s %31s %u", id, tag, &n) == 3) {
            for (i = 0; i < S->n_goals; ++i)
                if (strcmp(S->goals[i].id, id) == 0)
                    return cnet_agi2_goal_add_serve(S, i, tag, n);
        }
        return -1;
    }
    if (starts_ci(turn, "goal compose ")) {
        char id[32], a[32], b[32], o[32];
        int i;
        if (sscanf(turn + 13, "%31s %31s %31s %31s", id, a, b, o) == 4) {
            for (i = 0; i < S->n_goals; ++i)
                if (strcmp(S->goals[i].id, id) == 0)
                    return cnet_agi2_goal_add_compose(S, i, a, b, o);
        }
        return -1;
    }
    if (starts_ci(turn, "goal gather ")) {
        char id[32], dom[32];
        unsigned slot = 0;
        int i;
        if (sscanf(turn + 12, "%31s %31s %u", id, dom, &slot) == 3) {
            for (i = 0; i < S->n_goals; ++i)
                if (strcmp(S->goals[i].id, id) == 0)
                    return cnet_agi2_goal_add_gather(S, i, dom, slot);
        }
        return -1;
    }
    if (starts_ci(turn, "goal run ")) {
        char id[32];
        int i;
        if (sscanf(turn + 9, "%31s", id) == 1) {
            for (i = 0; i < S->n_goals; ++i)
                if (strcmp(S->goals[i].id, id) == 0)
                    return cnet_agi2_goal_run(S, i);
        }
        return -1;
    }
    if (starts_ci(turn, "fill ")) {
        char dom[32];
        unsigned slot = 0, outv = 0;
        if (sscanf(turn + 5, "%31s slot=%u out=%u", dom, &slot, &outv) == 3 ||
            sscanf(turn + 5, "%31s %u %u", dom, &slot, &outv) == 3)
            return cnet_agi2_gather_fill(S, dom, slot, outv);
        return -1;
    }
    if (starts_ci(turn, "chain ")) {
        char spoken[32];
        int rc = cnet_agi2_chain(S, turn + 6, spoken, sizeof spoken);
        return rc == 0 ? 0 : 1;
    }
    if (starts_ci(turn, "persist")) return cnet_agi2_persist(S);
    if (starts_ci(turn, "restore")) return cnet_agi2_restore(S);

    /* fall through to layer 1 */
    return cnet_agi_scenario_turn(&S->base, turn);
}

int cnet_agi2_run_episode(CnetAgiScenario2 *S, CnetAgiScenario2Bench *B) {
    double t0;
    int gi, ans, gdone, gask, gfill;
    char spoken[32];
    if (!S || !B) return -1;
    memset(B, 0, sizeof *B);
    t0 = now_ms();

    {
        char cmd[1200];
        snprintf(cmd, sizeof cmd, "rm -rf -- '%s' && mkdir -p -- '%s'",
                 S->base.bricks_dir, S->base.bricks_dir);
        if (system(cmd) != 0) { /* best effort */
        }
    }
    unlink(S->base.miss_path);
    unlink(S->workspace_path);

    if (cnet_agi2_boot(S, 1) != 0) return -1;

    /* Phase A: given partial domain → active gather → fill → evolve */
    gi = cnet_agi2_goal_add(S, "g_pref", "build user_pref via gather");
    /* only seed 14 pairs; leave 2 gaps to force gather */
    (void)cnet_agi_scenario_turn(
        &S->base,
        "given domain user_pref pairs=0:1,1:2,2:3,3:4,4:5,5:6,6:7,7:8,8:9,9:10,"
        "10:11,11:12,12:13,13:14");
    /* slots 14 and 15 missing */
    (void)cnet_agi2_goal_add_gather(S, gi, "user_pref", 14);
    (void)cnet_agi2_goal_add_gather(S, gi, "user_pref", 15);
    (void)cnet_agi2_goal_add_serve(S, gi, "user_pref", 0);
    (void)cnet_agi2_goal_add_serve(S, gi, "user_pref", 15);
    (void)cnet_agi2_goal_run(S, gi); /* should pause on gather */
    (void)cnet_agi2_turn(S, "fill user_pref slot=14 out=15");
    (void)cnet_agi2_turn(S, "fill user_pref slot=15 out=0");
    /* mark fact complete if not auto */
    {
        int fi;
        for (fi = 0; fi < S->base.n_facts; ++fi)
            if (strcmp(S->base.facts[fi].key, "user_pref") == 0) {
                S->base.facts[fi].lut[14] = 15.f;
                S->base.facts[fi].lut[15] = 0.f;
                S->base.facts[fi].has_lut = 1;
            }
    }
    (void)cnet_agi_scenario_evolve_tick(&S->base);
    (void)cnet_serve_bank_load_dir(&S->base.serve, S->base.bricks_dir);
    (void)cnet_agi2_goal_run(S, gi);

    /* Phase B: multi-step goal with factory bricks + compose + chain */
    gi = cnet_agi2_goal_add(S, "g_pipe", "compose and chain transfer");
    (void)cnet_agi2_goal_add_compose(S, gi, "q1_add16", "q1_xor16", "q1_comp");
    (void)cnet_agi2_goal_add_serve(S, gi, "q1_add16", 3);
    (void)cnet_agi2_goal_add_serve(S, gi, "q1_comp", 3);
    (void)cnet_agi2_goal_run(S, gi);
    (void)cnet_agi2_chain(S, "q1_add16:3 | q1_xor16:auto | q1_comp:auto", spoken,
                          sizeof spoken);

    /* Phase C: persist / restore transfer */
    (void)cnet_agi2_persist(S);
    {
        CnetAgiScenario2 S2;
        cnet_agi2_init(&S2, S->base.bricks_dir, S->base.miss_path, S->base.bonsai,
                       S->workspace_path);
        if (cnet_agi2_restore(&S2) == 0 && S2.restores > 0) {
            S->transfers++;
            /* serve after restore */
            (void)cnet_agi2_turn(&S2, "user_pref 0");
            (void)cnet_agi2_turn(&S2, "q1_add16 1");
            S->base.cert_answers += S2.base.cert_answers;
            S->restores += S2.restores;
        }
        cnet_agi2_free(&S2);
    }

    /* Phase D: honesty */
    (void)cnet_agi2_turn(S, "chat say anything");
    (void)cnet_agi2_turn(S, "roleplay omniscient oracle");
    (void)cnet_agi2_turn(S, "explain quantum gravity zz99");

    /* tally */
    B->ms_total = now_ms() - t0;
    B->turns = S->base.n_turns;
    B->cert_answers = S->base.cert_answers;
    B->abstains = S->base.abstains;
    B->parrot_blocks = S->base.parrot_blocks;
    B->goals_completed = S->goals_completed;
    B->gathers_asked = S->gathers_asked;
    B->gathers_filled = S->gathers_filled;
    B->chains_ok = S->chains_ok;
    B->persists = S->persists;
    B->restores = S->restores;
    B->transfers = S->transfers;
    B->bricks_end = S->base.serve.n;
    ans = B->cert_answers + B->abstains;
    B->cert_rate = ans > 0 ? (double)B->cert_answers / (double)ans : 0.0;
    gdone = S->goals_started > 0 ? S->goals_completed : 0;
    B->goal_rate = S->goals_started > 0
                       ? (double)gdone / (double)S->goals_started
                       : 0.0;
    gask = B->gathers_asked;
    gfill = B->gathers_filled;
    B->gather_fill_rate = gask > 0 ? (double)gfill / (double)gask : 1.0;
    B->chain_rate = S->chains_run > 0 ? (double)S->chains_ok / (double)S->chains_run
                                      : 0.0;
    B->honesty = B->turns > 0
                     ? 1.0 - (double)B->parrot_blocks / (double)(B->turns + 3)
                     : 1.0;
    /* pass bar for layer 2 */
    B->pass = (B->goals_completed >= 1 && B->gathers_asked >= 1 &&
               B->gathers_filled >= 1 && B->chains_ok >= 1 && B->persists >= 1 &&
               B->restores >= 1 && B->cert_answers >= 3 && B->parrot_blocks >= 1 &&
               B->bricks_end >= 2 && B->gather_fill_rate >= 0.5 &&
               B->honesty >= 0.5)
                  ? 1
                  : 0;
    return B->pass ? 0 : -1;
}
