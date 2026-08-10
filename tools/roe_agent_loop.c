/* ROE agent loop — highest-leverage non-LLM agent path.
 *
 *   always-on tool law → turn → miss → external gold + shell accept → promote
 *   → warm LOCAL → measure local_hit / teacher_rate
 *
 * Law: never self-CERT without accept/gold votes. OOD abstains.
 *
 * make roe_agent_loop → ROE_AGENT_LOOP_PASS
 *
 *   ./bin/roe_agent_loop              # selftest + KPI json
 *   ./bin/roe_agent_loop soak         # same
 *
 * Hermetic: links roe core only (no libcurl). Teach table = external teacher.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#include <sys/types.h>
#define MKDIR(p) mkdir((p), 0755)
#endif

#include "../include/cnet_roe_asi.h"

/* ---- hermetic net stubs (no live teacher; teach table only) ------------ */
void roe_net_init(RoeNet *N) {
    if (N) memset(N, 0, sizeof *N);
}
void roe_net_from_env(RoeNet *N) {
    if (N) memset(N, 0, sizeof *N);
}
int roe_net_llm(RoeNet *N, const char *prompt, char *answer, size_t answer_cap,
                uint64_t *tokens_est) {
    (void)N;
    (void)prompt;
    if (answer && answer_cap) answer[0] = 0;
    if (tokens_est) *tokens_est = 0;
    return -1;
}
int roe_net_lookup(RoeNet *N, const char *query, char *snippet, size_t cap,
                   uint64_t *tokens_est) {
    (void)N;
    (void)query;
    if (snippet && cap) snippet[0] = 0;
    if (tokens_est) *tokens_est = 0;
    return -1;
}

#define AL_ROOT "artifacts/roe_agent_loop"
#define AL_ALWAYS AL_ROOT "/pack_always_on"
#define AL_PERSONAL AL_ROOT "/pack_personal"
#define AL_REPORT AL_ROOT "/AGENT_LOOP.json"

static int failures, checks;

static void check(int ok, const char *name) {
    checks++;
    printf("  %-58s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static int mkdir_p(const char *path) {
    char tmp[768];
    size_t i, n;
    if (!path || !path[0]) return -1;
    n = strlen(path);
    if (n >= sizeof tmp) return -1;
    memcpy(tmp, path, n + 1);
    for (i = 1; i < n; i++) {
        if (tmp[i] == '/' || tmp[i] == '\\') {
            char c = tmp[i];
            tmp[i] = 0;
            if (MKDIR(tmp) != 0 && errno != EEXIST) return -1;
            tmp[i] = c;
        }
    }
    if (MKDIR(tmp) != 0 && errno != EEXIST) return -1;
    return 0;
}

typedef struct {
    const char *query;
    const char *pattern;
    const char *intent;
    const char *gold;
} AlItem;

/* Dense personal corpus: novel facts (not in always-on). */
static const AlItem CORPUS[] = {
    {"what is conformal abstention in cnet", "conformal abstention",
     "conf_abstain",
     "Accept only singleton conformal sets; else abstain and defer to teacher."},
    {"how does roe promote a skill", "roe promote", "roe_promote",
     "Miss → untrusted teacher → shell verify/accept votes → CERT skill pack."},
    {"prefer terminal tools over guessing files", "prefer terminal tools",
     "tool_prefer_q",
     "Prefer terminal/read_file/search_files/patch; verify with tool output."},
    {"daily pack load policy selective", "daily pack load", "pack_policy",
     "Load always-on trio plus at most one matched domain pack; never monobrain."},
    {"project l3 memory signature fix", "project l3 memory", "l3_mem",
     "L3 maps project_id + error signature to a certified fix after verify."},
};
static const int N_CORPUS = (int)(sizeof CORPUS / sizeof CORPUS[0]);

static RoeAsi *roe_new(void) {
    RoeAsi *R = (RoeAsi *)calloc(1, sizeof(RoeAsi));
    if (R) roe_init(R);
    return R;
}

static void seed_always_on(void) {
    RoeAsi *R = roe_new();
    (void)mkdir_p(AL_ALWAYS);
    (void)mkdir_p(AL_ALWAYS "/skills");
    check(R != NULL, "alloc always-on session");
    if (!R) return;
    roe_set_catalog_dir(R, AL_ALWAYS);
    roe_add_skill(R, "tc_prefer_tools", "tool_prefer", "use tools not guess",
                  "Prefer terminal/read_file/search_files/patch over guessing "
                  "file state; verify with tool output.",
                  0, 1);
    roe_add_skill(R, "self_law", "roe_law", "never self-cert",
                  "Never self-CERT from one unverified LLM reply; shell admits.",
                  0, 1);
    roe_add_skill(R, "self_who", "identity", "who are you",
                  "ROE agent loop: local CERT first; teacher only on miss.", 0,
                  1);
    check(roe_save_catalog(R) >= 3, "seed always-on catalog");
    free(R);
}

static void seed_personal_empty(void) {
    FILE *f;
    (void)mkdir_p(AL_PERSONAL);
    (void)mkdir_p(AL_PERSONAL "/skills");
    f = fopen(AL_PERSONAL "/catalog.jsonl", "w");
    check(f != NULL, "create empty personal catalog");
    if (f) fclose(f);
}

/* load_always: tool-law / identity packs. Omit during promote so
 * roe_save_catalog does not dump always-on skills into personal. */
static void load_session(RoeAsi *R, int with_teach, int load_always) {
    int i;
    roe_init(R);
    if (load_always) {
        roe_set_catalog_dir(R, AL_ALWAYS);
        (void)roe_load_catalog(R);
    }
    roe_set_catalog_dir(R, AL_PERSONAL);
    (void)roe_load_catalog(R);
    if (with_teach) {
        for (i = 0; i < N_CORPUS; i++)
            roe_add_teach(R, CORPUS[i].pattern, CORPUS[i].intent, CORPUS[i].gold);
    }
    /* Promotes must persist into personal, not always-on. */
    roe_set_catalog_dir(R, AL_PERSONAL);
}

static int run_pass(const char *label, int accept_misses, int load_always,
                    int *out_local, int *out_teacher, int *out_promotes) {
    RoeAsi *R = roe_new();
    int i, local = 0, teacher = 0, promotes = 0;
    printf("\n== %s ==\n", label);
    if (!R) {
        check(0, "alloc run_pass session");
        return -1;
    }
    load_session(R, /*with_teach=*/1, load_always);
    for (i = 0; i < N_CORPUS; i++) {
        RoeReply rep;
        int st = roe_turn(R, CORPUS[i].query, &rep);
        (void)st;
        printf("  Q%d src=%s miss=%d | %s\n", i, roe_source_name(rep.source),
               rep.source != ROE_SRC_LOCAL, CORPUS[i].query);
        if (rep.source == ROE_SRC_LOCAL)
            local++;
        else {
            teacher++;
            if (accept_misses) {
                if (roe_feedback_verify(R, CORPUS[i].query, CORPUS[i].gold, 1))
                    promotes++;
            }
        }
    }
    *out_local = local;
    *out_teacher = teacher;
    *out_promotes = promotes;
    printf("  local=%d teacher=%d promotes=%d hit=%.1f%% teacher_rate=%.1f%%\n",
           local, teacher, promotes,
           N_CORPUS ? 100.0 * (double)local / (double)N_CORPUS : 0.0,
           N_CORPUS ? 100.0 * (double)teacher / (double)N_CORPUS : 0.0);
    free(R);
    return 0;
}

static void write_report(int cold_local, int cold_teacher, int promotes,
                         int warm_local, int warm_teacher, int tool_ok,
                         int ood_abstain, int no_accept_ok) {
    FILE *f;
    double cold_hit = N_CORPUS ? (double)cold_local / (double)N_CORPUS : 0.0;
    double cold_tr = N_CORPUS ? (double)cold_teacher / (double)N_CORPUS : 0.0;
    double warm_hit = N_CORPUS ? (double)warm_local / (double)N_CORPUS : 0.0;
    double warm_tr = N_CORPUS ? (double)warm_teacher / (double)N_CORPUS : 0.0;
    (void)mkdir_p(AL_ROOT);
    f = fopen(AL_REPORT, "w");
    if (!f) return;
    fprintf(f,
            "{\n"
            "  \"n_corpus\": %d,\n"
            "  \"cold_local\": %d,\n"
            "  \"cold_teacher\": %d,\n"
            "  \"cold_local_hit\": %.4f,\n"
            "  \"cold_teacher_rate\": %.4f,\n"
            "  \"promotes\": %d,\n"
            "  \"warm_local\": %d,\n"
            "  \"warm_teacher\": %d,\n"
            "  \"warm_local_hit\": %.4f,\n"
            "  \"warm_teacher_rate\": %.4f,\n"
            "  \"kpi_warm_teacher_under_10pct\": %s,\n"
            "  \"kpi_warm_hit_ge_90pct\": %s,\n"
            "  \"tool_law_local\": %s,\n"
            "  \"ood_abstain\": %s,\n"
            "  \"no_accept_no_cert\": %s,\n"
            "  \"never_self_cert\": true,\n"
            "  \"load_policy\": \"always_on+personal\",\n"
            "  \"teacher\": \"teach_table_external_standin\"\n"
            "}\n",
            N_CORPUS, cold_local, cold_teacher, cold_hit, cold_tr, promotes,
            warm_local, warm_teacher, warm_hit, warm_tr,
            warm_tr <= 0.10 + 1e-9 ? "true" : "false",
            warm_hit >= 0.90 - 1e-9 ? "true" : "false",
            tool_ok ? "true" : "false", ood_abstain ? "true" : "false",
            no_accept_ok ? "true" : "false");
    fclose(f);
}

static int selftest(void) {
    int cold_local = 0, cold_teacher = 0, promotes = 0;
    int warm_local = 0, warm_teacher = 0, warm_prom = 0;
    int tool_ok = 0, ood_ok = 0, no_accept_ok = 0;
    RoeAsi *R;
    RoeReply rep;
    int st;

    failures = checks = 0;
    printf("=== ROE agent loop ===\n");
    (void)mkdir_p(AL_ROOT);
    seed_always_on();
    seed_personal_empty();

    /* Tool-law always-on hit (agent behavior without generation). */
    printf("\n== tool law always-on ==\n");
    R = roe_new();
    check(R != NULL, "alloc tool-law session");
    if (!R) return 1;
    load_session(R, 0, 1);
    check(roe_turn(R, "please use tools not guess file state", &rep) == ROE_OK,
          "tool-law turn");
    tool_ok = (rep.source == ROE_SRC_LOCAL);
    check(tool_ok, "tool-law LOCAL");
    free(R);

    /* Cold: teacher then accept-promote into personal (no always-on in R). */
    run_pass("cold (miss → accept → promote)", 1, /*load_always=*/0, &cold_local,
             &cold_teacher, &promotes);
    check(cold_teacher >= N_CORPUS - 1, "cold mostly teacher/miss");
    check(promotes >= N_CORPUS - 1, "promoted corpus into personal");

    /* Warm: always-on + personal; corpus must be LOCAL (residual budget). */
    run_pass("warm (local CERT)", 0, /*load_always=*/1, &warm_local,
             &warm_teacher, &warm_prom);
    check(warm_local >= N_CORPUS - 1, "warm local hit nearly all");
    check(warm_teacher <= 1, "warm teacher_rate <= ~10%");
    check((double)warm_teacher / (double)N_CORPUS <= 0.10 + 1e-9,
          "kpi warm teacher_rate under 10%");
    check((double)warm_local / (double)N_CORPUS >= 0.90 - 1e-9,
          "kpi warm local_hit >= 90%");

    /* OOD abstain — no silent freestyle. */
    printf("\n== OOD abstain ==\n");
    R = roe_new();
    check(R != NULL, "alloc OOD session");
    if (!R) return 1;
    load_session(R, 1, 1);
    st = roe_turn(R, "zzqq totally unknown mystic ooze 99", &rep);
    check(st == ROE_ABSTAIN || st == ROE_OK, "OOD turn returns");
    ood_ok = (rep.source == ROE_SRC_ABSTAIN || rep.source == ROE_SRC_ASK_USER);
    check(ood_ok, "OOD abstain/ask_user");
    check(rep.source != ROE_SRC_LOCAL, "OOD not LOCAL");
    free(R);

    /* No accept → no CERT on a fresh personal (anti self-cert). */
    printf("\n== no accept ⇒ no CERT ==\n");
    seed_personal_empty();
    R = roe_new();
    check(R != NULL, "alloc no-accept session");
    if (!R) return 1;
    load_session(R, 1, 0);
    {
        size_t skills_before = R->n_skills;
        RoeAsi *R2;
        (void)roe_turn(R, CORPUS[0].query, &rep);
        check(rep.source == ROE_SRC_LLM, "teacher answers without CERT");
        check(rep.verified == 0, "untrusted flag");
        R2 = roe_new();
        check(R2 != NULL, "alloc no-accept reload");
        if (R2) {
            load_session(R2, 1, 0);
            check(R2->n_skills == skills_before, "catalog unchanged without accept");
            (void)roe_turn(R2, CORPUS[0].query, &rep);
            no_accept_ok = (rep.source != ROE_SRC_LOCAL);
            check(no_accept_ok, "still miss without accept/promote");
            free(R2);
        }
    }
    free(R);

    write_report(cold_local, cold_teacher, promotes, warm_local, warm_teacher,
                 tool_ok, ood_ok, no_accept_ok);

    printf("\nchecks=%d failures=%d\n", checks, failures);
    printf("report → %s\n", AL_REPORT);
    if (failures) {
        printf("ROE_AGENT_LOOP_FAIL\n");
        return 1;
    }
    printf("ROE_AGENT_LOOP_PASS\n");
    return 0;
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    return selftest();
}
