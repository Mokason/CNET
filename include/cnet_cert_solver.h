#ifndef CNET_CERT_SOLVER_H
#define CNET_CERT_SOLVER_H

/*
 * Same-turn CERT solver loop.
 *
 * Apply existing operators (add/sub/mul/div/mod) until bind or budget.
 * Not FAQ. Not an LM. SHOW every hop. claimed_cert=1 only on bind.
 *
 * Grammar:
 *   gcd <a> <b>
 *   make <target> from n1 n2 ...
 *   countdown <target> n1 n2 ...
 */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CNET_CERT_SOLVER_SKILL "cert_solver_v1"
#define CNET_CERT_SOLVER_MAX_HOPS 16
#define CNET_CERT_SOLVER_MAX_NUMS 6
#define CNET_CERT_SOLVER_MAX_NODES 80000
#define CNET_CERT_SOLVER_SHOW 2048
#define CNET_CERT_SOLVER_SPOKEN 1024

typedef struct {
    int hit;          /* grammar matched */
    int bound;        /* operator loop found a value */
    int budget_trip;  /* node budget exhausted */
    int claimed_cert; /* 1 iff bound */
    int n_hops;
    int n_nodes;
    char value[64];
    char show[CNET_CERT_SOLVER_SHOW];
    char spoken[CNET_CERT_SOLVER_SPOKEN];
} CnetCertSolverResult;

/* 1 if this turn is solver-shaped (front-owns). */
int cnet_cert_solver_shaped(const char *turn);

/* 0 = this organ handled the turn (bind or abstain). 1 = not this organ. */
int cnet_cert_solver_ask(const char *turn, CnetCertSolverResult *out);

int cnet_cert_solver_selftest(void);

#ifdef __cplusplus
}
#endif

#endif /* CNET_CERT_SOLVER_H */
