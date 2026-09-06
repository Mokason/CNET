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

    /* RED marker: GOVERNANCE_PATH_SHELL_INJECTION_RED */
    {
        char root[] = "/tmp/cnet_gov_path_XXXXXX";
        char cwd[CNET_GOV_PATH_MAX] = {0};
        char hostile[CNET_GOV_PATH_MAX], safe_pin[CNET_GOV_PATH_MAX];
        char hostile_restore[CNET_GOV_PATH_MAX];
        char hostile_log[CNET_GOV_PATH_MAX * 2];
        char hostile_snapshot[CNET_GOV_PATH_MAX];
        char current[CNET_GOV_PATH_MAX * 2];
        char pin_dir[CNET_GOV_PATH_MAX];
        char *dir = mkdtemp(root);
        FILE *f;
        check(dir != NULL, "hostile path temp directory");
        check(getcwd(cwd, sizeof cwd) != NULL, "capture governance test cwd");
        if (dir && cwd[0]) {
            snprintf(hostile, sizeof hostile, "%s/base$(touch PWNED).cnb", dir);
            snprintf(safe_pin, sizeof safe_pin, "%s/safe_pin.cnb", dir);
            snprintf(hostile_restore, sizeof hostile_restore,
                     "%s/restore$(touch PWNED_RESTORE).cnb", dir);
            snprintf(hostile_log, sizeof hostile_log, "%s.restore.log",
                     hostile_restore);
            snprintf(pin_dir, sizeof pin_dir, "%s/pins", dir);
            f = fopen(hostile, "wb");
            check(f != NULL, "create hostile-name source");
            if (f) { fputs("HOSTILE_NAME_BYTES", f); fclose(f); }
            f = fopen(safe_pin, "wb");
            check(f != NULL, "create safe restore source");
            if (f) { fputs("RESTORE_BYTES", f); fclose(f); }
            snprintf(p.pin_dir, sizeof p.pin_dir, "%s", pin_dir);
            p.max_snapshots = 2;
            check(chdir(dir) == 0, "enter hostile path test directory");
            remove("PWNED");
            remove("PWNED_RESTORE");
            hostile_snapshot[0] = '\0';
            check(cnet_gov_pin_snapshot(&p, hostile, hostile_snapshot,
                                        sizeof hostile_snapshot) == 0,
                  "snapshot accepts shell metacharacters as data");
            check(access("PWNED", F_OK) != 0,
                  "snapshot path cannot execute shell syntax");
            check(cnet_gov_restore_pin(safe_pin, hostile_restore) == 0,
                  "restore accepts shell metacharacters as data");
            check(access("PWNED_RESTORE", F_OK) != 0,
                  "restore path cannot execute shell syntax");
            remove("PWNED");
            remove("PWNED_RESTORE");
            check(chdir(cwd) == 0, "restore governance test cwd");

            snprintf(current, sizeof current, "%s/CURRENT", pin_dir);
            remove(hostile_snapshot);
            remove(current);
            remove(hostile);
            remove(safe_pin);
            remove(hostile_restore);
            remove(hostile_log);
            rmdir(pin_dir);
            rmdir(dir);
        }
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
