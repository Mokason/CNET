/* Gate: unattended evolve tick (policy accept, no human button).
 *
 *   make roe_evolve_tick → ROE_EVOLVE_TICK_PASS
 *
 * CNET product path is C only. This gate drives bin/roe_evolve_tick (C) and
 * additionally ASSERTS that no Python remains on the evolve path — the unit
 * ExecStart, the Makefile target, and this gate itself. A regression that
 * reintroduces the retired interpreter entry point fails the build here.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures, checks;

static void check(int ok, const char *m) {
    checks++;
    printf("  %-60s %s\n", m, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static int file_has(const char *path, const char *needle) {
    FILE *f = fopen(path, "r");
    char buf[8192];
    size_t n, keep = strlen(needle);
    int hit = 0;
    char carry[512];
    size_t clen = 0;
    if (!f) return 0;
    if (keep >= sizeof carry) keep = sizeof carry - 1;
    while ((n = fread(buf, 1, sizeof buf - 1, f)) > 0) {
        char win[8192 + 512];
        size_t wn;
        buf[n] = 0;
        memcpy(win, carry, clen);
        memcpy(win + clen, buf, n + 1);
        wn = clen + n;
        if (strstr(win, needle)) { hit = 1; break; }
        clen = (wn < keep) ? wn : keep;          /* carry a boundary window */
        memcpy(carry, win + wn - clen, clen);
    }
    fclose(f);
    return hit;
}

static int file_exists(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    fclose(f);
    return 1;
}

int main(void) {
    int rc;
    failures = checks = 0;
    printf("=== ROE evolve tick (unattended, C engine) ===\n");

    /* Deterministic packs root: the gate must not follow a deploy override. */
    setenv("CNET_PACKS_ROOT", "artifacts/roe_daily_packs", 1);
    unsetenv("CNET_MINIMAL_ROOT");
    unsetenv("CNET_FRONT_DOOR_BIN");
    unsetenv("CNET_BLOCKLIST");

    check(file_exists("bin/roe_evolve_tick"), "C engine bin/roe_evolve_tick built");

    rc = system("./bin/roe_evolve_tick --selftest 2>&1 | tee logs/roe_evolve_tick_selftest.log");
    check(rc == 0, "engine selftest exit 0");
    check(file_has("logs/roe_evolve_tick_selftest.log", "ROE_EVOLVE_SELFTEST_PASS"),
          "ROE_EVOLVE_SELFTEST_PASS marker");
    check(file_has("logs/roe_evolve_tick_selftest.log", "EVOLVE_BLOCKLIST_OK"),
          "blocklist law asserted in C");

    /* seed demo miss+gold then run tick (gold skips reviewer) */
    rc = system("./bin/roe_evolve_tick --seed-demo --no-reviewer 2>&1 | tee logs/roe_evolve_tick.log");
    check(rc == 0, "evolve_tick exit 0");
    check(file_has("logs/roe_evolve_tick.log", "ROE_EVOLVE_TICK_PASS"),
          "ROE_EVOLVE_TICK_PASS marker");
    check(file_has("artifacts/roe_daily_packs/EVOLVE_TICK.json", "gold_file") ||
              file_has("artifacts/roe_daily_packs/EVOLVE_TICK.json", "multi_stable") ||
              file_has("artifacts/roe_daily_packs/EVOLVE_TICK.json", "promoted"),
          "EVOLVE_TICK.json written");
    check(file_has("artifacts/roe_daily_packs/pack_personal/PACK.abi", "pack_personal"),
          "pack_personal exists");
    check(file_has("artifacts/roe_daily_packs/pack_personal/catalog.jsonl", "auto_") ||
              file_has("logs/roe_evolve_tick.log", "promoted="),
          "promoted skill or log count");
    /* policy never requires human accept string */
    check(file_has("artifacts/roe_daily_packs/EVOLVE_TICK.json",
                    "\"human_accept_required\": false") ||
              file_has("artifacts/roe_daily_packs/EVOLVE_TICK.json",
                       "\"human_accept_required\":false"),
          "human_accept_required false");
    check(file_has("artifacts/roe_daily_packs/EVOLVE_TICK.json", "roe_evolve_tick_c"),
          "report stamped by the C engine");

    /* ---- product-path law: no interpreter anywhere on the evolve path ---- */
    {
        char needle[80];
        /* assembled at runtime so this source does not contain the literal */
        snprintf(needle, sizeof needle, "%s3 %s/roe_evolve_tick.py", "python", "tools");
        check(!file_exists("tools/roe_evolve_tick.py"),
              "retired tools/roe_evolve_tick.py absent");
        check(!file_has("tools/roe_evolve_tick_gate.c", needle),
              "gate does not shell out to an interpreter");
        check(!file_has("Makefile", needle), "Makefile evolve target is interpreter-free");
        check(file_exists("tools/roe_evolve_tick.c"), "C engine source present");
        if (file_exists("/home/marble/.config/systemd/user/roe-evolve-tick.service"))
            check(!file_has("/home/marble/.config/systemd/user/roe-evolve-tick.service",
                            "ExecStart=/usr/bin/python"),
                  "systemd ExecStart is not an interpreter");
    }

    printf("\nchecks=%d failures=%d\n", checks, failures);
    if (failures) {
        printf("ROE_EVOLVE_TICK_FAIL\n");
        return 1;
    }
    printf("ROE_EVOLVE_TICK_PASS\n");
    return 0;
}
