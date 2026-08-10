/* RED tests for Slice A — Library OS. Marker: ASI_LIBOS_PASS */
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

static double wall_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + 1e-9 * (double)ts.tv_nsec;
}

int main(void) {
    CnetAsiLib L;
    const char *out = NULL;
    char report[CNET_ASI_REPORT_MAX];
    int debt, r, i;
    double t0, t1;

    printf("=== ASI Slice A: Library OS ===\n");
    cnet_asi_init(&L);

    check(cnet_asi_add_skill(&L, "grasp_cup", "pick cylindrical mug handle",
                             /*need object_seen|arm_free*/ 0x3u, 1,
                             CNET_ASI_KIND_SPECIALIST) == 0,
          "add grasp_cup");
    check(cnet_asi_add_skill(&L, "open_door", "open hinged door push pull",
                             0x5u, 2, CNET_ASI_KIND_SPECIALIST) == 0,
          "add open_door");
    check(cnet_asi_add_skill(&L, "shared_residual", "shared residual polish",
                             0u, 0, CNET_ASI_KIND_SHARED) == 0,
          "add shared");
    check(cnet_asi_add_not_for(&L, "grasp_cup", "open_door") == 0,
          "not_for neighbor");
    check(cnet_asi_add_edge(&L, "grasp_cup", "shared_residual",
                            CNET_ASI_EDGE_REFINE) == 0,
          "edge refine");
    check(cnet_asi_edge_count(&L, "grasp_cup", CNET_ASI_EDGE_REFINE) == 1,
          "edge count refine=1");

    /* recall miss */
    r = cnet_asi_resolve(&L, "fly_helicopter", 0xffffffffu, -1.0, &out);
    check(r == CNET_ASI_RECALL_MISS && out == NULL, "unknown query recall miss");

    /* capability recall but exec blocked */
    r = cnet_asi_resolve(&L, "cylindrical mug", /*world missing bits*/ 0x0u, -1.0,
                         &out);
    check(r == CNET_ASI_EXEC_BLOCKED && out == NULL, "exec gate blocks");
    check(L.n_exec_block >= 1, "exec_block stat");

    /* exact name + full world */
    r = cnet_asi_resolve(&L, "grasp_cup", 0x3u, -1.0, &out);
    check(r == CNET_ASI_OK && out && strcmp(out, "grasp_cup") == 0,
          "exact name resolve OK");

    /* capability keyword with world */
    r = cnet_asi_resolve(&L, "hinged door", 0x5u, -1.0, &out);
    check(r == CNET_ASI_OK && out && strcmp(out, "open_door") == 0,
          "capability substring resolve");

    /* privilege: two skills both match "shared" word? use two alts same precond */
    check(cnet_asi_add_skill(&L, "tool_hi", "cut wire tool", 0x1u, 9,
                             CNET_ASI_KIND_SPECIALIST) == 0,
          "add tool_hi");
    check(cnet_asi_add_skill(&L, "tool_lo", "cut wire tool", 0x1u, 1,
                             CNET_ASI_KIND_SPECIALIST) == 0,
          "add tool_lo");
    r = cnet_asi_resolve(&L, "cut wire", 0x1u, -1.0, &out);
    check(r == CNET_ASI_OK && out && strcmp(out, "tool_lo") == 0,
          "least privilege wins");

    /* broken edge debt + empty capability */
    check(cnet_asi_add_edge(&L, "grasp_cup", "missing_skill", CNET_ASI_EDGE_DEPENDS) ==
              0,
          "add broken edge");
    check(cnet_asi_add_skill(&L, "empty_cap", "", 0u, 0, CNET_ASI_KIND_SPECIALIST) == 0,
          "add empty cap skill");
    check(cnet_asi_add_skill(&L, "dup_a", "same capability text xyz", 0u, 0,
                             CNET_ASI_KIND_SPECIALIST) == 0,
          "dup a");
    check(cnet_asi_add_skill(&L, "dup_b", "same capability text xyz", 0u, 0,
                             CNET_ASI_KIND_SPECIALIST) == 0,
          "dup b");
    check(cnet_asi_add_not_for(&L, "grasp_cup", "ghost_neighbor") == 0,
          "broken not_for");

    debt = cnet_asi_janitor(&L, report, sizeof report);
    check(debt >= 3, "janitor finds multiple debts");
    check(strstr(report, "orphan") != NULL || strstr(report, "broken") != NULL ||
              strstr(report, "empty") != NULL || strstr(report, "dup") != NULL,
          "janitor report nonempty keywords");
    printf("  janitor report: %s\n", report);

    /* load bench: many resolves */
    cnet_asi_reset_stats(&L);
    t0 = wall_s();
    for (i = 0; i < 50000; i++) {
        (void)cnet_asi_resolve(&L, (i & 1) ? "grasp_cup" : "cut wire", 0x3u, -1.0,
                               &out);
    }
    t1 = wall_s();
    printf("  load bench resolves=50000 wall=%.4fs ok=%llu exec_block=%llu miss=%llu\n",
           t1 - t0, (unsigned long long)L.n_resolve_ok,
           (unsigned long long)L.n_exec_block, (unsigned long long)L.n_recall_miss);
    check(L.n_resolve_ok + L.n_exec_block + L.n_recall_miss == 50000,
          "bench accounts all");
    check((t1 - t0) < 2.0, "bench under 2s");

    printf("checks=%d failures=%d\n", checks, failures);
    if (failures) {
        printf("ASI_LIBOS_FAIL\n");
        return 1;
    }
    printf("ASI_LIBOS_PASS\n");
    return 0;
}
