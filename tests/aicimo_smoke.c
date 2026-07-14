#include "../src/cce/cce_aicimo.h"
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    printf("=== AICIMO Smoke Test (CNET port) ===\n");

    AicimoRouter router;
    if (aicimo_router_init(&router, 4, 256) != 0) {
        printf("FAIL: router init\n");
        return 1;
    }
    printf("Router initialized with 4 adapters, base_dim=256\n");

    float input[256];
    float output[256];
    for (int i = 0; i < 256; i++) input[i] = (i % 7) * 0.1f;

    size_t used = 0;
    if (aicimo_route(&router, input, 256, output, 256, &used) != 0) {
        printf("FAIL: route\n");
        aicimo_router_free(&router);
        return 1;
    }
    printf("Route succeeded, used_ops=%zu\n", used);

    if (aicimo_compose(&router, input, 256, output, 256) != 0) {
        printf("FAIL: compose\n");
        aicimo_router_free(&router);
        return 1;
    }
    printf("Compose (16x) succeeded — adapter routing stability demonstrated\n");

    aicimo_router_free(&router);
    printf("=== AICIMO Smoke Test PASSED ===\n");
    return 0;
}