/* test_btn_train_plateau — dynamic growth must escape a dead-neuron plateau,
 * and must not report a plausible loss for a net that cannot be certified.
 *
 * WHY THIS EXISTS. `make certify` failed with `combine vs combine_contract :
 * DENIED (240/256)`. A read-only probe showed all 16 failures were one defect,
 * not sixteen: output bit 3 emitted 0.0000 where 1.0 was required, for exactly
 * the 16 inputs matching `(v & 0x1B) == 0x18`, and every one of them passed
 * `port_validate` — the net was confidently wrong, not ambiguous.
 *
 * Two controlled experiments located the cause in the TRAINER, not the demo:
 *
 *   max_hidden 64 -> 256   the trainer added 49 more neurons and the loss stayed
 *                          BIT-IDENTICAL at 0.008750
 *   seed 91u -> 123u       certified 256/256
 *
 * Capacity added to a plateau changed nothing, because `btn_add_hidden_neuron`
 * zeroes a new neuron's output weights: the neuron contributes nothing, and the
 * gradient reaching its input weights is scaled by that zero, so it has no path
 * back into the function it was added to improve. Stacking more of them behind
 * it cannot help.
 *
 * This gate reproduces that directly, with no dependency on the demo or the
 * certifier: the exact shape, data, hyperparameters and seed `combine` uses, and
 * the same certification predicate (`port_validate` then `port_canonicalize`
 * then exact compare) applied to all 256 exemplars.
 *
 * Nothing is written to disk.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/nn.h"

#define WIDTH 8
#define SAMPLES 256

static int failures;
static int checks;

static void check(int condition, const char *message) {
    checks++;
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
}

/* Exactly build_combine_data from the demo: two 4-bit nibble slots forming the
   byte they spell, which is the identity per bit. Targets are 0.9 / 0.1. */
static void build_combine_data(double inputs[SAMPLES][WIDTH],
                               double targets[SAMPLES][WIDTH]) {
    int value, bit;
    for (value = 0; value < SAMPLES; ++value) {
        for (bit = 0; bit < WIDTH; ++bit) {
            int b = (value >> (7 - bit)) & 1;
            inputs[value][bit] = (double)b;
            targets[value][bit] = b ? 0.9 : 0.1;
        }
    }
}

/* The certification predicate, applied per exemplar: validate the raw output
   against the port, canonicalize it, and compare to the canonical target. */
static size_t certifiable(BinaryTransformNetwork *net,
                          const double inputs[SAMPLES][WIDTH],
                          const double targets[SAMPLES][WIDTH],
                          size_t *first_bad_input, size_t *first_bad_bit) {
    Port out_port;
    size_t s, i, passed = 0;
    memset(&out_port, 0, sizeof out_port);
    out_port.family = PORT_BINARY_MSB;
    out_port.field_width = WIDTH;
    out_port.field_count = 1;
    if (first_bad_input) *first_bad_input = (size_t)-1;
    if (first_bad_bit) *first_bad_bit = (size_t)-1;

    for (s = 0; s < SAMPLES; ++s) {
        const double *raw = btn_forward(net, inputs[s]);
        double clean[WIDTH];
        int ok = raw != NULL;
        if (ok && !port_validate(out_port, raw)) ok = 0;
        if (ok && port_canonicalize(out_port, raw, clean) != 0) ok = 0;
        for (i = 0; ok && i < WIDTH; ++i) {
            double want = targets[s][i] >= 0.5 ? 1.0 : 0.0;
            if (clean[i] != want) {
                ok = 0;
                if (first_bad_input && *first_bad_input == (size_t)-1) {
                    *first_bad_input = s;
                    if (first_bad_bit) *first_bad_bit = i;
                }
            }
        }
        if (ok) passed++;
    }
    return passed;
}

int main(void) {
    static double inputs[SAMPLES][WIDTH];
    static double targets[SAMPLES][WIDTH];
    BinaryTransformNetwork net;
    double loss;
    size_t passed, bad_input = 0, bad_bit = 0;

    setvbuf(stdout, NULL, _IONBF, 0);
    build_combine_data(inputs, targets);

    /* The demo's exact configuration: 8->8, one hidden neuron to start, a
       ceiling of 64, learning rate 0.8, seed 91u. */
    memset(&net, 0, sizeof net);
    if (btn_init(&net, WIDTH, WIDTH, 1, 64, 0.8, 91u) != 0) {
        fprintf(stderr, "FAIL: btn_init\n");
        return 1;
    }
    loss = btn_train_dynamic(&net, &inputs[0][0], &targets[0][0], SAMPLES,
                             150000, 1000, 0.0008, 0.03);
    passed = certifiable(&net, inputs, targets, &bad_input, &bad_bit);

    printf("BTN_PLATEAU_RUN seed=91 max_hidden=64 hidden_used=%zu loss=%.6f "
           "certifiable=%zu/%d\n",
           net.hidden_count, loss, passed, SAMPLES);
    if (passed != SAMPLES)
        printf("BTN_PLATEAU_FIRST_MISS input=%zu bit=%zu\n", bad_input, bad_bit);

    check(loss >= 0.0,
          "training reports a loss, not an argument error");
    /* The whole point: this net must be certifiable over its exemplars. */
    check(passed == SAMPLES,
          "every exemplar certifies after dynamic growth");
    /* And the trainer must not report a plausible loss for a net that did not
       reach the target it was given -- a demo that persists such a net produces
       an uncertifiable artifact with no signal that anything went wrong. */
    check(!(loss >= 0.0 && loss > 0.0008 && passed != SAMPLES),
          "a run that misses its target loss does not report success silently");
    btn_free(&net);

    /* Control: a seed that already converged must still converge, so the fix is
       an escape from a plateau and not a change of optimum. */
    memset(&net, 0, sizeof net);
    if (btn_init(&net, WIDTH, WIDTH, 1, 64, 0.8, 123u) != 0) {
        fprintf(stderr, "FAIL: btn_init control\n");
        return 1;
    }
    loss = btn_train_dynamic(&net, &inputs[0][0], &targets[0][0], SAMPLES,
                             150000, 1000, 0.0008, 0.03);
    passed = certifiable(&net, inputs, targets, NULL, NULL);
    printf("BTN_PLATEAU_CONTROL seed=123 hidden_used=%zu loss=%.6f "
           "certifiable=%zu/%d\n", net.hidden_count, loss, passed, SAMPLES);
    check(passed == SAMPLES, "a seed that already converged still converges");
    btn_free(&net);

    /* Control: a genuinely impossible target must still terminate and must not
       claim success. Two contradictory exemplars cannot both be learned. */
    {
        static double bad_in[2][WIDTH];
        static double bad_tg[2][WIDTH];
        size_t i;
        for (i = 0; i < WIDTH; i++) {
            bad_in[0][i] = 1.0;
            bad_in[1][i] = 1.0;      /* identical inputs ... */
            bad_tg[0][i] = 0.9;
            bad_tg[1][i] = 0.1;      /* ... contradictory targets */
        }
        memset(&net, 0, sizeof net);
        if (btn_init(&net, WIDTH, WIDTH, 1, 8, 0.8, 5u) == 0) {
            loss = btn_train_dynamic(&net, &bad_in[0][0], &bad_tg[0][0], 2,
                                     2000, 50, 1e-9, 0.03);
            printf("BTN_PLATEAU_IMPOSSIBLE loss=%.6f\n", loss);
            check(loss != 0.0,
                  "an unlearnable target does not report a zero loss");
            btn_free(&net);
        }
    }

    if (failures) {
        printf("BTN_TRAIN_PLATEAU_FAIL checks=%d failures=%d\n", checks,
               failures);
        return 1;
    }
    printf("BTN_TRAIN_PLATEAU_PASS checks=%d certifiable=%d/%d\n", checks,
           SAMPLES, SAMPLES);
    return 0;
}
