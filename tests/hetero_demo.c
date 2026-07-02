/*
 * End-to-end heterogeneous multi-input demonstration.
 *
 * conditional_increment takes two DIFFERENTLY-typed inputs: a flag (BINARY 1)
 * and a value (BINARY 4), producing BINARY 5 = flag ? value+1 : value. With
 * hex_value and conditional_increment registered, and a flag source plus a
 * hex-digit source, the planner builds
 *     conditional_increment(flag_src, hex_value(digit_src))
 * sourcing each heterogeneous slot from a different place, and executes it.
 * Asserts the result equals flag ? value+1 : value.
 *
 * Requires hex_value_weights.txt + cond_increment_weights.txt from ./nn_demo.
 * Run from the repo root (make hetero).
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
    struct { char digit; int flag; } cases[] = {
        {'5', 0}, {'5', 1}, {'F', 1}, {'F', 0}, {'A', 1}, {'0', 0}
    };
    const size_t n_cases = sizeof(cases) / sizeof(cases[0]);
    BinaryTransformNetwork hexval = {0};
    BinaryTransformNetwork cond = {0};
    PrimitiveRegistry reg;
    int failures = 0;
    size_t k;
    int printed_tree = 0;

    if (btn_load(&hexval, "hex_value_weights.txt") != 0 ||
        btn_load(&cond, "cond_increment_weights.txt") != 0) {
        fprintf(stderr, "FAIL: could not load frozen primitives (run ./nn_demo "
                        "first).\n");
        return 1;
    }

    registry_init(&reg);
    registry_add(&reg, &hexval, "hex_value");
    registry_add(&reg, &cond, "conditional_increment");

    printf("DAG goal: BINARY_MSB 5 from a flag (BINARY 1) + a hex digit "
           "(ONEHOT 16)\n\n");

    for (k = 0; k < n_cases; ++k) {
        double flag_vec[1];
        double digit_vec[16] = {0};
        double output[5] = {0};
        DagSource sources[2];
        DagPlan plan = {0};
        int value = hex_digit_value(cases[k].digit);
        int flag = cases[k].flag;
        int got;
        int expected = flag ? value + 1 : value;

        flag_vec[0] = (double)flag;
        digit_vec[value] = 1.0;
        sources[0].type = P(PORT_BINARY_MSB, 1, 1);
        sources[0].values = flag_vec;
        sources[1].type = P(PORT_ONEHOT, 16, 1);
        sources[1].values = digit_vec;

        if (dag_plan(&reg, sources, 2, P(PORT_BINARY_MSB, 5, 1), &plan) != 0) {
            fprintf(stderr, "FAIL: no DAG for %c flag=%d\n", cases[k].digit, flag);
            ++failures;
            continue;
        }
        if (!printed_tree) {
            printf("discovered DAG:\n");
            print_node(plan.root, 1);
            printf("\n");
            printed_tree = 1;
        }
        if (dag_execute(&plan, sources, 2, output, 5) != 0) {
            fprintf(stderr, "FAIL: dag_execute failed for %c flag=%d\n",
                    cases[k].digit, flag);
            ++failures;
            dag_free(&plan);
            continue;
        }
        got = bits_to_int(output, 5);
        printf("  %c flag=%d -> [DAG] -> %2d | expected %2d %s\n",
               cases[k].digit, flag, got, expected,
               got == expected ? "OK" : "<-- MISMATCH");
        if (got != expected) {
            ++failures;
        }
        dag_free(&plan);
    }

    registry_free(&reg);
    btn_free(&hexval);
    btn_free(&cond);

    if (failures == 0) {
        printf("\nHETERO PASS: planner built AND executed "
               "conditional_increment(flag, hex_value(digit)) with no "
               "hand-wiring.\n");
        return 0;
    }
    printf("\nHETERO FAIL: %d/%lu mismatched.\n", failures, (unsigned long)n_cases);
    return 1;
}
