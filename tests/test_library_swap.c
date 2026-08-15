/* test_library_swap — live doors call the unit-tested swap law.
 *
 * registry_add_certified (same-name) and library_admit_candidate
 * (library_evolve door) call cnet_swap_admit. src/cnet_swap.c is in
 * LIBRARY / cnet.so. Does not rewrite the law.
 *
 * old_cov is the persisted incumbent certification table. Incoming is
 * new_cov only. Same table both sides is a dominate lie.
 *
 * No F11 / 448 / CHAT-1. No soft router. Residual never as a replacement.
 */
#include "../include/cnet_swap.h"
#include "../include/library.h"
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

static int train_map(BinaryTransformNetwork *b, const double *tgt,
                     unsigned seed) {
    double in[2] = {0.0, 1.0};
    Port p = bit_port("bit");
    unsigned attempt;
    for (attempt = 0; attempt < 8; ++attempt) {
        double loss = 0.0;
        memset(b, 0, sizeof *b);
        if (btn_init(b, 1, 1, 2, 16, 0.8, seed + attempt * 97u) != 0)
            return -1;
        if (btn_set_ports(b, p, p) != 0) {
            btn_free(b);
            return -1;
        }
        (void)btn_set_momentum(b, 0.9);
        if (btn_train_dynamic_checked(b, in, tgt, 2, 80000, 400,
                                      0.0005, 0.01, &loss) == BTN_TRAIN_OK &&
            loss <= 0.02)
            return 0;
        btn_free(b);
    }
    return -1;
}

static int train_bit(BinaryTransformNetwork *b, int invert, unsigned seed) {
    double tgt[2];
    tgt[0] = invert ? 1.0 : 0.0;
    tgt[1] = invert ? 0.0 : 1.0;
    return train_map(b, tgt, seed);
}

static int train_const0(BinaryTransformNetwork *b, unsigned seed) {
    double tgt[2] = {0.0, 0.0};
    return train_map(b, tgt, seed);
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

static int residual_forward(void *ctx, const double *in, size_t in_n,
                            double *out, size_t out_n) {
    (void)ctx;
    if (in == NULL || out == NULL || in_n != 1 || out_n != 1) return -1;
    out[0] = in[0];
    return 0;
}

static int entry_named(const PrimitiveRegistry *reg, const char *name,
                       const BinaryTransformNetwork *btn) {
    size_t i;
    for (i = 0; i < reg->count; ++i) {
        if (reg->entries[i].name != NULL &&
            strcmp(reg->entries[i].name, name) == 0)
            return reg->entries[i].btn == btn;
    }
    return 0;
}

static void test_registry_refuses_adapter(void) {
    BinaryTransformNetwork oldb, residual;
    double in[2] = {0.0, 1.0};
    double tg[2] = {0.0, 1.0};
    Port p = bit_port("bit");
    Contract c, rc;
    PrimitiveRegistry reg;

    printf("registry_add_certified refuses adapter new brick:\n");
    check(train_bit(&oldb, 0, 11u) == 0, "native old brick train");
    check(contract_init_borrowed(&c, "bit_id", &oldb, in, tg, 2) == 0 &&
          btn_certify(&oldb, &c, NULL) == 0,
          "old identity certifies");
    registry_init(&reg);
    check(registry_add_certified(&reg, &oldb, "old_bit", &c) == 0 &&
          reg.count == 1,
          "old certified identity admitted");

    memset(&residual, 0, sizeof residual);
    check(btn_init_adapter(&residual, 1, 1, &p, 1, &p, 1,
                           residual_forward, NULL, NULL, 0x7E51ULL, 1) == 0,
          "residual adapter init");
    check(btn_is_adapter(&residual) == 1, "new brick is adapter");
    check(contract_init_borrowed(&rc, "bit_id", &residual, in, tg, 2) == 0,
          "residual contract");
    check(registry_add_certified(&reg, &residual, "old_bit", &rc) == -1 &&
          reg.count == 1 && reg.entries[0].btn == &oldb,
          "adapter new brick REFUSE; old brick stays");

    contract_free(&rc);
    contract_free(&c);
    registry_free(&reg);
    btn_free(&oldb);
    btn_free(&residual);
}

static void test_registry_live_paths(void) {
    BinaryTransformNetwork old_id, new_id, hop2, new_zero;
    double in[2] = {0.0, 1.0};
    double id_tg[2] = {0.0, 1.0};
    double zero_tg[2] = {0.0, 0.0};
    double sub_in[1] = {0.0};
    double sub_tg[1] = {0.0};
    double hop2_rows[1] = {0.0};
    double hop2_break[1] = {1.0};
    double comp_in[1] = {0.0};
    double comp_break[1] = {1.0};
    Contract id_c, sub_c, zero_c, narrow_c;
    CnetSwapCoverage hop2cov;
    CnetSwapComposition comp;
    DagNodeGuard guard;
    PrimitiveRegistry reg;
    size_t i;
    int found_new;

    printf("registry_add_certified live paths (persisted old_cov):\n");
    check(train_bit(&old_id, 0, 11u) == 0 &&
          train_bit(&new_id, 0, 13u) == 0 &&
          train_bit(&hop2, 0, 19u) == 0 &&
          train_const0(&new_zero, 23u) == 0,
          "native identity / hop2 / const-0 train");
    check(contract_init_borrowed(&id_c, "bit_id", &old_id, in, id_tg, 2) == 0 &&
          btn_certify(&old_id, &id_c, NULL) == 0 &&
          btn_certify(&new_id, &id_c, NULL) == 0,
          "both identities certify the full table");
    check(contract_init_borrowed(&sub_c, "bit_id_sub", &new_id, sub_in, sub_tg, 1) == 0 &&
          btn_certify(&new_id, &sub_c, NULL) == 0,
          "new identity certifies a proper subset table");
    check(contract_init_borrowed(&zero_c, "bit_zero", &new_zero, in, zero_tg, 2) == 0 &&
          btn_certify(&new_zero, &zero_c, NULL) == 0,
          "const-0 certifies");
    check(contract_init_borrowed(&narrow_c, "bit_id_narrow", &old_id,
                                sub_in, sub_tg, 1) == 0 &&
          btn_certify(&old_id, &narrow_c, NULL) == 0,
          "old identity certifies the narrow {0} table");

    hop2cov.inputs = hop2_rows; hop2cov.targets = hop2_rows;
    hop2cov.n_rows = 1; hop2cov.in_dim = 1; hop2cov.out_dim = 1;
    memset(&comp, 0, sizeof comp);
    fill_id_plan(&comp.plan, &old_id, &hop2);
    comp.inputs = comp_in;
    comp.n_rows = 1;
    comp.in_dim = 1;
    memset(&guard, 0, sizeof guard);
    guard.allow = cnet_swap_hop_allow;
    guard.ctx = &hop2cov;

    /* persist + n_comps==0: ADD_ALONGSIDE, old stays */
    registry_init(&reg);
    check(registry_add_certified(&reg, &old_id, "old_bit", &id_c) == 0 &&
          reg.count == 1 &&
          reg.entries[0].cert_cov != NULL &&
          reg.entries[0].cert_cov->n_rows == 2,
          "old admitted; incumbent persisted its original 2-row table");
    cnet_swap_unbind_compositions();
    check(registry_add_certified(&reg, &new_id, "old_bit", &id_c) == 0 &&
          reg.count == 2 &&
          reg.entries[0].btn == &old_id,
          "dominate + n_comps==0 => ADD_ALONGSIDE; old still present");
    found_new = 0;
    for (i = 0; i < reg.count; ++i)
        if (reg.entries[i].btn == &new_id) found_new = 1;
    check(found_new, "new brick persisted under alongside name");
    registry_free(&reg);

    /* native dominate+hold REPLACE: new table covers the ORIGINAL rows */
    registry_init(&reg);
    check(registry_add_certified(&reg, &old_id, "old_bit", &id_c) == 0,
          "old certified identity re-admitted");
    cnet_swap_bind_compositions(&comp, 1, &guard);
    check(registry_add_certified(&reg, &new_id, "old_bit", &id_c) == 0 &&
          reg.count == 1 &&
          reg.entries[0].btn == &new_id &&
          reg.entries[0].certified == 1 &&
          reg.entries[0].cert_cov != NULL &&
          reg.entries[0].cert_cov->n_rows == 2,
          "dominate+hold REPLACE; new table covers incumbent original rows");
    cnet_swap_unbind_compositions();
    registry_free(&reg);

    /* anti-tautology: incoming is a proper subset of the original table */
    registry_init(&reg);
    check(registry_add_certified(&reg, &old_id, "old_bit", &id_c) == 0,
          "old full-table identity admitted for anti-tautology");
    cnet_swap_bind_compositions(&comp, 1, &guard);
    check(registry_add_certified(&reg, &new_id, "old_bit", &sub_c) == 0 &&
          reg.count == 2 &&
          reg.entries[0].btn == &old_id,
          "incoming subset != original table => dominate false, no REPLACE");
    found_new = 0;
    for (i = 0; i < reg.count; ++i)
        if (reg.entries[i].btn == &new_id) found_new = 1;
    check(found_new, "subset incoming added alongside, not as a replacement");
    cnet_swap_unbind_compositions();
    registry_free(&reg);

    /* const-0 against the full identity table is NOT dominate */
    registry_init(&reg);
    check(registry_add_certified(&reg, &old_id, "old_bit", &id_c) == 0,
          "old identity admitted for const-0");
    cnet_swap_bind_compositions(&comp, 1, &guard);
    check(registry_add_certified(&reg, &new_zero, "old_bit", &zero_c) == 0 &&
          reg.count == 2 &&
          reg.entries[0].btn == &old_id,
          "const-0 does not dominate the incumbent identity table; no REPLACE");
    check(reg.entries[0].btn != &new_zero,
          "const-0 did not replace the old brick");
    cnet_swap_unbind_compositions();
    registry_free(&reg);

    /* no persisted old_cov => cannot REPLACE even with compositions bound */
    registry_init(&reg);
    check(registry_add(&reg, &old_id, "old_bit") == 0 &&
          reg.entries[0].cert_cov == NULL,
          "uncertified add has no persisted coverage");
    cnet_swap_bind_compositions(&comp, 1, &guard);
    check(registry_add_certified(&reg, &new_id, "old_bit", &id_c) == 0 &&
          reg.count == 2 &&
          reg.entries[0].btn == &old_id,
          "no_old_coverage => ADD-ALONGSIDE, never REPLACE");
    cnet_swap_unbind_compositions();
    registry_free(&reg);

    /* honest dominate && compositions_hold==0: const-0 covers narrow {0} */
    hop2cov.inputs = hop2_break; hop2cov.targets = hop2_break;
    hop2cov.n_rows = 1; hop2cov.in_dim = 1; hop2cov.out_dim = 1;
    comp.inputs = comp_break;
    registry_init(&reg);
    check(registry_add_certified(&reg, &old_id, "old_bit", &narrow_c) == 0 &&
          reg.entries[0].cert_cov != NULL &&
          reg.entries[0].cert_cov->n_rows == 1,
          "old admitted on narrow {0} table");
    cnet_swap_bind_compositions(&comp, 1, &guard);
    check(registry_add_certified(&reg, &new_zero, "old_bit", &zero_c) == 0 &&
          reg.count == 2 &&
          reg.entries[0].btn == &old_id,
          "dominate && compositions_hold==0 => no REPLACE; old stays");
    cnet_swap_unbind_compositions();
    registry_free(&reg);

    contract_free(&id_c);
    contract_free(&sub_c);
    contract_free(&zero_c);
    contract_free(&narrow_c);
    btn_free(&old_id);
    btn_free(&new_id);
    btn_free(&hop2);
    btn_free(&new_zero);
}

static void test_library_door(void) {
    BinaryTransformNetwork old_id, new_id, hop2;
    double in[2] = {0.0, 1.0};
    double id_tg[2] = {0.0, 1.0};
    double hop2_rows[1] = {0.0};
    double comp_in[1] = {0.0};
    Contract id_c;
    CnetSwapCoverage hop2cov;
    CnetSwapComposition comp;
    DagNodeGuard guard;
    PrimitiveRegistry reg;

    printf("library_admit_candidate (library_evolve door):\n");
    check(train_bit(&old_id, 0, 11u) == 0 &&
          train_bit(&new_id, 0, 13u) == 0 &&
          train_bit(&hop2, 0, 19u) == 0,
          "native library-door bricks train");
    check(contract_init_borrowed(&id_c, "bit_id", &old_id, in, id_tg, 2) == 0 &&
          btn_certify(&old_id, &id_c, NULL) == 0 &&
          btn_certify(&new_id, &id_c, NULL) == 0,
          "both identities certify library-door contract");

    hop2cov.inputs = hop2_rows; hop2cov.targets = hop2_rows;
    hop2cov.n_rows = 1; hop2cov.in_dim = 1; hop2cov.out_dim = 1;
    memset(&comp, 0, sizeof comp);
    fill_id_plan(&comp.plan, &old_id, &hop2);
    comp.inputs = comp_in;
    comp.n_rows = 1;
    comp.in_dim = 1;
    memset(&guard, 0, sizeof guard);
    guard.allow = cnet_swap_hop_allow;
    guard.ctx = &hop2cov;

    registry_init(&reg);
    check(registry_add_certified(&reg, &old_id, "old_bit", &id_c) == 0,
          "old brick on evolve door");

    /* exact-same digest still dedups */
    cnet_swap_unbind_compositions();
    check(library_admit_candidate(&reg, &old_id, "old_bit", &id_c) == -1 &&
          reg.count == 1 &&
          reg.entries[0].btn == &old_id,
          "exact-same brick still dedups");

    /* already-known + n_comps==0 still dedups (fixed-point evolve) */
    check(library_admit_candidate(&reg, &new_id, "chunk_bit", &id_c) == -1 &&
          reg.count == 1 &&
          reg.entries[0].btn == &old_id,
          "already-known contract + n_comps==0 still dedups");

    /* cross-name first-certify must never REPLACE the wrong name */
    cnet_swap_bind_compositions(&comp, 1, &guard);
    check(library_admit_candidate(&reg, &new_id, "chunk_bit", &id_c) == 0 &&
          reg.count == 2 &&
          entry_named(&reg, "old_bit", &old_id) &&
          entry_named(&reg, "chunk_bit", &new_id),
          "cross-name first-certify ADD-ALONGSIDE, never REPLACE old_bit");
    cnet_swap_unbind_compositions();
    registry_free(&reg);

    /* same-name dominate+hold REPLACE (new table covers original rows) */
    registry_init(&reg);
    check(registry_add_certified(&reg, &old_id, "old_bit", &id_c) == 0,
          "old brick re-admitted for same-name evolve REPLACE");
    cnet_swap_bind_compositions(&comp, 1, &guard);
    check(library_admit_candidate(&reg, &new_id, "old_bit", &id_c) == 0 &&
          reg.count == 1 &&
          reg.entries[0].btn == &new_id,
          "same-name dominate+hold goes through the law (REPLACE)");
    cnet_swap_unbind_compositions();

    registry_free(&reg);
    contract_free(&id_c);
    btn_free(&old_id);
    btn_free(&new_id);
    btn_free(&hop2);
}

int main(void) {
    test_registry_refuses_adapter();
    test_registry_live_paths();
    test_library_door();
    printf("checks=%d failures=%d\n", checks, failures);
    if (failures == 0) {
        printf("CNET_LIBRARY_SWAP_PASS\n");
        return 0;
    }
    printf("CNET_LIBRARY_SWAP_FAIL\n");
    return 1;
}
