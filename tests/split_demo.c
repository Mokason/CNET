/*
 * End-to-end multi-output demonstration.
 *
 * split takes a byte (BINARY_MSB 8 "byte_value") and exposes TWO output
 * ports (BINARY_MSB 4 "nibble_value" each). With split and increment
 * registered and a byte source, the planner builds
 *     increment(split(byte)[0])
 * -- selecting an output PORT, not just a primitive -- and dag_execute
 * projects exactly that nibble segment into increment. Asserts the result
 * equals hi_nibble + 1 for several bytes.
 *
 * Requires split_weights.txt + increment_weights.txt from ./nn_demo. Run
 * from the repo root (make split).
 */
#include "../include/nn.h"
#include "../include/router.h"

#include <stdio.h>

static Port P(PortFamily family, size_t field_width, size_t field_count) {
    Port p;
    p.family = family;
    p.field_width = field_width;
    p.field_count = field_count;
    p.tag[0] = '\0';
    return p;
}

static int bits_to_int(const double *values, size_t n) {
    int value = 0;
    size_t i;

    for (i = 0; i < n; ++i) {
        value = (value << 1) | (values[i] >= 0.5 ? 1 : 0);
    }
    return value;
}

static void print_node(const DagNode *node, int indent) {
    int i;

    for (i = 0; i < indent; ++i) {
        printf("  ");
    }
    if (node->kind == DAG_SOURCE) {
        printf("source[%d]\n", node->source_index);
    } else {
        size_t s;

        printf("%s[out %d]\n", node->name, node->output_index);
        for (s = 0; s < node->child_count; ++s) {
            print_node(node->children[s], indent + 1);
        }
    }
}

int main(void) {
    static const unsigned char bytes[] = {0x12, 0xAF, 0x70, 0xF3, 0x05, 0xE9};
    const size_t n_bytes = sizeof(bytes) / sizeof(bytes[0]);
    BinaryTransformNetwork split = {0};
    BinaryTransformNetwork incr = {0};
    PrimitiveRegistry reg;
    int failures = 0;
    size_t k;
    int printed_tree = 0;

    if (btn_load(&split, "split_weights.txt") != 0 ||
        btn_load(&incr, "increment_weights.txt") != 0) {
        fprintf(stderr, "FAIL: could not load frozen primitives (run ./nn_demo "
                        "first).\n");
        return 1;
    }

    registry_init(&reg);
    registry_add(&reg, &split, "split");
    registry_add(&reg, &incr, "increment");

    printf("DAG goal: BINARY_MSB 5 from one byte source (BINARY_MSB 8)\n\n");

    for (k = 0; k < n_bytes; ++k) {
        double byte_vec[8];
        double output[5] = {0};
        DagSource sources[1];
        DagPlan plan = {0};
        int hi = (bytes[k] >> 4) & 0xF;
        int got;
        int expected = hi + 1;
        int bit;

        for (bit = 0; bit < 8; ++bit) {
            byte_vec[bit] = (double)((bytes[k] >> (7 - bit)) & 1);
        }
        sources[0].type = P(PORT_BINARY_MSB, 8, 1);
        sources[0].values = byte_vec;

        if (dag_plan(&reg, sources, 1, P(PORT_BINARY_MSB, 5, 1), &plan) != 0) {
            fprintf(stderr, "FAIL: no DAG for byte 0x%02X\n", bytes[k]);
            ++failures;
            continue;
        }
        if (!printed_tree) {
            printf("discovered DAG:\n");
            print_node(plan.root, 1);
            printf("\n");
            printed_tree = 1;
        }
        if (dag_execute(&plan, sources, 1, output, 5) != 0) {
            fprintf(stderr, "FAIL: dag_execute failed for byte 0x%02X\n",
                    bytes[k]);
            ++failures;
            dag_free(&plan);
            continue;
        }
        got = bits_to_int(output, 5);
        printf("  0x%02X -> [DAG] -> %2d | expected %2d (hi nibble %X + 1) %s\n",
               bytes[k], got, expected, hi,
               got == expected ? "OK" : "<-- MISMATCH");
        if (got != expected) {
            ++failures;
        }
        dag_free(&plan);
    }

    registry_free(&reg);
    btn_free(&split);
    btn_free(&incr);

    if (failures == 0) {
        printf("\nSPLIT PASS: planner selected an output PORT and executed "
               "increment(split(byte)[0]) with no hand-wiring.\n");
        return 0;
    }
    printf("\nSPLIT FAIL: %d/%lu mismatched.\n", failures,
           (unsigned long)n_bytes);
    return 1;
}
