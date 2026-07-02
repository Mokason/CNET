/*
 * fuzzy_study.c -- the information-loss probe, with the divergence backstop.
 * Tests whether the two instruments together tell FUNDAMENTAL crowding (the
 * interface can't carry the concept) apart from ARCHITECTURAL crowding (the net
 * underfits), which margin_study showed recovers with capacity.
 *
 * Construction. A SHARP concept (no label noise): label = parity of the band
 * floor(M*x + PHI) for x in [0,1) -- an M-band square wave, PHI = 1/3 a
 * non-dyadic phase so no boundary lands on a (dyadic) code edge. The net sees
 * only a lossy k-bit CODE = the bin of x. A code whose bin contains a boundary
 * STRADDLES it and carries both labels (50/50 by construction); no capacity
 * separates it. Other codes are clean, but the M-band wave needs ~M hidden
 * units, so low-width nets underfit the clean bands -- recoverable.
 *
 * Two instruments per code:
 *   - margin = distance of the raw output to the snap boundary (the channel);
 *   - divergence = the net's single COARSE output vs the FINE-GRAINED truth
 *     (sample real x inside the bin, label by band-parity) -- the function.
 *
 * The grid separates the two walls, in one table:
 *   - clean-code margin and divergence RECOVER WITH WIDTH      -> architectural;
 *   - straddle DIVERGENCE sits at the Bayes floor at EVERY width-> fundamental
 *     (capacity cannot recover information the code does not carry);
 *   - the ambiguous input-fraction (#straddle / 2^k) RECOVERS  -> only with
 *     RESOLUTION, never with width.
 * And the key catch: on the straddle codes, MARGIN is leaky -- a high-capacity
 * net outputs confidently on a 50/50 code (margin escapes the band) while being
 * confidently wrong. Divergence is the backstop margin can't replace.
 *
 * Standalone: links only src/nn.c, core untouched. Deterministic. Budgeted;
 * NOT part of make test.
 */

#include "../include/nn.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define M_BANDS          6             /* sharp square wave; needs ~M hidden units */
#define PHI              (1.0 / 3.0)   /* non-dyadic phase: boundaries miss code edges */
#define BAND             0.25          /* port_validate binary ambiguity half-band */
#define SAMPLES_PER_CODE 16
#define EPOCHS           30000
#define N_FINE           256           /* sub-bin samples for fine-grained divergence */
#define MAX_K            8
#define MAX_CODES        (1 << MAX_K)
#define MAX_SAMPLES      (MAX_CODES * SAMPLES_PER_CODE)

static void int_to_bits(int value, double *bits, size_t n) {
    size_t i;
    for (i = 0; i < n; ++i) {
        bits[i] = (double)((value >> (n - 1 - i)) & 1);
    }
}

static double bit_margin(double v) {
    return fabs(v - 0.5);
}

static int band_at(double p) {
    return (int)floor((double)M_BANDS * p + PHI);
}

static int is_straddle(int c, int k) {
    double lo = (double)c / (double)(1 << k);
    double hi = (double)(c + 1) / (double)(1 << k);
    return band_at(lo) != band_at(hi);
}

static int clean_label(int c, int k) {
    return band_at((double)c / (double)(1 << k)) & 1;
}

/* Fraction of true x inside code c's bin whose band-parity differs from the
   net's single coarse output `out`. The net only ever sees the coarse code, so
   on a straddle bin one of the two parities is always mislabeled. */
static double bin_divergence(int c, int k, int out) {
    double lo = (double)c / (double)(1 << k);
    double w = 1.0 / (double)(1 << k);
    int i, miss = 0;
    for (i = 0; i < N_FINE; ++i) {
        double x = lo + ((double)i + 0.5) / (double)N_FINE * w;
        if ((band_at(x) & 1) != out) ++miss;
    }
    return (double)miss / (double)N_FINE;
}

/* Irreducible per-bin error: the minority parity's share. No output beats it. */
static double bin_bayes_floor(int c, int k) {
    double lo = (double)c / (double)(1 << k);
    double w = 1.0 / (double)(1 << k);
    int i, ones = 0;
    double f1;
    for (i = 0; i < N_FINE; ++i) {
        double x = lo + ((double)i + 0.5) / (double)N_FINE * w;
        if (band_at(x) & 1) ++ones;
    }
    f1 = (double)ones / (double)N_FINE;
    return f1 < 1.0 - f1 ? f1 : 1.0 - f1;
}

static Port mkport(PortFamily family, size_t field_width, size_t field_count) {
    Port p;
    p.family = family;
    p.field_width = field_width;
    p.field_count = field_count;
    p.tag[0] = '\0';
    return p;
}

/* Honest lossy ADC: sample real x at a deterministic sub-bin grid and label
   each by the sharp concept. Clean codes (no boundary in the bin) come out
   consistent; straddle codes inherit the bin's TRUE parity ratio, so a trained
   net reaches -- but cannot beat -- the Bayes floor. No straddle special-case. */
static size_t build_training(int k, double *inputs, double *targets) {
    int codes = 1 << k;
    size_t n = 0;
    int c, s;
    for (c = 0; c < codes; ++c) {
        for (s = 0; s < SAMPLES_PER_CODE; ++s) {
            double x = ((double)c + ((double)s + 0.5) / (double)SAMPLES_PER_CODE)
                       / (double)(1 << k);
            int_to_bits(c, inputs + n * (size_t)k, (size_t)k);
            targets[n] = (double)(band_at(x) & 1);
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
    int widths[] = {2, 4, 8, 16, 32};
    const size_t nw = sizeof widths / sizeof widths[0];
    size_t wi;
    int c, n_straddle = 0;
    double floor_sum = 0.0;

    for (c = 0; c < codes; ++c) {
        if (is_straddle(c, k)) {
            ++n_straddle;
            floor_sum += bin_bayes_floor(c, k);
        }
    }
    n_samples = build_training(k, inputs, targets);

    printf("resolution k=%d | %d codes | %d straddle | ambiguous %.2f%% | straddle Bayes floor %.1f%%\n",
           k, codes, n_straddle, 100.0 * (double)n_straddle / (double)codes,
           n_straddle ? 100.0 * floor_sum / (double)n_straddle : 0.0);
    printf("  width | clean margin min/mean | clean div%% | straddle margin max/mean | straddle div%%\n");

    for (wi = 0; wi < nw; ++wi) {
        int w = widths[wi];
        BinaryTransformNetwork net;
        double clean_min = 0.5, clean_sum = 0.0;
        size_t clean_n = 0, clean_div = 0;
        double str_max = 0.0, str_msum = 0.0, str_dsum = 0.0;
        size_t str_n = 0;

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
            double raw, m;
            int snapped;
            int_to_bits(c, bits, (size_t)k);
            pred = btn_forward(&net, bits);
            raw = pred[0];
            m = bit_margin(raw);
            snapped = raw >= 0.5 ? 1 : 0;
            if (is_straddle(c, k)) {
                if (m > str_max) str_max = m;
                str_msum += m;
                str_dsum += bin_divergence(c, k, snapped);
                ++str_n;
            } else {
                if (m < clean_min) clean_min = m;
                clean_sum += m;
                clean_div += (size_t)(snapped != clean_label(c, k));
                ++clean_n;
            }
        }

        printf("  %5d |    %.3f / %.3f      |   %5.1f    |     %.3f / %.3f        |   %5.1f\n",
               w, clean_min, clean_sum / (double)clean_n,
               100.0 * (double)clean_div / (double)clean_n,
               str_max, str_n ? str_msum / (double)str_n : 0.0,
               str_n ? 100.0 * str_dsum / (double)str_n : 0.0);
        btn_free(&net);
    }
    printf("\n");
}

int main(void) {
    int ks[] = {5, 6, 7};
    const size_t nk = sizeof ks / sizeof ks[0];
    size_t i;

    printf("=== fuzzy: information-loss probe -- a sharp %d-band square wave under a lossy k-bit code ===\n",
           M_BANDS);
    printf("    clean margin + div recover with WIDTH (architectural);\n");
    printf("    straddle DIVERGENCE sits at the Bayes floor at every width (fundamental);\n");
    printf("    ambiguous input-fraction recovers only with RESOLUTION;\n");
    printf("    straddle MARGIN is leaky -- capacity makes it confidently wrong (div is the backstop).\n\n");

    for (i = 0; i < nk; ++i) {
        run_resolution(ks[i]);
    }

    printf("(clean recovers with width; straddle divergence floors at every width while its margin\n");
    printf(" leaks at high capacity -- margin is the early warning, divergence the backstop it can't\n");
    printf(" replace; only resolution shrinks the ambiguous share of the inputs.)\n");
    return 0;
}
