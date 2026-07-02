/* logic_gate_net.c -- a learnable boolean circuit (DLGN-style), born exact.
 * See include/logic_gate_net.h. Self-contained; no CNET core dependency.
 */

#include "../include/logic_gate_net.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ---- the 16 two-input boolean functions as multilinear polynomials --------
 * Gate id g in [0,16): output bit for inputs (a,b) is (g >> (2a+b)) & 1.
 * Multilinear interpolation f(a,b) = c0 + c1 a + c2 b + c3 ab with
 *   f00=g&1, f01=(g>>1)&1, f10=(g>>2)&1, f11=(g>>3)&1,
 *   c0=f00, c1=f10-f00, c2=f01-f00, c3=f00-f01-f10+f11.
 * (e.g. AND=8 -> ab; OR=14 -> a+b-ab; XOR=6 -> a+b-2ab.) */
static double g_coef[16][4];
static int    g_coef_ready = 0;

static void coef_init(void) {
    int g;
    if (g_coef_ready) return;
    for (g = 0; g < 16; ++g) {
        double f00 = (double)(g & 1), f01 = (double)((g >> 1) & 1);
        double f10 = (double)((g >> 2) & 1), f11 = (double)((g >> 3) & 1);
        g_coef[g][0] = f00;
        g_coef[g][1] = f10 - f00;
        g_coef[g][2] = f01 - f00;
        g_coef[g][3] = f00 - f01 - f10 + f11;
    }
    g_coef_ready = 1;
}

/* ---- small deterministic RNG (wiring + init) --------------------------- */
static unsigned long long lcg_next(unsigned long long *s) {
    *s = *s * 6364136223846793005ULL + 1442695040888963407ULL;
    return *s;
}
static double lcg_unif(unsigned long long *s) {
    return (double)((lcg_next(s) >> 11) & ((1ULL << 53) - 1)) / (double)(1ULL << 53);
}
static size_t lcg_range(unsigned long long *s, size_t n) {
    return (n == 0) ? 0 : (size_t)(lcg_unif(s) * (double)n) % n;
}

static void softmax16(const double *logit, double *p) {
    int k; double mx = logit[0], sum = 0.0;
    for (k = 1; k < 16; ++k) if (logit[k] > mx) mx = logit[k];
    for (k = 0; k < 16; ++k) { p[k] = exp(logit[k] - mx); sum += p[k]; }
    for (k = 0; k < 16; ++k) p[k] /= sum;
}

/* ---- construction ------------------------------------------------------- */

void lgn_free(LogicGateNetwork *net) {
    if (!net) return;
    free(net->in_a); free(net->in_b); free(net->logit); free(net->gate);
    memset(net, 0, sizeof *net);
}

int lgn_init(LogicGateNetwork *net, size_t input_count, size_t output_count,
             const size_t *hidden_widths, size_t num_hidden, unsigned seed) {
    size_t total = 0, L, i;
    unsigned long long rng = ((unsigned long long)seed << 1) | 1ULL;
    size_t prev_base, prev_width, gi;

    coef_init();
    if (!net || input_count == 0 || output_count == 0 ||
        !hidden_widths || num_hidden == 0) return -1;
    memset(net, 0, sizeof *net);

    for (L = 0; L < num_hidden; ++L) { if (hidden_widths[L] == 0) return -1; total += hidden_widths[L]; }
    total += output_count;

    net->input_count = input_count;
    net->output_count = output_count;
    net->total_gates = total;
    net->in_a  = (int *)malloc(total * sizeof(int));
    net->in_b  = (int *)malloc(total * sizeof(int));
    net->logit = (double *)malloc(total * 16 * sizeof(double));
    net->gate  = (int *)malloc(total * sizeof(int));
    if (!net->in_a || !net->in_b || !net->logit || !net->gate) { lgn_free(net); return -1; }

    /* Wire layer by layer. Sources of layer 0 are input bits (value index in
       [0,input_count)); sources of layer L are gates of layer L-1 (value index
       input_count + global gate index). */
    prev_base = 0;            /* value index base of the previous layer */
    prev_width = input_count; /* previous layer's unit count */
    gi = 0;                   /* running global gate index */
    for (L = 0; L <= num_hidden; ++L) {
        size_t width = (L < num_hidden) ? hidden_widths[L] : output_count;
        size_t g;
        for (g = 0; g < width; ++g, ++gi) {
            size_t sa = lcg_range(&rng, prev_width);
            size_t sb = lcg_range(&rng, prev_width);
            if (prev_width > 1 && sb == sa) sb = (sa + 1) % prev_width;
            net->in_a[gi] = (int)(prev_base + sa);
            net->in_b[gi] = (int)(prev_base + sb);
            { int k; for (k = 0; k < 16; ++k)
                net->logit[gi * 16 + k] = (lcg_unif(&rng) - 0.5) * 0.1; }
        }
        prev_base = input_count + (gi - width); /* this layer's value-index base */
        prev_width = width;
    }
    (void)i;
    return 0;
}

/* ---- soft forward (fills values[input_count + total_gates]) -------------- */
static void forward_soft(const LogicGateNetwork *net, const double *input,
                         double *values) {
    size_t g, ic = net->input_count;
    for (g = 0; g < ic; ++g) values[g] = input[g];
    for (g = 0; g < net->total_gates; ++g) {
        double a = values[net->in_a[g]], b = values[net->in_b[g]];
        double p[16], v = 0.0; int k;
        softmax16(net->logit + g * 16, p);
        for (k = 0; k < 16; ++k)
            v += p[k] * (g_coef[k][0] + g_coef[k][1] * a + g_coef[k][2] * b + g_coef[k][3] * a * b);
        values[ic + g] = v;
    }
}

/* ---- training ----------------------------------------------------------- */

double lgn_train(LogicGateNetwork *net, const double *inputs,
                 const double *targets, size_t rows, size_t epochs, double lr) {
    size_t nval, nparam, e, r, g, ic, oc, obase;
    double *values, *dval, *grad, *vel, mse = 0.0;
    if (!net || !inputs || !targets || rows == 0) return -1.0;

    ic = net->input_count; oc = net->output_count;
    nval = ic + net->total_gates;
    nparam = net->total_gates * 16;
    obase = ic + net->total_gates - oc;   /* value index of first output gate */

    values = (double *)malloc(nval * sizeof(double));
    dval   = (double *)malloc(nval * sizeof(double));
    grad   = (double *)malloc(nparam * sizeof(double));
    vel    = (double *)calloc(nparam, sizeof(double));
    if (!values || !dval || !grad || !vel) { free(values); free(dval); free(grad); free(vel); return -1.0; }

    for (e = 0; e < epochs; ++e) {
        memset(grad, 0, nparam * sizeof(double));
        mse = 0.0;
        for (r = 0; r < rows; ++r) {
            forward_soft(net, inputs + r * ic, values);
            memset(dval, 0, nval * sizeof(double));
            /* output error (MSE) */
            for (g = 0; g < oc; ++g) {
                double v = values[obase + g];
                double err = v - targets[r * oc + g];
                mse += err * err;
                dval[obase + g] = 2.0 * err / (double)oc;
            }
            /* reverse-mode backprop (gates are in topological order) */
            for (g = net->total_gates; g-- > 0; ) {
                double a = values[net->in_a[g]], b = values[net->in_b[g]];
                double vg = values[ic + g], dv = dval[ic + g];
                double p[16], da = 0.0, db = 0.0; int k;
                softmax16(net->logit + g * 16, p);
                for (k = 0; k < 16; ++k) {
                    double fk = g_coef[k][0] + g_coef[k][1] * a + g_coef[k][2] * b + g_coef[k][3] * a * b;
                    grad[g * 16 + k] += dv * p[k] * (fk - vg);   /* softmax jacobian */
                    da += p[k] * (g_coef[k][1] + g_coef[k][3] * b);
                    db += p[k] * (g_coef[k][2] + g_coef[k][3] * a);
                }
                dval[net->in_a[g]] += dv * da;
                dval[net->in_b[g]] += dv * db;
            }
        }
        /* SGD + momentum */
        { size_t i; for (i = 0; i < nparam; ++i) {
            vel[i] = 0.9 * vel[i] - lr * (grad[i] / (double)rows);
            net->logit[i] += vel[i];
        } }
    }

    free(values); free(dval); free(grad); free(vel);
    return mse / (double)rows / (double)oc;
}

void lgn_discretize(LogicGateNetwork *net) {
    size_t g; int k;
    if (!net) return;
    for (g = 0; g < net->total_gates; ++g) {
        int best = 0; double bestv = net->logit[g * 16];
        for (k = 1; k < 16; ++k) if (net->logit[g * 16 + k] > bestv) { bestv = net->logit[g * 16 + k]; best = k; }
        net->gate[g] = best;
    }
    net->discretized = 1;
}

int lgn_eval(const LogicGateNetwork *net, const double *input, double *out) {
    double *values; size_t g, ic, obase;
    if (!net || !input || !out || !net->discretized) return -1;
    ic = net->input_count;
    values = (double *)malloc((ic + net->total_gates) * sizeof(double));
    if (!values) return -1;
    for (g = 0; g < ic; ++g) values[g] = (input[g] > 0.5) ? 1.0 : 0.0;
    for (g = 0; g < net->total_gates; ++g) {
        int a = (values[net->in_a[g]] > 0.5) ? 1 : 0;
        int b = (values[net->in_b[g]] > 0.5) ? 1 : 0;
        int bit = (net->gate[g] >> (2 * a + b)) & 1;     /* exact boolean logic */
        values[ic + g] = (double)bit;
    }
    obase = ic + net->total_gates - net->output_count;
    for (g = 0; g < net->output_count; ++g) out[g] = values[obase + g];
    free(values);
    return 0;
}

/* Total bit-mismatch of the current discrete circuit against a table. */
static size_t discrete_mismatches(const LogicGateNetwork *net, const double *inputs,
                                  const double *targets, size_t rows) {
    size_t r, g, mm = 0;
    double *out = (double *)malloc(net->output_count * sizeof(double));
    if (!out) return (size_t)-1;
    for (r = 0; r < rows; ++r) {
        if (lgn_eval(net, inputs + r * net->input_count, out) != 0) { free(out); return (size_t)-1; }
        for (g = 0; g < net->output_count; ++g) {
            int got = (out[g] > 0.5) ? 1 : 0;
            int want = (targets[r * net->output_count + g] > 0.5) ? 1 : 0;
            if (got != want) ++mm;
        }
    }
    free(out);
    return mm;
}

void lgn_refine_discrete(LogicGateNetwork *net, const double *inputs,
                         const double *targets, size_t rows, size_t max_passes) {
    size_t pass;
    if (!net || !net->discretized) return;
    for (pass = 0; pass < max_passes; ++pass) {
        int improved = 0; size_t g;
        for (g = 0; g < net->total_gates; ++g) {
            int orig = net->gate[g], best = net->gate[g], k;
            size_t bestm = discrete_mismatches(net, inputs, targets, rows);
            if (bestm == 0) return;                 /* already exact */
            for (k = 0; k < 16; ++k) {
                if (k == orig) continue;
                net->gate[g] = k;
                { size_t m = discrete_mismatches(net, inputs, targets, rows);
                  if (m < bestm) { bestm = m; best = k; } }
            }
            net->gate[g] = best;
            if (best != orig) improved = 1;
        }
        if (!improved) break;                       /* local minimum */
    }
}

int lgn_certify_exhaustive(const LogicGateNetwork *net, const double *inputs,
                           const double *targets, size_t rows, size_t *mismatches) {
    size_t r, g, mm = 0;
    double *out;
    if (mismatches) *mismatches = 0;
    if (!net || !inputs || !targets || !net->discretized) return -1;
    if (net->input_count >= 8 * sizeof(size_t)) return -1;
    if (rows != ((size_t)1 << net->input_count)) return -1;   /* must be exhaustive */

    out = (double *)malloc(net->output_count * sizeof(double));
    if (!out) return -1;
    for (r = 0; r < rows; ++r) {
        if (lgn_eval(net, inputs + r * net->input_count, out) != 0) { free(out); return -1; }
        for (g = 0; g < net->output_count; ++g) {
            int got = (out[g] > 0.5) ? 1 : 0;
            int want = (targets[r * net->output_count + g] > 0.5) ? 1 : 0;
            if (got != want) ++mm;
        }
    }
    free(out);
    if (mismatches) *mismatches = mm;
    return (mm == 0) ? 0 : -1;
}

int lgn_learn_exact(LogicGateNetwork *net, size_t input_count, size_t output_count,
                    const size_t *hidden_widths, size_t num_hidden,
                    const double *inputs, const double *targets, size_t rows,
                    size_t epochs, double lr, size_t max_restarts, unsigned seed0) {
    size_t attempt;
    if (!net) return -1;
    for (attempt = 0; attempt <= max_restarts; ++attempt) {
        size_t mm = 0;
        if (lgn_init(net, input_count, output_count, hidden_widths, num_hidden,
                     seed0 + (unsigned)attempt * 2654435761u) != 0) return -1;
        lgn_train(net, inputs, targets, rows, epochs, lr);
        lgn_discretize(net);
        lgn_refine_discrete(net, inputs, targets, rows, 32);   /* close the soft/discrete gap */
        if (lgn_certify_exhaustive(net, inputs, targets, rows, &mm) == 0) return 0;
        if (attempt < max_restarts) lgn_free(net);
    }
    return -1;   /* last attempt kept in net */
}
