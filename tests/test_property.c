/*
 * Tests for property round-trip and malformed-file rejection.
 * TDD: written against stubs first; positive CHECKs are expected to FAIL
 * until src/property.c is fully implemented.
 */
#include "../include/nn.h"
#include "../include/router.h"
#include "../include/property.h"

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

static void fill_law(Property *p) {
    memset(p, 0, sizeof *p);
    strcpy(p->name, "unpack_inverts_pack");
    p->sources[0] = PT(PORT_BINARY_MSB, 1, 1, "bit");
    p->sources[1] = PT(PORT_BINARY_MSB, 1, 1, "bit");
    p->source_count = 2;
    strcpy(p->lhs[0], "pack");
    strcpy(p->lhs[1], "unpack");
    p->lhs_len = 2;
    p->rhs_len = 0;
}

static void test_roundtrip(void) {
    Property p, back;

    printf("property round-trip:\n");
    fill_law(&p);
    CHECK(property_save(&p, "tmp_property.txt") == 0, "saves");
    memset(&back, 0, sizeof back);
    CHECK(property_load(&back, "tmp_property.txt") == 0, "loads");
    CHECK(strcmp(back.name, "unpack_inverts_pack") == 0 &&
          back.source_count == 2 &&
          back.sources[0].family == PORT_BINARY_MSB &&
          strcmp(back.sources[1].tag, "bit") == 0 &&
          back.lhs_len == 2 && strcmp(back.lhs[0], "pack") == 0 &&
          strcmp(back.lhs[1], "unpack") == 0 && back.rhs_len == 0,
          "round-trip preserves name, sources, chains, identity RHS");

    {
        FILE *f = fopen("tmp_property_bad.txt", "w");
        if (f != NULL) { fputs("CNET_WRONG 1\n", f); fclose(f); }
        CHECK(property_load(&back, "tmp_property_bad.txt") == -1,
              "rejects a wrong magic");
    }
    {
        /* LHS 0 is not a law */
        FILE *f = fopen("tmp_property_bad.txt", "w");
        if (f != NULL) {
            fputs("CNET_PROPERTY 1\nx\nSOURCES 1\n"
                  "PORT binary_msb 1 1 bit\nLHS 0\nRHS 0\n", f);
            fclose(f);
        }
        CHECK(property_load(&back, "tmp_property_bad.txt") == -1,
              "rejects an empty LHS");
    }
    {
        FILE *f = fopen("tmp_property_bad.txt", "w");
        if (f != NULL) {
            fputs("CNET_PROPERTY 1\nbad name!\nSOURCES 1\n"
                  "PORT binary_msb 1 1 bit\nLHS 1\npack\nRHS 0\n", f);
            fclose(f);
        }
        CHECK(property_load(&back, "tmp_property_bad.txt") == -1,
              "rejects a non-atom name");
    }
    {
        FILE *f = fopen("tmp_property_bad.txt", "w");
        if (f != NULL) {
            fputs("CNET_PROPERTY 1\nx\nSOURCES 9\n", f);
            fclose(f);
        }
        CHECK(property_load(&back, "tmp_property_bad.txt") == -1,
              "rejects source count out of bounds");
    }
    {
        FILE *f = fopen("tmp_property_bad.txt", "w");
        if (f != NULL) {
            fputs("CNET_PROPERTY 1\nx\nSOURCES 1\n"
                  "PORT chrome 1 1 bit\nLHS 1\npack\nRHS 0\n", f);
            fclose(f);
        }
        CHECK(property_load(&back, "tmp_property_bad.txt") == -1,
              "rejects an unknown family");
    }
    CHECK(property_load(&back, "no_such_file.txt") == -1,
          "missing file returns -1");

    /* Step 3.0: untagged-source persistence path */
    {
        Property p2, back2;
        fill_law(&p2);
        p2.sources[1] = PT(PORT_BINARY_MSB, 1, 1, NULL);
        CHECK(property_save(&p2, "tmp_property_untag.txt") == 0,
              "untagged source saves");
        memset(&back2, 0, sizeof back2);
        CHECK(property_load(&back2, "tmp_property_untag.txt") == 0,
              "untagged source loads");
        CHECK(back2.sources[1].tag[0] == '\0',
              "untagged source round-trips as untagged");
        remove("tmp_property_untag.txt");
    }

    remove("tmp_property.txt");
    remove("tmp_property_bad.txt");
}

/* Two MSB-first bits of i (0..3). */
static void msb2(int i, double *out) {
    out[0] = (double)((i >> 1) & 1);
    out[1] = (double)(i & 1);
}

/* pack: [BINARY_MSB1 "bit", BINARY_MSB1 "bit"] -> BINARY_MSB2 "pair". */
static int make_pack(BinaryTransformNetwork *b) {
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

/* unpack: BINARY_MSB2 "pair" -> [BINARY_MSB1 "bit", BINARY_MSB1 "bit"]. */
static int make_unpack(BinaryTransformNetwork *b) {
    double inputs[4][2];
    double targets[4][2];
    Port in_port;
    Port out_ports[2];
    int i;
    if (btn_init(b, 2, 2, 1, 16, 0.8, 29u) != 0) return -1;
    in_port = PT(PORT_BINARY_MSB, 2, 1, "pair");
    out_ports[0] = PT(PORT_BINARY_MSB, 1, 1, "bit");
    out_ports[1] = PT(PORT_BINARY_MSB, 1, 1, "bit");
    if (btn_set_io_ports(b, &in_port, 1, out_ports, 2) != 0) return -1;
    for (i = 0; i < 4; ++i) {
        msb2(i, inputs[i]);
        msb2(i, targets[i]);
    }
    return btn_train_dynamic(b, &inputs[0][0], &targets[0][0], 4,
                             60000, 500, 0.0015, 0.01) <= 0.05 ? 0 : -1;
}

static void test_laws_hold(void) {
    BinaryTransformNetwork pack = {0};
    BinaryTransformNetwork unpack = {0};
    PrimitiveRegistry reg;
    Property p;
    PropertyReport rep;

    printf("property_check (laws hold):\n");
    CHECK(make_pack(&pack) == 0 && make_unpack(&unpack) == 0,
          "inverse pair trains");
    registry_init(&reg);
    registry_add(&reg, &pack, "pack");
    registry_add(&reg, &unpack, "unpack");

    pack.output_successes = 7;
    pack.output_failures = 3;

    fill_law(&p);  /* unpack(pack(a,b)) = identity */
    memset(&rep, 0, sizeof rep);
    CHECK(property_check(&p, &reg, 4096, &rep) == 0,
          "unpack inverts pack: the law holds");
    CHECK(rep.inputs == 4 && rep.held == 4 && rep.violated == 0,
          "report counts the full domain");
    CHECK(pack.output_successes == 7 && pack.output_failures == 3,
          "checking records no reliability evidence");

    /* the other direction: pack(unpack(p)) = identity on pairs */
    memset(&p, 0, sizeof p);
    strcpy(p.name, "pack_inverts_unpack");
    p.sources[0] = PT(PORT_BINARY_MSB, 2, 1, "pair");
    p.source_count = 1;
    strcpy(p.lhs[0], "unpack");
    strcpy(p.lhs[1], "pack");
    p.lhs_len = 2;
    p.rhs_len = 0;
    memset(&rep, 0, sizeof rep);
    CHECK(property_check(&p, &reg, 4096, &rep) == 0 &&
          rep.inputs == 4 && rep.held == 4,
          "pack inverts unpack: the law holds");

    registry_free(&reg);
    btn_free(&pack);
    btn_free(&unpack);
}

static void test_violation_and_gates(void) {
    BinaryTransformNetwork pack = {0};
    BinaryTransformNetwork unpack = {0};
    PrimitiveRegistry reg;
    Property p;
    PropertyReport rep;

    printf("property_check (violations + gates):\n");
    CHECK(make_pack(&pack) == 0 && make_unpack(&unpack) == 0,
          "inverse pair trains");
    registry_init(&reg);
    registry_add(&reg, &pack, "pack");
    registry_add(&reg, &unpack, "unpack");

    /* a broken retrain cannot hide from the algebra */
    /* +10.0: must be large enough to push outputs past the canonicalization
       margin -- a small nudge gets rounded away and produces no violation. */
    unpack.hidden_output_weights[0] += 10.0;
    fill_law(&p);
    memset(&rep, 0, sizeof rep);
    CHECK(property_check(&p, &reg, 4096, &rep) == -1,
          "a perturbed implementation violates the law");
    CHECK(rep.inputs == 4 && rep.violated >= 1 &&
          rep.held + rep.violated == 4,
          "report shows how broken it is");
    unpack.hidden_output_weights[0] -= 10.0;

    /* static gates: each refuses with no inputs checked */
    fill_law(&p);
    strcpy(p.lhs[0], "nosuch");
    memset(&rep, 0, sizeof rep);
    CHECK(property_check(&p, &reg, 4096, &rep) == -1 && rep.inputs == 0,
          "unresolvable name refuses before any execution");

    fill_law(&p);
    p.source_count = 1;  /* one bit source vs pack's two input ports */
    CHECK(property_check(&p, &reg, 4096, NULL) == -1,
          "source/step port-count mismatch refuses");

    fill_law(&p);
    p.lhs_len = 1;  /* pack alone: final [pair] vs identity [bit,bit] */
    CHECK(property_check(&p, &reg, 4096, NULL) == -1,
          "LHS/RHS representation mismatch refuses");

    fill_law(&p);
    p.sources[0] = PT(PORT_RAW, 1, 1, NULL);
    CHECK(property_check(&p, &reg, 4096, NULL) == -1,
          "a RAW source is not enumerable");

    fill_law(&p);
    CHECK(property_check(&p, &reg, 3, NULL) == -1,
          "refuses when the domain exceeds max_samples");

    fill_law(&p);
    p.lhs_len = 0;
    CHECK(property_check(&p, &reg, 4096, NULL) == -1,
          "an empty LHS is not a law");

    registry_free(&reg);
    btn_free(&pack);
    btn_free(&unpack);
}

int run_test_property(void) {
    test_roundtrip();
    test_laws_hold();
    test_violation_and_gates();

    if (failures == 0) {
        printf("\nAll property tests passed.\n");
        return 0;
    }
    printf("\n%d property test(s) FAILED.\n", failures);
    return 1;
}
