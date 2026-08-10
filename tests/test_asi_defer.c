/* Slice B: conformal abstain + L2D-lite defer. Marker: ASI_DEFER_PASS */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "../include/cnet_asi_improve.h"

static int failures, checks;
static void check(int ok, const char *m) {
    checks++;
    printf("  %-64s %s\n", m, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

int main(void) {
    CnetAsiLib L;
    const char *out = NULL;
    int r;
    double scores[4];
    int elig[4];
    int pick;
    int i;
    double t0, t1;
    struct timespec ts;

    printf("=== ASI Slice B: Conformal + L2D defer ===\n");
    cnet_asi_init(&L);
    check(cnet_asi_add_skill(&L, "expert_a", "classify mode a", 0u, 1,
                             CNET_ASI_KIND_SPECIALIST) == 0,
          "add a");
    check(cnet_asi_add_skill(&L, "expert_b", "classify mode b", 0u, 1,
                             CNET_ASI_KIND_SPECIALIST) == 0,
          "add b");
    check(cnet_asi_set_conf_q(&L, "expert_a", 0.25) == 0, "set conf_q a");
    check(cnet_asi_set_conf_q(&L, "expert_b", 0.25) == 0, "set conf_q b");

    /* residual under threshold → OK */
    r = cnet_asi_resolve(&L, "expert_a", 0xffffffffu, 0.10, &out);
    check(r == CNET_ASI_OK && out && strcmp(out, "expert_a") == 0,
          "low residual serves");

    /* residual above threshold → conformal abstain */
    r = cnet_asi_resolve(&L, "expert_a", 0xffffffffu, 0.80, &out);
    check(r == CNET_ASI_CONFORMAL_ABSTAIN && out == NULL, "high residual abstain");
    check(L.n_conf_abstain >= 1, "conf abstain stat");

    /* residual < 0 means not measured → conf disabled path still OK */
    r = cnet_asi_resolve(&L, "expert_b", 0xffffffffu, -1.0, &out);
    check(r == CNET_ASI_OK, "unmeasured residual skips conf gate");

    /* L2D-lite defer */
    scores[0] = 0.90;
    scores[1] = 0.88;
    scores[2] = 0.10;
    scores[3] = 0.05;
    elig[0] = elig[1] = elig[2] = elig[3] = 1;
    pick = cnet_asi_defer_pick(scores, elig, 4, 0.05);
    check(pick == -1, "margin 0.05 abstains on 0.02 gap");
    pick = cnet_asi_defer_pick(scores, elig, 4, 0.01);
    check(pick == 0, "margin 0.01 picks expert 0");

    elig[0] = 0;
    pick = cnet_asi_defer_pick(scores, elig, 4, 0.01);
    check(pick == 1, "ineligible 0 → pick 1");

    elig[0] = elig[1] = elig[2] = elig[3] = 0;
    pick = cnet_asi_defer_pick(scores, elig, 4, 0.0);
    check(pick == -1, "none eligible abstain");

    /* sole eligible */
    elig[2] = 1;
    pick = cnet_asi_defer_pick(scores, elig, 4, 0.5);
    check(pick == 2, "sole eligible ignores margin");

    /* bench defer */
    clock_gettime(CLOCK_MONOTONIC, &ts);
    t0 = (double)ts.tv_sec + 1e-9 * ts.tv_nsec;
    for (i = 0; i < 200000; i++) {
        scores[0] = 0.5 + (i % 7) * 0.01;
        scores[1] = 0.5 + (i % 5) * 0.01;
        (void)cnet_asi_defer_pick(scores, elig, 4, 0.02);
    }
    clock_gettime(CLOCK_MONOTONIC, &ts);
    t1 = (double)ts.tv_sec + 1e-9 * ts.tv_nsec;
    printf("  defer bench n=200000 wall=%.4fs\n", t1 - t0);
    check((t1 - t0) < 1.0, "defer bench under 1s");

    /* conformal resolve bench */
    cnet_asi_reset_stats(&L);
    clock_gettime(CLOCK_MONOTONIC, &ts);
    t0 = (double)ts.tv_sec + 1e-9 * ts.tv_nsec;
    for (i = 0; i < 30000; i++) {
        double res = (i % 3 == 0) ? 0.9 : 0.05;
        (void)cnet_asi_resolve(&L, "expert_a", 0xffffffffu, res, &out);
    }
    clock_gettime(CLOCK_MONOTONIC, &ts);
    t1 = (double)ts.tv_sec + 1e-9 * ts.tv_nsec;
    printf("  conf resolve bench n=30000 wall=%.4fs ok=%llu conf_abs=%llu\n",
           t1 - t0, (unsigned long long)L.n_resolve_ok,
           (unsigned long long)L.n_conf_abstain);
    check(L.n_conf_abstain > 0 && L.n_resolve_ok > 0, "both serve and abstain");

    printf("checks=%d failures=%d\n", checks, failures);
    if (failures) {
        printf("ASI_DEFER_FAIL\n");
        return 1;
    }
    printf("ASI_DEFER_PASS\n");
    return 0;
}
