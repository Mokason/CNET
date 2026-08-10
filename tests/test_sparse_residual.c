/* Slice 4: same-domain residual ACCUM (Brain LINK_ACCUM).
 * make sparse_residual -> SPARSE_RESIDUAL_PASS
 *
 * y = peak(x) + alpha * residual(x)  // same x, CERT both, coverage fail-closed
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/cnet_sparse_serve.h"
#include "../include/contract/contract.h"
#include "../include/hybrid_ai.h"
#include "../include/nn.h"
#include "../include/router.h"
#include "../include/specialist.h"

#define DIM 4
#define ALPHA 0.65

static int failures, checks;
static void check(int ok, const char *m) {
    checks++;
    printf("  %-60s %s\n", m, ok ? "PASS" : "FAIL");
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

/* Admit identity-like unit: in e_i -> out e_i (peak) or e_{(i+1)%n} (resid). */
static int admit_map(PrimitiveRegistry *reg, HybridAi *h, const char *name,
                     Port pin, Port pout, int shift) {
    BinaryTransformNetwork *btn;
    double in[DIM][DIM], tg[DIM][DIM];
    Contract c;
    Specialist s;
    int i, j;

    for (i = 0; i < DIM; i++) {
        oh(in[i], i);
        oh(tg[i], (i + shift) % DIM);
    }
    btn = (BinaryTransformNetwork *)calloc(1, sizeof *btn);
    if (!btn) return -1;
    if (btn_init(btn, DIM, DIM, 16, 64, 0.5, 7u + (unsigned)shift) != 0) {
        free(btn);
        return -1;
    }
    if (btn_set_ports(btn, pin, pout) != 0) {
        btn_free(btn);
        free(btn);
        return -1;
    }
    btn_train_dynamic(btn, (const double *)in, (const double *)tg, DIM, 20000, 200, 1e-6,
                      1e-8);
    btn_train(btn, (const double *)in, (const double *)tg, DIM, 6000);
    memset(&c, 0, sizeof c);
    if (contract_init_borrowed(&c, name, btn, (const double *)in, (const double *)tg,
                               DIM) != 0) {
        btn_free(btn);
        free(btn);
        return -1;
    }
    memset(&s, 0, sizeof s);
    if (specialist_wrap_btn(&s, btn, name) != 0 || specialist_admit(reg, &s, &c) != 0) {
        contract_free(&c);
        btn_free(btn);
        free(btn);
        return -1;
    }
    if (hybrid_coverage_record(h, pin, pout, name, (const double *)in, (const double *)tg,
                               DIM, DIM, DIM) != 0) {
        contract_free(&c);
        return -1;
    }
    contract_free(&c);
    (void)j;
    return 0;
}

int main(void) {
    HybridAi h;
    PrimitiveRegistry reg;
    Port pin = P("acc_in"), pout = P("acc_out");
    double x[DIM], y[DIM], y_peak[DIM];
    int used = 0, rc;

    failures = checks = 0;
    printf("== CNET sparse residual ACCUM (same-domain) ==\n");

    hybrid_ai_init(&h);
    registry_init(&reg);

    check(admit_map(&reg, &h, "peak_id", pin, pout, 0) == 0, "admit peak (identity)");
    check(admit_map(&reg, &h, "resid_shift", pin, pout, 1) == 0, "admit residual (shift+1)");

    oh(x, 0);
    /* peak only */
    rc = cnet_sparse_run_accum(&h, &reg, pin, pout, "peak_id", NULL, x, DIM, y_peak, DIM,
                               ALPHA, &used);
    check(rc == 0 && used == 0, "peak-only run");
    check(y_peak[0] > 0.9, "peak ~ e0");

    /* peak + residual ACCUM */
    used = 0;
    rc = cnet_sparse_run_accum(&h, &reg, pin, pout, "peak_id", "resid_shift", x, DIM, y, DIM,
                               ALPHA, &used);
    check(rc == 0 && used == 1, "accum run used residual");
    /* y ≈ e0 + 0.65 * e1 */
    check(y[0] > 0.85 && y[1] > 0.4 && y[1] < 0.9, "y ≈ e0 + 0.65*e1");
    check(fabs(y[0] - y_peak[0]) < 0.15, "peak component preserved");
    check(y[1] > y_peak[1] + 0.3, "residual added mass on dim1");

    /* OOD input refuses when coverage gated */
    {
        double ood[DIM];
        memset(ood, 0, sizeof ood);
        used = 0;
        rc = cnet_sparse_run_accum(&h, &reg, pin, pout, "peak_id", "resid_shift", ood, DIM, y,
                                   DIM, ALPHA, &used);
        check(rc != 0, "OOD accum refused (fail-closed)");
    }

    /* Missing residual name refuses */
    oh(x, 1);
    rc = cnet_sparse_run_accum(&h, &reg, pin, pout, "peak_id", "no_such_unit", x, DIM, y, DIM,
                               ALPHA, &used);
    check(rc != 0, "missing residual refuses");

    /* Unknown peak refuses */
    rc = cnet_sparse_run_accum(&h, &reg, pin, pout, "no_peak", NULL, x, DIM, y, DIM, ALPHA,
                               &used);
    check(rc != 0, "missing peak refuses");

    hybrid_ai_free(&h);
    registry_free(&reg);

    printf("\nchecks=%d failures=%d\n", checks, failures);
    if (failures == 0) {
        printf("SPARSE_RESIDUAL_PASS\n");
        return 0;
    }
    printf("SPARSE_RESIDUAL_FAIL\n");
    return 1;
}
