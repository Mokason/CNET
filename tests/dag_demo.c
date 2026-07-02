/*
 * End-to-end DAG demonstration: the planner discovers a BRANCHING DAG over real
 * frozen primitives and executes it.
 *
 * Goal: produce a byte (BINARY_MSB 8) from two hex-digit sources (ONEHOT 16).
 * With hex_value and combine registered, the planner builds
 *     combine(hex_value(src_hi), hex_value(src_lo))
 * -- reusing the frozen hex_value on each branch -- and dag_execute runs it,
 * assembling combine's two-slot input from the two sub-results. Asserts the byte
 * equals value(hi)*16 + value(lo).
 *
 * Requires hex_value_weights.txt + combine_weights.txt from ./nn_demo. Run from
 * the repo root (make dag).
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

static int hex_digit_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    return c - 'A' + 10;
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
        printf("%s\n", node->name);
        for (s = 0; s < node->child_count; ++s) {
            print_node(node->children[s], indent + 1);
        }
    }
}

int main(void) {
    static const char *pairs[] = {"41", "FF", "00", "10", "A5", "DE"};
    const size_t n_pairs = sizeof(pairs) / sizeof(pairs[0]);
    BinaryTransformNetwork hexval = {0};
    BinaryTransformNetwork combine = {0};
    PrimitiveRegistry reg;
    int failures = 0;
    size_t k;
    int printed_tree = 0;

    if (btn_load(&hexval, "hex_value_weights.txt") != 0 ||
        btn_load(&combine, "combine_weights.txt") != 0) {
        fprintf(stderr, "FAIL: could not load frozen primitives (run ./nn_demo "
                        "first).\n");
        return 1;
    }

    registry_init(&reg);
    registry_add(&reg, &hexval, "hex_value");
    registry_add(&reg, &combine, "combine");

    printf("DAG goal: two hex digits -> byte (BINARY_MSB 8)\n\n");

    for (k = 0; k < n_pairs; ++k) {
        double hi_vec[16] = {0};
        double lo_vec[16] = {0};
        double output[8] = {0};
        DagSource sources[2];
        DagPlan plan = {0};
        int hi = hex_digit_value(pairs[k][0]);
        int lo = hex_digit_value(pairs[k][1]);
        int got;
        int expected = hi * 16 + lo;

        hi_vec[hi] = 1.0;
        lo_vec[lo] = 1.0;
        sources[0].type = P(PORT_ONEHOT, 16, 1);
        sources[0].values = hi_vec;
        sources[1].type = P(PORT_ONEHOT, 16, 1);
        sources[1].values = lo_vec;

        if (dag_plan(&reg, sources, 2, P(PORT_BINARY_MSB, 8, 1), &plan) != 0) {
            fprintf(stderr, "FAIL: no DAG for pair %s\n", pairs[k]);
            ++failures;
            continue;
        }

        if (!printed_tree) {
            printf("discovered DAG:\n");
            print_node(plan.root, 1);
            printf("\n");
            printed_tree = 1;
        }

        if (dag_execute(&plan, sources, 2, output, 8) != 0) {
            fprintf(stderr, "FAIL: dag_execute failed for pair %s\n", pairs[k]);
            ++failures;
            dag_free(&plan);
            continue;
        }
        got = bits_to_int(output, 8);
        printf("  %c%c -> [DAG] -> 0x%02X (%3d) | expected 0x%02X (%3d) %s\n",
               pairs[k][0], pairs[k][1], got, got, expected, expected,
               got == expected ? "OK" : "<-- MISMATCH");
        if (got != expected) {
            ++failures;
        }
        dag_free(&plan);
    }

    registry_free(&reg);
    btn_free(&hexval);
    btn_free(&combine);

    if (failures == 0) {
        printf("\nDAG PASS: planner built AND executed combine(hex_value, "
               "hex_value) with no hand-wiring.\n");
        return 0;
    }
    printf("\nDAG FAIL: %d/%lu mismatched.\n", failures, (unsigned long)n_pairs);
    return 1;
}
