#include "../include/cce/cce_forest.h"
#include "../include/cce/cce_router.h"
#include <stdio.h>

int main(void) {
    printf("CCE Forest + Router Test\n");

    cce_forest* f = NULL;
    cce_forest_open(&f, "forest_test.cce", 16);

    cce_cascade cas;
    cce_cascade_init(&cas, 1);
    cce_block b;
    cce_block_init_linear(&b, 4, 2, 0.01f);
    cce_cascade_append(&cas, &b);

    cce_forest_add_branch(f, &cas, "test_branch");

    float input[4] = {0.1f, 0.2f, 0.3f, 0.4f};
    int idx;
    cce_forest_recall(f, input, 4, &idx);
    printf("Recall: branch %d\n", idx);

    cce_router r;
    cce_router_init(&r, 1.0f, 2);
    int routed;
    float score;
    cce_router_route(&r, f, input, 4, &routed, &score);
    printf("Router SSMax: branch %d score %.3f\n", routed, score);

    cce_forest_close(f);
    remove("forest_test.cce");

    printf("Forest + Router: SUCCESS\n");
    return 0;
}
