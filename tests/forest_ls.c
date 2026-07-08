/* forest_ls: quick inspector for a CCE forest archive (.cce)
 * Usage: ./bin/forest_ls path/to/xxx.cce
 *
 * Prints basic stats and first N branch names.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/cce/cce_forest.h"

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <forest.cce>\n", argv[0]);
        return 1;
    }
    const char* path = argv[1];

    cce_forest* f = NULL;
    cce_result rc = cce_forest_open(&f, path, 4096);
    if (rc != CCE_OK || !f) {
        fprintf(stderr, "error: cce_forest_open failed on %s (rc=%d)\n", path, (int)rc);
        return 2;
    }

    printf("forest: %s\n", path);
    printf("  branches: %d\n", f->num_branches);
    printf("  max:      %d\n", f->max_branches);

    int show = 20;
    if (argc > 2) show = atoi(argv[2]);
    if (show < 0) show = f->num_branches;

    printf("  first %d branches:\n", (show > f->num_branches ? f->num_branches : show));
    for (int i = 0; i < f->num_branches && i < show; i++) {
        printf("    [%3d] %s\n", i, f->branches[i].name);
    }
    if (f->num_branches > show) {
        printf("    ... (%d more)\n", f->num_branches - show);
    }

    // Rough param count from linear branches (best effort)
    size_t total_params = 0;
    for (int i = 0; i < f->num_branches; i++) {
        cce_cascade* cas = f->branches[i].cascade;
        if (!cas) continue;
        for (int b = 0; b < cas->num_blocks; b++) {
            cce_block* blk = &cas->blocks[b];
            if (blk->weights.ndim == 2 && blk->weights.data) {
                total_params += blk->weights.numel;
            }
        }
    }
    printf("  approx FP params in loaded weights: %zu (%.1f GB)\n",
           total_params, total_params * 4.0 / (1024*1024*1024));

    cce_forest_close(f);
    return 0;
}
