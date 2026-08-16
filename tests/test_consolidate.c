/*
 * Tests for chunk consolidation (plan distillation): a proven route/DAG plan
 * is distilled into a NEW single primitive trained with the plan as teacher,
 * verified against it over the full enumerated input domain, and seeded with
 * the verification outcome as reliability evidence. Uses tiny primitives
 * trained in-test with fixed seeds (deterministic, fast at these sizes).
 * End-to-end consolidation over the real frozen primitives lives in
 * chunk_demo.c (make chunk).
 */
#include "../include/nn.h"
#include "../include/router.h"
#include "../include/consolidate.h"

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

/* Two MSB-first bits of i (0..3). */
static void msb2(int i, double *out) {
    out[0] = (double)((i >> 1) & 1);
    out[1] = (double)(i & 1);
}

/* decoder: ONEHOT4 "sym" -> BINARY_MSB2 "val", i -> i. */
static int make_decoder(BinaryTransformNetwork *b) {
    double inputs[4][4] = {{0}};
    double targets[4][2];
    int i;
    if (btn_init(b, 4, 2, 1, 16, 0.8, 11u) != 0) return -1;
    if (btn_set_ports(b, PT(PORT_ONEHOT, 4, 1, "sym"),
                      PT(PORT_BINARY_MSB, 2, 1, "val")) != 0) return -1;
    for (i = 0; i < 4; ++i) {
        inputs[i][i] = 1.0;
        msb2(i, targets[i]);
    }
    return btn_train_dynamic(b, &inputs[0][0], &targets[0][0], 4,
                             60000, 500, 0.0015, 0.01) <= 0.05 ? 0 : -1;
}

/* inc: BINARY_MSB2 "val" -> BINARY_MSB2 "val2", i -> (i+1)%4.
   trained=0 leaves the net at its random initialization (teacher-abort
   tests need a member whose raw output is ambiguous). */
static int make_inc(BinaryTransformNetwork *b, int trained) {
    double inputs[4][2];
    double targets[4][2];
    int i;
    if (btn_init(b, 2, 2, 1, 16, 0.8, 13u) != 0) return -1;
    if (btn_set_ports(b, PT(PORT_BINARY_MSB, 2, 1, "val"),
                      PT(PORT_BINARY_MSB, 2, 1, "val2")) != 0) return -1;
    for (i = 0; i < 4; ++i) {
        msb2(i, inputs[i]);
        msb2((i + 1) % 4, targets[i]);
    }
    if (!trained) return 0;
    return btn_train_dynamic(b, &inputs[0][0], &targets[0][0], 4,
                             60000, 500, 0.0015, 0.01) <= 0.05 ? 0 : -1;
}

/* decoder2: ONEHOT2 "bsym" -> BINARY_MSB1 "bit", i -> i. */
static int make_decoder2(BinaryTransformNetwork *b) {
    double inputs[2][2] = {{1.0, 0.0}, {0.0, 1.0}};
    double targets[2][1] = {{0.0}, {1.0}};
    if (btn_init(b, 2, 1, 1, 8, 0.8, 17u) != 0) return -1;
    if (btn_set_ports(b, PT(PORT_ONEHOT, 2, 1, "bsym"),
                      PT(PORT_BINARY_MSB, 1, 1, "bit")) != 0) return -1;
    return btn_train_dynamic(b, &inputs[0][0], &targets[0][0], 2,
                             60000, 500, 0.0015, 0.01) <= 0.05 ? 0 : -1;
}

/* swap: BINARY_MSB2 "pair" -> BINARY_MSB2 "pair_swapped", (b1,b0) -> (b0,b1). */
static int make_swap(BinaryTransformNetwork *b) {
    double inputs[4][2];
    double targets[4][2];
    int i;
    if (btn_init(b, 2, 2, 1, 16, 0.8, 23u) != 0) return -1;
    if (btn_set_ports(b, PT(PORT_BINARY_MSB, 2, 1, "pair"),
                      PT(PORT_BINARY_MSB, 2, 1, "pair_swapped")) != 0) return -1;
    for (i = 0; i < 4; ++i) {
        msb2(i, inputs[i]);
        targets[i][0] = inputs[i][1];
        targets[i][1] = inputs[i][0];
    }
    return btn_train_dynamic(b, &inputs[0][0], &targets[0][0], 4,
                             60000, 500, 0.0015, 0.01) <= 0.05 ? 0 : -1;
}

/* combiner: [BINARY_MSB1 "bit", BINARY_MSB1 "bit"] -> BINARY_MSB2 "pair",
   (hi, lo) -> hi*2 + lo. */
static int make_combiner(BinaryTransformNetwork *b) {
    double inputs[4][2];
    double targets[4][2];
    Port in_ports[2];
    int i;
    if (btn_init(b, 2, 2, 1, 16, 0.8, 19u) != 0) return -1;
    in_ports[0] = PT(PORT_BINARY_MSB, 1, 1, "bit");
    in_ports[1] = PT(PORT_BINARY_MSB, 1, 1, "bit");
    if (btn_set_input_ports(b, in_ports, 2,
                            PT(PORT_BINARY_MSB, 2, 1, "pair")) != 0) return -1;
    for (i = 0; i < 4; ++i) {
        msb2(i, inputs[i]);
        msb2(i, targets[i]);
    }
    return btn_train_dynamic(b, &inputs[0][0], &targets[0][0], 4,
                             60000, 500, 0.0015, 0.01) <= 0.05 ? 0 : -1;
}

/* Forward in through the chunk and compare the canonicalized output. */
static int chunk_gives(BinaryTransformNetwork *chunk, const double *in,
                       const double *expect, size_t out_total) {
    double clean[16];
    const double *raw = btn_forward(chunk, in);
    size_t i;
    if (raw == NULL || out_total > 16) return 0;
    if (port_canonicalize(chunk->output_ports[0], raw, clean) != 0) return 0;
    for (i = 0; i < out_total; ++i) {
        if (clean[i] != expect[i]) return 0;
    }
    return 1;
}

static void test_defaults(void) {
    ConsolidateConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    consolidate_config_defaults(&cfg);
    printf("consolidate_config_defaults:\n");
    CHECK(cfg.initial_hidden == 0, "initial_hidden 0 (auto: scale to task)");
    CHECK(cfg.max_hidden == 128, "max_hidden 128");
    CHECK(cfg.learning_rate == 0.8, "learning_rate 0.8");
    CHECK(cfg.max_epochs == 160000, "max_epochs 160000");
    CHECK(cfg.growth_window == 1000, "growth_window 1000");
    CHECK(cfg.target_loss == 0.0015, "target_loss 0.0015");
    CHECK(cfg.min_improvement == 0.01, "min_improvement 0.01");
    CHECK(cfg.min_verify_rate == 1.0, "min_verify_rate 1.0");
    CHECK(cfg.max_samples == 4096, "max_samples 4096");
}

static void test_route_consolidation(void) {
    BinaryTransformNetwork decoder = {0};
    BinaryTransformNetwork inc = {0};
    BinaryTransformNetwork chunk = {0};
    PrimitiveRegistry reg;
    RoutePlan plan, replan;
    ConsolidateReport rep;
    int i;

    printf("consolidate_route:\n");
    CHECK(make_decoder(&decoder) == 0, "decoder trains (onehot4 -> bits)");
    CHECK(make_inc(&inc, 1) == 0, "inc trains ((i+1) mod 4)");

    registry_init(&reg);
    registry_add(&reg, &decoder, "decoder");
    registry_add(&reg, &inc, "inc");

    CHECK(route_plan(&reg, PT(PORT_ONEHOT, 4, 1, "sym"),
                     PT(PORT_BINARY_MSB, 2, 1, "val2"), &plan) == 0 &&
          plan.length == 2,
          "teacher chain plans decoder -> inc");

    /* The teacher chain actually computes (i+1)%4 (guards against an
       undertrained member making later checks misleading). */
    {
        int all = 1;
        for (i = 0; i < 4; ++i) {
            double in[4] = {0};
            double out[2] = {-1.0, -1.0};
            double want[2];
            in[i] = 1.0;
            msb2((i + 1) % 4, want);
            if (route_execute(&plan, in, 4, out, 2) != 0 ||
                out[0] != want[0] || out[1] != want[1]) {
                all = 0;
            }
        }
        CHECK(all, "teacher chain computes (i+1) mod 4 on all 4 inputs");
    }

    /* Pre-existing member evidence must survive consolidation untouched. */
    decoder.output_successes = 7; decoder.output_failures = 3;
    inc.output_successes = 5; inc.output_failures = 1;

    memset(&rep, 0, sizeof rep);
    CHECK(consolidate_route(&plan, NULL, &chunk, &rep) == 0,
          "consolidation succeeds");
    CHECK(rep.samples == 4 && rep.teacher_aborts == 0,
          "4 enumerated samples, no teacher aborts");
    CHECK(rep.verified == 4 && rep.missed == 0,
          "student verified on the full domain");

    CHECK(chunk.input_port_count == 1 &&
          chunk.input_ports[0].family == PORT_ONEHOT &&
          chunk.input_ports[0].field_width == 4 &&
          strcmp(chunk.input_ports[0].tag, "sym") == 0,
          "chunk input port copied from chain boundary (incl tag)");
    CHECK(chunk.output_port_count == 1 &&
          chunk.output_ports[0].family == PORT_BINARY_MSB &&
          chunk.output_ports[0].field_width == 2 &&
          strcmp(chunk.output_ports[0].tag, "val2") == 0,
          "chunk output port copied from last step (incl tag)");

    CHECK(chunk.output_successes == 4 && chunk.output_failures == 0,
          "verification outcome seeds the chunk's evidence");
    CHECK(decoder.output_successes == 7 && decoder.output_failures == 3 &&
          inc.output_successes == 5 && inc.output_failures == 1,
          "member evidence restored after the teacher pass");

    {
        int all = 1;
        for (i = 0; i < 4; ++i) {
            double in[4] = {0};
            double want[2];
            in[i] = 1.0;
            msb2((i + 1) % 4, want);
            if (!chunk_gives(&chunk, in, want, 2)) all = 0;
        }
        CHECK(all, "chunk reproduces the teacher on all 4 inputs");
    }

    /* The planner prefers the evidenced 1-step chunk over the chain
       (0.833 vs 0.667*0.75 = 0.5) with no planner changes. */
    registry_add(&reg, &chunk, "chunk");
    CHECK(route_plan(&reg, PT(PORT_ONEHOT, 4, 1, "sym"),
                     PT(PORT_BINARY_MSB, 2, 1, "val2"), &replan) == 0 &&
          replan.length == 1 && replan.steps[0] == &chunk,
          "route_plan now picks the 1-step chunk");

    btn_free(&chunk);
    registry_free(&reg);
    btn_free(&decoder);
    btn_free(&inc);
}

static void test_route_refusals(void) {
    BinaryTransformNetwork decoder = {0};
    BinaryTransformNetwork inc = {0};
    BinaryTransformNetwork inc_raw_a = {0};
    BinaryTransformNetwork inc_raw_b = {0};
    BinaryTransformNetwork untrained = {0};
    BinaryTransformNetwork chunk = {0};
    PrimitiveRegistry reg;
    RoutePlan plan;
    ConsolidateConfig cfg;
    ConsolidateReport rep;

    printf("consolidate_route refusals:\n");
    CHECK(make_decoder(&decoder) == 0, "decoder trains");
    CHECK(make_inc(&inc, 1) == 0, "inc trains");

    registry_init(&reg);
    registry_add(&reg, &decoder, "decoder");
    registry_add(&reg, &inc, "inc");

    /* One step is already a primitive: nothing to consolidate. */
    CHECK(route_plan(&reg, PT(PORT_ONEHOT, 4, 1, "sym"),
                     PT(PORT_BINARY_MSB, 2, 1, "val"), &plan) == 0 &&
          plan.length == 1,
          "one-step chain plans");
    CHECK(consolidate_route(&plan, NULL, &chunk, NULL) == -1,
          "refuses a 1-step plan");

    /* Length-0 (identity) plan. */
    {
        RoutePlan ident;
        memset(&ident, 0, sizeof ident);
        ident.goal = PT(PORT_BINARY_MSB, 2, 1, "val");
        CHECK(consolidate_route(&ident, NULL, &chunk, NULL) == -1,
              "refuses a length-0 plan");
    }

    /* RAW boundary ports are not enumerable. */
    {
        RoutePlan rawplan;
        CHECK(btn_init(&inc_raw_a, 4, 4, 1, 4, 0.5, 23u) == 0 &&
              btn_set_ports(&inc_raw_a, PT(PORT_RAW, 4, 1, NULL),
                            PT(PORT_RAW, 4, 1, NULL)) == 0 &&
              btn_init(&inc_raw_b, 4, 4, 1, 4, 0.5, 29u) == 0 &&
              btn_set_ports(&inc_raw_b, PT(PORT_RAW, 4, 1, NULL),
                            PT(PORT_RAW, 4, 1, NULL)) == 0,
              "raw primitives build");
        memset(&rawplan, 0, sizeof rawplan);
        rawplan.steps[0] = &inc_raw_a;
        rawplan.steps[1] = &inc_raw_b;
        rawplan.names[0] = "raw_a";
        rawplan.names[1] = "raw_b";
        rawplan.length = 2;
        rawplan.goal = PT(PORT_RAW, 4, 1, NULL);
        CHECK(consolidate_route(&rawplan, NULL, &chunk, NULL) == -1,
              "refuses a RAW input boundary");
    }

    /* Enumeration over the cap refuses honestly. */
    CHECK(route_plan(&reg, PT(PORT_ONEHOT, 4, 1, "sym"),
                     PT(PORT_BINARY_MSB, 2, 1, "val2"), &plan) == 0 &&
          plan.length == 2,
          "two-step chain plans");
    consolidate_config_defaults(&cfg);
    cfg.max_samples = 3;
    CHECK(consolidate_route(&plan, &cfg, &chunk, NULL) == -1,
          "refuses when the domain exceeds max_samples");

    /* An unverifiable student is refused, not registered. */
    consolidate_config_defaults(&cfg);
    cfg.max_epochs = 0;
    memset(&rep, 0, sizeof rep);
    CHECK(consolidate_route(&plan, &cfg, &chunk, &rep) == -1,
          "refuses a student below min_verify_rate");
    CHECK(rep.samples == 4 && rep.missed >= 1 &&
          rep.verified + rep.missed == 4,
          "report shows the verification misses");

    /* A teacher that cannot label its own domain (strict aborts) yields
       no training set; member evidence is still restored. */
    {
        PrimitiveRegistry reg2;
        RoutePlan plan2;
        CHECK(make_inc(&untrained, 0) == 0, "untrained inc builds");
        registry_init(&reg2);
        registry_add(&reg2, &decoder, "decoder");
        registry_add(&reg2, &untrained, "untrained_inc");
        CHECK(route_plan(&reg2, PT(PORT_ONEHOT, 4, 1, "sym"),
                         PT(PORT_BINARY_MSB, 2, 1, "val2"), &plan2) == 0 &&
              plan2.length == 2,
              "chain over the untrained member plans");
        memset(&rep, 0, sizeof rep);
        CHECK(consolidate_route(&plan2, NULL, &chunk, &rep) == -1,
              "refuses when the strict teacher aborts everywhere");
        CHECK(rep.teacher_aborts == 4,
              "report counts the teacher aborts");
        CHECK(untrained.output_successes == 0 &&
              untrained.output_failures == 0,
              "member evidence restored even on refusal");
        registry_free(&reg2);
    }

    registry_free(&reg);
    btn_free(&decoder);
    btn_free(&inc);
    btn_free(&inc_raw_a);
    btn_free(&inc_raw_b);
    btn_free(&untrained);
}

static void test_dag_consolidation(void) {
    BinaryTransformNetwork decoder2 = {0};
    BinaryTransformNetwork combiner = {0};
    BinaryTransformNetwork chunk = {0};
    PrimitiveRegistry reg;
    DagSource sources[2];
    DagPlan plan, replan;
    ConsolidateReport rep;
    double dummy_a[2] = {1.0, 0.0};
    double dummy_b[2] = {0.0, 1.0};
    int hi, lo;

    printf("consolidate_dag:\n");
    CHECK(make_decoder2(&decoder2) == 0, "decoder2 trains (onehot2 -> bit)");
    CHECK(make_combiner(&combiner) == 0, "combiner trains (2 bits -> pair)");

    registry_init(&reg);
    registry_add(&reg, &decoder2, "decoder2");
    registry_add(&reg, &combiner, "combiner");

    sources[0].type = PT(PORT_ONEHOT, 2, 1, "bsym");
    sources[0].values = dummy_a;
    sources[1].type = PT(PORT_ONEHOT, 2, 1, "bsym");
    sources[1].values = dummy_b;

    CHECK(dag_plan(&reg, sources, 2, PT(PORT_BINARY_MSB, 2, 1, "pair"),
                   &plan) == 0 &&
          plan.root != NULL && plan.root->btn == &combiner &&
          plan.root->child_count == 2,
          "teacher DAG plans combiner(decoder2, decoder2)");

    memset(&rep, 0, sizeof rep);
    CHECK(consolidate_dag(&plan, sources, 2, NULL, &chunk, &rep) == 0,
          "DAG consolidation succeeds");
    CHECK(rep.samples == 4 && rep.teacher_aborts == 0 &&
          rep.verified == 4 && rep.missed == 0,
          "2x2 domain enumerated and fully verified");

    CHECK(chunk.input_port_count == 2 &&
          chunk.input_ports[0].family == PORT_ONEHOT &&
          chunk.input_ports[1].family == PORT_ONEHOT &&
          strcmp(chunk.input_ports[0].tag, "bsym") == 0 &&
          strcmp(chunk.input_ports[1].tag, "bsym") == 0,
          "chunk input ports follow source index order (incl tags)");
    CHECK(chunk.output_port_count == 1 &&
          chunk.output_ports[0].family == PORT_BINARY_MSB &&
          chunk.output_ports[0].field_width == 2 &&
          strcmp(chunk.output_ports[0].tag, "pair") == 0,
          "chunk output port is the root's projected port");
    CHECK(chunk.output_successes == 4 && chunk.output_failures == 0,
          "verification outcome seeds the chunk's evidence");

    {
        int all = 1;
        for (hi = 0; hi < 2; ++hi) {
            for (lo = 0; lo < 2; ++lo) {
                double in[4] = {0};
                double want[2];
                in[hi] = 1.0;        /* slot 0: onehot2 */
                in[2 + lo] = 1.0;    /* slot 1: onehot2 */
                msb2(hi * 2 + lo, want);
                if (!chunk_gives(&chunk, in, want, 2)) all = 0;
            }
        }
        CHECK(all, "chunk reproduces the teacher on all 4 source pairs");
    }

    /* The planner roots the replanned DAG at the chunk (0.833 vs three
       fresh executions at 0.125). */
    registry_add(&reg, &chunk, "pair_chunk");
    CHECK(dag_plan(&reg, sources, 2, PT(PORT_BINARY_MSB, 2, 1, "pair"),
                   &replan) == 0 &&
          replan.root != NULL && replan.root->btn == &chunk &&
          replan.root->child_count == 2 &&
          replan.root->children[0]->kind == DAG_SOURCE &&
          replan.root->children[1]->kind == DAG_SOURCE,
          "dag_plan now roots at the chunk fed by the sources");

    dag_free(&replan);

    /* Ladder: an evidence-seeded chunk plans as an INTERIOR member of a
       deeper plan (swap needs a "pair"; the chunk is the best producer:
       0.5 * 0.833 beats the spelled-out subtree at 0.5^3), and that plan
       consolidates again -- a chunk of a chunk. */
    {
        BinaryTransformNetwork swap = {0};
        BinaryTransformNetwork chunk2 = {0};
        DagPlan deeper, final;
        ConsolidateReport rep2;

        CHECK(make_swap(&swap) == 0, "swap trains (pair -> swapped pair)");
        registry_add(&reg, &swap, "swap");

        CHECK(dag_plan(&reg, sources, 2,
                       PT(PORT_BINARY_MSB, 2, 1, "pair_swapped"),
                       &deeper) == 0 &&
              deeper.root != NULL && deeper.root->btn == &swap &&
              deeper.root->child_count == 1 &&
              deeper.root->children[0]->btn == &chunk,
              "chunk plans as an interior member of a deeper plan");

        memset(&rep2, 0, sizeof rep2);
        CHECK(consolidate_dag(&deeper, sources, 2, NULL, &chunk2,
                              &rep2) == 0 &&
              rep2.verified == 4 && rep2.missed == 0,
              "a plan containing a chunk consolidates (chunk of chunk)");
        CHECK(chunk2.output_successes == 4 && chunk2.output_failures == 0,
              "chunk-of-chunk evidence seeded");

        {
            int all = 1;
            for (hi = 0; hi < 2; ++hi) {
                for (lo = 0; lo < 2; ++lo) {
                    double in[4] = {0};
                    double want[2];
                    in[hi] = 1.0;
                    in[2 + lo] = 1.0;
                    msb2(lo * 2 + hi, want);  /* swapped bits */
                    if (!chunk_gives(&chunk2, in, want, 2)) all = 0;
                }
            }
            CHECK(all, "chunk-of-chunk computes the swapped pair");
        }

        registry_add(&reg, &chunk2, "swap_chunk");
        CHECK(dag_plan(&reg, sources, 2,
                       PT(PORT_BINARY_MSB, 2, 1, "pair_swapped"),
                       &final) == 0 &&
              final.root != NULL && final.root->btn == &chunk2,
              "the deeper goal collapses to one step again");

        dag_free(&final);
        dag_free(&deeper);
        btn_free(&chunk2);
        btn_free(&swap);
    }

    dag_free(&plan);
    btn_free(&chunk);
    registry_free(&reg);
    btn_free(&decoder2);
    btn_free(&combiner);
}

static void test_dag_refusals(void) {
    BinaryTransformNetwork decoder2 = {0};
    BinaryTransformNetwork combiner = {0};
    BinaryTransformNetwork chunk = {0};
    PrimitiveRegistry reg;
    DagSource sources[3];
    DagPlan plan;
    double dummy[2] = {1.0, 0.0};

    printf("consolidate_dag refusals:\n");
    CHECK(make_decoder2(&decoder2) == 0, "decoder2 trains");
    CHECK(make_combiner(&combiner) == 0, "combiner trains");

    sources[0].type = PT(PORT_ONEHOT, 2, 1, "bsym");
    sources[0].values = dummy;
    sources[1].type = PT(PORT_ONEHOT, 2, 1, "bsym");
    sources[1].values = dummy;
    sources[2].type = PT(PORT_ONEHOT, 2, 1, "bsym");
    sources[2].values = dummy;

    /* A bare source root is the identity. */
    {
        DagNode src_node;
        DagPlan ident;
        memset(&src_node, 0, sizeof src_node);
        src_node.kind = DAG_SOURCE;
        src_node.source_index = 0;
        memset(&ident, 0, sizeof ident);
        ident.root = &src_node;
        CHECK(consolidate_dag(&ident, sources, 1, NULL, &chunk, NULL) == -1,
              "refuses a bare-source root");
    }

    /* A single-primitive tree is already a primitive. */
    {
        DagNode src_a, src_b, prim;
        DagPlan single;
        memset(&src_a, 0, sizeof src_a);
        src_a.kind = DAG_SOURCE;
        src_a.source_index = 0;
        memset(&src_b, 0, sizeof src_b);
        src_b.kind = DAG_SOURCE;
        src_b.source_index = 1;
        memset(&prim, 0, sizeof prim);
        prim.kind = DAG_PRIMITIVE;
        prim.btn = &combiner;
        prim.name = "combiner";
        prim.output_index = 0;
        prim.children[0] = &src_a;
        prim.children[1] = &src_b;
        prim.child_count = 2;
        memset(&single, 0, sizeof single);
        single.root = &prim;
        /* Use BINARY sources so the hand tree is well-typed. */
        {
            DagSource bits[2];
            bits[0].type = PT(PORT_BINARY_MSB, 1, 1, "bit");
            bits[0].values = dummy;
            bits[1].type = PT(PORT_BINARY_MSB, 1, 1, "bit");
            bits[1].values = dummy;
            CHECK(consolidate_dag(&single, bits, 2, NULL, &chunk, NULL) == -1,
                  "refuses a single-primitive tree");
        }
    }

    /* Every source must be consumed: the chunk's arity tells the truth. */
    registry_init(&reg);
    registry_add(&reg, &decoder2, "decoder2");
    registry_add(&reg, &combiner, "combiner");
    CHECK(dag_plan(&reg, sources, 2, PT(PORT_BINARY_MSB, 2, 1, "pair"),
                   &plan) == 0 && plan.root != NULL,
          "two-source teacher DAG plans");
    CHECK(consolidate_dag(&plan, sources, 3, NULL, &chunk, NULL) == -1,
          "refuses an unconsumed source");

    dag_free(&plan);
    registry_free(&reg);
    btn_free(&decoder2);
    btn_free(&combiner);
}

int run_test_consolidate(void) {
    test_defaults();
    test_route_consolidation();
    test_route_refusals();
    test_dag_consolidation();
    test_dag_refusals();

    if (failures == 0) {
        printf("\nAll consolidation tests passed.\n");
        return 0;
    }
    printf("\n%d consolidation test(s) FAILED.\n", failures);
    return 1;
}
