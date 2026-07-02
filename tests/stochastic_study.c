/*
 * stochastic_study.c -- the third wall. The information-loss probe showed
 * margin fails when the INTERFACE starves the net of information while the world
 * still has a sharp boundary, recoverable by a finer ADC. This asks the deeper
 * question: what if the WORLD itself is graded -- the same x genuinely yields
 * conflicting labels, with perfect information? Then no resolution helps either.
 *
 * Construction (the comb's soft cousin): a single SOFT boundary,
 * P(label=1 | x) = sigmoid((x - THETA) / TAU). TAU sets the fuzziness. Labels are
 * drawn stochastically; the net sees a k-bit code (the bin of x). The decisive
 * contrast with the crisp comb:
 *   - crisp boundary  -> ONE ambiguous code per boundary -> ambiguous COUNT
 *     fixed, fraction = count/2^k SHRINKS with resolution (information loss, a
 *     finer ADC fixes the amount);
 *   - soft boundary   -> a TAU-wide ambiguous region in x -> ambiguous COUNT
 *     GROWS ~2^k, fraction stays CONSTANT -- resolution buys nothing.
 * And divergence floors at the Bayes rate across BOTH width and resolution: the
 * residue is in the world, not the encoding. That is "recovers with nothing."
 *
 * Divergence is measured analytically against the true conditional: the net's
 * single coarse output L_c versus E_x[P(label != L_c)] over the bin; the Bayes
 * floor is E_x[min(p, 1-p)], which no predictor beats. Only the training labels
 * are stochastic (deterministic RNG); the measurement is noise-free.
 *
 * Standalone: links only src/nn.c, core untouched. Deterministic. Budgeted;
 * NOT part of make test.
 */

#include "../include/nn.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define THETA            0.5
#define TAU              0.04          /* soft-boundary width; ambiguous span ~4.4*TAU */
#define AMB_FLOOR        0.10          /* a code is "ambiguous" if its Bayes floor exceeds this */
#define SAMPLES_PER_CODE 32
#define EPOCHS           20000
#define N_FINE           256
#define MAX_K            8
#define MAX_CODES        (1 << MAX_K)
#define MAX_SAMPLES      (MAX_CODES * SAMPLES_PER_CODE)

static void int_to_bits(int value, double *bits, size_t n) {
    size_t i;
    for (i = 0; i < n; ++i) {
        bits[i] = (double)((value >> (n - 1 - i)) & 1);
    }
}

static double p_label1(double x) {
    return 1.0 / (1.0 + exp(-(x - THETA) / TAU));
}

/* Expected divergence of a fixed coarse output L_c against fresh stochastic
   labels over code c's bin: E_x[ L_c ? (1-p) : p ]. */
static double expected_div(int c, int k, int out) {
    double lo = (double)c / (double)(1 << k);
    double w = 1.0 / (double)(1 << k);
    double acc = 0.0;
    int i;
    for (i = 0; i < N_FINE; ++i) {
        double x = lo + ((double)i + 0.5) / (double)N_FINE * w;
        double p = p_label1(x);
        acc += out ? (1.0 - p) : p;
    }
    return acc / (double)N_FINE;
}

/* Irreducible per-bin error E_x[min(p, 1-p)]: no output beats it. */
static double bayes_floor(int c, int k) {
    double lo = (double)c / (double)(1 << k);
    double w = 1.0 / (double)(1 << k);
    double acc = 0.0;
    int i;
    for (i = 0; i < N_FINE; ++i) {
        double x = lo + ((double)i + 0.5) / (double)N_FINE * w;
        double p = p_label1(x);
        acc += p < 1.0 - p ? p : 1.0 - p;
    }
    return acc / (double)N_FINE;
}

static Port mkport(PortFamily family, size_t field_width, size_t field_count) {
    Port p;
    p.family = family;
    p.field_width = field_width;
    p.field_count = field_count;
    p.tag[0] = '\0';
    return p;
}

/* Deterministic uniform in [0,1) for the stochastic labels. */
static unsigned long long g_rng = 0x2545F4914F6CDD1DULL;
static double urand(void) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return (double)(g_rng >> 11) * (1.0 / 9007199254740992.0);
}

static size_t build_training(int k, double *inputs, double *targets) {
    int codes = 1 << k;
    size_t n = 0;
    int c, s;
    for (c = 0; c < codes; ++c) {
        for (s = 0; s < SAMPLES_PER_CODE; ++s) {
            double x = ((double)c + ((double)s + 0.5) / (double)SAMPLES_PER_CODE)
                       / (double)(1 << k);
            double label = urand() < p_label1(x) ? 1.0 : 0.0;  /* graded world */
            int_to_bits(c, inputs + n * (size_t)k, (size_t)k);
            targets[n] = label;
            ++n;
        }
    }
    return n;
}

static void run_resolution(int k) {
    static double inputs[MAX_SAMPLES * MAX_K];
    static double targets[MAX_SAMPLES];
    int codes = 1 << k;
    size_t n_samples;
    Port in_port = mkport(PORT_BINARY_MSB, (size_t)k, 1);
    Port out_port = mkport(PORT_BINARY_MSB, 1, 1);
    int widths[] = {4, 8, 16, 32};
    const size_t nw = sizeof widths / sizeof widths[0];
    size_t wi;
    int c, amb_count = 0;
    double amb_floor_sum = 0.0;

    for (c = 0; c < codes; ++c) {
        if (bayes_floor(c, k) > AMB_FLOOR) {
            ++amb_count;
            amb_floor_sum += bayes_floor(c, k);
        }
    }
    n_samples = build_training(k, inputs, targets);

    printf("resolution k=%d | %d codes | %d ambiguous (count GROWS) | ambiguous fraction %.2f%% (CONST) | floor %.1f%%\n",
           k, codes, amb_count, 100.0 * (double)amb_count / (double)codes,
           amb_count ? 100.0 * amb_floor_sum / (double)amb_count : 0.0);
    printf("  width | clean div%% | ambiguous div%% (vs floor) | ambiguous margin mean\n");

    for (wi = 0; wi < nw; ++wi) {
        int w = widths[wi];
        BinaryTransformNetwork net;
        double clean_dsum = 0.0, amb_dsum = 0.0, amb_msum = 0.0;
        size_t clean_n = 0, amb_n = 0;

        memset(&net, 0, sizeof net);
        if (btn_init(&net, (size_t)k, 1, (size_t)w, (size_t)w, 0.8, 131u) != 0 ||
            btn_set_io_ports(&net, &in_port, 1, &out_port, 1) != 0) {
            fprintf(stderr, "FAIL: could not build width-%d net at k=%d.\n", w, k);
            return;
        }
        (void)btn_train(&net, inputs, targets, n_samples, EPOCHS);

        for (c = 0; c < codes; ++c) {
            double bits[MAX_K];
            const double *pred;
            double raw;
            int snapped;
            int_to_bits(c, bits, (size_t)k);
            pred = btn_forward(&net, bits);
            raw = pred[0];
            snapped = raw >= 0.5 ? 1 : 0;
            if (bayes_floor(c, k) > AMB_FLOOR) {
                amb_dsum += expected_div(c, k, snapped);
                amb_msum += fabs(raw - 0.5);
                ++amb_n;
            } else {
                clean_dsum += expected_div(c, k, snapped);
                ++clean_n;
            }
        }

        printf("  %5d |   %5.1f    |        %5.1f             |       %.3f\n",
               w,
               clean_n ? 100.0 * clean_dsum / (double)clean_n : 0.0,
               amb_n ? 100.0 * amb_dsum / (double)amb_n : 0.0,
               amb_n ? amb_msum / (double)amb_n : 0.0);
        btn_free(&net);
    }
    printf("\n");
}

int main(void) {
    int ks[] = {5, 6, 7};
    const size_t nk = sizeof ks / sizeof ks[0];
    size_t i;

    printf("=== stochastic: a GRADED world -- P(label=1|x) = sigmoid((x-%.2f)/%.2f) ===\n",
           THETA, TAU);
    printf("    ambiguous divergence should floor at the Bayes rate across BOTH width and resolution;\n");
    printf("    the ambiguous COUNT should grow ~2^k while the FRACTION stays constant\n");
    printf("    (contrast the comb: count fixed at #boundaries, fraction = count/2^k shrinks).\n\n");

    for (i = 0; i < nk; ++i) {
        run_resolution(ks[i]);
    }

    printf("(divergence floored across width AND resolution, ambiguous fraction constant =\n");
    printf(" the third wall: the residue is in the world, not the encoding. Neither a bigger\n");
    printf(" net nor a finer ADC helps -- CNET's only honest move is to refuse confidence.)\n");
    return 0;
}
