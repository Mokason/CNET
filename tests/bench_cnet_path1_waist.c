#include "cnet_core_paths.h"
#include <stdio.h>
int main(void) {
    CnetPath1Bench b;
    int rc = cnet_path1_bench(&b);
    printf("PATH1_WAIST %s cert=%d abstain=%d open_chat_block=%d roe_block=%d "
           "llm_block=%d ms=%.3f\n",
           rc == 0 ? "PASS" : "FAIL", b.cert_hits, b.abstains, b.open_chat_blocked,
           b.roe_blocked, b.llm_blocked, b.ms_total);
    if (rc) return 1;
    printf("CNET_PATH1_WAIST_PASS\n");
    printf("live_waist=1 cert_or_abstain=1 leftover_mouth=0 ms=%.3f\n", b.ms_total);
    return 0;
}
