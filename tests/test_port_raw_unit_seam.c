/* Where a continuous PORT_RAW specialist stops being portable.
 *
 * A visual specialist's head takes continuous features (PCA-projected HOG), so
 * its contract exemplars are arbitrary doubles. The contract layer accepts
 * that: contract.c exempts PORT_RAW from the canonical 0/1 check. The CNU unit
 * serialization does not - unit_save_mem bit-packs every exemplar value and
 * refuses anything that is not exactly 0.0 or 1.0, with no PORT_RAW branch.
 *
 * The consequence is not cosmetic: a PORT_RAW unit with real continuous
 * exemplars cannot enter a CnetBase, so it cannot be exported as a capsule at
 * all. This fixture pins that boundary so it is a known, tested seam rather
 * than a surprise, and so a future format change has something to flip.
 *
 * make port_raw_unit_seam -> PORT_RAW_UNIT_SEAM_PASS
 */
#include <stdio.h>
#include <string.h>

#include "../include/base.h"
#include "../include/nn.h"
#include "../include/contract/contract.h"

#define D 8

static int failures, checks;
static void check(int ok, const char *name) {
    checks++;
    printf("  %-64s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static Port mk(int family, const char *tag, size_t w) {
    Port p;
    memset(&p, 0, sizeof p);
    p.family = (PortFamily)family;
    p.field_width = w;
    p.field_count = 1;
    snprintf(p.tag, sizeof p.tag, "%s", tag);
    return p;
}

int main(void) {
    printf("== PORT_RAW continuous unit: contract vs CNU seam ==\n");

    /* continuous PORT_RAW exemplars: what a real visual head is trained on */
    {
        CnetBase b;
        BinaryTransformNetwork btn;
        Contract c;
        double x[2 * D], y[2 * 2];
        size_t i;
        for (i = 0; i < 2 * D; i++) x[i] = 0.37 * (double)(i + 1) - 1.5;  /* continuous */
        y[0] = 1; y[1] = 0; y[2] = 0; y[3] = 1;

        cnb_init(&b);
        memset(&btn, 0, sizeof btn);
        check(btn_init(&btn, D, 2, 8, 32, 0.5, 5u) == 0, "raw: btn_init");
        btn_set_ports(&btn, mk(PORT_RAW, "raw_in", D), mk(PORT_ONEHOT, "raw_goal", 2));
        btn_train_dynamic(&btn, x, y, 2, 2000, 100, 1e-6, 1e-8);

        memset(&c, 0, sizeof c);
        check(contract_init_borrowed(&c, "raw_unit", &btn, x, y, 2) == 0,
              "raw: the contract layer ACCEPTS continuous PORT_RAW exemplars");
        check(cnb_add_unit(&b, &btn, &c, NULL) != 0,
              "raw: cnb_add_unit REFUSES it (CNU exemplars are bit-packed 0/1)");
        check(b.unit_count == 0, "raw: the base is left unmutated by the refusal");
        contract_free(&c);
        btn_free(&btn);
        cnb_free(&b);
    }

    /* the same shape with canonical 0/1 exemplars serializes fine, so the
       refusal is about exemplar VALUES, not about PORT_RAW or the topology */
    {
        CnetBase b;
        BinaryTransformNetwork btn;
        Contract c;
        double x[2 * D], y[2 * 2];
        size_t i;
        for (i = 0; i < 2 * D; i++) x[i] = (i % 3 == 0) ? 1.0 : 0.0;
        y[0] = 1; y[1] = 0; y[2] = 0; y[3] = 1;

        cnb_init(&b);
        memset(&btn, 0, sizeof btn);
        check(btn_init(&btn, D, 2, 8, 32, 0.5, 5u) == 0, "binary: btn_init");
        btn_set_ports(&btn, mk(PORT_RAW, "raw_in", D), mk(PORT_ONEHOT, "raw_goal", 2));
        btn_train_dynamic(&btn, x, y, 2, 2000, 100, 1e-6, 1e-8);
        memset(&c, 0, sizeof c);
        check(contract_init_borrowed(&c, "bin_unit", &btn, x, y, 2) == 0, "binary: contract");
        check(cnb_add_unit(&b, &btn, &c, NULL) == 0,
              "binary: the identical PORT_RAW topology DOES serialize with 0/1 exemplars");
        check(b.unit_count == 1, "binary: unit present in the base");
        contract_free(&c);
        btn_free(&btn);
        cnb_free(&b);
    }

    printf("checks=%d failures=%d\n", checks, failures);
    if (failures == 0) { printf("PORT_RAW_UNIT_SEAM_PASS checks=%d\n", checks); return 0; }
    printf("PORT_RAW_UNIT_SEAM_FAIL failures=%d\n", failures);
    return 1;
}
