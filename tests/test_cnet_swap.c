/* test_cnet_swap — swap law for certified capsules/chunks.
 *
 * replace when dominate + compositions hold
 * add-alongside when they do not
 * refuse an explicit swap that would break a CERT composition
 * old coverage must be a subset of new (or new matches all old rows)
 *
 * Compression is not a swap signal. Soft routers are not used.
 * Teacher/residual adapters never admit. No 8B compete. No fake PASS.
 */
#include "../include/cnet_swap.h"
#include "../include/nn.h"
#include "../include/router.h"
#include "../include/contract/contract.h"

#include <stdio.h>
#include <string.h>

static int failures;
static int checks;

static void check(int cond, const char *msg) {
    checks++;
    if (cond) {
        printf("  ok   %s\n", msg);
    } else {
        printf("  FAIL %s\n", msg);
        failures++;
    }
}

static Port bit_port(const char *tag) {
    Port p;
    memset(&p, 0, sizeof p);
    p.family = PORT_BINARY_MSB;
    p.field_width = 1;
    p.field_count = 1;
    if (tag != NULL) snprintf(p.tag, sizeof p.tag, "%s", tag);
    return p;
}

/* Deterministic 1-bit maps: identity or invert. */
typedef struct {
    int invert;
    double out;
} BitMap;

static int bit_forward(void *ctx, const double *in, size_t in_n,
                       double *out, size_t out_n) {
    BitMap *m = (BitMap *)ctx;
    if (m == NULL || in == NULL || out == NULL || in_n != 1 || out_n != 1)
        return -1;
    m->out = m->invert ? (in[0] >= 0.5 ? 0.0 : 1.0) : (in[0] >= 0.5 ? 1.0 : 0.0);
    out[0] = m->out;
    return 0;
}

static int make_bit(BinaryTransformNetwork *b, BitMap *map, int invert,
                    unsigned long long digest) {
    Port p = bit_port("bit");
    memset(b, 0, sizeof *b);
    map->invert = invert;
    map->out = 0.0;
    return btn_init_adapter(b, 1, 1, &p, 1, &p, 1,
                            bit_forward, NULL, map, digest, 1);
}

static int train_bit(BinaryTransformNetwork *b, int invert, unsigned seed) {
    double in[2] = {0.0, 1.0};
    double tgt[2];
    Port p = bit_port("bit");
    tgt[0] = invert ? 1.0 : 0.0;
    tgt[1] = invert ? 0.0 : 1.0;
    memset(b, 0, sizeof *b);
    if (btn_init(b, 1, 1, 2, 16, 0.8, seed) != 0) return -1;
    if (btn_set_ports(b, p, p) != 0) return -1;
    if (btn_train_dynamic(b, in, tgt, 2, 80000, 400, 0.0005, 0.01) > 0.02)
        return -1;
    return 0;
}

static void test_coverage_subset(void) {
    double old_in[2] = {0.0, 1.0};
    double old_tg[2] = {0.0, 1.0};
    double new_in[3] = {0.0, 1.0, 0.0};
    double new_tg[3] = {0.0, 1.0, 0.0};
    double narrow_in[1] = {0.0};
    double narrow_tg[1] = {0.0};
    double mismatch_tg[2] = {1.0, 0.0};
    CnetSwapCoverage oldc, newc, narrow, bad;

    printf("coverage subset:\n");
    oldc.inputs = old_in; oldc.targets = old_tg;
    oldc.n_rows = 2; oldc.in_dim = 1; oldc.out_dim = 1;
    newc.inputs = new_in; newc.targets = new_tg;
    newc.n_rows = 2; newc.in_dim = 1; newc.out_dim = 1;
    check(cnet_swap_coverage_subset(&oldc, &newc) == 1,
          "equal tables: old is a subset of new");

    newc.n_rows = 3;
    check(cnet_swap_coverage_subset(&oldc, &newc) == 1,
          "old coverage is a subset of a larger new table");

    narrow.inputs = narrow_in; narrow.targets = narrow_tg;
    narrow.n_rows = 1; narrow.in_dim = 1; narrow.out_dim = 1;
    check(cnet_swap_coverage_subset(&oldc, &narrow) == 0,
          "old is not a subset of a narrower new table");

    bad = newc; bad.n_rows = 2; bad.targets = mismatch_tg;
    check(cnet_swap_coverage_subset(&oldc, &bad) == 0,
          "same inputs with different targets is not a subset");

    {
        CnetSwapCoverage empty = {0};
        check(cnet_swap_coverage_subset(&empty, &newc) == 0,
              "empty old coverage cannot dominate");
    }
}

static void test_dominate_replay(void) {
    BinaryTransformNetwork id, inv;
    BitMap id_map, inv_map;
    double in[2] = {0.0, 1.0};
    double id_tg[2] = {0.0, 1.0};
    CnetSwapCoverage oldc, newc;

    printf("dominate (subset or match old rows):\n");
    check(make_bit(&id, &id_map, 0, 0xA11DULL) == 0 &&
          make_bit(&inv, &inv_map, 1, 0xB0E7ULL) == 0,
          "adapter bricks init");
    oldc.inputs = in; oldc.targets = id_tg;
    oldc.n_rows = 2; oldc.in_dim = 1; oldc.out_dim = 1;
    newc = oldc;
    check(cnet_swap_matches_old_rows(&id, &oldc) == 1,
          "identity matches all old coverage rows");
    check(cnet_swap_matches_old_rows(&inv, &oldc) == 0,
          "invert does not match old coverage rows");
    check(cnet_swap_dominates(&id, &oldc, &newc) == 1,
          "identity dominates (subset and replay)");
    check(cnet_swap_dominates(&inv, &oldc, &newc) == 0,
          "invert does not dominate");
    /* table-only: new table is a proper superset */
    {
        double wide_in[3] = {0.0, 1.0, 0.0};
        double wide_tg[3] = {0.0, 1.0, 0.0};
        CnetSwapCoverage wide;
        wide.inputs = wide_in; wide.targets = wide_tg;
        wide.n_rows = 3; wide.in_dim = 1; wide.out_dim = 1;
        check(cnet_swap_dominates(NULL, &oldc, &wide) == 1,
              "table-only dominate when old rows subset new");
    }
    btn_free(&id);
    btn_free(&inv);
}

static void fill_id_plan(RoutePlan *plan, BinaryTransformNetwork *a,
                         BinaryTransformNetwork *b) {
    memset(plan, 0, sizeof *plan);
    plan->length = 2;
    plan->steps[0] = a;
    plan->steps[1] = b;
    plan->names[0] = "old_bit";
    plan->names[1] = "hop2";
    plan->goal = bit_port("bit");
    plan->strict = 1;
}

static void test_decide_replace_and_alongside(void) {
    BinaryTransformNetwork oldb, new_id, new_inv, hop2;
    BitMap old_map, id_map, inv_map, hop_map;
    double cov_in[2] = {0.0, 1.0};
    double cov_tg[2] = {0.0, 1.0};
    double hop2_rows[1] = {0.0}; /* hop2 certified only on 0 */
    double comp_in[1] = {0.0};
    CnetSwapCoverage oldc, newc, hop2cov;
    CnetSwapComposition comp;
    DagNodeGuard guard;
    CnetSwapReport rep;

    printf("decide replace vs add-alongside:\n");
    check(make_bit(&oldb, &old_map, 0, 0x01D1ULL) == 0 &&
          make_bit(&new_id, &id_map, 0, 0x01D2ULL) == 0 &&
          make_bit(&new_inv, &inv_map, 1, 0x01E7ULL) == 0 &&
          make_bit(&hop2, &hop_map, 0, 0xC0B2ULL) == 0,
          "composition bricks init");

    oldc.inputs = cov_in; oldc.targets = cov_tg;
    oldc.n_rows = 2; oldc.in_dim = 1; oldc.out_dim = 1;
    newc = oldc;
    hop2cov.inputs = hop2_rows; hop2cov.targets = hop2_rows;
    hop2cov.n_rows = 1; hop2cov.in_dim = 1; hop2cov.out_dim = 1;

    memset(&comp, 0, sizeof comp);
    fill_id_plan(&comp.plan, &oldb, &hop2);
    comp.inputs = comp_in;
    comp.n_rows = 1;
    comp.in_dim = 1;

    memset(&guard, 0, sizeof guard);
    guard.allow = cnet_swap_hop_allow;
    guard.ctx = &hop2cov;

    memset(&rep, 0, sizeof rep);
    check(cnet_swap_decide(&oldb, "old_bit", &new_id, &oldc, &newc,
                           &comp, 1, &guard, &rep) == 0 &&
          rep.verdict == CNET_SWAP_REPLACE &&
          rep.dominated == 1 && rep.compositions_hold == 1,
          "dominate + compositions hold => REPLACE");

    memset(&rep, 0, sizeof rep);
    check(cnet_swap_decide(&oldb, "old_bit", &new_inv, &oldc, &newc,
                           &comp, 1, &guard, &rep) == 0 &&
          rep.verdict == CNET_SWAP_ADD_ALONGSIDE &&
          rep.dominated == 0,
          "no dominate => ADD-ALONGSIDE (old brick kept)");

    /* invert maps 0 -> 1; hop2 coverage is only {0} so the guard refuses */
    memset(&rep, 0, sizeof rep);
    check(cnet_swap_compositions_hold(&oldb, "old_bit", &new_inv,
                                      &comp, 1, &guard) == 0,
          "invert swap would fail hop2 coverage guard");

    btn_free(&oldb);
    btn_free(&new_id);
    btn_free(&new_inv);
    btn_free(&hop2);
}

static void test_refuse_swap_breaks_composition(void) {
    BinaryTransformNetwork oldb, new_inv, hop2;
    BitMap old_map, inv_map, hop_map;
    double cov_in[2] = {0.0, 1.0};
    double cov_tg[2] = {0.0, 1.0};
    double hop2_rows[1] = {0.0};
    double comp_in[1] = {0.0};
    CnetSwapCoverage oldc, newc, hop2cov;
    CnetSwapComposition comp;
    DagNodeGuard guard;
    CnetSwapReport rep;
    PrimitiveRegistry reg;
    Contract c;

    printf("refuse swap that would break a CERT composition:\n");
    check(make_bit(&oldb, &old_map, 0, 0x0A11ULL) == 0 &&
          make_bit(&new_inv, &inv_map, 1, 0x0A17ULL) == 0 &&
          make_bit(&hop2, &hop_map, 0, 0x0A12ULL) == 0,
          "refuse-swap bricks init");

    oldc.inputs = cov_in; oldc.targets = cov_tg;
    oldc.n_rows = 2; oldc.in_dim = 1; oldc.out_dim = 1;
    newc = oldc;
    hop2cov.inputs = hop2_rows; hop2cov.targets = hop2_rows;
    hop2cov.n_rows = 1; hop2cov.in_dim = 1; hop2cov.out_dim = 1;
    memset(&comp, 0, sizeof comp);
    fill_id_plan(&comp.plan, &oldb, &hop2);
    comp.inputs = comp_in;
    comp.n_rows = 1;
    comp.in_dim = 1;
    memset(&guard, 0, sizeof guard);
    guard.allow = cnet_swap_hop_allow;
    guard.ctx = &hop2cov;

    registry_init(&reg);
    check(registry_add(&reg, &oldb, "old_bit") == 0, "old brick registered");
    check(contract_init_borrowed(&c, "bit_id", &new_inv, cov_in, cov_tg, 2) != 0 ||
          btn_certify(&new_inv, &c, NULL) != 0,
          "invert does not certify the old identity contract");
    /* invert still needs a contract it CAN certify, for the replace door */
    {
        double inv_tg[2] = {1.0, 0.0};
        Contract inv_c;
        check(contract_init_borrowed(&inv_c, "bit_not", &new_inv,
                                     cov_in, inv_tg, 2) == 0 &&
              btn_certify(&new_inv, &inv_c, NULL) == 0,
              "invert certifies its own contract");
        memset(&rep, 0, sizeof rep);
        check(cnet_swap_replace(&reg, "old_bit", &new_inv, &inv_c,
                                &oldc, &newc, &comp, 1, &guard, &rep) == -1 &&
              rep.verdict == CNET_SWAP_REFUSE &&
              reg.count == 1 && reg.entries[0].btn == &oldb,
              "explicit swap refused; old brick not deleted");
        check(rep.compositions_hold == 0,
              "reason: CERT composition hop guards would fail");
        contract_free(&inv_c);
    }
    contract_free(&c);
    registry_free(&reg);
    btn_free(&oldb);
    btn_free(&new_inv);
    btn_free(&hop2);
}

static void test_admit_replace_and_alongside(void) {
    BinaryTransformNetwork old_id, new_id, new_not;
    double in[2] = {0.0, 1.0};
    double id_tg[2] = {0.0, 1.0};
    double not_tg[2] = {1.0, 0.0};
    Contract id_c, not_c;
    CnetSwapCoverage oldc, newc;
    CnetSwapReport rep;
    PrimitiveRegistry reg;
    size_t i;

    printf("admit replace / add-alongside (native BTN):\n");
    check(train_bit(&old_id, 0, 11u) == 0 &&
          train_bit(&new_id, 0, 13u) == 0 &&
          train_bit(&new_not, 1, 17u) == 0,
          "native identity / invert train");
    check(contract_init_borrowed(&id_c, "bit_id", &old_id, in, id_tg, 2) == 0 &&
          btn_certify(&old_id, &id_c, NULL) == 0 &&
          btn_certify(&new_id, &id_c, NULL) == 0,
          "both identities certify the old contract");
    check(contract_init_borrowed(&not_c, "bit_not", &new_not, in, not_tg, 2) == 0 &&
          btn_certify(&new_not, &not_c, NULL) == 0,
          "invert certifies its own contract");

    oldc.inputs = in; oldc.targets = id_tg;
    oldc.n_rows = 2; oldc.in_dim = 1; oldc.out_dim = 1;
    newc = oldc;

    registry_init(&reg);
    check(registry_add_certified(&reg, &old_id, "old_bit", &id_c) == 0,
          "old certified identity admitted");

    /* compression fields are ignored — set them the "wrong" way */
    reg.entries[0].teacher_mac = 1;
    reg.entries[0].student_mac = 1000;
    reg.entries[0].compute_beneficial = 0;

    memset(&rep, 0, sizeof rep);
    check(cnet_swap_admit(&reg, "old_bit", &new_id, "old_bit_v2", &id_c,
                          &oldc, &newc, NULL, 0, NULL, &rep) == 0 &&
          rep.verdict == CNET_SWAP_REPLACE &&
          reg.count == 1 && reg.entries[0].btn == &new_id &&
          reg.entries[0].certified == 1,
          "dominate + no compositions => REPLACE (compression ignored)");

    /* restore old as the incumbent for the alongside case */
    reg.entries[0].btn = &old_id;
    memset(&rep, 0, sizeof rep);
    check(cnet_swap_admit(&reg, "old_bit", &new_not, "old_bit_v2", &not_c,
                          &oldc, &newc, NULL, 0, NULL, &rep) == 0 &&
          rep.verdict == CNET_SWAP_ADD_ALONGSIDE &&
          reg.count == 2,
          "no dominate => ADD-ALONGSIDE");
    check(reg.entries[0].btn == &old_id,
          "old brick still present after add-alongside");
    check(reg.entries[1].btn == &new_not &&
          reg.entries[1].name != NULL &&
          strcmp(reg.entries[1].name, "old_bit_v2") == 0,
          "new brick registered under alongside name");
    for (i = 0; i < reg.count; ++i)
        check(reg.entries[i].btn != NULL, "no brick deleted");

    registry_free(&reg);
    contract_free(&id_c);
    contract_free(&not_c);
    btn_free(&old_id);
    btn_free(&new_id);
    btn_free(&new_not);
}

static int residual_forward(void *ctx, const double *in, size_t in_n,
                            double *out, size_t out_n) {
    (void)ctx;
    if (in == NULL || out == NULL || in_n != 1 || out_n != 1) return -1;
    out[0] = in[0];
    return 0;
}

static void test_teacher_residual_never_admits(void) {
    BinaryTransformNetwork oldb, residual;
    BitMap old_map;
    double in[2] = {0.0, 1.0};
    double tg[2] = {0.0, 1.0};
    Port p = bit_port("bit");
    Contract c;
    CnetSwapCoverage cov;
    CnetSwapReport rep;
    PrimitiveRegistry reg;

    printf("teacher/residual never admits:\n");
    check(make_bit(&oldb, &old_map, 0, 0x0D01ULL) == 0, "old brick init");
    memset(&residual, 0, sizeof residual);
    check(btn_init_adapter(&residual, 1, 1, &p, 1, &p, 1,
                           residual_forward, NULL, NULL, 0x7E51ULL, 1) == 0,
          "residual adapter init");
    check(contract_init_borrowed(&c, "bit_id", &residual, in, tg, 2) == 0,
          "residual contract");
    cov.inputs = in; cov.targets = tg;
    cov.n_rows = 2; cov.in_dim = 1; cov.out_dim = 1;
    registry_init(&reg);
    check(registry_add(&reg, &oldb, "old_bit") == 0, "old registered");
    memset(&rep, 0, sizeof rep);
    check(cnet_swap_admit(&reg, "old_bit", &residual, "residual_v2", &c,
                          &cov, &cov, NULL, 0, NULL, &rep) == -1 &&
          rep.verdict == CNET_SWAP_REFUSE &&
          strstr(rep.reason, "teacher_residual") != NULL &&
          reg.count == 1,
          "adapter/residual is refused (never admits)");
    registry_free(&reg);
    contract_free(&c);
    btn_free(&oldb);
    btn_free(&residual);
}

int main(void) {
    test_coverage_subset();
    test_dominate_replay();
    test_decide_replace_and_alongside();
    test_refuse_swap_breaks_composition();
    test_admit_replace_and_alongside();
    test_teacher_residual_never_admits();
    printf("checks=%d failures=%d\n", checks, failures);
    if (failures == 0) {
        printf("CNET_SWAP_PASS\n");
        return 0;
    }
    printf("CNET_SWAP_FAIL\n");
    return 1;
}
