/* test_coverage_owner_seal — sealing must bind the OWNER's rows, not the
 * shape's first record.
 *
 * WHY THIS EXISTS. Phase 2 made coverage identity OWNER plus exact interface,
 * so two specialists behind one typed interface can coexist, each with its own
 * certified rows. `hybrid_coverage_record` was moved onto the owner-keyed
 * lookup — but two functions were left on the shape-only one:
 *
 *   hybrid_seal_mined_unit()  picked whichever record for the interface came
 *                             first and certified the unit against ITS rows.
 *                             With owners A and B on one interface, sealing B
 *                             could bind A's rows and A's contract NAME, which
 *                             is a unit certified on a domain it was never
 *                             trained on, carrying someone else's identity.
 *   hybrid_coverage_owner()   returned the first of several owners with no
 *                             signal that the answer was a coin flip.
 *
 * The fixture is the smallest thing that shows it: two units on ONE interface
 * with different rows, different targets and different contract names.
 *
 * Everything is written under a mkdtemp root. No real base, no runtime state.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../include/base.h"
#include "../include/contract/contract.h"
#include "../include/hybrid_ai.h"
#include "../include/nn.h"

#define SYM 4

static int failures;
static int checks;

static void check(int condition, const char *message) {
    checks++;
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
}

static char dir_template[] = "/tmp/cnet-ownerseal-XXXXXX";

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

/* Train a BTN implementing `map` over the whole one-hot domain. */
static int train_unit(BinaryTransformNetwork *btn, Port in_port, Port out_port,
                      const int *map) {
    double in[SYM][SYM], target[SYM][SYM];
    size_t i;
    for (i = 0; i < SYM; i++) {
        one_hot(in[i], (int)i);
        one_hot(target[i], map[i]);
    }
    memset(btn, 0, sizeof *btn);
    if (btn_init(btn, SYM, SYM, 16, 64, 0.5, 20260730u) != 0) return -1;
    btn_set_ports(btn, in_port, out_port);
    btn_train_dynamic(btn, (const double *)in, (const double *)target, SYM,
                      12000, 200, 1e-6, 1e-8);
    btn_train(btn, (const double *)in, (const double *)target, SYM, 3000);
    return 0;
}

int main(void) {
    CnetBase base;
    HybridAi cov;
    BinaryTransformNetwork unit_a, unit_b;
    char *scratch, base_path[600];
    Port pin = make_port("ownerseal_in");
    Port pout = make_port("ownerseal_out");
    /* Two DIFFERENT functions on the same interface. */
    const int map_a[SYM] = {0, 1, 2, 3};   /* identity */
    const int map_b[SYM] = {1, 2, 3, 0};   /* rotate   */
    double rows_a[SYM][SYM], targets_a[SYM][SYM];
    double rows_b[2][SYM], targets_b[2][SYM];
    const char *name_a = HYBRID_MINED_UNIT_PREFIX "ownera";
    const char *name_b = HYBRID_MINED_UNIT_PREFIX "ownerb";
    size_t i;
    int rc;

    setvbuf(stdout, NULL, _IONBF, 0);
    scratch = mkdtemp(dir_template);
    if (!scratch) {
        fprintf(stderr, "FAIL: cannot create scratch directory\n");
        return 1;
    }
    snprintf(base_path, sizeof base_path, "%s/owner.cnb", scratch);

    for (i = 0; i < SYM; i++) {
        one_hot(rows_a[i], (int)i);
        one_hot(targets_a[i], map_a[i]);
    }
    /* B is certified on only TWO rows, and on the rotate mapping. If a seal
       binds A's four identity rows to B, both the row count and the targets
       give it away. */
    for (i = 0; i < 2; i++) {
        one_hot(rows_b[i], (int)i);
        one_hot(targets_b[i], map_b[i]);
    }

    cnb_init(&base);
    hybrid_ai_init(&cov);

    check(hybrid_coverage_record(&cov, pin, pout, name_a, (const double *)rows_a,
                                 (const double *)targets_a, SYM, SYM, SYM) == 0,
          "owner A records coverage on the interface");
    check(hybrid_coverage_record(&cov, pin, pout, name_b, (const double *)rows_b,
                                 (const double *)targets_b, 2, SYM, SYM) == 0,
          "owner B records coverage on the SAME interface");
    check(hybrid_coverage_owner_count(&cov, pin, pout) == 2,
          "two owners coexist on one interface");

    /* --- the shape-only owner lookup must refuse to guess --------------- */
    check(hybrid_coverage_owner(&cov, pin, pout) == NULL,
          "a shape-only owner lookup must FAIL CLOSED while two owners share "
          "the interface, not return whichever came first");

    /* --- sealing B must bind B ------------------------------------------ */
    if (train_unit(&unit_b, pin, pout, map_b) != 0) {
        fprintf(stderr, "FAIL: cannot train unit B\n");
        return 1;
    }
    rc = hybrid_seal_mined_unit_owned(&cov, &base, &unit_b, name_b, NULL);
    check(rc == 0, "sealing B against its own owner record succeeds");
    check(cnb_has_unit(&base, name_b),
          "the sealed unit carries B's name, not A's");
    check(!cnb_has_unit(&base, name_a),
          "sealing B must not admit a unit under A's name");

    /* B was certified on 2 rows; A on 4. A seal that grabbed A's record would
       certify B over four exemplars it was never given. */
    {
        BinaryTransformNetwork got;
        Contract out;
        memset(&got, 0, sizeof got);
        memset(&out, 0, sizeof out);
        if (cnb_get_unit(&base, name_b, &got, &out) == 0) {
            check(out.exemplar_count == 2,
                  "B is certified on B's two rows, not on A's four");
            printf("OWNERSEAL_CONTRACT unit=%s exemplars=%zu\n", name_b,
                   (size_t)out.exemplar_count);
            contract_free(&out);
            btn_free(&got);
        } else {
            check(0, "the sealed unit for B can be read back");
        }
    }

    /* --- A is untouched -------------------------------------------------- */
    check(hybrid_coverage_binds_unit(&cov, name_a, pin, pout, SYM) == 1,
          "owner A's coverage record still binds A after B was sealed");
    check(hybrid_coverage_owner_count(&cov, pin, pout) == 2,
          "sealing B did not evict A");

    /* --- an owner with no record cannot seal ----------------------------- */
    rc = hybrid_seal_mined_unit_owned(&cov, &base, &unit_b,
                                      HYBRID_MINED_UNIT_PREFIX "ghost", NULL);
    check(rc == 1,
          "an owner with no coverage record has nothing to seal from");

    /* --- the shape-only seal refuses while ambiguous --------------------- */
    rc = hybrid_seal_mined_unit(&cov, &base, &unit_b, NULL);
    check(rc < 0,
          "the shape-only seal must REFUSE while two owners share the "
          "interface rather than certifying against a coin flip");

    /* --- and works once the shape has exactly one owner ------------------ */
    check(hybrid_coverage_forget_unit(&cov, name_a) == 1,
          "owner A's record is removed");
    check(hybrid_coverage_owner_count(&cov, pin, pout) == 1,
          "one owner remains");
    check(hybrid_coverage_owner(&cov, pin, pout) != NULL &&
              strcmp(hybrid_coverage_owner(&cov, pin, pout), name_b) == 0,
          "with one owner the shape-only lookup resolves it");
    if (train_unit(&unit_a, pin, pout, map_b) == 0) {
        rc = hybrid_seal_mined_unit(&cov, &base, &unit_a, NULL);
        check(rc == 0,
              "the shape-only seal works once the shape is unambiguous");
        btn_free(&unit_a);
    }

    check(cnb_save(&base, base_path) == 0, "the fixture base saves");

    btn_free(&unit_b);
    hybrid_ai_free(&cov);
    cnb_free(&base);
    (void)unlink(base_path);
    (void)rmdir(scratch);

    if (failures) {
        printf("COVERAGE_OWNER_SEAL_FAIL checks=%d failures=%d\n", checks,
               failures);
        return 1;
    }
    printf("COVERAGE_OWNER_SEAL_PASS checks=%d owners=2 seal=owner_keyed "
           "shape_only=refuses_ambiguity\n", checks);
    return 0;
}
