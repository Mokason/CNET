/* Slice E: role firewall + anytime-valid compare. Marker: ASI_FIREWALL_EVAL_PASS */
#include <stdio.h>
#include <time.h>

#include "../include/cnet_asi_improve.h"

static int failures, checks;
static void check(int ok, const char *m) {
    checks++;
    printf("  %-64s %s\n", m, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

enum { ROLE_DIRECTOR = 0, ROLE_CAST = 1, ROLE_STEWARD = 2, N_ROLES = 3 };
enum {
    FX_NARRATIVE = 0,
    FX_NPC_LOCAL = 1,
    FX_SEASON = 2,
    FX_LOOT = 3,
    N_FX = 4
};

int main(void) {
    /* row-major roles x effects */
    int allow[N_ROLES * N_FX] = {
        /* director */ 1, 0, 0, 0,
        /* cast     */ 0, 1, 0, 0,
        /* steward  */ 0, 0, 1, 0,
    };
    int d, i, dec;
    double t0, t1;
    struct timespec ts;

    printf("=== ASI Slice E: Role firewall + AV eval ===\n");

    check(cnet_asi_role_allow(ROLE_DIRECTOR, FX_NARRATIVE, allow, N_ROLES, N_FX) == 1,
          "director may narrative");
    check(cnet_asi_role_allow(ROLE_DIRECTOR, FX_NPC_LOCAL, allow, N_ROLES, N_FX) == 0,
          "director blocked npc_local");
    check(cnet_asi_role_allow(ROLE_CAST, FX_NARRATIVE, allow, N_ROLES, N_FX) == 0,
          "cast blocked narrative");
    check(cnet_asi_role_allow(ROLE_CAST, FX_NPC_LOCAL, allow, N_ROLES, N_FX) == 1,
          "cast may npc_local");
    check(cnet_asi_role_allow(ROLE_STEWARD, FX_SEASON, allow, N_ROLES, N_FX) == 1,
          "steward season");
    check(cnet_asi_role_allow(ROLE_STEWARD, FX_LOOT, allow, N_ROLES, N_FX) == 0,
          "steward no loot");
    check(cnet_asi_role_allow(-1, 0, allow, N_ROLES, N_FX) == 0, "bad role");
    check(cnet_asi_role_allow(0, 99, allow, N_ROLES, N_FX) == 0, "bad effect");

    /* AV compare: need clear separation */
    d = cnet_asi_av_compare(5, 5, 10, 0.05);
    check(d == 0, "tie → continue");

    d = cnet_asi_av_compare(3, 0, 3, 0.05);
    check(d == 0 || d == 1, "small n usually continue (or A if bound loose)");

    /* large n decisive A */
    d = cnet_asi_av_compare(800, 200, 1000, 0.01);
    check(d == 1, "A certified better at n=1000");

    d = cnet_asi_av_compare(200, 800, 1000, 0.01);
    check(d == -1, "B certified better");

    d = cnet_asi_av_compare(510, 490, 1000, 0.01);
    check(d == 0, "narrow gap continues");

    /* simulate bake-off loop until stop */
    {
        int wa = 0, wb = 0, n = 0, steps = 0;
        for (i = 0; i < 5000; i++) {
            /* A wins 70% */
            if (i % 10 < 7)
                wa++;
            else
                wb++;
            n++;
            dec = cnet_asi_av_compare(wa, wb, n, 0.05);
            steps++;
            if (dec != 0) break;
        }
        printf("  bake-off stop dec=%d n=%d wa=%d wb=%d steps=%d\n", dec, n, wa, wb,
               steps);
        check(dec == 1, "bake-off certifies A");
        check(n < 5000, "stopped before max");
    }

    clock_gettime(CLOCK_MONOTONIC, &ts);
    t0 = (double)ts.tv_sec + 1e-9 * ts.tv_nsec;
    for (i = 0; i < 200000; i++) {
        (void)cnet_asi_role_allow(i % N_ROLES, i % N_FX, allow, N_ROLES, N_FX);
        (void)cnet_asi_av_compare(600 + (i % 50), 400, 1000, 0.05);
    }
    clock_gettime(CLOCK_MONOTONIC, &ts);
    t1 = (double)ts.tv_sec + 1e-9 * ts.tv_nsec;
    printf("  firewall+av bench n=200000 wall=%.4fs\n", t1 - t0);
    check((t1 - t0) < 1.0, "bench under 1s");

    printf("checks=%d failures=%d\n", checks, failures);
    if (failures) {
        printf("ASI_FIREWALL_EVAL_FAIL\n");
        return 1;
    }
    printf("ASI_FIREWALL_EVAL_PASS\n");
    return 0;
}
