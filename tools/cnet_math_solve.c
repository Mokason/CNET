/* CLI: creative math solve (phases 1–4).
 *   bin/cnet_math_solve "legs 3 and 4 hypotenuse"
 *   bin/cnet_math_solve "Is 17 a prime number?"
 *   bin/cnet_math_solve "x^2 - 5x + 6 = 0"
 */
#include <stdio.h>
#include <string.h>

#include "../include/cnet_math_solve.h"

int main(int argc, char **argv) {
    CnetMathSolveConfig cfg;
    CnetMathSolveReport rep;
    const char *q = NULL;
    int i, rc;

    cnet_math_solve_config_from_env(&cfg);
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--no-creative") == 0)
            cfg.allow_creative = 0;
        else if (strcmp(argv[i], "--no-skill") == 0)
            cfg.write_skill = 0;
        else if (argv[i][0] != '-')
            q = argv[i];
    }
    if (!q) {
        fprintf(stderr, "usage: %s [--no-creative] [--no-skill] \"question\"\n", argv[0]);
        return 2;
    }
    rc = cnet_math_solve(q, &cfg, &rep);
    if (rc != 0) {
        fprintf(stderr, "math_solve: abstain (%s)\n", rep.detail);
        return 1;
    }
    printf("tier=%s method=%s verified=%d plans=%d skill_written=%d\n",
           cnet_math_tier_name(rep.tier), rep.method, rep.verified, rep.plans_tried,
           rep.skill_written);
    printf("steps=%s\n", rep.steps);
    printf("answer=%s\n", rep.answer);
    if (rep.skill_path[0]) printf("skill=%s\n", rep.skill_path);
    printf("CNET_MATH_SOLVE_OK\n");
    return 0;
}
