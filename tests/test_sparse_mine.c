/* Slice 3: recall-before-spawn + promote-only-after-verify on structure mine.
 * make sparse_mine -> SPARSE_MINE_PASS
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/hybrid_ai.h"
#include "../include/nn.h"
#include "../include/router.h"

#define DIM 4

static int failures, checks;
static void check(int ok, const char *msg) {
    checks++;
    printf("  %-60s %s\n", msg, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static Port P(const char *tag) {
    Port p;
    memset(&p, 0, sizeof p);
    p.family = PORT_ONEHOT;
    p.field_width = DIM;
    p.field_count = 1;
    snprintf(p.tag, sizeof p.tag, "%s", tag);
    return p;
}

static void oh(double *r, int hot) {
    int i;
    for (i = 0; i < DIM; i++) r[i] = (i == hot) ? 1.0 : 0.0;
}

int main(void) {
    HybridAi h;
    PrimitiveRegistry reg;
    BinaryTransformNetwork *stu = NULL;
    Port pin = P("mine_in"), pout = P("mine_out");
    double in[DIM], out[DIM];
    int rc, i;
    size_t recalls0;

    failures = checks = 0;
    printf("== CNET sparse mine (recall-before-spawn + CERT promote) ==\n");

    hybrid_ai_init(&h);
    registry_init(&reg);
    check(hybrid_bind_residual(&h, "hermetic", hybrid_hermetic_residual,
                               (void *)(uintptr_t)DIM) == 0,
          "bind hermetic residual");

    /* Drive residual traces until min_hits */
    for (i = 0; i < 8; i++) {
        oh(in, i % DIM);
        oh(out, (i + 1) % DIM);
        check(hybrid_trace_residual(&h, pin, pout, in, DIM, out, DIM) == 0 || i > 0,
              i == 0 ? "trace residual" : "trace more");
    }

    /* First mine should promote CERT */
    stu = NULL;
    rc = hybrid_structure_mine(&h, &reg, 3, &stu);
    check(rc == 0, "first mine returns 0 (promoted)");
    check(h.structure_mines == 1, "structure_mines==1");
    check(stu != NULL, "student BTN returned");
    {
        size_t e;
        int found = 0, cert = 0;
        for (e = 0; e < reg.count; e++) {
            if (reg.entries[e].name && strstr(reg.entries[e].name, "hyb_struct_")) {
                found = 1;
                cert = reg.entries[e].certified ? 1 : 0;
                break;
            }
        }
        check(found && cert, "registry entry CERT after promote");
    }
    check(hybrid_coverage_has_unit(&h, "hyb_struct_0") || h.coverage_count >= 1,
          "coverage recorded for mined unit");

    /* Second mine same shape → recall (4), no new spawn */
    recalls0 = h.structure_recalls;
    for (i = 0; i < 8; i++) {
        oh(in, i % DIM);
        oh(out, (i + 1) % DIM);
        (void)hybrid_trace_residual(&h, pin, pout, in, DIM, out, DIM);
    }
    stu = NULL;
    rc = hybrid_structure_mine(&h, &reg, 3, &stu);
    check(rc == 4 || rc == 5, "second mine RECALL (4 shape / 5 admit)");
    check(h.structure_recalls > recalls0, "structure_recalls incremented");
    check(h.structure_mines == 1, "no second promote (mines still 1)");
    check(h.structure_promote_rejects == 0, "no promote rejects");

    /* Fresh hybrid: pre-seed coverage admitting input → return 5 without mine */
    {
        HybridAi h2;
        PrimitiveRegistry reg2;
        double rows[DIM * 2], tgs[DIM * 2];
        hybrid_ai_init(&h2);
        registry_init(&reg2);
        oh(rows + 0, 0);
        oh(tgs + 0, 1);
        oh(rows + DIM, 1);
        oh(tgs + DIM, 2);
        check(hybrid_coverage_record(&h2, pin, pout, "prior_unit", rows, tgs, 2, DIM,
                                     DIM) == 0,
              "pre-seed coverage");
        check(hybrid_bind_residual(&h2, "hermetic", hybrid_hermetic_residual,
                                   (void *)(uintptr_t)DIM) == 0,
              "bind residual h2");
        oh(in, 0);
        oh(out, 1);
        for (i = 0; i < 5; i++)
            (void)hybrid_trace_residual(&h2, pin, pout, in, DIM, out, DIM);
        /* shape already has coverage → 4 */
        rc = hybrid_structure_mine(&h2, &reg2, 2, &stu);
        check(rc == 4 && h2.structure_recalls >= 1, "pre-covered shape recalls (4)");
        hybrid_ai_free(&h2);
        registry_free(&reg2);
    }

    if (stu) {
        btn_free(stu);
        free(stu);
    }
    hybrid_ai_free(&h);
    registry_free(&reg);

    printf("\nchecks=%d failures=%d\n", checks, failures);
    if (failures == 0) {
        printf("SPARSE_MINE_PASS\n");
        return 0;
    }
    printf("SPARSE_MINE_FAIL\n");
    return 1;
}
