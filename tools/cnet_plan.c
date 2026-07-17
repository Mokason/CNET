/* CLI: placement plan / doctor (Colibrì-inspired).
 * Build: make cnet_plan_cli
 * Usage: bin/cnet_plan plan|doctor|json
 */
#include <stdio.h>
#include <string.h>

#include "../include/cnet_placement.h"

int main(int argc, char **argv) {
    CnetPlacementPlan plan;
    const char *cmd = (argc > 1) ? argv[1] : "plan";
    char buf[4096];

    if (cnet_placement_plan(&plan, NULL, NULL, NULL, NULL, 70) != 0) {
        fprintf(stderr, "cnet_plan: plan failed\n");
        return 2;
    }
    if (strcmp(cmd, "json") == 0) {
        cnet_placement_json(&plan, buf, sizeof buf);
        printf("%s\n", buf);
        return 0;
    }
    if (strcmp(cmd, "doctor") == 0) {
        int rc = cnet_placement_doctor(&plan, buf, sizeof buf);
        fputs(buf, stdout);
        return rc;
    }
    /* plan (default) */
    printf("cnet plan\n");
    printf("  mem_available=%.2f GiB\n",
           plan.mem_available_bytes / (1024.0 * 1024.0 * 1024.0));
    printf("  cnb=%s (%.1f MiB) %s\n", plan.cnb_path,
           plan.cnb_bytes / (1024.0 * 1024.0),
           plan.cnb_path_ok ? "ok" : "MISSING");
    printf("  residual peak≈%.2f GiB dual_safe=%d prefer_warm_only=%d\n",
           plan.peak_residual_alone / (1024.0 * 1024.0 * 1024.0),
           plan.dual_safe, plan.prefer_warm_only);
    printf("  advice: %s\n", plan.advice);
    return 0;
}
