/* Resource governor + compute orchestrator gate.
 * make resource_governor → RESOURCE_GOVERNOR_PASS
 */
#include <stdio.h>
#include "../include/cnet_platform.h"
#include <stdlib.h>
#include <string.h>

#include "../include/resource_governor.h"

static int failures, checks;

static void check(int ok, const char *name) {
    checks++;
    printf("  %-62s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

int main(void) {
    CnetResourceGovernor g;
    CnetGovPolicy p;
    CnetGovUsage u;
    char line[512];
    int i;

    printf("== resource governor + compute orchestrator ==\n");

    check(cnet_gov_policy_validate(NULL) == 0, "NULL policy invalid");
    memset(&p, 0, sizeof p);
    check(cnet_gov_policy_validate(&p) == 0, "zero policy invalid (abi)");
    cnet_gov_policy_deploy_defaults(&p);
    check(cnet_gov_policy_validate(&p), "deploy defaults validate");
    check(p.want_oracle_int8 == 1 && p.want_train_fast == 1 &&
              p.acq_stages == 40 && p.max_closures_per_drain == 4,
          "deploy free-wins + budgeted drain defaults");
    check(p.want_topk_set == 0, "TOPK_SET not silent-default (new goldens)");
    check(p.compute_profile == CNET_GOV_PROFILE_BALANCED,
          "default compute profile balanced");

    check(cnet_gov_open(&g, &p) == CNET_GOV_OK, "governor opens");
    check(cnet_gov_admit(&g, 100, 0) == CNET_GOV_OK, "unlimited RAM admits");
    p.ram_budget_bytes = 1000;
    cnet_gov_close(&g);
    check(cnet_gov_open(&g, &p) == CNET_GOV_OK, "reopen with RAM budget");
    check(cnet_gov_admit(&g, 600, 0) == CNET_GOV_OK, "600 under 1000");
    check(cnet_gov_admit(&g, 500, 0) == CNET_GOV_OVER_BUDGET,
          "500 more refuses fail-closed");
    check(g.refuses == 1 && g.admits == 1, "admit/refuse counters");
    check(cnet_gov_release(&g, 600, 0) == CNET_GOV_OK, "release RAM");
    check(cnet_gov_admit(&g, 500, 0) == CNET_GOV_OK, "after release admits");

    cnet_gov_begin_drain(&g);
    for (i = 0; i < 4; i++)
        check(cnet_gov_note_close(&g) == 1, "close within budget");
    check(cnet_gov_note_close(&g) == 0, "5th close refused by rate limit");

    memset(&u, 0, sizeof u);
    u.teacher_resident = 1;
    u.teacher_idle_for_sec = 299;
    u.phase = CNET_GOV_PHASE_IDLE;
    cnet_gov_report_usage(&g, &u);
    check(cnet_gov_teacher_should_sleep(&g) == 0, "idle under threshold");
    u.teacher_idle_for_sec = 300;
    cnet_gov_report_usage(&g, &u);
    check(cnet_gov_teacher_should_sleep(&g) == 1, "idle at threshold sleeps");

    check(cnet_gov_format(&g, line, sizeof line) > 0 &&
              strstr(line, "gov ") != NULL,
          "format produces log line");

    /* env overlay */
    cnet_setenv("CNET_LANE_MAX_CLOSURES", "2", 1);
    cnet_setenv("CNET_TEACHER_IDLE_SEC", "60", 1);
    cnet_gov_policy_deploy_defaults(&p);
    check(cnet_gov_policy_from_env(&p) == 0, "env overlay");
    check(p.max_closures_per_drain == 2 && p.teacher_idle_sec == 60,
          "env max_closures + idle applied");
    cnet_unsetenv("CNET_LANE_MAX_CLOSURES");
    cnet_unsetenv("CNET_TEACHER_IDLE_SEC");

    cnet_gov_close(&g);

    /* ---- Compute orchestrator ---- */
    printf("-- compute orchestrator --\n");
    check(cnet_gov_orchestrate_boot(&g, "eco") == CNET_GOV_OK, "boot eco");
    check(g.policy.compute_profile == CNET_GOV_PROFILE_ECO, "eco profile set");
    check(g.plan.applied == 1, "compute env applied");
    check(g.plan.dsa_enable == 1 && g.plan.dsa_fraction > 0.f &&
              g.plan.dsa_fraction <= 0.25f,
          "eco DSA sparse fraction");
    check(g.plan.kv_page == 1 && g.plan.kv_hot_pages <= 4, "eco paged HOT");
    check(g.plan.dsa_speed == 1, "eco uses speed DSA profile");
    check(g.plan.min_token_gap_ms > 0, "eco paces tokens");
    check(getenv("CNET_DSA") && getenv("CNET_DSA")[0] == '1',
          "CNET_DSA=1 in env");
    check(getenv("CNET_KV_PAGE") && getenv("CNET_KV_PAGE")[0] == '1',
          "CNET_KV_PAGE=1 in env");
    check(getenv("CNET_SPARSE_KV") != NULL, "CNET_SPARSE_KV set");

    check(g.usage.phase == CNET_GOV_PHASE_IDLE, "boot phase idle");
    check(cnet_gov_begin_generate(&g) == CNET_GOV_OK, "begin generate");
    check(g.usage.phase == CNET_GOV_PHASE_GENERATE, "phase generate");
    check(g.usage.generate_bursts == 1, "burst counted");
    for (i = 0; i < 3; i++)
        check(cnet_gov_between_token(&g) == CNET_GOV_OK, "between token");
    check(g.usage.tokens_generated == 3, "tokens counted");
    check(g.usage.pace_yields >= 1, "eco pace yielded at least once");
    check(cnet_gov_end_generate(&g) == 1, "end generate cool recommended");
    check(g.usage.phase == CNET_GOV_PHASE_IDLE, "phase idle after end");
    check(cnet_gov_should_cool(&g) == 1, "should cool after generate");

    /* balanced: no forced pace */
    cnet_gov_close(&g);
    cnet_unsetenv("CNET_DSA");
    cnet_unsetenv("CNET_SPARSE_KV");
    cnet_unsetenv("CNET_KV_PAGE");
    cnet_unsetenv("CNET_DSA_PROFILE");
    check(cnet_gov_orchestrate_boot(&g, "balanced") == CNET_GOV_OK,
          "boot balanced");
    check(g.plan.dsa_speed == 0, "balanced quality DSA (no floor)");
    check(g.plan.min_token_gap_ms == 0, "balanced no token pace");
    check(g.plan.kv_hot_pages >= 4, "balanced larger HOT");

    /* turbo */
    cnet_gov_close(&g);
    cnet_unsetenv("CNET_DSA");
    cnet_unsetenv("CNET_MTP_K");
    check(cnet_gov_orchestrate_boot(&g, "turbo") == CNET_GOV_OK, "boot turbo");
    check(g.plan.mtp_k >= 2 && g.plan.kv_hot_pages >= 4, "turbo mtp+hot");
    check(g.plan.kv_rehydrate == 1, "turbo may rehydrate COLD");

    /* force overwrite */
    cnet_setenv("CNET_DSA", "0", 1);
    cnet_gov_close(&g);
    cnet_gov_policy_deploy_defaults(&p);
    cnet_gov_policy_set_profile(&p, CNET_GOV_PROFILE_ECO);
    p.force_env = 1;
    check(cnet_gov_open(&g, &p) == CNET_GOV_OK, "open force");
    check(cnet_gov_apply_compute_env(&g) == CNET_GOV_OK, "apply force");
    check(getenv("CNET_DSA") && getenv("CNET_DSA")[0] == '1',
          "force overwrites CNET_DSA");

    check(cnet_gov_format(&g, line, sizeof line) > 0 &&
              strstr(line, "prof=eco") != NULL &&
              strstr(line, "phase=") != NULL,
          "format includes profile+phase");

    /* invalid profile name */
    check(cnet_gov_policy_set_profile_name(&p, "nope") == CNET_GOV_INVALID,
          "bad profile name refused");

    cnet_gov_close(&g);
    cnet_unsetenv("CNET_DSA");
    cnet_unsetenv("CNET_SPARSE_KV");
    cnet_unsetenv("CNET_KV_PAGE");
    cnet_unsetenv("CNET_KV_HOT_PAGES");
    cnet_unsetenv("CNET_MLA_KV");
    cnet_unsetenv("CNET_MTP_K");
    cnet_unsetenv("CNET_DSA_PROFILE");
    cnet_unsetenv("CNET_GOV_FORCE");

    printf("RESOURCE_GOVERNOR_PASS checks=%d failures=%d\n", checks, failures);
    return failures ? 1 : 0;
}
