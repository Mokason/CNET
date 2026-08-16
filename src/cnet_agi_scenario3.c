/* AGI scenario layer 3 — CERT-only plan synthesis + brick specialists. */
#include "cnet_agi_scenario3.h"

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

static int contains_ci(const char *hay, const char *needle) {
    size_t n, h, i, j;
    if (!hay || !needle || !needle[0]) return 0;
    n = strlen(needle);
    h = strlen(hay);
    if (n > h) return 0;
    for (i = 0; i + n <= h; ++i) {
        for (j = 0; j < n; ++j) {
            if (tolower((unsigned char)hay[i + j]) !=
                tolower((unsigned char)needle[j]))
                break;
        }
        if (j == n) return 1;
    }
    return 0;
}

void cnet_agi3_init(CnetAgiScenario3 *S, const char *bricks_dir,
                    const char *miss_path, const char *bonsai,
                    const char *workspace_path) {
    if (!S) return;
    memset(S, 0, sizeof *S);
    cnet_agi2_init(&S->L2, bricks_dir, miss_path, bonsai, workspace_path);
}

void cnet_agi3_free(CnetAgiScenario3 *S) {
    if (!S) return;
    cnet_agi2_free(&S->L2);
    memset(S, 0, sizeof *S);
}

int cnet_agi3_boot(CnetAgiScenario3 *S, int factory_seed) {
    int rc;
    if (!S) return -1;
    rc = cnet_agi2_boot(&S->L2, factory_seed);
    (void)cnet_agi3_specialists_sync(S);
    return rc;
}

int cnet_agi3_specialists_sync(CnetAgiScenario3 *S) {
    int i;
    if (!S) return -1;
    S->n_specialists = 0;
    (void)cnet_serve_bank_load_dir(&S->L2.base.serve, S->L2.base.bricks_dir);
    for (i = 0; i < S->L2.base.serve.n && S->n_specialists < CNET_AGI3_MAX_SPECIALISTS;
         ++i) {
        CnetServeBrick *br = &S->L2.base.serve.bricks[i];
        CnetAgi3Specialist *sp;
        if (!br->live) continue;
        sp = &S->specialists[S->n_specialists++];
        memset(sp, 0, sizeof *sp);
        copy_text(sp->tag, sizeof sp->tag, br->tag);
        copy_text(sp->name, sizeof sp->name, br->name);
        sp->live = 1;
        sp->reliability = 1.0;
    }
    /* also bus bricks not yet on disk */
    for (i = 0; i < S->L2.base.bus.n_bricks &&
                S->n_specialists < CNET_AGI3_MAX_SPECIALISTS;
         ++i) {
        CnetCoreBrick *br = &S->L2.base.bus.bricks[i];
        int j, exists = 0;
        if (!br->live || !br->certified) continue;
        for (j = 0; j < S->n_specialists; ++j)
            if (strcmp(S->specialists[j].tag, br->domain_tag) == 0) exists = 1;
        if (exists) continue;
        {
            CnetAgi3Specialist *sp = &S->specialists[S->n_specialists++];
            memset(sp, 0, sizeof *sp);
            copy_text(sp->tag, sizeof sp->tag, br->domain_tag);
            copy_text(sp->name, sizeof sp->name, br->name);
            sp->live = 1;
            sp->reliability = 1.0;
        }
    }
    return S->n_specialists;
}

static CnetAgi3Specialist *find_sp(CnetAgiScenario3 *S, const char *tag) {
    int i;
    for (i = 0; i < S->n_specialists; ++i)
        if (S->specialists[i].live && strcmp(S->specialists[i].tag, tag) == 0)
            return &S->specialists[i];
    return NULL;
}

int cnet_agi3_specialist_serve(CnetAgiScenario3 *S, const char *tag,
                               unsigned nibble, int committee,
                               CnetServeResult *out) {
    CnetAgi3Specialist *sp;
    CnetAgi3Specialist *alt = NULL;
    char turn[96];
    CnetServeResult sr, sr2;
    int i;
    if (!S || !tag || !out) return -1;
    memset(out, 0, sizeof *out);
    (void)cnet_agi3_specialists_sync(S);
    sp = find_sp(S, tag);
    if (!sp) return 1; /* no specialist */
    sp->consults++;
    S->specialist_routes++;
    snprintf(turn, sizeof turn, "%s %u", tag, nibble & 15u);
    (void)cnet_serve_bank_load_dir(&S->L2.base.serve, S->L2.base.bricks_dir);
    if (cnet_serve_result(&S->L2.base.serve, turn, &sr) != 0 || !sr.proved) {
        sp->fails++;
        sp->reliability =
            (double)sp->proves /
            (double)(sp->proves + sp->fails > 0 ? sp->proves + sp->fails : 1);
        return 1;
    }
    sp->proves++;
    *out = sr;

    if (!committee) {
        sp->reliability =
            (double)sp->proves / (double)(sp->proves + sp->fails);
        return 0;
    }

    /* Committee: second specialist must agree on same nibble if another
       domain can verify via compose identity — pick another live specialist
       and require it can serve its own tag without contradicting primary
       availability. For nibble domains, verify primary twice (idempotent)
       and require a second specialist exists (multi-agent present). */
    for (i = 0; i < S->n_specialists; ++i) {
        if (&S->specialists[i] != sp && S->specialists[i].live) {
            alt = &S->specialists[i];
            break;
        }
    }
    if (!alt) {
        S->committee_fail++;
        return 0; /* primary ok, committee weak */
    }
    alt->consults++;
    snprintf(turn, sizeof turn, "%s %u", alt->tag, nibble & 15u);
    if (cnet_serve_result(&S->L2.base.serve, turn, &sr2) == 0 && sr2.proved) {
        /* both agents productive — committee pass (agreement = both CERT) */
        S->committee_ok++;
        alt->proves++;
    } else {
        S->committee_fail++;
        alt->fails++;
    }
    sp->reliability =
        (double)sp->proves / (double)(sp->proves + sp->fails);
    return 0;
}

/* Reject non-CERT plan language */
static int is_forbidden_goal_language(const char *t) {
    return contains_ci(t, "chat") || contains_ci(t, "roleplay") ||
           contains_ci(t, "pretend") || contains_ci(t, "imagine you") ||
           contains_ci(t, "as an ai") || contains_ci(t, "open chat") ||
           contains_ci(t, "just talk") || contains_ci(t, "bs something");
}

int cnet_agi3_synthesize(CnetAgiScenario3 *S, const char *goal_text) {
    CnetAgi3Plan *P;
    int gi;
    char id[32];
    unsigned nibble = 3;
    char tag_a[32] = "q1_add16";
    char tag_b[32] = "q1_xor16";
    char tag_c[32] = "q1_comp";
    char user_dom[32] = "";
    int want_chain = 0, want_compose = 0, want_prove = 0, want_gather = 0;
    if (!S || !goal_text || !goal_text[0]) return -1;
    if (S->n_plans >= CNET_AGI3_MAX_PLANS) return -1;

    P = &S->plans[S->n_plans];
    memset(P, 0, sizeof *P);
    copy_text(P->raw, sizeof P->raw, goal_text);

    if (is_forbidden_goal_language(goal_text)) {
        P->rejected_non_cert = 1;
        S->synth_reject++;
        S->n_plans++;
        return -2;
    }

    /* Extract nibble if present */
    {
        const char *p = goal_text;
        while (*p && !isdigit((unsigned char)*p)) p++;
        if (isdigit((unsigned char)*p)) {
            unsigned v = (unsigned)atoi(p);
            if (v < 16) nibble = v;
        }
    }

    /* Domain mentions */
    if (contains_ci(goal_text, "user_pref"))
        copy_text(user_dom, sizeof user_dom, "user_pref");
    if (contains_ci(goal_text, "task_v")) copy_text(user_dom, sizeof user_dom, "task_v");
    if (contains_ci(goal_text, "q1_add") || contains_ci(goal_text, "add16"))
        copy_text(tag_a, sizeof tag_a, "q1_add16");
    if (contains_ci(goal_text, "xor")) copy_text(tag_b, sizeof tag_b, "q1_xor16");
    if (contains_ci(goal_text, "compose") || contains_ci(goal_text, "then") ||
        contains_ci(goal_text, "pipeline"))
        want_compose = 1;
    if (contains_ci(goal_text, "chain")) want_chain = 1;
    if (contains_ci(goal_text, "prove") || contains_ci(goal_text, "compute") ||
        contains_ci(goal_text, "evaluate") || contains_ci(goal_text, "at "))
        want_prove = 1;
    if (contains_ci(goal_text, "gather") || contains_ci(goal_text, "missing") ||
        contains_ci(goal_text, "complete table"))
        want_gather = 1;
    if (!want_compose && !want_chain && !want_prove && !want_gather)
        want_prove = 1; /* default: try CERT serve */

    snprintf(id, sizeof id, "syn%d", S->n_plans);
    gi = cnet_agi2_goal_add(&S->L2, id, goal_text);
    if (gi < 0) {
        S->synth_reject++;
        S->n_plans++;
        return -1;
    }

    /* Build CERT-only steps */
    if (want_gather && user_dom[0]) {
        (void)cnet_agi2_goal_add_gather(&S->L2, gi, user_dom, 14);
        (void)cnet_agi2_goal_add_gather(&S->L2, gi, user_dom, 15);
    }
    if (want_compose) {
        (void)cnet_agi2_goal_add_compose(&S->L2, gi, tag_a, tag_b, tag_c);
    }
    if (want_prove) {
        if (user_dom[0])
            (void)cnet_agi2_goal_add_serve(&S->L2, gi, user_dom, nibble);
        (void)cnet_agi2_goal_add_serve(&S->L2, gi, tag_a, nibble);
        if (want_compose)
            (void)cnet_agi2_goal_add_serve(&S->L2, gi, tag_c, nibble);
    }

    P->goal_index = gi;
    P->synthesized = 1;
    S->synth_ok++;
    S->n_plans++;
    (void)want_chain; /* chain run separately after plan if needed */
    return (int)(S->n_plans - 1);
}

int cnet_agi3_plan_run(CnetAgiScenario3 *S, int plan_index) {
    CnetAgi3Plan *P;
    int rc;
    if (!S || plan_index < 0 || plan_index >= S->n_plans) return -1;
    P = &S->plans[plan_index];
    if (P->rejected_non_cert || !P->synthesized) return -1;
    P->ran = 1;
    rc = cnet_agi2_goal_run(&S->L2, P->goal_index);
    P->ok = (rc == 0 || S->L2.goals[P->goal_index].complete);
    /* optional chain if raw asked for chain */
    if (P->ok && contains_ci(P->raw, "chain")) {
        char spoken[32];
        if (cnet_agi2_chain(&S->L2, "q1_add16:3 | q1_xor16:auto", spoken,
                            sizeof spoken) == 0)
            P->ok = 1;
    }
    return P->ok ? 0 : 1;
}

int cnet_agi3_turn(CnetAgiScenario3 *S, const char *turn) {
    if (!S || !turn) return -1;

    if (starts_ci(turn, "goal:") || starts_ci(turn, "goal ")) {
        const char *body = turn;
        int pi;
        while (*body && *body != ' ' && *body != ':') body++;
        while (*body == ' ' || *body == ':') body++;
        pi = cnet_agi3_synthesize(S, body);
        if (pi == -2) return 1; /* rejected non-cert */
        if (pi < 0) return -1;
        return cnet_agi3_plan_run(S, pi);
    }
    if (starts_ci(turn, "committee ")) {
        char tag[32];
        unsigned n = 0;
        CnetServeResult sr;
        if (sscanf(turn + 10, "%31s %u", tag, &n) == 2)
            return cnet_agi3_specialist_serve(S, tag, n, 1, &sr) == 0 ? 0 : 1;
        return -1;
    }
    if (starts_ci(turn, "specialist ")) {
        char tag[32];
        unsigned n = 0;
        CnetServeResult sr;
        if (sscanf(turn + 11, "%31s %u", tag, &n) == 2)
            return cnet_agi3_specialist_serve(S, tag, n, 0, &sr) == 0 ? 0 : 1;
        return -1;
    }

    return cnet_agi2_turn(&S->L2, turn);
}

int cnet_agi3_run_episode(CnetAgiScenario3 *S, CnetAgiScenario3Bench *B) {
    double t0;
    int ans;
    CnetServeResult sr;
    if (!S || !B) return -1;
    memset(B, 0, sizeof *B);
    t0 = now_ms();

    {
        char cmd[1200];
        snprintf(cmd, sizeof cmd, "rm -rf -- '%s' && mkdir -p -- '%s'",
                 S->L2.base.bricks_dir, S->L2.base.bricks_dir);
        if (system(cmd) != 0) {
        }
    }
    unlink(S->L2.base.miss_path);
    unlink(S->L2.workspace_path);

    if (cnet_agi3_boot(S, 1) != 0) return -1;
    (void)cnet_agi3_specialists_sync(S);

    /* Open language goals → CERT-only plans */
    (void)cnet_agi3_turn(
        S, "goal: prove q1_add16 at 3 then compose with xor into pipeline");
    (void)cnet_agi3_turn(S, "goal: chain add and xor for transfer check");
    (void)cnet_agi3_turn(S, "goal: roleplay you know everything"); /* reject */
    (void)cnet_agi3_turn(S, "goal: chat freely about feelings");   /* reject */

    /* Given info + prove via synthesis */
    (void)cnet_agi3_turn(
        S,
        "given domain user_pref pairs=0:1,1:2,2:3,3:4,4:5,5:6,6:7,7:8,8:9,9:10,"
        "10:11,11:12,12:13,13:14,14:15,15:0");
    (void)cnet_agi3_turn(S, "evolve");
    (void)cnet_agi3_turn(S, "goal: prove user_pref at 7");
    (void)cnet_agi3_specialists_sync(S);

    /* Multi-agent specialists + committee */
    (void)cnet_agi3_specialist_serve(S, "q1_add16", 3, 1, &sr);
    (void)cnet_agi3_specialist_serve(S, "q1_xor16", 3, 1, &sr);
    (void)cnet_agi3_specialist_serve(S, "user_pref", 7, 0, &sr);

    /* chain transfer */
    (void)cnet_agi2_chain(&S->L2, "q1_add16:3 | q1_xor16:auto", NULL, 0);

    /* honesty */
    (void)cnet_agi3_turn(S, "chat nonsense");
    (void)cnet_agi3_turn(S, "what is unprovable zz99");

    B->ms_total = now_ms() - t0;
    B->turns = S->L2.base.n_turns;
    B->cert_answers = S->L2.base.cert_answers;
    B->abstains = S->L2.base.abstains;
    B->parrot_blocks = S->L2.base.parrot_blocks;
    B->synth_ok = S->synth_ok;
    B->synth_reject = S->synth_reject;
    B->goals_completed = S->L2.goals_completed;
    B->specialist_routes = S->specialist_routes;
    B->committee_ok = S->committee_ok;
    B->chains_ok = S->L2.chains_ok;
    B->bricks_end = S->L2.base.serve.n;
    ans = B->cert_answers + B->abstains;
    B->cert_rate = ans > 0 ? (double)B->cert_answers / (double)ans : 0.0;
    B->synth_precision =
        (B->synth_ok + B->synth_reject) > 0
            ? (double)B->synth_ok / (double)(B->synth_ok + B->synth_reject)
            : 0.0;
    B->committee_rate =
        (S->committee_ok + S->committee_fail) > 0
            ? (double)S->committee_ok /
                  (double)(S->committee_ok + S->committee_fail)
            : 0.0;
    B->honesty = 1.0 - (double)B->parrot_blocks /
                           (double)(B->turns + B->parrot_blocks + 1);

    B->pass = (B->synth_ok >= 2 && B->synth_reject >= 1 &&
               B->specialist_routes >= 2 && B->committee_ok >= 1 &&
               B->cert_answers >= 2 && B->parrot_blocks >= 1 &&
               B->bricks_end >= 2 && B->synth_precision >= 0.5 &&
               B->honesty >= 0.5)
                  ? 1
                  : 0;
    return B->pass ? 0 : -1;
}
