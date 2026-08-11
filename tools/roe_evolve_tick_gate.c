/* Gate: unattended evolve tick (policy accept, no human button).
 * make roe_evolve_tick → ROE_EVOLVE_TICK_PASS
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
    size_t n;
    int hit = 0;
    if (!f) return 0;
    while ((n = fread(buf, 1, sizeof buf - 1, f)) > 0) {
        buf[n] = 0;
        if (strstr(buf, needle)) {
            hit = 1;
            break;
        }
    }
    fclose(f);
    return hit;
}

int main(void) {
    int rc;
    failures = checks = 0;
    printf("=== ROE evolve tick (unattended) ===\n");

    /* seed demo miss+gold then run tick */
    rc = system("python3 tools/roe_evolve_tick.py --seed-demo 2>&1 | tee logs/roe_evolve_tick.log");
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

    printf("\nchecks=%d failures=%d\n", checks, failures);
    if (failures) {
        printf("ROE_EVOLVE_TICK_FAIL\n");
        return 1;
    }
    printf("ROE_EVOLVE_TICK_PASS\n");
    return 0;
}
