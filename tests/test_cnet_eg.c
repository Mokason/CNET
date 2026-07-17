/* Local EG hill-climb metric gate.
 * make eg → EG_PASS
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "../include/cnet_eg.h"

static int failures, checks;

static void check(int ok, const char *name) {
    checks++;
    printf("  %-62s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

int main(void) {
    CnetEgSample s;
    CnetEgResult r;
    char buf[512];
    const char *path = "tmp_eg_hill.jsonl";

    printf("== cnet_eg (local hill-climb) ==\n");
    remove(path);

    memset(&s, 0, sizeof s);
    s.teacher_work = 64;
    s.seals = 2;
    s.hours = 1.0;
    cnet_eg_compute(&s, 32.0, &r);
    check(r.ok && fabs(r.cost_per_seal - 32.0) < 1e-9, "cost_per_seal = 64/2");
    check(fabs(r.local_eg - 1.0) < 1e-9, "local_eg = 1 at baseline");
    check(fabs(r.seal_rate - 2.0) < 1e-9, "seal_rate = 2/hour");

    s.teacher_work = 32;
    s.seals = 2;
    cnet_eg_compute(&s, 32.0, &r);
    check(r.local_eg > 1.5, "better efficiency → local_eg > 1");

    s.teacher_work = 100;
    s.seals = 0;
    cnet_eg_compute(&s, 32.0, &r);
    check(r.cost_per_seal == 100.0, "zero seals uses work/1");

    check(cnet_eg_log_tick(path, 1, 10, 1, 0, 2, 270, 0) == 0, "log tick 1");
    check(cnet_eg_log_tick(path, 2, 20, 3, 1, 0, 273, 2) == 0, "log tick 2");
    {
        CnetEgSample agg;
        check(cnet_eg_aggregate_file(path, 0, 0, &agg) == 0, "aggregate file");
        check(agg.teacher_work == 30 && agg.seals == 4, "aggregate sums work/seals");
        check(agg.curiosity_proposed == 2 && agg.units == 273, "agg curiosity+units");
        cnet_eg_compute(&agg, 32.0, &r);
        check(cnet_eg_result_json(&r, &agg, buf, sizeof buf) == 0 &&
                  strstr(buf, "local_eg"),
              "result JSON");
    }

    remove(path);
    if (failures) {
        printf("EG_FAIL failures=%d checks=%d\n", failures, checks);
        return 1;
    }
    printf("EG_PASS checks=%d\n", checks);
    return 0;
}
