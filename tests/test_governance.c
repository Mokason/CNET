/* Governance policy unit gate — no full soul link.
 * make governance → GOVERNANCE_PASS
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../include/cnet_governance.h"

static int fails;
static void check(int ok, const char *n) {
    printf("  %-50s %s\n", n, ok ? "PASS" : "FAIL");
    if (!ok) fails++;
}

int main(void) {
    CnetGovernancePolicy p;
    const char *path = "tmp_gov_policy.env";
    char pin[CNET_GOV_PATH_MAX];
    const char *tiny = "tmp_gov_tiny.cnb";

    remove(path);
    cnet_gov_policy_defaults(&p);
    check(p.version == CNET_GOV_VERSION, "defaults version");
    check(p.max_units == 2000, "defaults max_units");
    check(cnet_gov_policy_save(&p, path) == 0, "save policy");
    memset(&p, 0, sizeof p);
    check(cnet_gov_policy_load(&p, path) == 0, "load policy");
    check(p.max_units == 2000, "loaded max_units");
    check(p.low_rel_floor > 0.5 && p.low_rel_floor < 0.6, "loaded floor");

    /* tiny fake base for pin */
    {
        FILE *f = fopen(tiny, "wb");
        check(f != NULL, "tiny base create");
        if (f) {
            fputs("CNBFAKE", f);
            fclose(f);
        }
    }
    snprintf(p.pin_dir, sizeof p.pin_dir, "tmp_gov_pins");
    p.max_snapshots = 2;
    p.snapshot_on_run = 1;
    pin[0] = '\0';
    check(cnet_gov_pin_snapshot(&p, tiny, pin, sizeof pin) == 0, "pin snapshot");
    check(pin[0] != '\0' && access(pin, R_OK) == 0, "pin file exists");
    {
        char cur[CNET_GOV_PATH_MAX];
        check(cnet_gov_current_pin(&p, cur, sizeof cur) == 0, "CURRENT pin");
        check(strcmp(cur, pin) == 0, "CURRENT matches");
    }
    /* second + third pin then rotate */
    check(cnet_gov_pin_snapshot(&p, tiny, pin, sizeof pin) == 0, "pin2");
    check(cnet_gov_pin_snapshot(&p, tiny, pin, sizeof pin) == 0, "pin3");
    {
        /* restore last pin over tiny2 */
        const char *out = "tmp_gov_restored.cnb";
        check(cnet_gov_restore_pin(pin, out) == 0, "restore pin");
        check(access(out, R_OK) == 0, "restored exists");
        remove(out);
        remove("tmp_gov_restored.cnb.restore.log");
    }

    remove(path);
    remove(tiny);
    /* best-effort cleanup pins */
    system("rm -rf tmp_gov_pins");

    if (fails) {
        printf("GOVERNANCE_FAIL fails=%d\n", fails);
        return 1;
    }
    printf("GOVERNANCE_PASS checks_ok\n");
    return 0;
}
