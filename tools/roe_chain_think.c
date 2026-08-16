/* ROE chain-of-thought CLI — pure C (no Python).
 *
 *   ./bin/roe_chain_think "who are you"
 *   ./bin/roe_chain_think --test
 *   ./bin/roe_chain_think --no-act "query"     # skeleton only
 *   ./bin/roe_chain_think --teacher "query"    # allow teacher on miss ACT
 *
 * make roe_chain_think → ROE_CHAIN_THINK_PASS
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/cnet_roe_cot.h"

int main(int argc, char **argv) {
    RoeCotChain C;
    const char *q = NULL;
    int i, do_test = 0, no_act = 0, teacher = 0;
    const char *root = NULL, *gov = NULL;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--test") == 0)
            do_test = 1;
        else if (strcmp(argv[i], "--no-act") == 0)
            no_act = 1;
        else if (strcmp(argv[i], "--teacher") == 0)
            teacher = 1;
        else if (strcmp(argv[i], "--root") == 0 && i + 1 < argc)
            root = argv[++i];
        else if (strcmp(argv[i], "--gov") == 0 && i + 1 < argc)
            gov = argv[++i];
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            fprintf(stderr,
                    "usage: %s [--test] [--no-act] [--teacher] [--root DIR] "
                    "[--gov DIR] \"query\"\n",
                    argv[0]);
            return 2;
        } else if (argv[i][0] != '-')
            q = argv[i];
    }

    if (do_test) return roe_cot_selftest();

    if (!q || !q[0]) {
        fprintf(stderr, "need query or --test\n");
        return 2;
    }

    roe_cot_init(&C);
    if (root || gov) roe_cot_set_paths(&C, root, gov);
    C.run_act = no_act ? 0 : 1;
    C.allow_teacher = teacher ? 1 : 0;
    if (roe_cot_run(&C, q) != 0) {
        fprintf(stderr, "cot run failed\n");
        return 1;
    }
    roe_cot_print_panel(&C);
    (void)roe_cot_persist(&C);
    printf("ROE_CHAIN_THINK_OK\n");
    return 0;
}
