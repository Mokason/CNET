/* Slice C: elbow top-M + specialization HHI + shared/specialist split.
 * Marker: ASI_ROUTE_PASS */
#include <math.h>
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
    double scores[8];
    int idx[8];
    int k, i, n_sh, n_sp;
    uint64_t counts[4];
    double hhi;
    double t0, t1;
    struct timespec ts;
    const char *out = NULL;

    printf("=== ASI Slice C: Routing improve ===\n");
    cnet_asi_init(&L);

    /* elbow: clear gap after rank 2 */
    scores[0] = 1.0;
    scores[1] = 0.95;
    scores[2] = 0.40;
    scores[3] = 0.39;
    k = cnet_asi_elbow_topm(scores, 4, 4, 0.2, idx);
    check(k == 2, "elbow stops at gap>0.2 → k=2");
    check(idx[0] == 0 && idx[1] == 1, "top indices 0,1");

    k = cnet_asi_elbow_topm(scores, 4, 1, 0.0, idx);
    check(k == 1 && idx[0] == 0, "max_m=1");

    scores[0] = 0.5;
    scores[1] = 0.49;
    scores[2] = 0.48;
    scores[3] = 0.47;
    k = cnet_asi_elbow_topm(scores, 4, 4, 0.05, idx);
    check(k == 4, "small gaps take all within max_m");

    /* HHI */
    counts[0] = 100;
    counts[1] = 0;
    counts[2] = 0;
    counts[3] = 0;
    hhi = cnet_asi_specialization_hhi(counts, 4);
    check(fabs(hhi - 1.0) < 1e-9, "single expert HHI=1");

    counts[0] = 25;
    counts[1] = 25;
    counts[2] = 25;
    counts[3] = 25;
    hhi = cnet_asi_specialization_hhi(counts, 4);
    check(fabs(hhi - 0.25) < 1e-9, "uniform HHI=1/n");

    /* shared vs specialist registration + select tracking */
    check(cnet_asi_add_skill(&L, "shared_res", "shared residual", 0u, 0,
                             CNET_ASI_KIND_SHARED) == 0,
          "shared");
    check(cnet_asi_add_skill(&L, "spec_a", "mode alpha", 0u, 1,
                             CNET_ASI_KIND_SPECIALIST) == 0,
          "spec a");
    check(cnet_asi_add_skill(&L, "spec_b", "mode beta", 0u, 1,
                             CNET_ASI_KIND_SPECIALIST) == 0,
          "spec b");
    cnet_asi_kind_counts(&L, &n_sh, &n_sp);
    check(n_sh == 1 && n_sp == 2, "kind counts 1 shared 2 spec");

    for (i = 0; i < 10; i++)
        (void)cnet_asi_resolve(&L, "spec_a", 0xffffffffu, -1.0, &out);
    for (i = 0; i < 2; i++)
        (void)cnet_asi_resolve(&L, "spec_b", 0xffffffffu, -1.0, &out);
    {
        uint64_t sc[3];
        sc[0] = L.skills[0].n_select;
        sc[1] = L.skills[1].n_select;
        sc[2] = L.skills[2].n_select;
        hhi = cnet_asi_specialization_hhi(sc, 3);
        check(hhi > 0.5, "skewed selects → higher HHI");
        printf("  select HHI=%.4f counts=%llu,%llu,%llu\n", hhi,
               (unsigned long long)sc[0], (unsigned long long)sc[1],
               (unsigned long long)sc[2]);
    }

    /* bench elbow */
    for (i = 0; i < 8; i++) scores[i] = 1.0 - 0.05 * i;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    t0 = (double)ts.tv_sec + 1e-9 * ts.tv_nsec;
    for (i = 0; i < 100000; i++)
        (void)cnet_asi_elbow_topm(scores, 8, 4, 0.12, idx);
    clock_gettime(CLOCK_MONOTONIC, &ts);
    t1 = (double)ts.tv_sec + 1e-9 * ts.tv_nsec;
    printf("  elbow bench n=100000 wall=%.4fs\n", t1 - t0);
    check((t1 - t0) < 1.0, "elbow bench under 1s");

    printf("checks=%d failures=%d\n", checks, failures);
    if (failures) {
        printf("ASI_ROUTE_FAIL\n");
        return 1;
    }
    printf("ASI_ROUTE_PASS\n");
    return 0;
}
