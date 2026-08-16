/* Gate: roe_explore_tick must never self-CERT; curriculum auto_cert=false only. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;

#define CHECK(c, m)                                                            \
    do {                                                                       \
        int _ok = (c);                                                         \
        printf("  %-56s %s\n", m, _ok ? "PASS" : "FAIL");                      \
        if (!_ok) fails++;                                                     \
    } while (0)

int main(void) {
    int rc;
    printf("=== roe_explore_tick gate ===\n");
    rc = system("python3 tools/roe_explore_tick.py --force --dry-run >/tmp/explore_dry.log 2>&1");
    CHECK(rc == 0, "explore_tick dry-run exit 0");
    CHECK(system("grep -q ROE_EXPLORE_TICK_PASS /tmp/explore_dry.log") == 0,
          "ROE_EXPLORE_TICK_PASS marker");
    CHECK(system("grep -q 'auto_cert=false' /tmp/explore_dry.log") == 0,
          "reports auto_cert=false");
    CHECK(system("grep -q 'pack_personal=never' /tmp/explore_dry.log") == 0,
          "never pack_personal");
    /* live force one tick (may queue 0 if all sandbox fail — still PASS) */
    rc = system("python3 tools/roe_explore_tick.py --force >/tmp/explore_live.log 2>&1");
    CHECK(rc == 0, "explore_tick force exit 0");
    CHECK(system("grep -q ROE_EXPLORE_TICK_PASS /tmp/explore_live.log") == 0,
          "live PASS marker");
    if (system("test -f artifacts/roe_daily_packs/EXPLORE_TICK.json") == 0) {
        CHECK(system("grep -q '\"auto_cert\": false' artifacts/roe_daily_packs/EXPLORE_TICK.json") ==
                  0,
              "report auto_cert false JSON");
        CHECK(system("grep -q '\"writes_pack_personal\": false' "
                     "artifacts/roe_daily_packs/EXPLORE_TICK.json") == 0,
              "report writes_pack_personal false");
    }
    printf("\nfailures=%d\n", fails);
    if (fails) {
        printf("ROE_EXPLORE_TICK_FAIL\n");
        return 1;
    }
    printf("ROE_EXPLORE_TICK_GATE_PASS\n");
    return 0;
}
