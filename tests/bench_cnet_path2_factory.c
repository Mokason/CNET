#include "cnet_core_paths.h"
#include <stdio.h>
int main(void) {
    CnetPath2Bench b;
    int i, rc = cnet_path2_bench(&b);
    printf("PATH2_FACTORY %s built=%d/%d served=%d skip_gate=%d ms_total=%.3f\n",
           rc == 0 ? "PASS" : "FAIL", b.n_built, b.n_requested, b.n_served_ok,
           b.skipped_for_gate, b.ms_total);
    for (i = 0; i < b.n_built; ++i)
        printf("  brick[%d] tag=%s ms=%.3f\n", i, b.tags[i], b.ms_per_brick[i]);
    if (rc) return 1;
    printf("CNET_PATH2_FACTORY_PASS\n");
    printf("brick_factory=1 n_plus_one_after_serve=1 teacher_gone=1 bricks=%d "
           "ms=%.3f\n",
           b.n_built, b.ms_total);
    return 0;
}
