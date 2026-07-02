/*
 * Regression test for the attention_retrieve_top_k stack overflow.
 *
 * Bug: for reg->count > 64, attention_retrieve_top_k passed a fixed
 * `size_t tmp[64]` to rank_by_reliability, which writes reg->count indices --
 * a stack-buffer overflow. Any attention mode (>= SHADOW) on a registry of
 * more than 64 primitives smashed the stack inside dag_plan_circuit's
 * telemetry path. Pre-fix this test SEGFAULTs; post-fix it returns cleanly.
 *
 * Standalone (own main, no scan.c dependency) so it builds independently of
 * the aggregate test_all suite.
 */
#include "../include/nn.h"
#include "../include/router.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;
#define CHECK(c, d) do { if (c) { printf("  ok   %s\n", (d)); } \
    else { printf("  FAIL %s\n", (d)); ++failures; } } while (0)

static Port P(PortFamily f, size_t w, size_t c) {
    Port p; p.family = f; p.field_width = w; p.field_count = c; p.tag[0] = '\0';
    return p;
}

int main(void) {
    printf("test_attention_overflow (reg->count>64 + attention SHADOW):\n");
    enum { NDECOY = 80 };  /* solver + 80 decoys = 81 > 64 */
    static BinaryTransformNetwork solver = {0};
    static BinaryTransformNetwork decoys[NDECOY];
    memset(decoys, 0, sizeof(decoys));

    PrimitiveRegistry reg;
    registry_init(&reg);

    if (btn_init(&solver, 16, 5, 1, 4, 0.5, 1u) != 0 ||
        btn_set_ports(&solver, P(PORT_ONEHOT, 16, 1), P(PORT_BINARY_MSB, 5, 1)) != 0) {
        CHECK(0, "solver setup"); return 1;
    }
    registry_add(&reg, &solver, "solver");
    for (int i = 0; i < NDECOY; ++i) {
        size_t iw = 20 + (size_t)i, ow = 21 + (size_t)i;  /* distinct unrelated types */
        if (btn_init(&decoys[i], iw, ow, 1, 4, 0.5, 1u) != 0 ||
            btn_set_ports(&decoys[i], P(PORT_BINARY_MSB, iw, 1), P(PORT_BINARY_MSB, ow, 1)) != 0) {
            CHECK(0, "decoy setup"); return 1;
        }
        registry_add(&reg, &decoys[i], "decoy");
    }
    CHECK(reg.count > 64, "registry exceeds 64 (exercises the >64 attention branch)");

    reg.attention_mode = CNET_ATTENTION_SHADOW;  /* triggers attention_retrieve_top_k */

    DagSource src; src.type = P(PORT_ONEHOT, 16, 1); src.values = NULL;
    Port goal = P(PORT_BINARY_MSB, 5, 1);
    CircuitPlan plan;
    int rc = dag_plan_circuit(&reg, &src, 1, &goal, 1, &plan);
    CHECK(rc == 0, "dag_plan_circuit with >64 prims + SHADOW returns a plan (no stack smash)");
    if (rc == 0) circuit_free(&plan);

    btn_free(&solver);
    for (int i = 0; i < NDECOY; ++i) btn_free(&decoys[i]);
    registry_free(&reg);

    if (failures == 0) { printf("PASS (no overflow)\n"); return 0; }
    printf("%d FAIL\n", failures);
    return 1;
}
