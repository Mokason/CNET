/*
 * Tests for per-primitive interface contracts (Port model + persistence).
 *
 * port_compatible: static wire-time check (producer output vs consumer input).
 * port_validate:   runtime domain check of an actual vector against a port.
 * btn_set_ports:   authoring guard (port totals must match in/out counts).
 * persistence:     v2 round-trip preserves ports; v1 files load as RAW.
 */
#include "../include/nn.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, desc) do {                       \
    if (cond) { printf("  ok   %s\n", (desc)); }     \
    else { printf("  FAIL %s\n", (desc)); ++failures; } \
} while (0)

static Port port(PortFamily family, size_t field_width, size_t field_count) {
    Port p;
    p.family = family;
    p.field_width = field_width;
    p.field_count = field_count;
    p.tag[0] = '\0';
    return p;
}

static void test_port_compatible(void) {
    printf("port_compatible:\n");
    CHECK(port_compatible(port(PORT_BINARY_MSB, 4, 1),
                          port(PORT_BINARY_MSB, 4, 1)) == 1,
          "binary_msb 4x1 -> binary_msb 4x1 compatible");
    CHECK(port_compatible(port(PORT_BINARY_MSB, 4, 1),
                          port(PORT_BINARY_LSB, 4, 1)) == 0,
          "binary_msb 4x1 -> binary_lsb 4x1 INCOMPATIBLE (bit-order, same width)");
    CHECK(port_compatible(port(PORT_BINARY_MSB, 4, 1),
                          port(PORT_BINARY_MSB, 5, 1)) == 0,
          "binary_msb 4x1 -> binary_msb 5x1 INCOMPATIBLE (width)");
    CHECK(port_compatible(port(PORT_ONEHOT, 16, 2),
                          port(PORT_ONEHOT, 16, 2)) == 1,
          "onehot 16x2 -> onehot 16x2 compatible");
    CHECK(port_compatible(port(PORT_ONEHOT, 16, 2),
                          port(PORT_ONEHOT, 32, 1)) == 0,
          "onehot 16x2 -> onehot 32x1 INCOMPATIBLE (field structure, same total)");
    CHECK(port_compatible(port(PORT_ONEHOT, 16, 1),
                          port(PORT_BINARY_MSB, 16, 1)) == 0,
          "onehot 16 -> binary_msb 16 INCOMPATIBLE (family)");
    CHECK(port_compatible(port(PORT_RAW, 4, 1), port(PORT_RAW, 4, 1)) == 1,
          "raw 4 -> raw 4 compatible (legacy width-only)");
    CHECK(port_compatible(port(PORT_RAW, 4, 1), port(PORT_RAW, 5, 1)) == 0,
          "raw 4 -> raw 5 INCOMPATIBLE (width)");
}

static void test_port_validate(void) {
    double onehot16[16] = {0};
    double twohot[16] = {0};
    double mushy[16];
    double two_fields[32] = {0};
    double empty_field[32] = {0};
    double bits[4] = {1.0, 0.0, 1.0, 0.0};
    double ambiguous[4] = {0.5, 0.5, 0.5, 0.5};
    double anything[3] = {0.37, -2.0, 99.0};
    size_t i;

    printf("port_validate:\n");

    onehot16[10] = 1.0;
    twohot[3] = 1.0;
    twohot[9] = 1.0;
    for (i = 0; i < 16; ++i) {
        mushy[i] = 0.3;
    }
    two_fields[4] = 1.0;
    two_fields[16 + 5] = 1.0;
    empty_field[4] = 1.0; /* first field one-hot, second field all zero */

    CHECK(port_validate(port(PORT_ONEHOT, 16, 1), onehot16) == 1,
          "onehot 16x1 accepts a clean one-hot");
    CHECK(port_validate(port(PORT_ONEHOT, 16, 1), twohot) == 0,
          "onehot 16x1 rejects a two-hot");
    CHECK(port_validate(port(PORT_ONEHOT, 16, 1), mushy) == 0,
          "onehot 16x1 rejects all-0.3 (no clear winner)");
    CHECK(port_validate(port(PORT_ONEHOT, 16, 2), two_fields) == 1,
          "onehot 16x2 accepts two valid one-hot fields");
    CHECK(port_validate(port(PORT_ONEHOT, 16, 2), empty_field) == 0,
          "onehot 16x2 rejects an empty second field");
    CHECK(port_validate(port(PORT_BINARY_MSB, 4, 1), bits) == 1,
          "binary_msb 4x1 accepts clean bits");
    CHECK(port_validate(port(PORT_BINARY_MSB, 4, 1), ambiguous) == 0,
          "binary_msb 4x1 rejects {0.5,0.5,0.5,0.5}");
    CHECK(port_validate(port(PORT_RAW, 3, 1), anything) == 1,
          "raw accepts anything (escape hatch)");
}

static void test_btn_set_ports(void) {
    BinaryTransformNetwork b = {0};

    printf("btn_set_ports:\n");
    if (btn_init(&b, 16, 4, 1, 8, 0.5, 1u) != 0) {
        CHECK(0, "btn_init for set_ports test");
        return;
    }
    CHECK(btn_set_ports(&b, port(PORT_ONEHOT, 16, 1),
                        port(PORT_BINARY_MSB, 4, 1)) == 0,
          "accepts ports whose totals match input/output counts");
    CHECK(btn_set_ports(&b, port(PORT_ONEHOT, 8, 1),
                        port(PORT_BINARY_MSB, 4, 1)) != 0,
          "rejects input port total != input_count");
    CHECK(btn_set_ports(&b, port(PORT_ONEHOT, 16, 1),
                        port(PORT_BINARY_MSB, 5, 1)) != 0,
          "rejects output port total != output_count");
    btn_free(&b);
}

static void test_persistence_roundtrip(void) {
    BinaryTransformNetwork a = {0};
    BinaryTransformNetwork b = {0};
    const char *tmp = "tmp_contract_roundtrip.txt";

    printf("port persistence (v3 round-trip, single input):\n");
    if (btn_init(&a, 16, 4, 1, 8, 0.5, 1u) != 0 ||
        btn_set_ports(&a, port(PORT_ONEHOT, 16, 1),
                      port(PORT_BINARY_MSB, 4, 1)) != 0 ||
        btn_save(&a, tmp) != 0 ||
        btn_load(&b, tmp) != 0) {
        CHECK(0, "round-trip setup");
        btn_free(&a);
        return;
    }
    CHECK(b.input_port_count == 1, "loaded single input port count");
    CHECK(b.input_ports[0].family == PORT_ONEHOT &&
          b.input_ports[0].field_width == 16 &&
          b.input_ports[0].field_count == 1,
          "loaded input_ports[0] matches saved (onehot 16x1)");
    CHECK(b.output_ports[0].family == PORT_BINARY_MSB &&
          b.output_ports[0].field_width == 4 &&
          b.output_ports[0].field_count == 1,
          "loaded output_ports[0] matches saved (binary_msb 4x1)");
    btn_free(&a);
    btn_free(&b);
    remove(tmp);
}

static void test_heterogeneous_ports(void) {
    BinaryTransformNetwork a = {0};
    BinaryTransformNetwork b = {0};
    const char *tmp = "tmp_contract_hetero.txt";
    Port good[2];
    Port bad_total[2];

    printf("heterogeneous input ports:\n");

    /* combine flag(BINARY 1) + value(BINARY 4) = 5 inputs. */
    good[0] = port(PORT_BINARY_MSB, 1, 1);
    good[1] = port(PORT_BINARY_MSB, 4, 1);
    bad_total[0] = port(PORT_BINARY_MSB, 1, 1);
    bad_total[1] = port(PORT_BINARY_MSB, 8, 1); /* totals 9 != input_count 5 */

    if (btn_init(&a, 5, 5, 1, 8, 0.5, 1u) != 0) {
        CHECK(0, "btn_init for heterogeneous test");
        return;
    }
    CHECK(btn_set_input_ports(&a, good, 2, port(PORT_BINARY_MSB, 5, 1)) == 0,
          "btn_set_input_ports accepts two differently-typed ports summing to 5");
    CHECK(btn_set_input_ports(&a, bad_total, 2,
                              port(PORT_BINARY_MSB, 5, 1)) != 0,
          "rejects ports whose totals != input_count");
    CHECK(btn_set_input_ports(&a, good, 0, port(PORT_BINARY_MSB, 5, 1)) != 0,
          "rejects n = 0");
    CHECK(btn_set_input_ports(&a, good, BTN_MAX_INPUT_PORTS + 1,
                              port(PORT_BINARY_MSB, 5, 1)) != 0,
          "rejects n > BTN_MAX_INPUT_PORTS");

    /* re-set good ports, then v3 round-trip preserves both */
    if (btn_set_input_ports(&a, good, 2, port(PORT_BINARY_MSB, 5, 1)) != 0 ||
        btn_save(&a, tmp) != 0 ||
        btn_load(&b, tmp) != 0) {
        CHECK(0, "v3 heterogeneous round-trip setup");
        btn_free(&a);
        return;
    }
    CHECK(b.input_port_count == 2, "v3 round-trip preserves 2 input ports");
    CHECK(b.input_ports[0].family == PORT_BINARY_MSB &&
          b.input_ports[0].field_width == 1,
          "input_ports[0] is BINARY_MSB 1 (flag)");
    CHECK(b.input_ports[1].family == PORT_BINARY_MSB &&
          b.input_ports[1].field_width == 4,
          "input_ports[1] is BINARY_MSB 4 (value)");
    btn_free(&a);
    btn_free(&b);
    remove(tmp);
}

static void test_v2_backward_compat(void) {
    BinaryTransformNetwork b = {0};

    printf("v2 backward compatibility:\n");
    if (btn_load(&b, "tests/fixtures/v2_hex_value.txt") != 0) {
        CHECK(0, "load genuine v2 fixture");
        return;
    }
    CHECK(b.input_port_count == 1, "v2 loads as a single input port");
    CHECK(b.input_ports[0].family == PORT_ONEHOT &&
          b.input_ports[0].field_width == 16 &&
          b.input_ports[0].field_count == 1,
          "v2 input_ports[0] is onehot 16x1");
    CHECK(b.output_ports[0].family == PORT_BINARY_MSB &&
          b.output_ports[0].field_width == 4,
          "v2 output_ports[0] is binary_msb 4");
    btn_free(&b);
}

static void test_v1_backward_compat(void) {
    BinaryTransformNetwork b = {0};

    printf("v1 backward compatibility:\n");
    if (btn_load(&b, "tests/fixtures/v1_hex_value.txt") != 0) {
        CHECK(0, "load genuine v1 fixture");
        return;
    }
    CHECK(b.input_count == 16 && b.output_count == 4,
          "v1 dims load correctly (16 -> 4)");
    CHECK(b.input_port_count == 1, "v1 has a single input port");
    CHECK(b.input_ports[0].family == PORT_RAW &&
          b.input_ports[0].field_width == 16 &&
          b.input_ports[0].field_count == 1,
          "v1 input_ports[0] defaults to RAW (width = input_count)");
    CHECK(b.output_ports[0].family == PORT_RAW &&
          b.output_ports[0].field_width == 4 &&
          b.output_ports[0].field_count == 1,
          "v1 output_ports[0] defaults to RAW (width = output_count)");
    btn_free(&b);
}

static Port tagged(PortFamily family, size_t field_width, size_t field_count,
                   const char *tag) {
    Port p = port(family, field_width, field_count);

    port_set_tag(&p, tag);
    return p;
}

static void test_semantic_tags(void) {
    printf("semantic tags:\n");
    CHECK(port_compatible(tagged(PORT_BINARY_MSB, 4, 1, "nibble_value"),
                          tagged(PORT_BINARY_MSB, 4, 1, "nibble_value")) == 1,
          "same representation, same tag -> compatible");
    CHECK(port_compatible(tagged(PORT_BINARY_MSB, 4, 1, "card_rank"),
                          tagged(PORT_BINARY_MSB, 4, 1, "nibble_value")) == 0,
          "same representation, different tag -> INCOMPATIBLE");
    CHECK(port_compatible(tagged(PORT_BINARY_MSB, 4, 1, "nibble_value"),
                          port(PORT_BINARY_MSB, 4, 1)) == 1,
          "tagged producer -> untagged consumer compatible (wildcard)");
    CHECK(port_compatible(port(PORT_BINARY_MSB, 4, 1),
                          tagged(PORT_BINARY_MSB, 4, 1, "nibble_value")) == 1,
          "untagged producer -> tagged consumer compatible (wildcard)");
    CHECK(port_compatible(tagged(PORT_BINARY_MSB, 4, 1, "nibble_value"),
                          tagged(PORT_BINARY_LSB, 4, 1, "nibble_value")) == 0,
          "same tag never overrides a representation mismatch");
    CHECK(port_compatible(tagged(PORT_RAW, 4, 1, "card_rank"),
                          tagged(PORT_BINARY_MSB, 4, 1, "nibble_value")) == 0,
          "tag mismatch beats even the RAW width fallback");

    {
        Port p = port(PORT_BINARY_MSB, 4, 1);

        CHECK(port_set_tag(&p, "nibble_value") == 0 &&
              strcmp(p.tag, "nibble_value") == 0,
              "port_set_tag stores a valid tag");
        CHECK(port_set_tag(&p, "") == 0 && p.tag[0] == '\0',
              "empty tag clears (untagged)");
        CHECK(port_set_tag(&p,
              "this_tag_is_far_too_long_to_fit_in_the_field") != 0,
              "rejects an overlong tag");
        CHECK(port_set_tag(&p, "has space") != 0,
              "rejects whitespace in a tag");
        CHECK(port_set_tag(&p, "-") != 0,
              "rejects '-' (reserved as the untagged file sentinel)");
    }
}

static void test_tag_persistence(void) {
    BinaryTransformNetwork a = {0};
    BinaryTransformNetwork b = {0};
    const char *tmp = "tmp_contract_tags.txt";
    Port ins[2];

    printf("tag persistence (v4 round-trip):\n");
    ins[0] = tagged(PORT_BINARY_MSB, 1, 1, "cond_flag");
    ins[1] = port(PORT_BINARY_MSB, 4, 1); /* untagged stays untagged */
    if (btn_init(&a, 5, 5, 1, 8, 0.5, 1u) != 0 ||
        btn_set_input_ports(&a, ins, 2,
                            tagged(PORT_BINARY_MSB, 5, 1,
                                   "cond_result")) != 0 ||
        btn_save(&a, tmp) != 0 ||
        btn_load(&b, tmp) != 0) {
        CHECK(0, "tagged round-trip setup");
        btn_free(&a);
        return;
    }
    CHECK(strcmp(b.input_ports[0].tag, "cond_flag") == 0,
          "round-trip preserves the tagged input port");
    CHECK(b.input_ports[1].tag[0] == '\0',
          "round-trip preserves the untagged input port");
    CHECK(strcmp(b.output_ports[0].tag, "cond_result") == 0,
          "round-trip preserves the output tag");
    btn_free(&a);
    btn_free(&b);
    remove(tmp);
}

static void test_v3_backward_compat(void) {
    BinaryTransformNetwork b = {0};

    printf("v3 backward compatibility:\n");
    if (btn_load(&b, "tests/fixtures/v3_hex_value.txt") != 0) {
        CHECK(0, "load genuine v3 fixture");
        return;
    }
    CHECK(b.input_port_count == 1 &&
          b.input_ports[0].family == PORT_ONEHOT &&
          b.input_ports[0].field_width == 16,
          "v3 input port loads (onehot 16)");
    CHECK(b.input_ports[0].tag[0] == '\0' && b.output_ports[0].tag[0] == '\0',
          "v3 ports load untagged");
    btn_free(&b);
}

static void test_reliability_score(void) {
    BinaryTransformNetwork b = {0};

    printf("reliability score:\n");
    if (btn_init(&b, 4, 4, 1, 4, 0.5, 1u) != 0) {
        CHECK(0, "btn_init for reliability test");
        return;
    }
    CHECK(btn_reliability(&b) == 0.5,
          "fresh primitive scores the 0.5 prior");
    b.output_successes = 3;
    CHECK(btn_reliability(&b) == 4.0 / 5.0,
          "3 successes -> (3+1)/(3+2)");
    b.output_failures = 2;
    CHECK(btn_reliability(&b) == 4.0 / 7.0,
          "mixed record -> (3+1)/(3+2+2)");
    btn_free(&b);
}

static void test_multi_output_authoring(void) {
    BinaryTransformNetwork b = {0};
    Port in1 = port(PORT_BINARY_MSB, 8, 1);
    Port outs[2];
    Port bad_outs[2];

    printf("multi-output authoring:\n");
    outs[0] = port(PORT_BINARY_MSB, 4, 1);
    outs[1] = port(PORT_BINARY_MSB, 4, 1);
    bad_outs[0] = port(PORT_BINARY_MSB, 4, 1);
    bad_outs[1] = port(PORT_BINARY_MSB, 8, 1); /* totals 12 != 8 */

    if (btn_init(&b, 8, 8, 1, 8, 0.5, 1u) != 0) {
        CHECK(0, "btn_init for multi-output test");
        return;
    }
    CHECK(btn_set_io_ports(&b, &in1, 1, outs, 2) == 0 &&
          b.output_port_count == 2,
          "accepts two output ports summing to output_count");
    CHECK(btn_set_io_ports(&b, &in1, 1, bad_outs, 2) != 0,
          "rejects output ports whose totals != output_count");
    CHECK(btn_set_io_ports(&b, &in1, 1, outs, 0) != 0,
          "rejects n_out = 0");
    CHECK(btn_set_io_ports(&b, &in1, 1, outs, BTN_MAX_OUTPUT_PORTS + 1) != 0,
          "rejects n_out > BTN_MAX_OUTPUT_PORTS");
    {
        Port bad_in = port(PORT_BINARY_MSB, 4, 1); /* total 4 != 8 */

        CHECK(btn_set_io_ports(&b, &bad_in, 1, outs, 2) != 0,
              "rejects input ports whose totals != input_count");
    }
    btn_free(&b);
}

static void test_multi_output_persistence(void) {
    BinaryTransformNetwork a = {0};
    BinaryTransformNetwork b = {0};
    const char *tmp = "tmp_contract_multiout.txt";
    Port in1 = port(PORT_BINARY_MSB, 8, 1);
    Port outs[2];

    printf("multi-output persistence (v5 round-trip):\n");
    outs[0] = tagged(PORT_BINARY_MSB, 4, 1, "left_half");
    outs[1] = tagged(PORT_BINARY_MSB, 4, 1, "right_half");
    if (btn_init(&a, 8, 8, 1, 8, 0.5, 1u) != 0 ||
        btn_set_io_ports(&a, &in1, 1, outs, 2) != 0 ||
        btn_save(&a, tmp) != 0 ||
        btn_load(&b, tmp) != 0) {
        CHECK(0, "multi-output round-trip setup");
        btn_free(&a);
        return;
    }
    CHECK(b.output_port_count == 2,
          "round-trip preserves 2 output ports");
    CHECK(b.output_port_count == 2 &&
          b.output_ports[1].family == PORT_BINARY_MSB &&
          b.output_ports[1].field_width == 4 &&
          strcmp(b.output_ports[1].tag, "right_half") == 0,
          "output_ports[1] round-trips with its tag");
    btn_free(&a);
    btn_free(&b);
    remove(tmp);
}

static void test_v4_backward_compat(void) {
    BinaryTransformNetwork b = {0};

    printf("v4 backward compatibility:\n");
    if (btn_load(&b, "tests/fixtures/v4_hex_value.txt") != 0) {
        CHECK(0, "load genuine v4 fixture");
        return;
    }
    CHECK(b.output_port_count == 1 &&
          b.output_ports[0].family == PORT_BINARY_MSB &&
          strcmp(b.output_ports[0].tag, "nibble_value") == 0,
          "v4 loads as a single tagged output port");
    btn_free(&b);
}

static void test_stats_persistence(void) {
    BinaryTransformNetwork a = {0};
    BinaryTransformNetwork b = {0};
    const char *tmp = "tmp_contract_stats.txt";

    printf("reliability stats sidecar:\n");
    if (btn_init(&a, 4, 4, 1, 4, 0.5, 1u) != 0 ||
        btn_init(&b, 4, 4, 1, 4, 0.5, 1u) != 0) {
        CHECK(0, "btn_init for stats sidecar test");
        btn_free(&a);
        return;
    }
    a.output_successes = 7;
    a.output_failures = 3;

    CHECK(btn_save_stats(&a, tmp) == 0, "saves a stats sidecar");
    CHECK(btn_load_stats(&b, tmp) == 0 &&
          b.output_successes == 7 && b.output_failures == 3,
          "round-trip restores the counters");
    CHECK(btn_reliability(&b) == 8.0 / 12.0,
          "restored evidence drives the score");

    b.output_successes = 99;
    b.output_failures = 99;
    CHECK(btn_load_stats(&b, "tests/fixtures/v1_hex_value.txt") == -1 &&
          b.output_successes == 99 && b.output_failures == 99,
          "rejects a non-stats file and leaves counters untouched");
    CHECK(btn_load_stats(&b, "no_such_stats_file.txt") == -1,
          "missing sidecar returns -1");

    btn_free(&a);
    btn_free(&b);
    remove(tmp);
}

int run_test_contract(void) {
    test_port_compatible();
    test_port_validate();
    test_btn_set_ports();
    test_persistence_roundtrip();
    test_heterogeneous_ports();
    test_v2_backward_compat();
    test_v1_backward_compat();
    test_semantic_tags();
    test_tag_persistence();
    test_v3_backward_compat();
    test_reliability_score();
    test_multi_output_authoring();
    test_multi_output_persistence();
    test_v4_backward_compat();
    test_stats_persistence();

    if (failures == 0) {
        printf("\nAll contract tests passed.\n");
        return 0;
    }
    printf("\n%d contract test(s) FAILED.\n", failures);
    return 1;
}
