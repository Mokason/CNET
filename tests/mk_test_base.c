/* mk_test_base — build a throwaway CNB base (and optional coverage sidecar)
 * for the health and coverage negatives.
 *
 * Several gates need "a base that really loads" in order to prove what happens
 * when the state AROUND it is wrong: no coverage sidecar under an armed gate,
 * a truncated sidecar, a mined unit whose guard is gone. Hand-writing container
 * bytes would test the fixture rather than the loader, so this builds one
 * through the ordinary cnb_ and hybrid_coverage_ APIs.
 *
 * Usage: mk_test_base <out.cnb> [--mined|--plain] [--coverage <path>]
 *   --mined     name the unit with the structure-mined prefix, so it must
 *               carry coverage to be servable. Default.
 *   --plain     hand-admitted unit instead.
 *   --coverage  also write a VALID sidecar for the unit at <path>. Corruption
 *               fixtures are produced by mutating that file, so the mutation is
 *               always a one-byte-level difference from something that loads.
 *
 * It writes only where it is told. Callers pass mkdtemp paths; nothing here
 * touches a real base, the repository, or any runtime state.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/base.h"
#include "../include/hybrid_ai.h"
#include "../include/nn.h"
#include "../include/contract/contract.h"

#define SYM 4

static Port make_port(const char *tag) {
    Port p;
    memset(&p, 0, sizeof p);
    p.family = PORT_ONEHOT;
    p.field_width = SYM;
    p.field_count = 1;
    snprintf(p.tag, sizeof p.tag, "%s", tag);
    return p;
}

static void one_hot(double *row, int hot) {
    int i;
    for (i = 0; i < SYM; i++) row[i] = (i == hot) ? 1.0 : 0.0;
}

int main(int argc, char **argv) {
    CnetBase base;
    HybridAi hybrid;
    BinaryTransformNetwork btn;
    Contract contract;
    Port pin, pout;
    double in[SYM][SYM], target[SYM][SYM];
    char name[CNB_NAME_MAX];
    const char *coverage_path = NULL;
    int mined = 1;
    int i, a, rc = 1;

    if (argc < 2) {
        fprintf(stderr,
                "usage: %s <out.cnb> [--mined|--plain] [--coverage <path>]\n",
                argv[0]);
        return 2;
    }
    for (a = 2; a < argc; a++) {
        if (!strcmp(argv[a], "--mined")) mined = 1;
        else if (!strcmp(argv[a], "--plain")) mined = 0;
        else if (!strcmp(argv[a], "--coverage") && a + 1 < argc)
            coverage_path = argv[++a];
        else {
            fprintf(stderr, "unknown option %s\n", argv[a]);
            return 2;
        }
    }

    if (mined)
        snprintf(name, sizeof name, "%shealthfix", HYBRID_MINED_UNIT_PREFIX);
    else
        snprintf(name, sizeof name, "healthfix_plain");

    /* Tags are deliberately >=2 edits apart: tag governance refuses near-miss
       mints, so "hf_in"/"hf_out" would be rejected as a collision family. */
    pin = make_port("hfixture_input");
    pout = make_port("hfixture_result");

    for (i = 0; i < SYM; i++) {
        one_hot(in[i], i);
        one_hot(target[i], (i + 1) % SYM);
    }

    memset(&btn, 0, sizeof btn);
    memset(&contract, 0, sizeof contract);
    cnb_init(&base);
    hybrid_ai_init(&hybrid);

    if (btn_init(&btn, SYM, SYM, 16, 64, 0.5, 20260730u) != 0) {
        fprintf(stderr, "mk_test_base: btn_init failed\n");
        goto done;
    }
    btn_set_ports(&btn, pin, pout);
    btn_train_dynamic(&btn, (const double *)in, (const double *)target, SYM,
                      12000, 200, 1e-6, 1e-8);
    btn_train(&btn, (const double *)in, (const double *)target, SYM, 3000);

    if (contract_init_borrowed(&contract, name, &btn, (const double *)in,
                               (const double *)target, SYM) != 0) {
        fprintf(stderr, "mk_test_base: contract_init_borrowed failed\n");
        goto done;
    }
    if (cnb_add_unit(&base, &btn, &contract, NULL) != 0) {
        fprintf(stderr, "mk_test_base: cnb_add_unit failed\n");
        goto done;
    }
    if (cnb_save(&base, argv[1]) != 0) {
        fprintf(stderr, "mk_test_base: cnb_save failed for %s\n", argv[1]);
        goto done;
    }

    if (coverage_path) {
        if (hybrid_coverage_record(&hybrid, pin, pout, name, (const double *)in,
                                   (const double *)target, SYM, SYM, SYM) != 0) {
            fprintf(stderr, "mk_test_base: hybrid_coverage_record failed\n");
            goto done;
        }
        if (hybrid_coverage_save(&hybrid, coverage_path) != 0) {
            fprintf(stderr, "mk_test_base: hybrid_coverage_save failed\n");
            goto done;
        }
        printf("MK_TEST_BASE_COVERAGE path=%s rows=%d\n", coverage_path, SYM);
    }

    printf("MK_TEST_BASE_OK path=%s unit=%s mined=%d\n", argv[1], name, mined);
    rc = 0;

done:
    contract_free(&contract);
    btn_free(&btn);
    hybrid_ai_free(&hybrid);
    cnb_free(&base);
    return rc;
}
