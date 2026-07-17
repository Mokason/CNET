/* Resource governor gate — unified budget policy for self-improve deploy.
 * make resource_governor → RESOURCE_GOVERNOR_PASS
 */
#include <stdio.h>
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
    char line[256];
    int i;

    printf("== resource governor ==\n");

    check(cnet_gov_policy_validate(NULL) == 0, "NULL policy invalid");
    memset(&p, 0, sizeof p);
    check(cnet_gov_policy_validate(&p) == 0, "zero policy invalid (abi)");
    cnet_gov_policy_deploy_defaults(&p);
    check(cnet_gov_policy_validate(&p), "deploy defaults validate");
    check(p.want_oracle_int8 == 1 && p.want_train_fast == 1 &&
          p.acq_stages == 40 && p.max_closures_per_drain == 4,
          "deploy free-wins + budgeted drain defaults");
    check(p.want_topk_set == 0, "TOPK_SET not silent-default (new goldens)");

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
    cnet_gov_report_usage(&g, &u);
    check(cnet_gov_teacher_should_sleep(&g) == 0, "idle under threshold");
    u.teacher_idle_for_sec = 300;
    cnet_gov_report_usage(&g, &u);
    check(cnet_gov_teacher_should_sleep(&g) == 1, "idle at threshold sleeps");

    check(cnet_gov_format(&g, line, sizeof line) > 0 &&
          strstr(line, "gov ") != NULL, "format produces log line");

    /* env overlay */
    setenv("CNET_LANE_MAX_CLOSURES", "2", 1);
    setenv("CNET_TEACHER_IDLE_SEC", "60", 1);
    cnet_gov_policy_deploy_defaults(&p);
    check(cnet_gov_policy_from_env(&p) == 0, "env overlay");
    check(p.max_closures_per_drain == 2 && p.teacher_idle_sec == 60,
          "env max_closures + idle applied");
    unsetenv("CNET_LANE_MAX_CLOSURES");
    unsetenv("CNET_TEACHER_IDLE_SEC");

    cnet_gov_close(&g);
    printf("RESOURCE_GOVERNOR_PASS checks=%d\n", checks);
    return failures ? 1 : 0;
}
