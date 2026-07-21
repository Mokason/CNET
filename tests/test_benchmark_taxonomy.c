/* P6 Benchmark taxonomy honesty.
 * measured_* requires an executed path; absent external datasets stay withheld;
 * contract_pass must not be relabeled measured_pass.
 * make benchmark_taxonomy → BENCHMARK_TAXONOMY_PASS
 */
#include <stdio.h>
#include <string.h>

static int failures, checks;

static void check(int ok, const char *name) {
    checks++;
    printf("  %-62s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

/* Mirror of tools/run_phase123_benchmarks.py claim rules (C-side gate). */
static const char *classify_claim(int path_executed, int dataset_present,
                                  int metric_ok, int is_contract_only) {
    if (is_contract_only) return "contract_pass";
    if (!dataset_present || !path_executed) return "withheld";
    return metric_ok ? "measured_pass" : "measured_fail";
}

static int refuse_fake_measured(const char *status, int path_executed,
                                int dataset_present) {
    if (strcmp(status, "measured_pass") == 0 ||
        strcmp(status, "measured_fail") == 0) {
        return path_executed && dataset_present;
    }
    return 1;
}

int main(void) {
    const char *s;

    printf("== benchmark_taxonomy (P6) ==\n");

    s = classify_claim(0, 0, 1, 0);
    check(strcmp(s, "withheld") == 0, "no path + no dataset → withheld");

    s = classify_claim(0, 1, 1, 0);
    check(strcmp(s, "withheld") == 0, "dataset present but path not executed → withheld");

    s = classify_claim(1, 0, 1, 0);
    check(strcmp(s, "withheld") == 0, "path without dataset → withheld");

    s = classify_claim(1, 1, 1, 0);
    check(strcmp(s, "measured_pass") == 0, "executed path + dataset + ok → measured_pass");

    s = classify_claim(1, 1, 0, 0);
    check(strcmp(s, "measured_fail") == 0, "executed path + dataset + bad → measured_fail");

    s = classify_claim(1, 1, 1, 1);
    check(strcmp(s, "contract_pass") == 0,
          "native contract-only evidence stays contract_pass");

    check(refuse_fake_measured("measured_pass", 0, 1) == 0,
          "refuse measured_pass without executed path");
    check(refuse_fake_measured("measured_pass", 1, 0) == 0,
          "refuse measured_pass without dataset");
    check(refuse_fake_measured("withheld", 0, 0) == 1, "withheld always allowed");
    check(refuse_fake_measured("contract_pass", 0, 0) == 1, "contract_pass allowed");

    /* FACTOR/TruthfulQA / LongBench remain withheld until real paths exist. */
    check(strcmp(classify_claim(0, 1, 1, 0), "withheld") == 0,
          "TruthfulQA-class without runtime path stays withheld");
    check(strcmp(classify_claim(0, 0, 1, 0), "withheld") == 0,
          "LongBench-class without dataset+path stays withheld");

    if (failures) {
        printf("BENCHMARK_TAXONOMY_FAIL failures=%d checks=%d\n", failures, checks);
        return 1;
    }
    printf("BENCHMARK_TAXONOMY_PASS checks=%d\n", checks);
    return 0;
}
