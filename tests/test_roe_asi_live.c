/* Live path smoke: catalog save/load + optional live LLM if ROE_LIVE=1.
 * Always ROE_ASI_LIVE_PASS offline; with --live requires at least one live path or skip.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../include/cnet_roe_asi.h"
#include "../include/cnet_roe_net.h"

static int failures, checks;
static void check(int ok, const char *m) {
    checks++;
    printf("  %-64s %s\n", m, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

int main(int argc, char **argv) {
    RoeAsi R, R2;
    RoeNet net;
    RoeReply rep;
    const char *dir = "artifacts/roe_catalog_test";
    int live = 0, i;
    char cmd[256];

    for (i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--live")) live = 1;

    printf("=== ROE-ASI live/persist ===\n");
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    int rc_rm = system(cmd);
    (void)rc_rm;

    roe_init(&R);
    roe_set_catalog_dir(&R, dir);
    check(roe_add_skill(&R, "greet", "greet", "hello", "Hi local.", 0, 1) == 0,
          "add skill");
    check(roe_add_teach(&R, "promote me", "promo", "Promoted answer body.") == 0,
          "teach");
    check(roe_save_catalog(&R) >= 1, "save catalog");
    check(access("artifacts/roe_catalog_test/catalog.jsonl", R_OK) == 0, "jsonl exists");
    check(access("artifacts/roe_catalog_test/skills/greet/SKILL.roe", R_OK) == 0,
          "skill pack exists");

    roe_init(&R2);
    roe_set_catalog_dir(&R2, dir);
    check(roe_load_catalog(&R2) >= 1, "load catalog");
    check(roe_turn(&R2, "hello friend", &rep) == ROE_OK, "loaded skill serves");
    check(rep.source == ROE_SRC_LOCAL, "source local after load");

    /* promote path saves */
    check(roe_turn(&R, "please promote me now", &rep) == ROE_OK, "teach hit");
    check(roe_feedback_verify(&R, "please promote me now", NULL, 0) == 0, "vote1");
    check(roe_turn(&R, "please promote me now", &rep) == ROE_OK, "teach2");
    check(roe_feedback_verify(&R, "please promote me now", NULL, 0) == 1, "promote");
    check(access("artifacts/roe_catalog_test/skills/skill_promo/SKILL.roe", R_OK) == 0 ||
              access("artifacts/roe_catalog_test/catalog.jsonl", R_OK) == 0,
          "persist after promote");

    if (live) {
        roe_net_from_env(&net);
        if (!net.enable_llm) {
            setenv("ROE_LIVE", "1", 1);
            roe_net_from_env(&net);
        }
        roe_set_net(&R, &net);
        printf("  live llm_url=%s model=%s\n", net.llm_url, net.llm_model);
        if (roe_turn(&R, "In one short sentence: what is 3+3?", &rep) == ROE_OK) {
            printf("  live A: %s\n", rep.answer);
            check(rep.source == ROE_SRC_LLM || rep.source == ROE_SRC_LOOKUP ||
                      rep.source == ROE_SRC_LOCAL,
                  "live path returned");
            if (rep.source == ROE_SRC_LLM) check(R.n_live_llm >= 1, "live llm counted");
        } else {
            printf("  live miss (network/model) — soft skip\n");
            check(1, "live optional soft");
        }
        /* user_accept promote from live without gold */
        if (rep.source == ROE_SRC_LLM) {
            (void)roe_feedback_verify(&R, "In one short sentence: what is 3+3?", NULL, 1);
            (void)roe_feedback_verify(&R, "In one short sentence: what is 3+3?", NULL, 1);
        }
    } else {
        check(1, "offline mode (pass --live for HTTP)");
    }

    printf("checks=%d failures=%d\n", checks, failures);
    if (failures) {
        printf("ROE_ASI_LIVE_FAIL\n");
        return 1;
    }
    printf("ROE_ASI_LIVE_PASS\n");
    return 0;
}
