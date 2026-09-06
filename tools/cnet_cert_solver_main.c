/* CLI + gate for CERT solver loop.
 *   ./bin/cnet_cert_solver --test
 *   ./bin/cnet_cert_solver "make 24 from 8 8 3 3"
 */
#include <stdio.h>
#include <string.h>

#include "../include/cnet_cert_solver.h"

int main(int argc, char **argv) {
    const char *q = NULL;
    int i, do_test = 0;
    CnetCertSolverResult r;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--test") == 0)
            do_test = 1;
        else if (argv[i][0] != '-')
            q = argv[i];
    }
    if (do_test) return cnet_cert_solver_selftest();
    if (!q) {
        fprintf(stderr, "usage: %s [--test] [\"query\"]\n", argv[0]);
        return 2;
    }
    if (cnet_cert_solver_ask(q, &r) != 0) {
        printf("rc=1 hit=0 CERT_SOLVER_MISS\n");
        return 1;
    }
    printf("hit=%d bound=%d claimed_cert=%d n_hops=%d n_nodes=%d budget=%d\n",
           r.hit, r.bound, r.claimed_cert, r.n_hops, r.n_nodes, r.budget_trip);
    printf("value=%s\n%s", r.value[0] ? r.value : "-", r.show);
    return r.claimed_cert ? 0 : 1;
}
