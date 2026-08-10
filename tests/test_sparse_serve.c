/* Sparse serve policy: center-rank among coverage-admitting units.
 * make sparse_serve -> SPARSE_SERVE_PASS
 *
 * Slice 2 of plans/cnet_sparse_hash_board_laws_20260801.md
 * Fail-closed: OOD input → no pick. No floor lowering.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/cnet_sparse_serve.h"
#include "../include/hybrid_ai.h"
#include "../include/nn.h"

#define SYM 4

static int failures, checks;

static void check(int ok, const char *name) {
    checks++;
    printf("  %-60s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static Port P(const char *tag) {
    Port p;
    memset(&p, 0, sizeof p);
    p.family = PORT_ONEHOT;
    p.field_width = SYM;
    p.field_count = 1;
    snprintf(p.tag, sizeof p.tag, "%s", tag);
    return p;
}

static void oh(double *r, int hot) {
    int i;
    for (i = 0; i < SYM; i++) r[i] = (i == hot) ? 1.0 : 0.0;
}

int main(void) {
    HybridAi h;
    Port pin = P("sp_in");
    Port poutA = P("sp_out_A");
    Port poutB = P("sp_out_B");
    Port pout_wild;
    double inA[SYM * 2], tgA[SYM * 2];
    double inB[SYM * 2], tgB[SYM * 2];
    double probe[SYM], ood[SYM];
    char name[64];
    double score = -1.0;
    CnetSparseCandidate cand[8];
    size_t n = 0;
    int rc;

    failures = checks = 0;
    printf("== CNET sparse serve (center-rank CERT coverage) ==\n");

    hybrid_ai_init(&h);
    memset(&pout_wild, 0, sizeof pout_wild);
    pout_wild.family = PORT_ONEHOT;
    pout_wild.field_width = SYM;
    pout_wild.field_count = 1;
    /* empty tag = wildcard out */

    /* Unit A certified on modes 0,1 — own goal tag (coverage is per port shape) */
    oh(inA + 0 * SYM, 0);
    oh(tgA + 0 * SYM, 0);
    oh(inA + 1 * SYM, 1);
    oh(tgA + 1 * SYM, 1);
    check(hybrid_coverage_record(&h, pin, poutA, "unit_A", inA, tgA, 2, SYM, SYM) == 0,
          "record unit_A coverage");

    /* Unit B certified on modes 2,3 */
    oh(inB + 0 * SYM, 2);
    oh(tgB + 0 * SYM, 2);
    oh(inB + 1 * SYM, 3);
    oh(tgB + 1 * SYM, 3);
    check(hybrid_coverage_record(&h, pin, poutB, "unit_B", inB, tgB, 2, SYM, SYM) == 0,
          "record unit_B coverage");

    /* Probe mode 0 with wildcard out → A */
    oh(probe, 0);
    rc = cnet_sparse_pick_covered_unit(&h, pin, pout_wild, probe, SYM, name, sizeof name,
                                       &score);
    check(rc == 0, "pick admits for mode0");
    check(strcmp(name, "unit_A") == 0, "mode0 → unit_A");
    check(score == 0.0, "exact row score 0");

    /* Probe mode 3 → B */
    oh(probe, 3);
    rc = cnet_sparse_pick_covered_unit(&h, pin, pout_wild, probe, SYM, name, sizeof name,
                                       &score);
    check(rc == 0 && strcmp(name, "unit_B") == 0, "mode3 → unit_B");

    /* OOD: all-zero not in any certified set → abstain */
    memset(ood, 0, sizeof ood);
    rc = cnet_sparse_pick_covered_unit(&h, pin, pout_wild, ood, SYM, name, sizeof name, &score);
    check(rc != 0, "OOD abstain (fail-closed)");

    /* Rank: mode 1 only A admits */
    oh(probe, 1);
    rc = cnet_sparse_rank_covered(&h, pin, pout_wild, probe, SYM, cand, 8, &n);
    check(rc == 0 && n == 1 && strcmp(cand[0].unit, "unit_A") == 0, "rank single admit A");

    /* Center of A is mean of e0 and e1 */
    {
        double ctr[SYM];
        check(cnet_sparse_unit_center(&h, "unit_A", ctr, SYM) == 0, "center A");
        check(ctr[0] > 0.4 && ctr[0] < 0.6 && ctr[1] > 0.4 && ctr[1] < 0.6, "center A ~ avg");
    }

    /* Exact out port still works */
    oh(probe, 2);
    check(hybrid_coverage_admits(&h, pin, poutB, probe, SYM) == 1, "shape admits mode2 on B");
    check(cnet_sparse_pick_covered_unit(&h, pin, poutB, probe, SYM, name, sizeof name, NULL) == 0 &&
              strcmp(name, "unit_B") == 0,
          "pick with exact out port B");

    hybrid_ai_free(&h);

    printf("\nchecks=%d failures=%d\n", checks, failures);
    if (failures == 0) {
        printf("SPARSE_SERVE_PASS\n");
        return 0;
    }
    printf("SPARSE_SERVE_FAIL\n");
    return 1;
}
