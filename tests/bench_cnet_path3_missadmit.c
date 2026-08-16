#include "cnet_core_paths.h"
#include <stdio.h>
int main(void) {
    CnetPath3Bench b;
    int rc = cnet_path3_bench(&b);
    printf("PATH3_MISSADMIT %s propose_rc=%d proposed=%d table=%d admit=%d "
           "serve=%d admit_delta=%d ms_propose=%.3f ms_table=%.3f ms_total=%.3f\n",
           rc == 0 ? "PASS" : "FAIL", b.propose_rc, b.proposed, b.table_ok,
           b.admit_ok, b.served_ok, b.admit_calls_delta, b.ms_propose,
           b.ms_table_admit, b.ms_total);
    if (rc) return 1;
    printf("CNET_PATH3_MISSADMIT_PASS\n");
    printf("miss_to_admit=1 propose_neq_admit=1 table_ge_0.95=1 serve_no_llm=1 "
           "ms=%.3f\n",
           b.ms_total);
    return 0;
}
