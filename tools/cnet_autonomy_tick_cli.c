/* CLI wrapper for cnet_autonomy_tick — called from roe_evolve_tick / ops.
 * Usage: cnet_autonomy_tick_cli <workdir> [turn]
 * Exit 0 always on soft success; prints AUTONOMY_TICK_CLI_PASS.
 */
#include "cnet_distrust.h"

#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    CnetAutonomyTickResult r;
    const char *workdir;
    const char *turn = NULL;
    int rc;

    if (argc < 2) {
        fprintf(stderr, "usage: %s <workdir> [turn]\n", argv[0]);
        return 2;
    }
    workdir = argv[1];
    if (argc >= 3) turn = argv[2];

    memset(&r, 0, sizeof r);
    rc = cnet_autonomy_tick(workdir, turn, NULL, &r);
    if (rc != 0) {
        fprintf(stderr, "cnet_autonomy_tick failed rc=%d\n", rc);
        return 1;
    }
    printf("domain=%s admitted=%d residual_rejected=%d goals=%d miss_rows=%d "
           "seal=%d detail=%s\n",
           r.domain, r.admitted, r.residual_rejected, r.goals_queued,
           r.miss_rows_appended, (int)r.seal_decision, r.detail);
    printf("AUTONOMY_TICK_CLI_PASS\n");
    return 0;
}
