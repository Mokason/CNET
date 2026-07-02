/*
 * Composition test for the "frozen primitives" thesis.
 *
 * Loads two independently-trained, independently-frozen primitives from disk
 * and chains them with NO training in between:
 *
 *     hex symbol --[hex_value]--> 4-bit value --[increment]--> 5-bit (value+1)
 *
 * hex_value and increment never saw each other during training. If their
 * interfaces line up, "increment a hex digit" emerges for free from the two
 * frozen parts. If they do not, the output pinpoints where the interface
 * contract is missing -- which is an equally useful finding.
 *
 * Requires the weight files produced by ./nn_demo:
 *   hex_value_weights.txt, increment_weights.txt
 * Run from the repository root (where those files live).
 */
#include "../include/nn.h"

#include <stdio.h>

static int bits_to_int(const char *bits) {
    int value = 0;
    size_t i;

    for (i = 0; bits[i] != '\0'; ++i) {
        value = (value << 1) | (bits[i] == '1' ? 1 : 0);
    }
    return value;
}

/* "1010" -> {1,0,1,0}, MSB-first, matching increment's input encoding. */
static void bits_to_input(const char *bits, double *input, size_t count) {
    size_t i;

    for (i = 0; i < count; ++i) {
        input[i] = (bits[i] == '1') ? 1.0 : 0.0;
    }
}

int run_test_composition(void) {
    static const char digits[] = "0123456789ABCDEF";
    BinaryTransformNetwork hexval = {0};
    BinaryTransformNetwork incr = {0};
    int failures = 0;
    int i;

    if (btn_load(&hexval, "hex_value_weights.txt") != 0) {
        fprintf(stderr, "FAIL: could not load frozen hex_value primitive "
                        "(run ./nn_demo first).\n");
        return 1;
    }
    if (btn_load(&incr, "increment_weights.txt") != 0) {
        fprintf(stderr, "FAIL: could not load frozen increment primitive "
                        "(run ./nn_demo first).\n");
        btn_free(&hexval);
        return 1;
    }

    printf("hex_value: %lu in -> %lu out | increment: %lu in -> %lu out\n",
           (unsigned long)hexval.input_count,
           (unsigned long)hexval.output_count,
           (unsigned long)incr.input_count,
           (unsigned long)incr.output_count);

    /* Interface contract check via the stored ports. Unlike a bare width
       compare, port_compatible also rejects a bit-order mismatch
       (BINARY_MSB vs BINARY_LSB) that would silently produce garbage. */
    if (!port_compatible(hexval.output_ports[0], incr.input_ports[0])) {
        fprintf(stderr, "FAIL: incompatible ports; the primitives cannot be "
                        "composed.\n");
        btn_free(&hexval);
        btn_free(&incr);
        return 1;
    }
    printf("ports compatible: hex_value.out -> increment.in\n");

    printf("\ncomposing hex_value -> increment (no training):\n");
    for (i = 0; i < 16; ++i) {
        char value_bits[8];
        char inc_bits[8];
        double inc_input[4];
        int got;
        int expected = i + 1;

        if (btn_predict_hex_symbol_value(&hexval, digits[i],
                                         value_bits, sizeof(value_bits)) != 0) {
            fprintf(stderr, "FAIL: hex_value rejected '%c'\n", digits[i]);
            ++failures;
            continue;
        }
        bits_to_input(value_bits, inc_input, 4);
        if (!port_validate(incr.input_ports[0], inc_input)) {
            fprintf(stderr, "FAIL: hex_value output for '%c' is out of "
                            "increment's input domain\n", digits[i]);
            ++failures;
            continue;
        }
        if (btn_predict_bits(&incr, inc_input,
                             inc_bits, sizeof(inc_bits)) != 0) {
            fprintf(stderr, "FAIL: increment rejected input from '%c'\n", digits[i]);
            ++failures;
            continue;
        }
        got = bits_to_int(inc_bits);

        printf("  %c -> hex_value %s (=%2d) -> increment %s (=%2d) | "
               "expected %2d %s\n",
               digits[i], value_bits, bits_to_int(value_bits),
               inc_bits, got, expected,
               got == expected ? "OK" : "<-- MISMATCH");
        if (got != expected) {
            ++failures;
        }
    }

    btn_free(&hexval);
    btn_free(&incr);

    if (failures == 0) {
        printf("\nCOMPOSITION PASS: increment-a-hex-digit emerged from two "
               "frozen primitives that never trained together.\n");
        return 0;
    }
    printf("\nCOMPOSITION FAIL: %d/16 mismatched.\n", failures);
    return 1;
}
