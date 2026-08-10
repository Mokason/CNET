/* ROE debug L3 CLI
 *   roe_asi_debug_cli turn --project P --tb "traceback..."
 *   roe_asi_debug_cli learn --project P --tb "..." --fix "..." [--note "..."]
 *   roe_asi_debug_cli verify --project P --tb "..." --fix "..." --tests-pass
 */
#include <stdio.h>
#include <string.h>

#include "../include/cnet_roe_debug.h"

static void usage(const char *a0) {
    fprintf(stderr,
            "usage:\n"
            "  %s turn --project ID --tb \"traceback or error\"\n"
            "  %s learn --project ID --tb \"...\" --fix \"...\" [--note \"...\"]\n"
            "  %s verify --project ID --tb \"...\" --fix \"...\" --tests-pass\n"
            "  %s stats\n"
            "catalog default: artifacts/roe_debug_catalog\n",
            a0, a0, a0, a0);
}

int main(int argc, char **argv) {
    RoeDebug D;
    const char *cmd = NULL, *proj = NULL, *tb = NULL, *fix = NULL, *note = NULL;
    const char *cat = "artifacts/roe_debug_catalog";
    int tests_pass = 0, i;
    char stats[800];

    for (i = 1; i < argc; i++) {
        if (!cmd && argv[i][0] != '-') cmd = argv[i];
        else if (!strcmp(argv[i], "--project") && i + 1 < argc) proj = argv[++i];
        else if (!strcmp(argv[i], "--tb") && i + 1 < argc) tb = argv[++i];
        else if (!strcmp(argv[i], "--fix") && i + 1 < argc) fix = argv[++i];
        else if (!strcmp(argv[i], "--note") && i + 1 < argc) note = argv[++i];
        else if (!strcmp(argv[i], "--catalog") && i + 1 < argc) cat = argv[++i];
        else if (!strcmp(argv[i], "--tests-pass")) tests_pass = 1;
        else if (!strcmp(argv[i], "-h")) {
            usage(argv[0]);
            return 0;
        }
    }
    if (!cmd) {
        usage(argv[0]);
        return 2;
    }

    roe_dbg_init(&D);
    roe_dbg_set_catalog(&D, cat);
    roe_dbg_seed_curriculum(&D);
    (void)roe_dbg_load(&D);

    if (!strcmp(cmd, "turn")) {
        RoeDbgReply r;
        if (!proj || !tb) {
            usage(argv[0]);
            return 2;
        }
        roe_dbg_turn(&D, proj, tb, &r);
        printf("level=%d sig=%s tokens=%llu skill=%s\n", r.level, r.sig,
               (unsigned long long)r.tokens_est, r.skill_id);
        printf("A: %s\n", r.answer);
        printf("ROE_DBG_TURN_PASS\n");
        return 0;
    }
    if (!strcmp(cmd, "learn") || !strcmp(cmd, "verify")) {
        int pr;
        if (!proj || !tb || !fix) {
            usage(argv[0]);
            return 2;
        }
        if (!strcmp(cmd, "verify"))
            pr = roe_dbg_verify(&D, proj, tb, fix, tests_pass, tests_pass ? 0 : 1);
        else
            pr = roe_dbg_learn(&D, proj, tb, fix, note);
        printf("promoted=%d\n", pr);
        (void)roe_dbg_save(&D);
        printf("ROE_DBG_LEARN_PASS\n");
        return pr ? 0 : 1;
    }
    if (!strcmp(cmd, "stats")) {
        roe_dbg_dump_stats(&D, stats, sizeof stats);
        printf("%s\nmem=%zu projects=%zu\n", stats, D.n_mem, D.n_projects);
        printf("ROE_DBG_STATS_PASS\n");
        return 0;
    }
    usage(argv[0]);
    return 2;
}
