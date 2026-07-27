/* Analytic fixtures for the continuous-domain coverage gate.
 *
 * The gate decides whether a feature vector is the kind of input the head was
 * certified over. Its failure modes are asymmetric: admitting an out-of-domain
 * vector produces a confident wrong detection, which is exactly what coverage
 * exists to prevent, so every ambiguous or unusable input must refuse.
 *
 * make vision_coverage_test -> VISION_COVERAGE_TEST_PASS
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../tools/vision_detection/vd_coverage.h"

static int failures, checks;
static void check(int ok, const char *name) {
    checks++;
    printf("  %-62s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}
static int near(double a, double b) { return fabs(a - b) < 1e-9; }

#define D 4
int main(void) {
    VdCoverage c;
    /* four reference rows on a unit-ish lattice */
    double ref[4 * D] = {
        0,0,0,0,
        1,0,0,0,
        0,1,0,0,
        0,0,1,0
    };
    double origin[D] = {0,0,0,0};
    double far[D]    = {100,100,100,100};
    printf("== continuous coverage gate fixtures ==\n");

    check(vd_cov_init(&c, ref, 4, D, 1, 0.5) == 0, "init: builds a reference set");
    check(near(vd_cov_score(&c, origin), 0.0), "score: exact reference row scores 0");
    check(vd_cov_admit(&c, origin) == 1, "admit: an exact reference row is admitted");
    check(vd_cov_score(&c, far) > 100.0, "score: a distant vector scores large");
    check(vd_cov_admit(&c, far) == 0, "admit: a distant vector is refused");

    /* k>1 averages, so the score of the origin rises to the mean of its k nearest */
    vd_cov_free(&c);
    check(vd_cov_init(&c, ref, 4, D, 4, 10.0) == 0, "init: k=4");
    check(near(vd_cov_score(&c, origin), (0.0 + 1.0 + 1.0 + 1.0) / 4.0),
          "score: k-nearest mean is exact for a known lattice");

    /* monotonicity: moving away never lowers the score */
    {
        double a[D] = {0.1,0,0,0}, b[D] = {0.5,0,0,0};
        check(vd_cov_score(&c, a) <= vd_cov_score(&c, b) + 1e-12,
              "score: moving away from the reference set does not decrease");
    }

    /* unusable inputs must refuse, never score */
    {
        double nan_v[D] = {0,0,0,0}, inf_v[D] = {0,0,0,0}, big[D] = {0,0,0,0};
        nan_v[2] = NAN; inf_v[1] = INFINITY; big[0] = VD_COV_MAX_ABS * 10.0;
        check(isnan(vd_cov_score(&c, nan_v)), "score: NaN coordinate yields NaN");
        check(vd_cov_admit(&c, nan_v) == 0, "admit: NaN coordinate refused");
        check(isnan(vd_cov_score(&c, inf_v)), "score: infinite coordinate yields NaN");
        check(vd_cov_admit(&c, inf_v) == 0, "admit: infinite coordinate refused");
        check(isnan(vd_cov_score(&c, big)), "score: extreme magnitude yields NaN");
        check(vd_cov_admit(&c, big) == 0, "admit: extreme magnitude refused");
        check(vd_cov_admit(&c, NULL) == 0, "admit: NULL vector refused");
    }

    /* threshold is respected exactly at the boundary */
    {
        double at[D] = {0,0,0,0};
        vd_cov_free(&c);
        check(vd_cov_init(&c, ref, 4, D, 1, 0.0) == 0, "init: tau=0");
        check(vd_cov_admit(&c, at) == 1, "admit: score exactly at tau is admitted");
        vd_cov_free(&c);
        check(vd_cov_init(&c, ref, 4, D, 1, -1.0) == 0, "init: tau below any score");
        check(vd_cov_admit(&c, at) == 0, "admit: tau below the score refuses");
    }

    /* quantile helper */
    {
        double s[5] = {5,1,4,2,3};
        check(near(vd_cov_quantile(s, 5, 0.0), 1.0), "quantile: 0 -> min");
        {
            double t[5] = {5,1,4,2,3};
            check(near(vd_cov_quantile(t, 5, 1.0), 5.0), "quantile: 1 -> max");
        }
        {
            double u[5] = {5,1,4,2,3};
            check(near(vd_cov_quantile(u, 5, 0.5), 3.0), "quantile: 0.5 -> median");
        }
    }
    vd_cov_free(&c);

    printf("checks=%d failures=%d\n", checks, failures);
    if (failures == 0) { printf("VISION_COVERAGE_TEST_PASS checks=%d\n", checks); return 0; }
    printf("VISION_COVERAGE_TEST_FAIL failures=%d\n", failures);
    return 1;
}
