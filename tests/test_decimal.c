/*
 * Decimal domain tests, in two halves.
 *
 * Hermetic half: cross-domain tag safety with synthetic BTNs (no training,
 * no weight files) -- a hex nibble and a decimal digit share BINARY_MSB 4,
 * so the dec_digit tag is the only thing keeping hex out of decimal adders.
 *
 * Frozen half: loads the committed dec_* weight/contract/property files
 * (regenerate with ./decimal_demo) and sweeps what the demo only samples:
 * the full adder domain, the EXHAUSTIVE 20000-case ripple-carry add, chunk
 * certification, and both decimal laws.
 */
#include "../include/nn.h"
#include "../include/router.h"
#include "../include/contract/contract.h"
#include "../include/property.h"

#if __has_include("generated.h")
#include "generated.h"
#define USE_FROZEN_COMMITTED 1
#endif

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, desc) do {                       \
    if (cond) { printf("  ok   %s\n", (desc)); }     \
    else { printf("  FAIL %s\n", (desc)); ++failures; } \
} while (0)

static Port PT(PortFamily family, size_t field_width, size_t field_count,
               const char *tag) {
    Port p;
    p.family = family;
    p.field_width = field_width;
    p.field_count = field_count;
    p.tag[0] = '\0';
    if (tag != NULL) {
        port_set_tag(&p, tag);
    }
    return p;
}

static int bits_to_int(const double *bits, size_t n) {
    int value = 0;
    size_t i;
    for (i = 0; i < n; ++i) {
        value = value * 2 + (bits[i] > 0.5 ? 1 : 0);
    }
    return value;
}

static void int_to_bits(int value, double *bits, size_t n) {
    size_t i;
    for (i = 0; i < n; ++i) {
        bits[i] = (double)((value >> (n - 1 - i)) & 1);
    }
}

/* Synthetic adder-shaped net: (bin4 dec_digit, bin4 dec_digit, bin1
   dec_carry) -> (bin4 dec_sum, bin1 dec_carry). Untrained: the planner
   reads only contracts. */
static int make_adder_shape(BinaryTransformNetwork *b) {
    Port ins[3];
    Port outs[2];

    ins[0] = PT(PORT_BINARY_MSB, 4, 1, "dec_digit");
    ins[1] = PT(PORT_BINARY_MSB, 4, 1, "dec_digit");
    ins[2] = PT(PORT_BINARY_MSB, 1, 1, "dec_carry");
    outs[0] = PT(PORT_BINARY_MSB, 4, 1, "dec_sum");
    outs[1] = PT(PORT_BINARY_MSB, 1, 1, "dec_carry");
    if (btn_init(b, 9, 5, 1, 4, 0.5, 1u) != 0) {
        return -1;
    }
    return btn_set_io_ports(b, ins, 3, outs, 2);
}

static int make_btn(BinaryTransformNetwork *b, size_t in, size_t out,
                    Port input_port, Port output_port) {
    if (btn_init(b, in, out, 1, 4, 0.5, 1u) != 0) {
        return -1;
    }
    return btn_set_ports(b, input_port, output_port);
}

static void hermetic_tag_safety(void) {
    BinaryTransformNetwork adder = {0};
    BinaryTransformNetwork hexish = {0};
    BinaryTransformNetwork decval = {0};
    PrimitiveRegistry reg;
    DagSource sources[3];
    DagPlan plan = {0};

    printf("cross-domain tag safety (hermetic):\n");

    CHECK(port_compatible(PT(PORT_BINARY_MSB, 4, 1, "nibble_value"),
                          PT(PORT_BINARY_MSB, 4, 1, "dec_digit")) == 0,
          "representation-identical hex nibble does NOT feed a dec_digit port");
    CHECK(port_compatible(PT(PORT_BINARY_MSB, 4, 1, "dec_digit"),
                          PT(PORT_BINARY_MSB, 4, 1, "dec_digit")) == 1,
          "matching dec_digit tags are compatible");
    CHECK(port_compatible(PT(PORT_BINARY_MSB, 4, 1, NULL),
                          PT(PORT_BINARY_MSB, 4, 1, "dec_digit")) == 1,
          "an UNTAGGED producer is a wildcard (tags only bind when both sides carry one)");

    if (make_adder_shape(&adder) != 0 ||
        make_btn(&hexish, 16, 4, PT(PORT_ONEHOT, 16, 1, "hex_digit"),
                 PT(PORT_BINARY_MSB, 4, 1, "nibble_value")) != 0 ||
        make_btn(&decval, 10, 4, PT(PORT_ONEHOT, 10, 1, "dec_symbol"),
                 PT(PORT_BINARY_MSB, 4, 1, "dec_digit")) != 0) {
        CHECK(0, "synthetic primitive setup");
        return;
    }

    registry_init(&reg);
    registry_add(&reg, &hexish, "hex_value");
    registry_add(&reg, &decval, "dec_value");
    registry_add(&reg, &adder, "dec_full_add");

    sources[0].type = PT(PORT_ONEHOT, 16, 1, "hex_digit");
    sources[0].values = NULL; /* planner ignores values */
    sources[1].type = PT(PORT_ONEHOT, 16, 1, "hex_digit");
    sources[1].values = NULL;
    sources[2].type = PT(PORT_BINARY_MSB, 1, 1, "dec_carry");
    sources[2].values = NULL;

    CHECK(dag_plan(&reg, sources, 3,
                   PT(PORT_BINARY_MSB, 4, 1, "dec_sum"), &plan) == -1,
          "dag_plan refuses hex_digit sources for a dec_sum goal");
    dag_free(&plan);

    sources[0].type = PT(PORT_ONEHOT, 10, 1, "dec_symbol");
    sources[1].type = PT(PORT_ONEHOT, 10, 1, "dec_symbol");

    CHECK(dag_plan(&reg, sources, 3,
                   PT(PORT_BINARY_MSB, 4, 1, "dec_sum"), &plan) == 0 &&
          plan.root != NULL && plan.root->kind == DAG_PRIMITIVE &&
          strcmp(plan.root->name, "dec_full_add") == 0 &&
          plan.root->child_count == 3 &&
          plan.root->children[0]->kind == DAG_PRIMITIVE &&
          strcmp(plan.root->children[0]->name, "dec_value") == 0 &&
          plan.root->children[1]->kind == DAG_PRIMITIVE &&
          strcmp(plan.root->children[1]->name, "dec_value") == 0 &&
          plan.root->children[2]->kind == DAG_SOURCE,
          "the same goal plans from dec_symbol sources through dec_value");
    dag_free(&plan);

    registry_free(&reg);
    btn_free(&adder);
    btn_free(&hexish);
    btn_free(&decval);
}

static void frozen_decimal_suite(void) {
    static BinaryTransformNetwork dec_value = {0};
    static BinaryTransformNetwork dec_fa = {0};
    static BinaryTransformNetwork dec_to_sym = {0};
    static BinaryTransformNetwork dec_swap = {0};
    static BinaryTransformNetwork chunk = {0};

    printf("frozen decimal primitives:\n");
#ifdef USE_FROZEN_COMMITTED
    if (btn_init_committed(&dec_value, "dec_value") != 0 ||
        btn_init_committed(&dec_fa, "dec_full_add") != 0 ||
        btn_init_committed(&dec_to_sym, "dec_to_symbol") != 0 ||
        btn_init_committed(&dec_swap, "dec_swap_ab") != 0 ||
        btn_init_committed(&chunk, "dec_add_unit") != 0) {
        CHECK(0, "init frozen committed dec_* (run make freeze after ./decimal_demo)");
        return;
    }
#else
    if (btn_load(&dec_value, "dec_value_weights.txt") != 0 ||
        btn_load(&dec_fa, "dec_full_add_weights.txt") != 0 ||
        btn_load(&dec_to_sym, "dec_to_symbol_weights.txt") != 0 ||
        btn_load(&dec_swap, "dec_swap_ab_weights.txt") != 0 ||
        btn_load(&chunk, "dec_add_unit_weights.txt") != 0) {
        CHECK(0, "load dec_* weight files (run ./decimal_demo first)");
        return;
    }
#endif
    CHECK(1, "all five dec_* weight files load");

    /* Full adder domain: raw output in-domain on every port and exact
       after the snap -- the executor's bar, over all 200 members. */
    {
        int a, b, cin;
        int misses = 0;

        for (a = 0; a < 10; ++a) {
            for (b = 0; b < 10; ++b) {
                for (cin = 0; cin < 2; ++cin) {
                    double in[9];
                    double sum_bits[4], carry_bit[1];
                    const double *raw;
                    int total = a + b + cin;

                    int_to_bits(a, in, 4);
                    int_to_bits(b, in + 4, 4);
                    in[8] = (double)cin;
                    raw = btn_forward(&dec_fa, in);
                    if (!port_validate(dec_fa.output_ports[0], raw) ||
                        !port_validate(dec_fa.output_ports[1], raw + 4) ||
                        port_canonicalize(dec_fa.output_ports[0], raw,
                                          sum_bits) != 0 ||
                        port_canonicalize(dec_fa.output_ports[1], raw + 4,
                                          carry_bit) != 0 ||
                        bits_to_int(sum_bits, 4) != total % 10 ||
                        bits_to_int(carry_bit, 1) != total / 10) {
                        ++misses;
                    }
                }
            }
        }
        CHECK(misses == 0, "dec_full_add masters its full 200-member domain");
    }

    /* Exhaustive ripple-carry sweep: every a, b in 0..99 and cin in {0,1},
       through three strict hand-built trees (sum0, sum1, carry_out). Plans
       are trees with single-use sources, so the 3-output circuit is a
       program over the frozen primitives -- the strict executor still
       validates every handoff. */
    {
        static double a0_v[4], a1_v[4], b0_v[4], b1_v[4], cin_v[1];
        DagSource rsrc[5];
        DagNode s_a0, s_a1, s_b0, s_b1, s_cin;
        DagNode ones_sum, ones_carry, tens_sum, tens_carry;
        DagPlan p_sum0 = {0}, p_sum1 = {0}, p_cout = {0};
        int a, b, cin;
        long wrong = 0;

        rsrc[0].type = PT(PORT_BINARY_MSB, 4, 1, "dec_digit");
        rsrc[0].values = a0_v;
        rsrc[1].type = PT(PORT_BINARY_MSB, 4, 1, "dec_digit");
        rsrc[1].values = a1_v;
        rsrc[2].type = PT(PORT_BINARY_MSB, 4, 1, "dec_digit");
        rsrc[2].values = b0_v;
        rsrc[3].type = PT(PORT_BINARY_MSB, 4, 1, "dec_digit");
        rsrc[3].values = b1_v;
        rsrc[4].type = PT(PORT_BINARY_MSB, 1, 1, "dec_carry");
        rsrc[4].values = cin_v;

        memset(&s_a0, 0, sizeof s_a0);
        s_a0.kind = DAG_SOURCE;
        s_a0.source_index = 0;
        memset(&s_a1, 0, sizeof s_a1);
        s_a1.kind = DAG_SOURCE;
        s_a1.source_index = 1;
        memset(&s_b0, 0, sizeof s_b0);
        s_b0.kind = DAG_SOURCE;
        s_b0.source_index = 2;
        memset(&s_b1, 0, sizeof s_b1);
        s_b1.kind = DAG_SOURCE;
        s_b1.source_index = 3;
        memset(&s_cin, 0, sizeof s_cin);
        s_cin.kind = DAG_SOURCE;
        s_cin.source_index = 4;

        memset(&ones_sum, 0, sizeof ones_sum);
        ones_sum.kind = DAG_PRIMITIVE;
        ones_sum.btn = &dec_fa;
        ones_sum.name = "dec_full_add";
        ones_sum.output_index = 0;
        ones_sum.children[0] = &s_a0;
        ones_sum.children[1] = &s_b0;
        ones_sum.children[2] = &s_cin;
        ones_sum.child_count = 3;

        ones_carry = ones_sum;
        ones_carry.output_index = 1;

        memset(&tens_sum, 0, sizeof tens_sum);
        tens_sum.kind = DAG_PRIMITIVE;
        tens_sum.btn = &dec_fa;
        tens_sum.name = "dec_full_add";
        tens_sum.output_index = 0;
        tens_sum.children[0] = &s_a1;
        tens_sum.children[1] = &s_b1;
        tens_sum.children[2] = &ones_carry;
        tens_sum.child_count = 3;

        tens_carry = tens_sum;
        tens_carry.output_index = 1;

        p_sum0.root = &ones_sum;
        p_sum0.strict = 1;
        p_sum1.root = &tens_sum;
        p_sum1.strict = 1;
        p_cout.root = &tens_carry;
        p_cout.strict = 1;

        for (a = 0; a < 100; ++a) {
            for (b = 0; b < 100; ++b) {
                for (cin = 0; cin < 2; ++cin) {
                    double s0[4], s1[4], co[1];

                    int_to_bits(a % 10, a0_v, 4);
                    int_to_bits(a / 10, a1_v, 4);
                    int_to_bits(b % 10, b0_v, 4);
                    int_to_bits(b / 10, b1_v, 4);
                    cin_v[0] = (double)cin;

                    if (dag_execute(&p_sum0, rsrc, 5, s0, 4) != 0 ||
                        dag_execute(&p_sum1, rsrc, 5, s1, 4) != 0 ||
                        dag_execute(&p_cout, rsrc, 5, co, 1) != 0 ||
                        bits_to_int(co, 1) * 100 + bits_to_int(s1, 4) * 10 +
                            bits_to_int(s0, 4) != a + b + cin) {
                        ++wrong;
                    }
                }
            }
        }
        CHECK(wrong == 0,
              "ripple-carry adds ALL 20000 cases exactly (strict execution)");
    }

    /* The chunk wears its teacher's contract. */
    {
        Contract c = {0};
        CertifyReport rep;

        if (contract_load(&c, "dec_add_unit_contract.txt") != 0) {
            CHECK(0, "load dec_add_unit_contract.txt");
        } else {
            CHECK(btn_certify(&chunk, &c, &rep) == 0 &&
                  rep.exemplars == 200 && rep.passed == 200,
                  "dec_add_unit certifies against its teacher's contract (200/200)");
            contract_free(&c);
        }
    }

    /* Both decimal laws load from disk and hold. */
    {
        PrimitiveRegistry reg;
        Property law;
        PropertyReport rep;

        registry_init(&reg);
        registry_add(&reg, &dec_value, "dec_value");
        registry_add(&reg, &dec_to_sym, "dec_to_symbol");
        registry_add(&reg, &dec_swap, "dec_swap_ab");
        registry_add(&reg, &chunk, "dec_add_unit");

        memset(&law, 0, sizeof law);
        CHECK(property_load(&law, "dec_value_roundtrip_property.txt") == 0 &&
              property_check(&law, &reg, 4096, &rep) == 0 &&
              rep.inputs == 10 && rep.held == 10,
              "dec_value_roundtrip holds (10/10)");

        memset(&law, 0, sizeof law);
        CHECK(property_load(&law, "dec_add_commutes_property.txt") == 0 &&
              property_check(&law, &reg, 4096, &rep) == 0 &&
              rep.inputs == 200 && rep.held == 200,
              "dec_add_commutes holds (200/200)");

        registry_free(&reg);
    }

    btn_free(&dec_value);
    btn_free(&dec_fa);
    btn_free(&dec_to_sym);
    btn_free(&dec_swap);
    btn_free(&chunk);
}

int run_test_decimal(void) {
    hermetic_tag_safety();
    frozen_decimal_suite();

    if (failures != 0) {
        printf("\n%d decimal test(s) failed.\n", failures);
        return 1;
    }
    printf("\nAll decimal tests passed.\n");
    return 0;
}

