/* Slice D: episodic memory + continual regression. Marker: ASI_EPISODE_PASS */
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
    CnetAsiEpisode ep;
    int i, reg;
    double t0, t1;
    struct timespec ts;

    printf("=== ASI Slice D: Episodic + continual regression ===\n");
    cnet_asi_init(&L);
    check(cnet_asi_add_skill(&L, "skill_x", "do x", 0u, 0,
                             CNET_ASI_KIND_SPECIALIST) == 0,
          "add x");
    check(cnet_asi_add_skill(&L, "skill_y", "do y", 0u, 0,
                             CNET_ASI_KIND_SPECIALIST) == 0,
          "add y");

    check(cnet_asi_episode_log(&L, "skill_x", 0x1u, 1, 0.1) == 0, "log ok x");
    check(cnet_asi_episode_log(&L, "skill_x", 0x1u, 1, 0.05) == 0, "log ok x2");
    check(cnet_asi_episode_last(&L, "skill_x", &ep) == 1, "last x found");
    check(ep.ok == 1 && ep.residual == 0.05, "last x is newest ok");
    check(cnet_asi_continual_regressions(&L) == 0, "no regression yet");

    check(cnet_asi_episode_log(&L, "skill_x", 0x1u, 0, 0.9) == 0, "log fail x");
    reg = cnet_asi_continual_regressions(&L);
    check(reg == 1, "regression on skill_x");

    check(cnet_asi_episode_log(&L, "skill_y", 0u, 1, 0.0) == 0, "log y ok");
    check(cnet_asi_continual_regressions(&L) == 1, "still only x regresses");

    /* never serves from episodes — resolve independent */
    {
        const char *out = NULL;
        int r = cnet_asi_resolve(&L, "skill_y", 0xffffffffu, -1.0, &out);
        check(r == CNET_ASI_OK, "resolve still works; episodes are evidence only");
    }

    /* ring stress */
    clock_gettime(CLOCK_MONOTONIC, &ts);
    t0 = (double)ts.tv_sec + 1e-9 * ts.tv_nsec;
    for (i = 0; i < 10000; i++)
        (void)cnet_asi_episode_log(&L, (i & 1) ? "skill_x" : "skill_y", (uint32_t)i,
                                   i % 3 != 0, 0.01 * (i % 10));
    clock_gettime(CLOCK_MONOTONIC, &ts);
    t1 = (double)ts.tv_sec + 1e-9 * ts.tv_nsec;
    check(L.ep_n == CNET_ASI_MAX_EP, "ring full at MAX_EP");
    check(cnet_asi_episode_last(&L, "skill_y", &ep) == 1, "last after ring");
    reg = cnet_asi_continual_regressions(&L);
    printf("  episode bench log=10000 wall=%.4fs regressions=%d ep_n=%zu\n",
           t1 - t0, reg, L.ep_n);
    check((t1 - t0) < 1.0, "episode bench under 1s");

    printf("checks=%d failures=%d\n", checks, failures);
    if (failures) {
        printf("ASI_EPISODE_FAIL\n");
        return 1;
    }
    printf("ASI_EPISODE_PASS\n");
    return 0;
}
