#include "cnet_core_paths.h"
#include <stdio.h>
int main(void) {
    CnetPath4Bench b;
    int rc = cnet_path4_bench(&b);
    printf("PATH4_COMPOSE %s split=%d compose=%d child_a=%d child_b=%d "
           "composed=%d n1_block=%d ms_split=%.3f ms_compose=%.3f ms_total=%.3f\n",
           rc == 0 ? "PASS" : "FAIL", b.split_ok, b.compose_ok, b.child_a_ok,
           b.child_b_ok, b.composed_ok, b.n_plus_one_blocked, b.ms_split,
           b.ms_compose, b.ms_total);
    if (rc) return 1;
    printf("CNET_PATH4_COMPOSE_PASS\n");
    printf("split=1 compose=1 n_plus_one_gate=1 ms=%.3f\n", b.ms_total);
    return 0;
}
