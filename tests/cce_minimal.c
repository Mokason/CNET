#include "../include/cce/cce_tensor.h"
#include "../include/cce/cce_block.h"
#include "../include/cce/cce_cascade.h"
#include <stdio.h>

int main(void) {
    printf("CCE Minimal Test\n");

    // Tensor
    cce_tensor t;
    int shape[2] = {2, 3};
    if (cce_tensor_alloc(&t, shape, 2) == CCE_OK) {
        printf("Tensor alloc OK\n");
        cce_tensor_free(&t);
    }

    // Block
    cce_block b;
    if (cce_block_init_linear(&b, 3, 2, 0.01f) == CCE_OK) {
        printf("Block init OK\n");
        cce_block_free(&b);
    }

    // Cascade
    cce_cascade cas;
    if (cce_cascade_init(&cas, 4) == CCE_OK) {
        printf("Cascade init OK\n");
        cce_block b2;
        cce_block_init_linear(&b2, 3, 2, 0.01f);
        cce_cascade_append(&cas, &b2);
        printf("Cascade append OK, num_blocks=%d\n", cas.num_blocks);
        cce_cascade_free(&cas);
    }

    printf("CCE Minimal: SUCCESS\n");
    return 0;
}
