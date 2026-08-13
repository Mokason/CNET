/* Claim honesty / WITHHELD floors from tests/test_phase123_benchmarks.py.
 * Prints PHASE123_BENCHMARK_PASS when all authority checks hold.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef struct {
    int contract_pass;
    int dataset_present;
    int runtime_integrated;
    double target_gain;
    int success_metric_claimed;
    char status[32];
} TQA;

typedef struct {
    int quality_metric_claimed;
    char status[32];
    double needle_retention;
} LC;

typedef struct {
    int passed;
    double delta;
    char status[32];
} CP;

static int failures;

static void expect(int ok, const char *m) {
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", m);
        failures++;
    }
}

static int truthfulqa_claim(int contract_pass, int dataset_present, int runtime_integrated,
                            const double *gain, TQA *out) {
    memset(out, 0, sizeof *out);
    out->contract_pass = !!contract_pass;
    out->dataset_present = !!dataset_present;
    out->runtime_integrated = !!runtime_integrated;
    out->target_gain = 0.025;
    if (gain && !(dataset_present && runtime_integrated)) return -1;
    if (!contract_pass) {
        snprintf(out->status, sizeof out->status, "contract_fail");
        return 0;
    }
    if (!(dataset_present && runtime_integrated)) {
        snprintf(out->status, sizeof out->status, "withheld");
        return 0;
    }
    out->success_metric_claimed = 1;
    snprintf(out->status, sizeof out->status,
             (*gain >= 0.025) ? "measured_pass" : "measured_fail");
    return 0;
}

static void long_context_claim(int contract_pass, int dataset_present, int runtime_integrated,
                               double needle, LC *out) {
    memset(out, 0, sizeof *out);
    out->needle_retention = needle;
    if (!contract_pass) {
        snprintf(out->status, sizeof out->status, "contract_fail");
        return;
    }
    if (!(dataset_present && runtime_integrated)) {
        snprintf(out->status, sizeof out->status, "withheld");
        return;
    }
    out->quality_metric_claimed = 1;
    snprintf(out->status, sizeof out->status, "measured_pass");
}

static void creative_preservation(const double *ref, const double *cand, int n,
                                  double tol, CP *out) {
    double rs = 0, cs = 0;
    int i;
    for (i = 0; i < n; i++) {
        rs += ref[i];
        cs += cand[i];
    }
    out->delta = (cs / n) - (rs / n);
    out->passed = out->delta >= -fabs(tol);
    snprintf(out->status, sizeof out->status, out->passed ? "measured_pass" : "measured_fail");
}

static const char *overall_verdict(const char *p1, const char *p2, const char *p3) {
    if (strcmp(p3, "measured_pass") != 0) return "FAIL_CREATIVE_PRESERVATION";
    if (strcmp(p1, "withheld") == 0 || strcmp(p2, "withheld") == 0)
        return "PASS_WITH_EXTERNAL_CLAIMS_WITHHELD";
    if (strcmp(p1, "measured_pass") == 0 && strcmp(p2, "measured_pass") == 0)
        return "PASS_ALL_MEASURED";
    return "FAIL_BENCHMARK_CLAIM";
}

int main(void) {
    TQA t;
    LC l;
    CP passed, failed;
    double g;
    double ref[2] = {0.80, 0.70};
    double ok[2] = {0.78, 0.72};
    double bad[2] = {0.60, 0.62};
    const char *v;

    failures = 0;
    expect(truthfulqa_claim(1, 0, 0, NULL, &t) == 0, "withheld call ok");
    expect(strcmp(t.status, "withheld") == 0, "status withheld");
    expect(!t.success_metric_claimed, "no success claim");
    expect(fabs(t.target_gain - 0.025) < 1e-12, "target_gain 0.025");

    g = 0.04;
    expect(truthfulqa_claim(1, 0, 1, &g, &t) == -1, "requires both prereqs");

    g = 0.03;
    truthfulqa_claim(1, 1, 1, &g, &t);
    expect(strcmp(t.status, "measured_pass") == 0, "gain floor pass");
    g = 0.02;
    truthfulqa_claim(1, 1, 1, &g, &t);
    expect(strcmp(t.status, "measured_fail") == 0, "gain floor fail");

    long_context_claim(1, 0, 0, 1.0, &l);
    expect(strcmp(l.status, "withheld") == 0, "long context withheld");
    expect(!l.quality_metric_claimed, "no quality claim");
    expect(fabs(l.needle_retention - 1.0) < 1e-12, "needle retained locally");

    creative_preservation(ref, ok, 2, 0.05, &passed);
    creative_preservation(ref, bad, 2, 0.05, &failed);
    expect(passed.passed, "creative non-regression pass");
    expect(!failed.passed, "creative regression fail");
    expect(fabs(passed.delta - 0.0) < 1e-9, "delta ~0");

    v = overall_verdict("withheld", "withheld", "measured_pass");
    expect(strcmp(v, "PASS_WITH_EXTERNAL_CLAIMS_WITHHELD") == 0,
           "overall names WITHHELD external claims");

    if (failures) {
        printf("PHASE123_BENCHMARK_FAIL\n");
        return 1;
    }
    printf("PHASE123_BENCHMARK_PASS\n");
    return 0;
}
