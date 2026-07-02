/*
 * margin_study.c -- instrument per-port MARGIN and CORRECTNESS DIVERGENCE
 * through composition, to put the first real numbers on the table for the
 * "discrete fabric" argument.
 *
 * Margin = distance of a raw (pre-snap) output to the nearest canonicalization
 * boundary. For a binary port it is min over the port's bits of |v - 0.5|
 * (range [0, 0.5]); port_validate treats a bit as ambiguous when it sits in
 * the closed band |v - 0.5| <= 0.25. Two facts under test:
 *
 *   1. The snap RESETS margin at every boundary (canonicalization recenters
 *      each output on its prototype), so margin should NOT erode with
 *      composition depth. The depth pass checks this directly.
 *   2. Margin measures the CHANNEL (is the handoff unambiguous), not the
 *      FUNCTION (is it correct). A primitive can be confidently, maximal-
 *      margin wrong. So we log margin AND divergence (snapped symbol vs the
 *      arithmetic ground truth) -- two different instruments.
 *
 * Default run: the verified dec_full_add over the 2-digit ripple (all 20000
 * cases) as the well-formed-and-correct baseline, then an N-digit depth pass.
 * `--stress` trains fixed-width students on the 200-member full-adder to find
 * where margin first crowds the band -- the empirical edge of the reach.
 *
 * Standalone: links only src/nn.c, the core is untouched. Loads the committed
 * frozen dec_full_add weights.
 */

#include "../include/nn.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define BAND 0.25        /* port_validate binary ambiguity half-band */
#define MAX_DEPTH 16

/* MSB-first, matching tests/test_decimal.c. */
static int bits_to_int(const double *bits, size_t n) {
    int value = 0;
    size_t i;
    for (i = 0; i < n; ++i) {
        value = value * 2 + (bits[i] > 0.5 ? 1 : 0);
    }
    return value;
}

static void int_to_bits(int value, double *bits, size_t n) {
    size_t i;
    for (i = 0; i < n; ++i) {
        bits[i] = (double)((value >> (n - 1 - i)) & 1);
    }
}

/* Minimum distance-to-boundary over a binary port's values: min |v - 0.5|.
   Range [0, 0.5]; <= BAND means at least one value sits in port_validate's
   ambiguity band (the weakest bit is the one closest to flipping). */
static double binary_margin(const double *v, size_t n) {
    double m = 0.5;
    size_t i;
    for (i = 0; i < n; ++i) {
        double d = fabs(v[i] - 0.5);
        if (d < m) m = d;
    }
    return m;
}

static Port mkport(PortFamily family, size_t field_width, size_t field_count) {
    Port p;
    p.family = family;
    p.field_width = field_width;
    p.field_count = field_count;
    p.tag[0] = '\0';
    return p;
}

/* Running accumulator for one measured port-slot. */
typedef struct {
    char label[16];
    size_t count;
    double margin_min;
    double margin_sum;
    size_t ambiguous;   /* margin <= BAND */
    size_t diverged;    /* snapped symbol != arithmetic ground truth */
} Slot;

static void slot_init(Slot *s, const char *label) {
    snprintf(s->label, sizeof s->label, "%s", label);
    s->count = 0;
    s->margin_min = 0.5;
    s->margin_sum = 0.0;
    s->ambiguous = 0;
    s->diverged = 0;
}

static void slot_add(Slot *s, double margin, int ambiguous, int diverged) {
    s->count++;
    if (margin < s->margin_min) s->margin_min = margin;
    s->margin_sum += margin;
    s->ambiguous += (size_t)(ambiguous != 0);
    s->diverged += (size_t)(diverged != 0);
}

static void slot_print(const Slot *s) {
    double n = s->count ? (double)s->count : 1.0;
    printf("  %-11s  margin[min %.3f  mean %.3f]   ambiguous %6.2f%%   diverged %6.2f%%\n",
           s->label, s->margin_min, s->margin_sum / n,
           100.0 * (double)s->ambiguous / n, 100.0 * (double)s->diverged / n);
}

/* One full-adder step on the symbols the net actually sees. Forwards, measures
   the raw sum and carry margins, snaps both, compares to arithmetic ground
   truth, records into the two slots, and returns the snapped carry for
   chaining. The btn's output ports drive canonicalization (ports[0]=sum BIN4,
   ports[1]=carry BIN1 for both the frozen primitive and the students). */
static int adder_step(BinaryTransformNetwork *fa, int a, int b, int cin,
                      int *out_sum, Slot *sum_slot, Slot *carry_slot) {
    double in[9];
    double raw[5];
    double snap_sum[4];
    double snap_carry[1];
    const double *pred;
    double m_sum, m_carry;
    int got_sum, got_carry, partial, exp_sum, exp_carry;

    int_to_bits(a, in, 4);
    int_to_bits(b, in + 4, 4);
    in[8] = (double)cin;

    pred = btn_forward(fa, in);
    memcpy(raw, pred, 5 * sizeof(double));

    m_sum = binary_margin(raw, 4);
    m_carry = binary_margin(raw + 4, 1);
    port_canonicalize(fa->output_ports[0], raw, snap_sum);
    port_canonicalize(fa->output_ports[1], raw + 4, snap_carry);
    got_sum = bits_to_int(snap_sum, 4);
    got_carry = bits_to_int(snap_carry, 1);

    partial = a + b + cin;
    exp_sum = partial % 10;
    exp_carry = partial / 10;

    slot_add(sum_slot, m_sum, m_sum <= BAND, got_sum != exp_sum);
    slot_add(carry_slot, m_carry, m_carry <= BAND, got_carry != exp_carry);

    *out_sum = got_sum;
    return got_carry;
}

/* Thread an n-digit ripple, snapping the carry between positions exactly as
   the executor would. Records per-position into sum[]/carry[]. */
static void run_ripple(BinaryTransformNetwork *fa, const int *a, const int *b,
                       int n, int cin, Slot *sum, Slot *carry,
                       int *final_carry, int *digits) {
    int carry_in = cin;
    int i;
    for (i = 0; i < n; ++i) {
        int s;
        carry_in = adder_step(fa, a[i], b[i], carry_in, &s, &sum[i], &carry[i]);
        digits[i] = s;
    }
    *final_carry = carry_in;
}

/* Deterministic xorshift so runs are reproducible. unsigned long long: MinGW's
   unsigned long is 32-bit, too narrow for the 64-bit state. */
static unsigned long long g_rng = 88172645463325252ULL;
static unsigned long long xorshift(void) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return g_rng;
}

static int run_baseline(BinaryTransformNetwork *fa) {
    Slot sum[2], carry[2];
    size_t correct = 0, total = 0;
    int a, b, cin;

    slot_init(&sum[0], "ones.sum");
    slot_init(&carry[0], "ones.carry");
    slot_init(&sum[1], "tens.sum");
    slot_init(&carry[1], "tens.carry");

    for (a = 0; a < 100; ++a) {
        for (b = 0; b < 100; ++b) {
            for (cin = 0; cin < 2; ++cin) {
                int ad[2], bd[2], digs[2], fco;
                ad[0] = a % 10; ad[1] = a / 10;
                bd[0] = b % 10; bd[1] = b / 10;
                run_ripple(fa, ad, bd, 2, cin, sum, carry, &fco, digs);
                if (fco * 100 + digs[1] * 10 + digs[0] == a + b + cin) ++correct;
                ++total;
            }
        }
    }

    printf("=== baseline: verified dec_full_add, 2-digit ripple, all %lu cases ===\n",
           (unsigned long)total);
    slot_print(&sum[0]);
    slot_print(&carry[0]);
    slot_print(&sum[1]);
    slot_print(&carry[1]);
    printf("  chain-correct: %lu/%lu\n\n", (unsigned long)correct, (unsigned long)total);
    return correct == total ? 0 : 1;
}

static void run_depth(BinaryTransformNetwork *fa, int n, int samples) {
    Slot sum[MAX_DEPTH], carry[MAX_DEPTH];
    int i, k;

    if (n > MAX_DEPTH) n = MAX_DEPTH;
    for (i = 0; i < n; ++i) {
        char ls[16], lc[16];
        snprintf(ls, sizeof ls, "p%02d.sum", i);
        snprintf(lc, sizeof lc, "p%02d.carry", i);
        slot_init(&sum[i], ls);
        slot_init(&carry[i], lc);
    }

    for (k = 0; k < samples; ++k) {
        int ad[MAX_DEPTH], bd[MAX_DEPTH], digs[MAX_DEPTH], fco;
        int cin = (int)(xorshift() % 2);
        for (i = 0; i < n; ++i) {
            ad[i] = (int)(xorshift() % 10);
            bd[i] = (int)(xorshift() % 10);
        }
        run_ripple(fa, ad, bd, n, cin, sum, carry, &fco, digs);
    }

    printf("=== depth pass: %d-digit ripple, %d sampled additions (does margin erode?) ===\n",
           n, samples);
    for (i = 0; i < n; ++i) {
        slot_print(&sum[i]);
        slot_print(&carry[i]);
    }
    printf("\n");
}

static void run_stress(void) {
    static double inputs[200][9];
    static double targets[200][5];
    static int exp_sum[200];
    static int exp_carry[200];
    int widths[] = {4, 8, 16, 32, 64};
    const size_t nw = sizeof widths / sizeof widths[0];
    Port in_ports[3];
    Port out_ports[2];
    size_t epochs = 40000;
    size_t wi;
    int a, b, cin, idx = 0;

    for (a = 0; a < 10; ++a) {
        for (b = 0; b < 10; ++b) {
            for (cin = 0; cin < 2; ++cin) {
                int p = a + b + cin;
                int_to_bits(a, inputs[idx], 4);
                int_to_bits(b, inputs[idx] + 4, 4);
                inputs[idx][8] = (double)cin;
                int_to_bits(p % 10, targets[idx], 4);
                int_to_bits(p / 10, targets[idx] + 4, 1);
                exp_sum[idx] = p % 10;
                exp_carry[idx] = p / 10;
                ++idx;
            }
        }
    }

    in_ports[0] = mkport(PORT_BINARY_MSB, 4, 1);
    in_ports[1] = mkport(PORT_BINARY_MSB, 4, 1);
    in_ports[2] = mkport(PORT_BINARY_MSB, 1, 1);
    out_ports[0] = mkport(PORT_BINARY_MSB, 4, 1);
    out_ports[1] = mkport(PORT_BINARY_MSB, 1, 1);

    printf("=== capacity stress: fixed-width students on the 200-member full-adder ===\n");
    printf("  (margin crowds the band as capacity drops; watch margin lead divergence)\n");
    printf("  width |    sum margin min/mean  amb%%  div%% |  carry margin min/mean  amb%%  div%% | exact\n");

    for (wi = 0; wi < nw; ++wi) {
        int w = widths[wi];
        BinaryTransformNetwork st;
        Slot sm, cy;
        size_t exact = 0;
        int i;

        memset(&st, 0, sizeof st);
        if (btn_init(&st, 9, 5, (size_t)w, (size_t)w, 0.8, 131u) != 0 ||
            btn_set_io_ports(&st, in_ports, 3, out_ports, 2) != 0) {
            fprintf(stderr, "FAIL: could not build width-%d student.\n", w);
            return;
        }
        (void)btn_train(&st, &inputs[0][0], &targets[0][0], 200, epochs);

        slot_init(&sm, "sum");
        slot_init(&cy, "carry");
        for (i = 0; i < 200; ++i) {
            const double *pred = btn_forward(&st, inputs[i]);
            double raw[5];
            double snap_sum[4], snap_carry[1];
            double m_sum, m_carry;
            int got_sum, got_carry, dsum, dcar;

            memcpy(raw, pred, 5 * sizeof(double));
            m_sum = binary_margin(raw, 4);
            m_carry = binary_margin(raw + 4, 1);
            port_canonicalize(out_ports[0], raw, snap_sum);
            port_canonicalize(out_ports[1], raw + 4, snap_carry);
            got_sum = bits_to_int(snap_sum, 4);
            got_carry = bits_to_int(snap_carry, 1);
            dsum = got_sum != exp_sum[i];
            dcar = got_carry != exp_carry[i];
            slot_add(&sm, m_sum, m_sum <= BAND, dsum);
            slot_add(&cy, m_carry, m_carry <= BAND, dcar);
            if (!dsum && !dcar) ++exact;
        }

        printf("  %5d |    %.3f / %.3f   %4.1f  %4.1f |    %.3f / %.3f   %4.1f  %4.1f | %lu/200\n",
               w,
               sm.margin_min, sm.margin_sum / 200.0,
               100.0 * (double)sm.ambiguous / 200.0, 100.0 * (double)sm.diverged / 200.0,
               cy.margin_min, cy.margin_sum / 200.0,
               100.0 * (double)cy.ambiguous / 200.0, 100.0 * (double)cy.diverged / 200.0,
               (unsigned long)exact);
        btn_free(&st);
    }
    printf("\n");
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--stress") == 0) {
        run_stress();
        return 0;
    }

    {
        BinaryTransformNetwork fa;
        int rc;
        memset(&fa, 0, sizeof fa);
        if (btn_load(&fa, "dec_full_add_weights.txt") != 0) {
            fprintf(stderr, "FAIL: could not load dec_full_add_weights.txt "
                            "(run `make decimal` to regenerate the frozen weights).\n");
            return 1;
        }
        rc = run_baseline(&fa);
        run_depth(&fa, 8, 4000);
        btn_free(&fa);
        printf("(margin certifies the channel; divergence certifies the function. "
               "Run with --stress to map the edge.)\n");
        return rc;
    }
}
