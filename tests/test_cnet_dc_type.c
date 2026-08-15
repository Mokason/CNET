#include "../include/cnet_dc_type.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define REQUIRE(cond, reason)                                                 \
    do {                                                                      \
        if (!(cond)) {                                                        \
            printf("CNET_DC_TYPE_RED reason=%s\n", reason);                   \
            ++failures;                                                       \
            return;                                                           \
        }                                                                     \
    } while (0)

static Port PT(PortFamily family, size_t width, size_t count, const char *tag) {
    Port p;
    p.family = family;
    p.field_width = width;
    p.field_count = count;
    p.tag[0] = '\0';
    if (tag != NULL) {
        size_t n = strlen(tag);
        if (n >= PORT_TAG_MAX) n = PORT_TAG_MAX - 1u;
        memcpy(p.tag, tag, n);
        p.tag[n] = '\0';
    }
    return p;
}

static void test_unify_ground(void) {
    CnetDcArena a;
    int tint, tbool, tlist_int, tlist_bool;
    cnet_dc_arena_init(&a);
    tint = cnet_dc_base(&a, "int");
    tbool = cnet_dc_base(&a, "bool");
    tlist_int = cnet_dc_list(&a, tint);
    tlist_bool = cnet_dc_list(&a, tbool);
    REQUIRE(cnet_dc_can_unify(&a, tint, tint) == 1, "int_int");
    REQUIRE(cnet_dc_can_unify(&a, tint, tbool) == 0, "int_bool");
    REQUIRE(cnet_dc_can_unify(&a, tlist_int, tlist_int) == 1, "list_int");
    REQUIRE(cnet_dc_can_unify(&a, tlist_int, tlist_bool) == 0, "list_mismatch");
}

static void test_instantiate_unify(void) {
    CnetDcArena a;
    CnetDcContext ctx;
    int t0, tint, list_t0, list_int, inst, applied;
    cnet_dc_arena_init(&a);
    cnet_dc_ctx_init(&ctx);
    t0 = cnet_dc_var(&a, 0);
    tint = cnet_dc_base(&a, "int");
    list_t0 = cnet_dc_list(&a, t0);
    list_int = cnet_dc_list(&a, tint);
    REQUIRE(cnet_dc_instantiate(&a, &ctx, list_t0, &inst) == 0, "inst");
    REQUIRE(cnet_dc_unify(&a, &ctx, inst, list_int) == 0, "unify_list");
    REQUIRE(cnet_dc_apply(&a, &ctx, inst, &applied) == 0, "apply");
    REQUIRE(cnet_dc_equal(&a, applied, list_int), "applied_list_int");
}

static void test_occurs(void) {
    CnetDcArena a;
    CnetDcContext ctx;
    int t0, list_t0;
    cnet_dc_arena_init(&a);
    cnet_dc_ctx_init(&ctx);
    t0 = cnet_dc_var(&a, 0);
    list_t0 = cnet_dc_list(&a, t0);
    REQUIRE(cnet_dc_unify(&a, &ctx, t0, list_t0) == 1, "occurs");
}

static void test_apply_fn(void) {
    CnetDcArena a;
    int t0, tint, tbool, id, incr, car, list_int, result;
    cnet_dc_arena_init(&a);
    t0 = cnet_dc_var(&a, 0);
    tint = cnet_dc_base(&a, "int");
    tbool = cnet_dc_base(&a, "bool");
    id = cnet_dc_arrow(&a, t0, t0);
    incr = cnet_dc_arrow(&a, tint, tint);
    car = cnet_dc_arrow(&a, cnet_dc_list(&a, t0), t0);
    list_int = cnet_dc_list(&a, tint);
    REQUIRE(cnet_dc_apply_fn(&a, id, tint, &result) == 0 &&
                cnet_dc_equal(&a, result, tint),
            "id_int");
    REQUIRE(cnet_dc_apply_fn(&a, incr, tint, &result) == 0 &&
                cnet_dc_equal(&a, result, tint),
            "incr_int");
    REQUIRE(cnet_dc_apply_fn(&a, incr, tbool, &result) == 1, "incr_bool");
    REQUIRE(cnet_dc_apply_fn(&a, car, list_int, &result) == 0 &&
                cnet_dc_equal(&a, result, tint),
            "car_list_int");
}

static void test_ports_agree(void) {
    Port a = PT(PORT_BINARY_MSB, 4, 1, NULL);
    Port b = PT(PORT_BINARY_MSB, 4, 1, NULL);
    Port c = PT(PORT_BINARY_MSB, 5, 1, NULL);
    Port d = PT(PORT_ONEHOT, 16, 1, NULL);
    Port raw4 = PT(PORT_RAW, 4, 1, NULL);
    Port tagged = PT(PORT_BINARY_MSB, 4, 1, "nibble_value");
    Port other = PT(PORT_BINARY_MSB, 4, 1, "card_rank");
    REQUIRE(cnet_dc_ports_unify(a, b) == port_compatible(a, b), "same");
    REQUIRE(cnet_dc_ports_unify(a, c) == port_compatible(a, c), "width");
    REQUIRE(cnet_dc_ports_unify(a, d) == port_compatible(a, d), "family");
    REQUIRE(cnet_dc_ports_unify(raw4, a) == port_compatible(raw4, a), "raw");
    REQUIRE(cnet_dc_ports_unify(tagged, a) == port_compatible(tagged, a),
            "tag_wild");
    REQUIRE(cnet_dc_ports_unify(tagged, other) == port_compatible(tagged, other),
            "tag_mismatch");
}

static void test_contract_arrow(void) {
    CnetDcArena a;
    Contract c;
    int ty;
    char shown[80];
    memset(&c, 0, sizeof c);
    memcpy(c.name, "inc", 4);
    c.input_port_count = 1;
    c.output_port_count = 1;
    c.input_ports[0] = PT(PORT_BINARY_MSB, 2, 1, NULL);
    c.output_ports[0] = PT(PORT_BINARY_MSB, 3, 1, NULL);
    cnet_dc_arena_init(&a);
    ty = cnet_dc_from_contract(&a, &c);
    REQUIRE(ty >= 0 && cnet_dc_is_arrow(&a, ty), "arrow");
    REQUIRE(cnet_dc_show(&a, ty, shown, sizeof shown) == 0, "show");
    REQUIRE(strstr(shown, "->") != NULL, "shows_arrow");
}

static void test_route_types(void) {
    RoutePlan plan;
    BinaryTransformNetwork dec, inc;
    Port source = PT(PORT_ONEHOT, 4, 1, NULL);
    Port mid = PT(PORT_BINARY_MSB, 2, 1, NULL);
    Port goal = PT(PORT_BINARY_MSB, 3, 1, NULL);
    memset(&dec, 0, sizeof dec);
    memset(&inc, 0, sizeof inc);
    dec.input_port_count = 1;
    dec.output_port_count = 1;
    dec.input_ports[0] = source;
    dec.output_ports[0] = mid;
    inc.input_port_count = 1;
    inc.output_port_count = 1;
    inc.input_ports[0] = mid;
    inc.output_ports[0] = goal;
    memset(&plan, 0, sizeof plan);
    plan.steps[0] = &dec;
    plan.steps[1] = &inc;
    plan.length = 2;
    plan.goal = goal;
    REQUIRE(cnet_dc_route_well_typed(&plan, source) == 0, "well_typed");
    plan.goal = source;
    REQUIRE(cnet_dc_route_well_typed(&plan, source) == 1, "bad_goal");
}

int main(void) {
    test_unify_ground();
    test_instantiate_unify();
    test_occurs();
    test_apply_fn();
    test_ports_agree();
    test_contract_arrow();
    test_route_types();
    if (failures != 0) return 1;
    printf("CNET_DC_TYPE_PASS unify=1 instantiate=1 occurs=1 apply=1 "
           "ports=1 contract=1 route=1 broader_claims=WITHHELD\n");
    return 0;
}
