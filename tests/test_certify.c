/*
 * Tests for contract round-trip and malformed-file rejection.
 * TDD: written against stubs first; positive CHECKs are expected to FAIL
 * until src/contract.c is fully implemented.
 */
#include "../include/nn.h"
#include "../include/router.h"
#include "../include/contract/contract.h"
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

/* decoder_b: ONEHOT4 "sym" -> BINARY_MSB2 "val", i -> i (same as decoder but
   seed 37u, so it certifies against the same contract). */
static int make_decoder_b(BinaryTransformNetwork *b) {
    double inputs[4][4] = {{0}};
    double targets[4][2];
    int i;
    if (btn_init(b, 4, 2, 1, 16, 0.8, 37u) != 0) return -1;
    if (btn_set_ports(b, PT(PORT_ONEHOT, 4, 1, "sym"),
                      PT(PORT_BINARY_MSB, 2, 1, "val")) != 0) return -1;
    for (i = 0; i < 4; ++i) {
        inputs[i][i] = 1.0;
        msb2(i, targets[i]);
    }
    return btn_train_dynamic(b, &inputs[0][0], &targets[0][0], 4,
                             60000, 500, 0.0015, 0.01) <= 0.05 ? 0 : -1;
}

static void test_registry_and_knob(void) {
    BinaryTransformNetwork decoder = {0};
    BinaryTransformNetwork decoder_b = {0};
    BinaryTransformNetwork inc = {0};
    Contract c;
    PrimitiveRegistry reg;
    RoutePlan plan;
    double inputs[4][4] = {{0}};
    double targets[4][2];
    int i;

    printf("registry_add_certified + require_certified:\n");
    CHECK(make_decoder(&decoder) == 0 && make_decoder_b(&decoder_b) == 0 &&
          make_inc(&inc, 1) == 0,
          "primitives train");
    for (i = 0; i < 4; ++i) {
        inputs[i][i] = 1.0;
        msb2(i, targets[i]);
    }
    CHECK(contract_init_borrowed(&c, "decode_sym", &decoder,
                                 &inputs[0][0], &targets[0][0], 4) == 0,
          "contract built");

    registry_init(&reg);
    CHECK(reg.require_certified == 0, "knob zero-initialized");

    /* the imposter: inc claims the decoder contract -- refused, nothing
       registered */
    CHECK(registry_add_certified(&reg, &inc, "imposter", &c) == -1 &&
          reg.count == 0,
          "an imposter is refused and not registered");

    CHECK(registry_add_certified(&reg, &decoder, "decoder", &c) == 0 &&
          reg.count == 1 && reg.entries[0].certified == 1,
          "a certified net registers with the flag set");

    /* decoy: same contract-compatible behavior, registered PLAIN, with
       better evidence -- wins on score until the knob filters it */
    registry_add(&reg, &decoder_b, "decoy");
    CHECK(reg.entries[1].certified == 0, "plain registration stays uncertified");
    decoder_b.output_successes = 50;
    decoder_b.output_failures = 0;

    CHECK(route_plan(&reg, PT(PORT_ONEHOT, 4, 1, "sym"),
                     PT(PORT_BINARY_MSB, 2, 1, "val"), &plan) == 0 &&
          plan.length == 1 && plan.steps[0] == &decoder_b,
          "knob off: the better-evidenced uncertified decoy wins");

    reg.require_certified = 1;
    CHECK(route_plan(&reg, PT(PORT_ONEHOT, 4, 1, "sym"),
                     PT(PORT_BINARY_MSB, 2, 1, "val"), &plan) == 0 &&
          plan.length == 1 && plan.steps[0] == &decoder,
          "knob on: only the certified entry is considered");

    /* an uncertified-only registry yields no plan under the knob */
    {
        PrimitiveRegistry reg2;
        registry_init(&reg2);
        registry_add(&reg2, &decoder_b, "decoy");
        reg2.require_certified = 1;
        CHECK(route_plan(&reg2, PT(PORT_ONEHOT, 4, 1, "sym"),
                         PT(PORT_BINARY_MSB, 2, 1, "val"), &plan) == -1,
              "knob on + nothing certified -> no plan");
        registry_free(&reg2);
    }

    /* dag planner honors the knob too */
    {
        PrimitiveRegistry reg3;
        DagSource src;
        DagPlan dplan = {0};
        double dummy[4] = {1.0, 0.0, 0.0, 0.0};

        registry_init(&reg3);
        registry_add(&reg3, &decoder, "decoder");  /* plain -> uncertified */
        src.type = PT(PORT_ONEHOT, 4, 1, "sym");
        src.values = dummy;
        reg3.require_certified = 1;
        CHECK(dag_plan(&reg3, &src, 1, PT(PORT_BINARY_MSB, 2, 1, "val"),
                       &dplan) == -1,
              "dag knob on + nothing certified -> no plan");
        reg3.require_certified = 0;
        CHECK(dag_plan(&reg3, &src, 1, PT(PORT_BINARY_MSB, 2, 1, "val"),
                       &dplan) == 0,
              "dag knob off: plan found");
        dag_free(&dplan);
        registry_free(&reg3);
    }

    registry_free(&reg);
    btn_free(&decoder);
    btn_free(&decoder_b);
    btn_free(&inc);
}

static void test_certification(void) {
    BinaryTransformNetwork decoder = {0};
    Contract c;
    CertifyReport rep;
    double inputs[4][4] = {{0}};
    double targets[4][2];
    int i;

    printf("btn_certify:\n");
    CHECK(make_decoder(&decoder) == 0, "decoder trains");
    for (i = 0; i < 4; ++i) {
        inputs[i][i] = 1.0;
        msb2(i, targets[i]);
    }
    CHECK(contract_init_borrowed(&c, "decode_sym", &decoder,
                                 &inputs[0][0], &targets[0][0], 4) == 0,
          "contract built");

    decoder.output_successes = 7;
    decoder.output_failures = 3;
    memset(&rep, 0, sizeof rep);
    CHECK(btn_certify(&decoder, &c, &rep) == 0,
          "a correct net certifies");
    CHECK(rep.exemplars == 4 && rep.passed == 4 && rep.failed == 0,
          "report counts the full pass");
    CHECK(decoder.output_successes == 7 && decoder.output_failures == 3,
          "certification leaves reliability counters untouched");

    /* behavior gate: one tampered exemplar fails the whole claim */
    targets[2][0] = 1.0 - targets[2][0];
    memset(&rep, 0, sizeof rep);
    CHECK(btn_certify(&decoder, &c, &rep) == -1,
          "a tampered exemplar denies certification");
    CHECK(rep.failed >= 1 && rep.passed + rep.failed == 4,
          "report shows the miss");
    targets[2][0] = 1.0 - targets[2][0];

    /* signature gate: tags and shape must match EXACTLY */
    {
        Contract wrong = c;
        CHECK(port_set_tag(&wrong.input_ports[0], "other") == 0 &&
              btn_certify(&decoder, &wrong, NULL) == -1,
              "a different tag denies certification");
        wrong = c;
        CHECK(port_set_tag(&wrong.input_ports[0], "") == 0 &&
              btn_certify(&decoder, &wrong, NULL) == -1,
              "an untagged contract port vs a tagged primitive denies");
        wrong = c;
        wrong.input_ports[0].field_width = 5;
        CHECK(btn_certify(&decoder, &wrong, NULL) == -1,
              "a shape mismatch denies certification");
    }

    btn_free(&decoder);
}

static void test_contract_swap_if_better(void) {
    BinaryTransformNetwork active = {0};
    BinaryTransformNetwork candidate = {0};
    BinaryTransformNetwork rejected = {0};
    BinaryTransformNetwork *active_layer = NULL;
    Contract c;
    double inputs[4][4] = {{0}};
    double targets[4][2];
    int i;

    printf("runtime contract improvement gate:\n");
    for (i = 0; i < 4; ++i) {
        inputs[i][i] = 1.0;
        msb2(i, targets[i]);
    }
    CHECK(btn_init(&active, 4, 2, 1, 16, 0.8, 11u) == 0 &&
          btn_set_ports(&active, PT(PORT_ONEHOT, 4, 1, "sym"),
                        PT(PORT_BINARY_MSB, 2, 1, "val")) == 0,
          "active incumbent created with matching ports");
    CHECK(make_decoder(&candidate) == 0 &&
          contract_init_borrowed(&c, "decode_sym", &candidate,
                                &inputs[0][0], &targets[0][0], 4) == 0,
          "test contract built");
    CHECK(
          contract_better_if(&c, active_layer, &candidate) == 1 &&
          contract_swap_if_better(&c, &active_layer, &candidate) == 1 &&
          active_layer == &candidate,
          "null incumbent can be replaced by the certified candidate");

    CHECK(btn_init(&rejected, 4, 2, 1, 16, 0.8, 99u) == 0 &&
          btn_set_ports(&rejected, PT(PORT_ONEHOT, 4, 1, "sym"),
                        PT(PORT_BINARY_MSB, 2, 1, "other")) == 0 &&
          contract_swap_if_better(&c, &active_layer, &rejected) == 0 &&
          active_layer == &candidate,
          "a non-certified replacement is rejected");

    CHECK(active_layer != NULL &&
          active_layer == &candidate &&
          contract_better_if(&c, active_layer, &candidate) == 0,
          "same certified network does not replace itself");

    btn_free(&active);
    btn_free(&candidate);
    btn_free(&rejected);
}

static void test_roundtrip(void) {
    BinaryTransformNetwork decoder = {0};
    Contract c, back;
    double inputs[4][4] = {{0}};
    double targets[4][2];
    int i;

    printf("contract round-trip:\n");
    CHECK(make_decoder(&decoder) == 0, "decoder trains");
    for (i = 0; i < 4; ++i) {
        inputs[i][i] = 1.0;
        msb2(i, targets[i]);
    }
    CHECK(contract_init_borrowed(&c, "decode_sym", &decoder,
                                 &inputs[0][0], &targets[0][0], 4) == 0,
          "init from a borrowed table");
    CHECK(c.owns_data == 0 && c.exemplar_count == 4 &&
          c.input_port_count == 1 && c.output_port_count == 1 &&
          strcmp(c.input_ports[0].tag, "sym") == 0,
          "signature copied from the primitive");
    CHECK(contract_init_borrowed(&c, "bad name!", &decoder,
                                 &inputs[0][0], &targets[0][0], 4) == -1,
          "rejects a non-atom name");

    CHECK(contract_init_borrowed(&c, "decode_sym", &decoder,
                                 &inputs[0][0], &targets[0][0], 4) == 0 &&
          contract_save(&c, "tmp_contract.txt") == 0,
          "saves");
    memset(&back, 0, sizeof back);
    CHECK(contract_load(&back, "tmp_contract.txt") == 0,
          "loads");
    CHECK(back.owns_data == 1 && back.exemplar_count == 4 &&
          strcmp(back.name, "decode_sym") == 0 &&
          back.input_ports[0].family == PORT_ONEHOT &&
          back.input_ports[0].field_width == 4 &&
          strcmp(back.input_ports[0].tag, "sym") == 0 &&
          strcmp(back.output_ports[0].tag, "val") == 0,
          "round-trip preserves name, ports, tags");
    {
        int same = (back.inputs != NULL && back.outputs != NULL) ? 1 : 0;
        for (i = 0; same && i < 4 * 4; ++i) {
            if (back.inputs[i] != (&inputs[0][0])[i]) same = 0;
        }
        for (i = 0; same && i < 4 * 2; ++i) {
            if (back.outputs[i] != (&targets[0][0])[i]) same = 0;
        }
        CHECK(same, "round-trip preserves the exemplar values");
    }
    contract_free(&back);

    /* malformed files refuse */
    {
        FILE *f = fopen("tmp_contract_bad.txt", "w");
        if (f != NULL) {
            fputs("CNET_WRONG 1\n", f);
            fclose(f);
        }
        memset(&back, 0, sizeof back);
        CHECK(contract_load(&back, "tmp_contract_bad.txt") == -1,
              "rejects a wrong magic");
    }
    {
        /* valid header, non-canonical exemplar value */
        FILE *f = fopen("tmp_contract_bad.txt", "w");
        if (f != NULL) {
            fputs("CNET_CONTRACT 1\ndecode_sym\nINPUTS 1\n"
                  "PORT_IN onehot 4 1 sym\nOUTPUTS 1\n"
                  "PORT_OUT binary_msb 2 1 val\nEXEMPLARS 1\n"
                  "1 0 0 0 0.5 0\n", f);
            fclose(f);
        }
        memset(&back, 0, sizeof back);
        CHECK(contract_load(&back, "tmp_contract_bad.txt") == -1,
              "rejects a non-canonical exemplar value");
    }
    CHECK(contract_load(&back, "no_such_file.txt") == -1,
          "missing file returns -1");
    {
        /* Absurd field_width/field_count/exemplar_count designed to wrap
           size_t multiplication on both 32- and 64-bit targets.  A malformed
           file must be refused, not trusted with arithmetic. */
        FILE *f = fopen("tmp_contract_bad.txt", "w");
        if (f != NULL) {
            fputs("CNET_CONTRACT 1\n"
                  "decode_sym\n"
                  "INPUTS 1\n"
                  "PORT_IN onehot 4294967295 4294967295 sym\n"
                  "OUTPUTS 1\n"
                  "PORT_OUT binary_msb 2 1 val\n"
                  "EXEMPLARS 4294967295\n"
                  "1 0 0 0 1 0\n", f);
            fclose(f);
        }
        memset(&back, 0, sizeof back);
        CHECK(contract_load(&back, "tmp_contract_bad.txt") == -1,
              "rejects overflowing declared dimensions");
    }

    remove("tmp_contract.txt");
    remove("tmp_contract_bad.txt");
    btn_free(&decoder);
}

static void test_emission(void) {
    BinaryTransformNetwork decoder = {0};
    BinaryTransformNetwork inc = {0};
    PrimitiveRegistry reg;
    RoutePlan plan;
    Contract c;
    int i;

    printf("contract_from_route / contract_from_dag:\n");
    CHECK(make_decoder(&decoder) == 0 && make_inc(&inc, 1) == 0,
          "primitives train");
    registry_init(&reg);
    registry_add(&reg, &decoder, "decoder");
    registry_add(&reg, &inc, "inc");

    CHECK(route_plan(&reg, PT(PORT_ONEHOT, 4, 1, "sym"),
                     PT(PORT_BINARY_MSB, 2, 1, "val2"), &plan) == 0 &&
          plan.length == 2,
          "teacher chain plans");

    decoder.output_successes = 7;
    decoder.output_failures = 3;
    memset(&c, 0, sizeof c);
    CHECK(contract_from_route(&plan, "sym_inc", 4096, &c) == 0,
          "route emission succeeds");
    CHECK(c.owns_data == 1 && c.exemplar_count == 4 &&
          strcmp(c.name, "sym_inc") == 0 &&
          strcmp(c.input_ports[0].tag, "sym") == 0 &&
          strcmp(c.output_ports[0].tag, "val2") == 0,
          "emitted signature is the plan boundary (incl tags)");
    CHECK(decoder.output_successes == 7 && decoder.output_failures == 3,
          "member evidence restored after emission");
    {
        /* the table IS the teacher: row i = onehot(i) -> bits((i+1)%4) */
        int all = 1;
        for (i = 0; i < 4; ++i) {
            double want[2];
            int hot = -1, j;
            for (j = 0; j < 4; ++j) {
                if (c.inputs[(size_t)i * 4 + j] == 1.0) hot = j;
            }
            msb2((hot + 1) % 4, want);
            if (c.outputs[(size_t)i * 2] != want[0] ||
                c.outputs[(size_t)i * 2 + 1] != want[1]) all = 0;
        }
        CHECK(all, "emitted rows match the teacher's mapping");
    }
    contract_free(&c);

    /* a 1-step plan is allowed for emission (unlike consolidation) */
    CHECK(route_plan(&reg, PT(PORT_ONEHOT, 4, 1, "sym"),
                     PT(PORT_BINARY_MSB, 2, 1, "val"), &plan) == 0 &&
          plan.length == 1 &&
          contract_from_route(&plan, "decode_sym", 4096, &c) == 0 &&
          c.exemplar_count == 4,
          "1-step emission allowed");
    /* ...and the original primitive certifies against its own emitted
       contract */
    CHECK(btn_certify(&decoder, &c, NULL) == 0,
          "primitive certifies against its own emitted contract");
    contract_free(&c);

    /* guards: cap */
    CHECK(route_plan(&reg, PT(PORT_ONEHOT, 4, 1, "sym"),
                     PT(PORT_BINARY_MSB, 2, 1, "val2"), &plan) == 0 &&
          contract_from_route(&plan, "sym_inc", 3, &c) == -1,
          "refuses when the domain exceeds max_samples");
    CHECK(contract_from_route(&plan, "bad name!", 4096, &c) == -1,
          "refuses a non-atom name");

    registry_free(&reg);
    btn_free(&decoder);
    btn_free(&inc);
}

static void test_emission_certifies_chunk(void) {
    BinaryTransformNetwork decoder2 = {0};
    BinaryTransformNetwork combiner = {0};
    BinaryTransformNetwork chunk = {0};
    PrimitiveRegistry reg;
    DagSource sources[2];
    DagPlan plan = {0};
    Contract c;
    double dummy_a[2] = {1.0, 0.0};
    double dummy_b[2] = {0.0, 1.0};

    printf("emission certifies the chunk (the loop closes):\n");
    CHECK(make_decoder2(&decoder2) == 0 && make_combiner(&combiner) == 0,
          "primitives train");
    registry_init(&reg);
    registry_add(&reg, &decoder2, "decoder2");
    registry_add(&reg, &combiner, "combiner");
    sources[0].type = PT(PORT_ONEHOT, 2, 1, "bsym");
    sources[0].values = dummy_a;
    sources[1].type = PT(PORT_ONEHOT, 2, 1, "bsym");
    sources[1].values = dummy_b;
    CHECK(dag_plan(&reg, sources, 2, PT(PORT_BINARY_MSB, 2, 1, "pair"),
                   &plan) == 0,
          "teacher DAG plans");

    CHECK(consolidate_dag(&plan, sources, 2, NULL, &chunk, NULL) == 0,
          "chunk distills");
    memset(&c, 0, sizeof c);
    CHECK(contract_from_dag(&plan, sources, 2, "pair_from_bsyms",
                            4096, &c) == 0 && c.exemplar_count == 4,
          "contract emitted from the same teacher plan");
    CHECK(btn_certify(&chunk, &c, NULL) == 0,
          "the chunk certifies against its teacher's contract");

    /* guard: an unconsumed source refuses */
    {
        DagSource three[3];
        three[0] = sources[0];
        three[1] = sources[1];
        three[2] = sources[0];
        CHECK(contract_from_dag(&plan, three, 3, "pair_from_bsyms",
                                4096, &c) == -1,
              "refuses an unconsumed source");
    }

    contract_free(&c);
    dag_free(&plan);
    btn_free(&chunk);
    registry_free(&reg);
    btn_free(&decoder2);
    btn_free(&combiner);
}

/* Margin-floor certification: btn_certify reports the worst-case output
   headroom, and btn_certify_robust raises the bar from "not ambiguous" to
   "has margin >= floor" -- so a correct primitive can be refused for thin
   headroom. Operationalizes the margin/correctness-orthogonality finding. */
static void test_margin_floor(void) {
    BinaryTransformNetwork decoder = {0};
    Contract c;
    CertifyReport report;
    double inputs[4][4] = {{0}};
    double targets[4][2];
    double m;
    int i;

    printf("margin-floor certification:\n");
    CHECK(make_decoder(&decoder) == 0, "decoder trains");
    for (i = 0; i < 4; ++i) {
        inputs[i][i] = 1.0;
        msb2(i, targets[i]);
    }
    CHECK(contract_init_borrowed(&c, "decode_sym", &decoder,
                                 &inputs[0][0], &targets[0][0], 4) == 0,
          "contract built");

    /* port_margin: native per-family headroom (binary min|v-0.5|; onehot gap). */
    {
        double v_bin[2] = {0.9, 0.1};
        double v_tight[2] = {0.9, 0.55};
        double v_oh[4] = {0.8, 0.1, 0.05, 0.05};
        double mg = -1.0;
        CHECK(port_margin(PT(PORT_BINARY_MSB, 2, 1, NULL), v_bin, &mg) == 0 &&
              mg > 0.399 && mg < 0.401, "binary margin = min|v-0.5|");
        CHECK(port_margin(PT(PORT_BINARY_MSB, 2, 1, NULL), v_tight, &mg) == 0 &&
              mg > 0.049 && mg < 0.051, "binary margin tracks the weakest bit");
        CHECK(port_margin(PT(PORT_ONEHOT, 4, 1, NULL), v_oh, &mg) == 0 &&
              mg > 0.699 && mg < 0.701, "onehot margin = top1 - top2");
    }

    /* btn_certify now reports the worst-case headroom. */
    CHECK(btn_certify(&decoder, &c, &report) == 0, "decoder certifies");
    m = report.min_margin;
    CHECK(m > 0.25 && m <= 0.5, "reported headroom is above the band, below the rail");

    /* The robust bar: floor 0 == certify; below the headroom passes; above refuses. */
    CHECK(btn_certify_robust(&decoder, &c, 0.0, &report) == 0,
          "floor 0 reduces to plain certification");
    CHECK(btn_certify_robust(&decoder, &c, m - 0.05, &report) == 0,
          "clears a floor below its headroom");
    CHECK(btn_certify_robust(&decoder, &c, m + 0.05, &report) == -1,
          "a CORRECT primitive is refused for thin headroom");

    contract_free(&c);
    btn_free(&decoder);
}

/* point 5: exercise the frozen contract init (layered parent, borrowed
   const tables from a would-be generated data). Always runs (no generated dep). */
static void test_frozen_contract_init(void) {
    /* tiny 2-exemplar contract over 1 raw-ish port (for test only) */
    static const Port in_p = { PORT_BINARY_MSB, 2, 1, "t" };
    static const Port out_p = { PORT_BINARY_MSB, 2, 1, "t" };
    static const double ins[2*2] = { 0.0, 0.0,  1.0, 0.0 };   /* 2 rows x 2-wide */
    static const double outs[2*2] = { 0.0, 1.0,  1.0, 1.0 };
    static const FrozenContractData fd = {
        "layered_t", "parent_p",
        1, 1, &in_p, &out_p,
        ins, outs, 2
    };
    Contract c;
    printf("frozen contract init:\n");
    CHECK(contract_init_frozen(&c, &fd) == 0, "init frozen contract");
    CHECK(strcmp(c.name, "layered_t") == 0, "name copied");
    CHECK(strcmp(c.parent, "parent_p") == 0, "parent copied (layered)");
    CHECK(c.exemplar_count == 2, "exemplar count");
    CHECK(c.inputs == ins && c.outputs == outs, "tables borrowed (no own)");
    CHECK(c.owns_data == 0, "not owner");
    contract_free(&c);
    CHECK(c.inputs == NULL, "free clears");
}

int run_test_certify(void) {
    test_roundtrip();
    test_certification();
    test_contract_swap_if_better();
    test_registry_and_knob();
    test_emission();
    test_emission_certifies_chunk();
    test_margin_floor();
    test_frozen_contract_init();

    if (failures == 0) {
        printf("\nAll certification tests passed.\n");
        return 0;
    }
    printf("\n%d certification test(s) FAILED.\n", failures);
    return 1;
}

