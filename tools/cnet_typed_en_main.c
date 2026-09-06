/* CLI + gate for typed English student.
 *   ./bin/cnet_typed_en --test
 *   ./bin/cnet_typed_en "past tense of go"
 */
#include <stdio.h>
#include <string.h>

#include "../include/cnet_typed_en.h"

int main(int argc, char **argv) {
    const char *q = NULL;
    int i, do_test = 0;
    CnetTeResult r;
    int c;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--test") == 0)
            do_test = 1;
        else if (argv[i][0] != '-')
            q = argv[i];
    }
    if (do_test) return cnet_te_selftest();
    if (!q) {
        fprintf(stderr, "usage: %s [--test] [\"query\"]\n", argv[0]);
        return 2;
    }
    cnet_te_init();
    if (cnet_te_ask(q, &r) != 0) {
        printf("rc=-1\nTYPED_EN_ERR\n");
        return 1;
    }
    printf("n_cand=%d claimed_cert=%d any_gap=%d audit_ok=%d\n", r.n_cand,
           r.claimed_cert, r.any_gap, cnet_te_audit_ok(&r));
    for (c = 0; c < r.n_cand; c++)
        printf("  cand%d %s\n", c, r.cand[c].show);
    printf("show:\n%s\n", r.show);
    return r.claimed_cert ? 0 : 1;
}
