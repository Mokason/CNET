/*
 * Hermetic tests for the primitive lifecycle spine: PrimitiveState transitions
 * (FUZZY on add, FROZEN on certify), registry_set_state, evidence-based
 * promotion, and the opt-in RESET-skip in the planner. Synthetic BTNs only --
 * the planner reads contracts/reliability, so no training or weight files.
 */
#include "../include/nn.h"
#include "../include/router.h"
#include "../include/contract/contract.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, desc) do {                       \
    if (cond) { printf("  ok   %s\n", (desc)); }     \
    else { printf("  FAIL %s\n", (desc)); ++failures; } \
} while (0)

static Port P(PortFamily family, size_t field_width, size_t field_count) {
    Port p;
    p.family = family;
    p.field_width = field_width;
    p.field_count = field_count;
    p.tag[0] = '\0';
    return p;
}

static int make_btn(BinaryTransformNetwork *b, size_t in, size_t out,
                    Port input_port, Port output_port) {
    if (btn_init(b, in, out, 1, 4, 0.5, 1u) != 0) {
        return -1;
    }
    return btn_set_ports(b, input_port, output_port);
}

static void test_fuzzy_default(void) {
    BinaryTransformNetwork b = {0};
    PrimitiveRegistry reg;

    printf("lifecycle: FUZZY default on registry_add:\n");
    if (make_btn(&b, 16, 4, P(PORT_ONEHOT, 16, 1), P(PORT_BINARY_MSB, 4, 1)) != 0) {
        CHECK(0, "synthetic primitive setup");
        return;
    }
    registry_init(&reg);
    CHECK(reg.lifecycle_enabled == 0, "registry_init zeroes lifecycle_enabled");
    CHECK(registry_add(&reg, &b, "p") == 0, "add p");
    CHECK(reg.entries[0].state == PRIM_FUZZY, "registry_add sets state FUZZY");
    registry_free(&reg);
    btn_free(&b);
}

static void test_set_state(void) {
    BinaryTransformNetwork b = {0};
    PrimitiveRegistry reg;

    printf("lifecycle: registry_set_state:\n");
    if (make_btn(&b, 16, 4, P(PORT_ONEHOT, 16, 1), P(PORT_BINARY_MSB, 4, 1)) != 0) {
        CHECK(0, "synthetic primitive setup");
        return;
    }
    registry_init(&reg);
    registry_add(&reg, &b, "p");
    CHECK(registry_set_state(&reg, "p", PRIM_RESET) == 0, "set p -> RESET returns 0");
    CHECK(reg.entries[0].state == PRIM_RESET, "p is now RESET");
    CHECK(registry_set_state(&reg, "missing", PRIM_FROZEN) == -1,
          "unknown name returns -1");
    registry_free(&reg);
    btn_free(&b);
}

static void test_frozen_on_certify(void) {
    BinaryTransformNetwork b = {0};
    PrimitiveRegistry reg;
    Contract c = {0};
    double in[4] = {1.0, 0.0, 1.0, 0.0};   /* canonical BINARY_MSB(4,1) */
    double tgt[5];
    const double *out;

    printf("lifecycle: FROZEN on registry_add_certified:\n");
    /* RAW(5,1) output: certify replays `in`, copies raw output, matches tgt. */
    if (make_btn(&b, 4, 5, P(PORT_BINARY_MSB, 4, 1), P(PORT_RAW, 5, 1)) != 0) {
        CHECK(0, "synthetic primitive setup");
        return;
    }
    out = btn_forward(&b, in);
    memcpy(tgt, out, sizeof tgt);

    CHECK(contract_init_borrowed(&c, "rawid", &b, in, tgt, 1) == 0,
          "contract_init_borrowed ok");
    registry_init(&reg);
    CHECK(registry_add_certified(&reg, &b, "rawid", &c) == 0,
          "registry_add_certified succeeds");
    CHECK(reg.entries[0].certified == 1, "entry is certified");
    CHECK(reg.entries[0].state == PRIM_FROZEN,
          "registry_add_certified sets state FROZEN");

    contract_free(&c);
    registry_free(&reg);
    btn_free(&b);
}

static void test_promote_provisional(void) {
    BinaryTransformNetwork hot = {0};   /* enough good evidence -> promote */
    BinaryTransformNetwork cold = {0};  /* too little evidence -> stays FUZZY */
    BinaryTransformNetwork froz = {0};  /* FROZEN -> untouched */
    BinaryTransformNetwork rst = {0};   /* RESET -> untouched */
    PrimitiveRegistry reg;

    printf("lifecycle: promote FUZZY -> PROVISIONAL by evidence:\n");
    if (make_btn(&hot, 4, 4, P(PORT_BINARY_MSB, 4, 1), P(PORT_BINARY_MSB, 4, 1)) != 0 ||
        make_btn(&cold, 4, 4, P(PORT_BINARY_MSB, 4, 1), P(PORT_BINARY_MSB, 4, 1)) != 0 ||
        make_btn(&froz, 4, 4, P(PORT_BINARY_MSB, 4, 1), P(PORT_BINARY_MSB, 4, 1)) != 0 ||
        make_btn(&rst, 4, 4, P(PORT_BINARY_MSB, 4, 1), P(PORT_BINARY_MSB, 4, 1)) != 0) {
        CHECK(0, "synthetic primitive setup");
        return;
    }
    hot.output_successes = 20; hot.output_failures = 0;   /* rel ~0.95, ev 20 */
    cold.output_successes = 2; cold.output_failures = 0;  /* rel ~0.75, ev 2 */
    froz.output_successes = 20; froz.output_failures = 0;
    rst.output_successes = 20; rst.output_failures = 0;

    registry_init(&reg);
    registry_add(&reg, &hot, "hot");
    registry_add(&reg, &cold, "cold");
    registry_add(&reg, &froz, "froz");
    registry_add(&reg, &rst, "rst");
    registry_set_state(&reg, "froz", PRIM_FROZEN);
    registry_set_state(&reg, "rst", PRIM_RESET);

    lifecycle_promote_provisional(&reg, 0.9, 16);

    CHECK(reg.entries[0].state == PRIM_PROVISIONAL, "hot promoted to PROVISIONAL");
    CHECK(reg.entries[1].state == PRIM_FUZZY, "cold stays FUZZY (low evidence)");
    CHECK(reg.entries[2].state == PRIM_FROZEN, "froz untouched");
    CHECK(reg.entries[3].state == PRIM_RESET, "rst untouched");

    registry_free(&reg);
    btn_free(&hot); btn_free(&cold); btn_free(&froz); btn_free(&rst);
}

static void test_reset_skip_opt_in(void) {
    BinaryTransformNetwork bad = {0};
    BinaryTransformNetwork good = {0};
    PrimitiveRegistry reg;
    RoutePlan plan;
    Port in = P(PORT_ONEHOT, 16, 1);
    Port goal = P(PORT_BINARY_MSB, 5, 1);

    printf("lifecycle: opt-in RESET-skip in planner:\n");
    if (make_btn(&bad, 16, 5, in, goal) != 0 ||
        make_btn(&good, 16, 5, in, goal) != 0) {
        CHECK(0, "synthetic primitive setup");
        return;
    }
    registry_init(&reg);
    registry_add(&reg, &bad, "bad");    /* index 0: default pick by reg order */
    registry_add(&reg, &good, "good");  /* index 1 */

    /* lifecycle off: registry order picks "bad". */
    CHECK(route_plan(&reg, in, goal, &plan) == 0 && plan.length == 1 &&
          strcmp(plan.names[0], "bad") == 0,
          "lifecycle off -> picks 'bad' (registry order)");

    /* mark bad RESET + enable lifecycle: planner must route to "good". */
    registry_set_state(&reg, "bad", PRIM_RESET);
    reg.lifecycle_enabled = 1;
    CHECK(route_plan(&reg, in, goal, &plan) == 0 && plan.length == 1 &&
          strcmp(plan.names[0], "good") == 0,
          "lifecycle on + bad RESET -> picks 'good'");

    /* lifecycle off again: RESET is ignored (legacy) -> "bad" returns. */
    reg.lifecycle_enabled = 0;
    CHECK(route_plan(&reg, in, goal, &plan) == 0 && plan.length == 1 &&
          strcmp(plan.names[0], "bad") == 0,
          "lifecycle off -> RESET ignored, 'bad' again (legacy)");

    registry_free(&reg);
    btn_free(&bad);
    btn_free(&good);
}

static void test_cost_aware_ranking(void) {
    BinaryTransformNetwork big = {0};   /* higher hidden_count -> higher cost */
    BinaryTransformNetwork small = {0}; /* lower hidden_count -> lower cost */
    PrimitiveRegistry reg;
    DagSource src;
    DagPlan plan = {0};
    Port in = P(PORT_ONEHOT, 16, 1);
    Port goal = P(PORT_BINARY_MSB, 4, 1);

    printf("lifecycle: cost-aware ranking (power_mode):\n");
    /* same ports -> interchangeable for planning; different hidden_count ->
       different btn_cost. Fresh stats -> equal reliability, so cost decides. */
    if (btn_init(&big, 16, 4, 8, 8, 0.5, 1u) != 0 || btn_set_ports(&big, in, goal) != 0 ||
        btn_init(&small, 16, 4, 2, 2, 0.5, 1u) != 0 || btn_set_ports(&small, in, goal) != 0) {
        CHECK(0, "synthetic primitive setup");
        return;
    }
    CHECK(btn_cost(&big) > btn_cost(&small), "big costs more than small");

    registry_init(&reg);
    registry_add(&reg, &big, "big");     /* registry order: big first */
    registry_add(&reg, &small, "small");
    src.type = in;
    src.values = NULL;

    /* DEFAULT: equal reliability -> registry order -> 'big'. */
    reg.power_mode = CNET_POWER_DEFAULT;
    CHECK(dag_plan(&reg, &src, 1, goal, &plan) == 0 &&
          plan.root != NULL && plan.root->kind == DAG_PRIMITIVE &&
          strcmp(plan.root->name, "big") == 0,
          "DEFAULT picks 'big' (registry order)");
    dag_free(&plan);

    /* LOW: equal reliability -> cheaper -> 'small'. */
    reg.power_mode = CNET_POWER_LOW;
    CHECK(dag_plan(&reg, &src, 1, goal, &plan) == 0 &&
          plan.root != NULL && plan.root->kind == DAG_PRIMITIVE &&
          strcmp(plan.root->name, "small") == 0,
          "LOW picks 'small' (cheaper)");
    dag_free(&plan);

    registry_free(&reg);
    btn_free(&big);
    btn_free(&small);
}

static void test_self_healing_route(void) {
    BinaryTransformNetwork bad = {0};
    BinaryTransformNetwork good = {0};
    PrimitiveRegistry reg;
    double in_bits[4] = {0.0, 1.0, 0.0, 1.0};
    double out_bit[1] = {0};
    size_t h;
    Port in = P(PORT_BINARY_MSB, 4, 1);
    Port goal = P(PORT_BINARY_MSB, 1, 1);

    printf("lifecycle: self-healing re-route:\n");
    if (make_btn(&bad, 4, 1, in, goal) != 0 || make_btn(&good, 4, 1, in, goal) != 0) {
        CHECK(0, "synthetic primitive setup");
        return;
    }
    /* bad: zeroed output layer + zero bias -> output exactly 0.5 -> ambiguous
       (out-of-domain for BINARY_MSB) -> faults under strict execution. */
    for (h = 0; h < bad.max_hidden_count; ++h) bad.hidden_output_weights[h] = 0.0;
    bad.output_bias[0] = 0.0;
    /* good: zeroed output layer + strong positive bias -> output ~1.0 -> in-domain. */
    for (h = 0; h < good.max_hidden_count; ++h) good.hidden_output_weights[h] = 0.0;
    good.output_bias[0] = 8.0;

    registry_init(&reg);
    reg.lifecycle_enabled = 1;            /* required for RESET-skip on re-plan */
    registry_add(&reg, &bad, "bad");      /* registry order: planner picks 'bad' first */
    registry_add(&reg, &good, "good");

    CHECK(route_execute_healing(&reg, in, goal, in_bits, 4, out_bit, 1, 3) == 0 &&
          out_bit[0] == 1.0,
          "heals around a faulting primitive to a clean route (output 1.0)");
    CHECK(reg.entries[0].state == PRIM_RESET, "the faulting 'bad' is RESET");
    CHECK(bad.output_failures >= 1, "the fault was recorded as evidence");

    registry_free(&reg);
    btn_free(&bad);
    btn_free(&good);
}

static void test_self_healing_no_alternative(void) {
    BinaryTransformNetwork bad = {0};
    PrimitiveRegistry reg;
    double in_bits[4] = {0.0, 1.0, 0.0, 1.0};
    double out_bit[1] = {0};
    size_t h;
    Port in = P(PORT_BINARY_MSB, 4, 1);
    Port goal = P(PORT_BINARY_MSB, 1, 1);

    printf("lifecycle: self-healing with no alternative:\n");
    if (make_btn(&bad, 4, 1, in, goal) != 0) { CHECK(0, "synthetic primitive setup"); return; }
    for (h = 0; h < bad.max_hidden_count; ++h) bad.hidden_output_weights[h] = 0.0;
    bad.output_bias[0] = 0.0;

    registry_init(&reg);
    reg.lifecycle_enabled = 1;
    registry_add(&reg, &bad, "bad");

    CHECK(route_execute_healing(&reg, in, goal, in_bits, 4, out_bit, 1, 3) == -1,
          "no alternative route -> returns -1");
    CHECK(reg.entries[0].state == PRIM_RESET, "the only primitive ends RESET");

    registry_free(&reg);
    btn_free(&bad);
}

static void test_certified_add_null_name_safe(void) {
    BinaryTransformNetwork dummy = {0};
    BinaryTransformNetwork rawid = {0};
    PrimitiveRegistry reg;
    Contract c = {0};
    double in[4] = {1.0, 0.0, 1.0, 0.0};
    double tgt[5];
    const double *out;

    printf("lifecycle: registry_add_certified survives a NULL-named entry:\n");
    if (make_btn(&dummy, 4, 5, P(PORT_BINARY_MSB, 4, 1), P(PORT_RAW, 5, 1)) != 0 ||
        make_btn(&rawid, 4, 5, P(PORT_BINARY_MSB, 4, 1), P(PORT_RAW, 5, 1)) != 0) {
        CHECK(0, "synthetic primitive setup");
        return;
    }
    out = btn_forward(&rawid, in);
    memcpy(tgt, out, sizeof tgt);

    registry_init(&reg);
    registry_add(&reg, &dummy, NULL);   /* a NULL-named incumbent */
    CHECK(contract_init_borrowed(&c, "rawid", &rawid, in, tgt, 1) == 0,
          "contract_init_borrowed ok");
    /* Must not crash on strcmp(NULL, "rawid"); should append + certify. */
    CHECK(registry_add_certified(&reg, &rawid, "rawid", &c) == 0,
          "certified add appends past the NULL-named entry");
    CHECK(reg.count == 2 && reg.entries[1].state == PRIM_FROZEN,
          "new entry appended and FROZEN");

    contract_free(&c);
    registry_free(&reg);
    btn_free(&dummy);
    btn_free(&rawid);
}

static void test_record_fault(void) {
    BinaryTransformNetwork p = {0};
    PrimitiveRegistry reg;
    double in_bits[4] = {1.0, 0.0, 1.0, 0.0};
    double raw[1] = {0.5};

    printf("lifecycle: record_fault parks unlabeled + RESETs:\n");
    if (make_btn(&p, 4, 1, P(PORT_BINARY_MSB, 4, 1), P(PORT_BINARY_MSB, 1, 1)) != 0) {
        CHECK(0, "synthetic primitive setup");
        return;
    }
    registry_init(&reg);
    registry_add(&reg, &p, "p");
    CHECK(registry_record_fault(&reg, "p", in_bits, raw) == 0, "record_fault ok");
    CHECK(reg.entries[0].state == PRIM_RESET, "primitive is RESET after fault");
    CHECK(registry_pending_labels(&reg, "p") == 1, "one unlabeled fault parked");
    CHECK(registry_record_fault(&reg, "missing", in_bits, raw) == -1, "unknown name -> -1");
    registry_free(&reg);   /* must free the queue without leaking */
    btn_free(&p);
}

static void test_supply_label_external(void) {
    BinaryTransformNetwork p = {0};
    PrimitiveRegistry reg;
    double in_bits[4] = {1.0, 0.0, 1.0, 0.0};
    double raw[1] = {0.5};
    double target[1] = {1.0};

    printf("lifecycle: external-oracle supply_label:\n");
    if (make_btn(&p, 4, 1, P(PORT_BINARY_MSB, 4, 1), P(PORT_BINARY_MSB, 1, 1)) != 0) {
        CHECK(0, "synthetic primitive setup"); return;
    }
    registry_init(&reg);
    registry_add(&reg, &p, "p");
    registry_record_fault(&reg, "p", in_bits, raw);
    CHECK(registry_supply_label(&reg, "p", in_bits, target) == 0, "supply_label ok");
    CHECK(registry_pending_labels(&reg, "p") == 0, "no unlabeled left");
    CHECK(reg.entries[0].queue != NULL && reg.entries[0].queue->labeled_count == 1,
          "one labeled exemplar");
    {
        double other[4] = {0.0, 0.0, 0.0, 0.0};
        CHECK(registry_supply_label(&reg, "p", other, target) == -1,
              "no matching parked input -> -1");
    }
    registry_free(&reg);
    btn_free(&p);
}

static void test_label_via_teacher(void) {
    BinaryTransformNetwork p = {0};
    BinaryTransformNetwork teacher = {0};
    PrimitiveRegistry reg;
    double in_bits[4] = {1.0, 0.0, 1.0, 0.0};
    double raw[1] = {0.5};
    size_t h;

    printf("lifecycle: teacher labeling:\n");
    if (make_btn(&p, 4, 1, P(PORT_BINARY_MSB, 4, 1), P(PORT_BINARY_MSB, 1, 1)) != 0 ||
        make_btn(&teacher, 4, 1, P(PORT_BINARY_MSB, 4, 1), P(PORT_BINARY_MSB, 1, 1)) != 0) {
        CHECK(0, "synthetic primitive setup"); return;
    }
    /* teacher: deterministic in-domain output (strong bias -> 1.0). */
    for (h = 0; h < teacher.max_hidden_count; ++h) teacher.hidden_output_weights[h] = 0.0;
    teacher.output_bias[0] = 8.0;

    registry_init(&reg);
    registry_add(&reg, &p, "p");
    registry_add(&reg, &teacher, "teacher");
    registry_record_fault(&reg, "p", in_bits, raw);
    CHECK(registry_label_via_teacher(&reg, "p") == 1, "teacher labeled one fault");
    CHECK(registry_pending_labels(&reg, "p") == 0, "no unlabeled left");
    CHECK(reg.entries[0].queue->labeled_count == 1 &&
          reg.entries[0].queue->labeled_targets[0] == 1.0,
          "labeled target is the teacher's canonical output (1.0)");
    registry_free(&reg);
    btn_free(&p);
    btn_free(&teacher);
}

/* Build a 1-bit identity contract (0->0, 1->1) borrowed against `p`. */
static int make_identity_contract(Contract *c, const BinaryTransformNetwork *p,
                                  double *ins, double *outs) {
    ins[0] = 0.0; outs[0] = 0.0;
    ins[1] = 1.0; outs[1] = 1.0;
    return contract_init_borrowed(c, "id1", p, ins, outs, 2);
}

static void test_heal_restores_frozen(void) {
    BinaryTransformNetwork p = {0};
    PrimitiveRegistry reg;
    Contract c = {0};
    double ins[2], outs[2];
    double fin[1] = {1.0};
    double fraw[1] = {0.5};
    double ftgt[1] = {1.0};
    Port b1 = P(PORT_BINARY_MSB, 1, 1);

    printf("lifecycle: heal retrains + re-certifies -> FROZEN:\n");
    if (btn_init(&p, 1, 1, 4, 4, 0.5, 7u) != 0 || btn_set_ports(&p, b1, b1) != 0) {
        CHECK(0, "synthetic primitive setup"); return;
    }
    CHECK(make_identity_contract(&c, &p, ins, outs) == 0, "identity contract built");

    registry_init(&reg);
    registry_add(&reg, &p, "p");
    registry_record_fault(&reg, "p", fin, fraw);          /* RESET + unlabeled */
    CHECK(registry_supply_label(&reg, "p", fin, ftgt) == 0, "labeled the fault");

    CHECK(registry_heal(&reg, "p", &c, 20000) == 1, "heal succeeds");
    CHECK(reg.entries[0].state == PRIM_FROZEN, "primitive restored to FROZEN");
    CHECK(reg.entries[0].certified == 1, "primitive marked certified");
    CHECK(reg.entries[0].queue->labeled_count == 0, "labeled queue cleared");

    contract_free(&c);
    registry_free(&reg);
    btn_free(&p);
}

static void test_heal_no_label_stays_reset(void) {
    BinaryTransformNetwork p = {0};
    PrimitiveRegistry reg;
    Contract c = {0};
    double ins[2], outs[2];
    double fin[1] = {1.0};
    double fraw[1] = {0.5};
    Port b1 = P(PORT_BINARY_MSB, 1, 1);

    printf("lifecycle: heal without a verified target stays RESET:\n");
    if (btn_init(&p, 1, 1, 4, 4, 0.5, 7u) != 0 || btn_set_ports(&p, b1, b1) != 0) {
        CHECK(0, "synthetic primitive setup"); return;
    }
    make_identity_contract(&c, &p, ins, outs);
    registry_init(&reg);
    registry_add(&reg, &p, "p");
    registry_record_fault(&reg, "p", fin, fraw);   /* unlabeled only */

    CHECK(registry_heal(&reg, "p", &c, 20000) == 0, "heal is a no-op without a label");
    CHECK(reg.entries[0].state == PRIM_RESET, "primitive stays RESET");

    contract_free(&c);
    registry_free(&reg);
    btn_free(&p);
}

static void test_heal_cannot_certify_stays_reset(void) {
    BinaryTransformNetwork p = {0};
    PrimitiveRegistry reg;
    Contract c = {0};
    /* Contradictory contract: input 1 -> 0 AND input 1 -> 1. Cannot be certified. */
    double ins[2] = {1.0, 1.0};
    double outs[2] = {0.0, 1.0};
    double fin[1] = {1.0};
    double fraw[1] = {0.5};
    double ftgt[1] = {1.0};
    Port b1 = P(PORT_BINARY_MSB, 1, 1);

    printf("lifecycle: heal that can't certify stays RESET (the guarantee):\n");
    if (btn_init(&p, 1, 1, 4, 4, 0.5, 7u) != 0 || btn_set_ports(&p, b1, b1) != 0) {
        CHECK(0, "synthetic primitive setup"); return;
    }
    CHECK(contract_init_borrowed(&c, "bad1", &p, ins, outs, 2) == 0, "contradictory contract built");
    registry_init(&reg);
    registry_add(&reg, &p, "p");
    registry_record_fault(&reg, "p", fin, fraw);
    registry_supply_label(&reg, "p", fin, ftgt);

    CHECK(registry_heal(&reg, "p", &c, 20000) == 0, "heal returns 0 (certify failed)");
    CHECK(reg.entries[0].state == PRIM_RESET, "primitive stays RESET");
    CHECK(reg.entries[0].certified == 0, "primitive is NOT certified");

    contract_free(&c);
    registry_free(&reg);
    btn_free(&p);
}

static void test_shadow_excluded_from_planning(void) {
    BinaryTransformNetwork a = {0};
    BinaryTransformNetwork s = {0};
    PrimitiveRegistry reg;
    DagSource src;
    DagPlan plan = {0};
    Port in = P(PORT_ONEHOT, 16, 1);
    Port goal = P(PORT_BINARY_MSB, 4, 1);

    printf("lifecycle: shadow excluded from planning:\n");
    if (make_btn(&a, 16, 4, in, goal) != 0 || make_btn(&s, 16, 4, in, goal) != 0) {
        CHECK(0, "synthetic primitive setup"); return;
    }
    registry_init(&reg);
    registry_add(&reg, &s, "s");   /* shadow registered FIRST (reg order would pick it) */
    registry_add(&reg, &a, "a");
    CHECK(registry_set_shadow(&reg, "s", "a") == 0, "mark s as shadow of a");
    src.type = in; src.values = NULL;

    reg.lifecycle_enabled = 1;
    CHECK(dag_plan(&reg, &src, 1, goal, &plan) == 0 &&
          plan.root != NULL && plan.root->kind == DAG_PRIMITIVE &&
          strcmp(plan.root->name, "a") == 0,
          "lifecycle on -> shadow 's' skipped, 'a' chosen");
    dag_free(&plan);

    registry_free(&reg);
    btn_free(&a);
    btn_free(&s);
}

static void test_shadow_accrues_evidence(void) {
    BinaryTransformNetwork a = {0};
    BinaryTransformNetwork s = {0};
    PrimitiveRegistry reg;
    double in_bits[4] = {0.0, 1.0, 0.0, 1.0};
    size_t h, ran;
    Port in = P(PORT_BINARY_MSB, 4, 1);
    Port goal = P(PORT_BINARY_MSB, 1, 1);

    printf("lifecycle: shadow accrues evidence without driving output:\n");
    if (make_btn(&a, 4, 1, in, goal) != 0 || make_btn(&s, 4, 1, in, goal) != 0) {
        CHECK(0, "synthetic primitive setup"); return;
    }
    for (h = 0; h < s.max_hidden_count; ++h) s.hidden_output_weights[h] = 0.0;
    s.output_bias[0] = 8.0;   /* deterministic in-domain output (1.0) */

    registry_init(&reg);
    registry_add(&reg, &a, "a");
    registry_add(&reg, &s, "s");
    registry_set_shadow(&reg, "s", "a");

    ran = registry_run_shadows(&reg, "a", in_bits, 4);
    CHECK(ran == 1, "one shadow run");
    CHECK(s.output_successes == 1 && s.output_failures == 0, "shadow recorded a success");
    CHECK(a.output_successes == 0 && a.output_failures == 0, "active unaffected");

    registry_free(&reg);
    btn_free(&a);
    btn_free(&s);
}

static void test_shadow_promote(void) {
    BinaryTransformNetwork a = {0};
    BinaryTransformNetwork s = {0};
    PrimitiveRegistry reg;
    Contract c = {0};
    double in_bits[4] = {0.0, 1.0, 0.0, 1.0};
    double cin[4] = {0.0, 1.0, 0.0, 1.0};
    double cout[1] = {1.0};
    size_t h, i;
    Port in = P(PORT_BINARY_MSB, 4, 1);
    Port goal = P(PORT_BINARY_MSB, 1, 1);

    printf("lifecycle: shadow evidence-gated promotion:\n");
    if (make_btn(&a, 4, 1, in, goal) != 0 || make_btn(&s, 4, 1, in, goal) != 0) {
        CHECK(0, "synthetic primitive setup"); return;
    }
    /* both constant 1.0 -> both certify the contract {in -> 1}. */
    for (h = 0; h < a.max_hidden_count; ++h) a.hidden_output_weights[h] = 0.0;
    a.output_bias[0] = 8.0;
    for (h = 0; h < s.max_hidden_count; ++h) s.hidden_output_weights[h] = 0.0;
    s.output_bias[0] = 8.0;

    CHECK(contract_init_borrowed(&c, "const1", &a, cin, cout, 1) == 0, "contract built");

    registry_init(&reg);
    registry_add_certified(&reg, &a, "a", &c);   /* a is active, FROZEN */
    registry_add(&reg, &s, "s");
    registry_set_shadow(&reg, "s", "a");

    CHECK(shadow_promote_if_ready(&reg, "s", &c, 8) == 0, "not promoted yet (no evidence)");
    for (i = 0; i < 8; ++i) registry_run_shadows(&reg, "a", in_bits, 4);
    CHECK(shadow_promote_if_ready(&reg, "s", &c, 8) == 1, "promoted after evidence");

    {
        size_t si = 0, ai = 0, k;
        for (k = 0; k < reg.count; ++k) {
            if (strcmp(reg.entries[k].name, "s") == 0) si = k;
            if (strcmp(reg.entries[k].name, "a") == 0) ai = k;
        }
        CHECK(reg.entries[si].shadow_of == NULL && reg.entries[si].state == PRIM_FROZEN &&
              reg.entries[si].certified == 1, "shadow promoted to FROZEN primary");
        CHECK(reg.entries[ai].state == PRIM_RESET, "old active demoted to RESET");
    }

    contract_free(&c);
    registry_free(&reg);
    btn_free(&a);
    btn_free(&s);
}

int run_test_lifecycle(void) {
    test_certified_add_null_name_safe();
    test_fuzzy_default();
    test_set_state();
    test_frozen_on_certify();
    test_promote_provisional();
    test_reset_skip_opt_in();
    test_cost_aware_ranking();
    test_self_healing_route();
    test_self_healing_no_alternative();
    test_record_fault();
    test_supply_label_external();
    test_label_via_teacher();
    test_heal_restores_frozen();
    test_heal_no_label_stays_reset();
    test_heal_cannot_certify_stays_reset();
    test_shadow_excluded_from_planning();
    test_shadow_accrues_evidence();
    test_shadow_promote();

    if (failures == 0) {
        printf("\nAll lifecycle tests passed.\n");
        return 0;
    }
    printf("\n%d lifecycle test(s) FAILED.\n", failures);
    return 1;
}

