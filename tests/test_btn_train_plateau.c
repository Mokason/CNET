/* test_btn_train_plateau — dynamic growth must escape a dead-neuron plateau,
 * report a STATUS rather than smuggling one into a double, and never train on
 * the rows it holds out.
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
 * A later review found four more defects in the escape itself:
 *
 *   F2a  reaching target_loss jumped straight to cleanup WITHOUT snapshotting
 *        the successful net, so cleanup could restore older, worse weights over
 *        a run that had just succeeded, and a stale stuck counter could return
 *        the plateau sentinel for it;
 *   F2b  that sentinel was an undocumented -2.0. Every caller in this tree asks
 *        `btn_train_dynamic(...) <= 0.05`, and -2.0 satisfies that, so the one
 *        value meaning "this net is not fit to certify" READ AS THE BEST RESULT
 *        POSSIBLE. src/legacy/main.c then persisted such a net unconditionally;
 *   F3a  the held-out rows were still trained on -- the SGD loop walked every
 *        sample and `train_sample_count` stayed at the full count -- so the
 *        "validation" loss measured rows the optimiser had already fitted;
 *   F3b  the whole-set loss double-counted those rows, because train_loss
 *        already covered them and the validation term was added again.
 *
 * This gate reproduces the original defect directly, with no dependency on the
 * demo or the certifier -- the exact shape, data, hyperparameters and seed
 * `combine` uses, and the same certification predicate (`port_validate` then
 * `port_canonicalize` then exact compare) applied to all 256 exemplars -- and
 * then pins each of the four above.
 *
 * Nothing is written to disk.
 */
#include <math.h>
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

/* The loss a CALLER can reproduce: mean squared error over the WHOLE dataset,
   through the public forward. The trainer's reported loss must equal this
   exactly -- a number the caller cannot recompute is not a measurement. */
static double whole_set_loss(BinaryTransformNetwork *net, const double *inputs,
                             const double *targets, size_t n, size_t in_dim,
                             size_t out_dim) {
    double total = 0.0;
    size_t s, o;
    for (s = 0; s < n; ++s) {
        const double *out = btn_forward(net, inputs + s * in_dim);
        if (out == NULL) return -1.0;
        for (o = 0; o < out_dim; ++o) {
            double error = targets[s * out_dim + o] - out[o];
            total += error * error;
        }
    }
    return total / (double)(n * out_dim);
}

/* ---- the n>=64 exclusion experiment: apparatus -------------------------
   Everything below exists to answer one question DIRECTLY rather than by
   inference: does a held-out row's TARGET reach the gradient?

   The earlier held-out test used 10 samples, which is below
   BTN_TRAIN_MIN_SPLIT_SAMPLES, so no mask was ever built and the exclusion
   path was never executed by any test in this tree. */
#define EX_WIDTH 8
#define EX_N 64
/* The mask the trainer builds at n=64 is deterministic and documented:
   target_val = 64/5 = 12, stride = 64/12 = 5, offset = stride/2 = 2, and the
   scan stops once 12 rows are taken. So rows 2,7,12,...,57 are held out and
   row 62 -- which also satisfies s%5==2 -- is not. This test does not trust
   that arithmetic: it MEASURES which rows are excluded and requires the
   measured set to be exactly this one. */
#define EX_VAL_ROWS 12
#define EX_CELLS (EX_WIDTH * 64 + 64 * EX_WIDTH + 64 + EX_WIDTH)

static size_t weight_cells(const BinaryTransformNetwork *net) {
    return net->input_count * net->max_hidden_count +
           net->max_hidden_count * net->output_count +
           net->max_hidden_count + net->output_count;
}

/* Every gradient-updated cell, in one flat buffer, so "unchanged" can mean
   BYTE-IDENTICAL rather than "close enough". */
static void snapshot_weights(const BinaryTransformNetwork *net, double *out) {
    size_t ih = net->input_count * net->max_hidden_count;
    size_t ho = net->max_hidden_count * net->output_count;
    memcpy(out, net->input_hidden, ih * sizeof(double));
    memcpy(out + ih, net->hidden_output_weights, ho * sizeof(double));
    memcpy(out + ih + ho, net->hidden_bias,
           net->max_hidden_count * sizeof(double));
    memcpy(out + ih + ho + net->max_hidden_count, net->output_bias,
           net->output_count * sizeof(double));
}

/* One run of the fixture, from an identical initial net and RNG seed. */
static int run_exclusion_case(const double *inputs, const double *targets,
                              size_t hidden0, size_t max_hidden,
                              size_t max_epochs, size_t window,
                              double target, double min_improvement,
                              double *weights_out, size_t *cells_out,
                              double *loss_out, size_t *hidden_out) {
    BinaryTransformNetwork net;
    int status;
    memset(&net, 0, sizeof net);
    if (btn_init(&net, EX_WIDTH, EX_WIDTH, hidden0, max_hidden, 0.5,
                 4242u) != 0) {
        return BTN_TRAIN_INVALID;
    }
    status = btn_train_dynamic_checked(&net, inputs, targets, EX_N, max_epochs,
                                       window, target, min_improvement,
                                       loss_out);
    *cells_out = weight_cells(&net);
    *hidden_out = net.hidden_count;
    snapshot_weights(&net, weights_out);
    btn_free(&net);
    return status;
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
    double loss, reported;
    int status;
    size_t passed, bad_input = 0, bad_bit = 0;

    setvbuf(stdout, NULL, _IONBF, 0);
    build_combine_data(inputs, targets);

    /* ---- the original defect: the demo's exact configuration ------------
       8->8, one hidden neuron to start, a ceiling of 64, lr 0.8, seed 91u. */
    memset(&net, 0, sizeof net);
    if (btn_init(&net, WIDTH, WIDTH, 1, 64, 0.8, 91u) != 0) {
        fprintf(stderr, "FAIL: btn_init\n");
        return 1;
    }
    status = btn_train_dynamic_checked(&net, &inputs[0][0], &targets[0][0],
                                       SAMPLES, 150000, 1000, 0.0008, 0.03,
                                       &reported);
    passed = certifiable(&net, inputs, targets, &bad_input, &bad_bit);
    loss = whole_set_loss(&net, &inputs[0][0], &targets[0][0], SAMPLES, WIDTH,
                          WIDTH);

    printf("BTN_PLATEAU_RUN seed=91 max_hidden=64 hidden_used=%zu status=%d "
           "reported=%.17g recomputed=%.17g certifiable=%zu/%d\n",
           net.hidden_count, status, reported, loss, passed, SAMPLES);
    if (passed != SAMPLES)
        printf("BTN_PLATEAU_FIRST_MISS input=%zu bit=%zu\n", bad_input, bad_bit);

    check(status == BTN_TRAIN_OK,
          "the combine run reports success as a STATUS, not as a number");
    /* The whole point: this net must be certifiable over its exemplars. */
    check(passed == SAMPLES,
          "every exemplar certifies after dynamic growth");
    /* F2a: a run that crossed its target after escaping must RETURN the net
       that crossed it, and the loss it reports must describe that net. */
    check(reported == loss,
          "the reported loss is exactly a fresh whole-dataset recomputation");
    check(reported <= 0.0008,
          "a run reporting success actually met the target it was given");
    btn_free(&net);

    /* ---- control: a second seed, held to what genuine exclusion permits ---
       This control used to demand 256/256 from seed 123, and it got it -- while
       the trainer was still taking gradient steps on the rows it called
       held-out. Once those rows are genuinely excluded they are UNSEEN DATA,
       and demanding that a net certify exemplars it was never shown is not a
       control, it is a request to train on the validation set.
       What the run must still guarantee is sharper, and is asserted instead:
         * every row the trainer WAS allowed to see certifies;
         * the reported loss is exactly a whole-set recomputation;
         * the status is documented and the same on a repeat run.
       The held-out count is printed, not floored: it is a generalisation
       observation, and turning an observation into a pass bar is how a number
       stops meaning anything. */
    {
        size_t trained_ok = 0, heldout_ok = 0, trained_n = 0, heldout_n = 0;
        size_t s, i;
        int status_again;
        double reported_again;
        Port out_port;
        memset(&out_port, 0, sizeof out_port);
        out_port.family = PORT_BINARY_MSB;
        out_port.field_width = WIDTH;
        out_port.field_count = 1;

        memset(&net, 0, sizeof net);
        if (btn_init(&net, WIDTH, WIDTH, 1, 64, 0.8, 123u) != 0) {
            fprintf(stderr, "FAIL: btn_init control\n");
            return 1;
        }
        status = btn_train_dynamic_checked(&net, &inputs[0][0], &targets[0][0],
                                           SAMPLES, 150000, 1000, 0.0008, 0.03,
                                           &reported);
        loss = whole_set_loss(&net, &inputs[0][0], &targets[0][0], SAMPLES,
                              WIDTH, WIDTH);
        passed = certifiable(&net, inputs, targets, NULL, NULL);

        /* The mask the trainer builds is deterministic: target_val = 256/5 = 51,
           stride = 256/51 = 5, offset = stride/2 = 2, so every row with
           s % 5 == 2 is held out -- exactly 51 of them. */
        for (s = 0; s < SAMPLES; ++s) {
            const double *raw = btn_forward(&net, inputs[s]);
            double clean[WIDTH];
            int ok = raw != NULL && port_validate(out_port, raw) &&
                     port_canonicalize(out_port, raw, clean) == 0;
            for (i = 0; ok && i < WIDTH; ++i) {
                double want = targets[s][i] >= 0.5 ? 1.0 : 0.0;
                if (clean[i] != want) ok = 0;
            }
            if ((s % 5) == 2) {
                heldout_n++;
                heldout_ok += (size_t)(ok != 0);
            } else {
                trained_n++;
                trained_ok += (size_t)(ok != 0);
            }
        }
        printf("BTN_PLATEAU_CONTROL seed=123 hidden_used=%zu status=%d "
               "reported=%.17g certifiable=%zu/%d trained=%zu/%zu "
               "heldout=%zu/%zu\n",
               net.hidden_count, status, reported, passed, SAMPLES,
               trained_ok, trained_n, heldout_ok, heldout_n);

        check(status == BTN_TRAIN_OK || status == BTN_TRAIN_PLATEAU_STATUS,
              "the control run reports a documented status");
        check(reported == loss,
              "the control's reported loss is exactly a whole-set recomputation");
        check(heldout_n == 51,
              "the split is the documented one: 51 of 256 rows held out");
        /* A run that CLAIMS success must have fitted what it was shown. A run
           that reports BTN_TRAIN_PLATEAU_STATUS is saying it did not reach its
           target, and seed 123 does exactly that here -- 202 of its own 205
           training rows -- so there is no certification claim to hold it to.
           The status is the guarantee; asserting past it would be inventing
           one. */
        if (status == BTN_TRAIN_OK) {
            check(trained_ok == trained_n,
                  "a run reporting SUCCESS certifies every row it was allowed "
                  "to see");
        } else {
            /* ... and a run that reports failure must not also be sitting at
               the target it says it missed. The status and the number have to
               agree, or one of them is decoration. */
            check(reported > 0.0008,
                  "a run reporting that it missed its target is not in fact "
                  "at or below that target");
            check(trained_ok < trained_n,
                  "a run reporting failure did not silently fit everything it "
                  "was shown -- the status describes what actually happened");
            printf("BTN_PLATEAU_CONTROL_HONEST status=plateau trained=%zu/%zu "
                   "(the run says it did not converge, and it did not)\n",
                   trained_ok, trained_n);
        }
        btn_free(&net);

        /* Same seed, same data, same answer. A status that varies run to run is
           not a status. */
        memset(&net, 0, sizeof net);
        if (btn_init(&net, WIDTH, WIDTH, 1, 64, 0.8, 123u) == 0) {
            status_again = btn_train_dynamic_checked(
                &net, &inputs[0][0], &targets[0][0], SAMPLES, 150000, 1000,
                0.0008, 0.03, &reported_again);
            check(status_again == status,
                  "the same seed reports the same status on a repeat run");
            check(reported_again == reported,
                  "the same seed reports the same loss, bit for bit");
            btn_free(&net);
        }
    }

    /* ---- F2b: an unlearnable target must not read as the best result ever
       Two identical inputs with contradictory targets cannot both be learned.
       The old sentinel was -2.0, and every caller in this tree asks
       `btn_train_dynamic(...) <= 0.05`. */
    {
        static double bad_in[2][WIDTH];
        static double bad_tg[2][WIDTH];
        size_t i;
        double compat;
        for (i = 0; i < WIDTH; i++) {
            bad_in[0][i] = 1.0;
            bad_in[1][i] = 1.0;      /* identical inputs ... */
            bad_tg[0][i] = 0.9;
            bad_tg[1][i] = 0.1;      /* ... contradictory targets */
        }
        memset(&net, 0, sizeof net);
        if (btn_init(&net, WIDTH, WIDTH, 1, 8, 0.8, 5u) == 0) {
            status = btn_train_dynamic_checked(&net, &bad_in[0][0],
                                               &bad_tg[0][0], 2, 2000, 50,
                                               1e-9, 0.03, &reported);
            printf("BTN_PLATEAU_IMPOSSIBLE status=%d reported=%.17g\n",
                   status, reported);
            check(status != BTN_TRAIN_OK,
                  "an unlearnable target does not report success");
            btn_free(&net);
        }

        /* The compatibility double-return, on the same impossible problem. */
        memset(&net, 0, sizeof net);
        if (btn_init(&net, WIDTH, WIDTH, 1, 8, 0.8, 5u) == 0) {
            compat = btn_train_dynamic(&net, &bad_in[0][0], &bad_tg[0][0], 2,
                                       2000, 50, 1e-9, 0.03);
            printf("BTN_PLATEAU_COMPAT value=%.17g finite=%d\n", compat,
                   isfinite(compat));
            check(!(compat <= 0.05),
                  "the failure value cannot satisfy the `<= 0.05` check ~20 "
                  "call sites in this tree use");
            check(!(compat <= 1e-9),
                  "the failure value cannot satisfy the tightest target either");
            check(!(compat < 0.0),
                  "the failure value is not NEGATIVE -- a negative satisfies "
                  "every positive threshold, which is how -2.0 read as the "
                  "best result possible");
            check(!isfinite(compat),
                  "the failure value is not a plausible loss a report could "
                  "print as if it were a measurement");
            btn_free(&net);
        }
    }

    /* ---- contradictions cannot be reported as learned --------------------
       NOTE ON WHAT THIS DOES AND DOES NOT TEST. With 10 samples there is NO
       split at all -- BTN_TRAIN_MIN_SPLIT_SAMPLES is 64 -- so this exercises
       the below-floor path, where every row is trained on and none is called
       held out. It was previously labelled as the held-out test, which it
       cannot be: at n=10 the mask is never built. The decisive exclusion
       experiment is the n>=64 one further down.

       What it does prove: two rows contradict row 0 outright, so no run can
       report a near-zero loss for this set, and the whole-set loss the
       trainer reports must contain them. */
    {
        enum { VN = 10 };
        static double vin[VN][WIDTH];
        static double vtg[VN][WIDTH];
        size_t s, i;
        for (s = 0; s < VN; ++s) {
            for (i = 0; i < WIDTH; ++i) {
                vin[s][i] = ((s >> (i % 3)) & 1) ? 1.0 : 0.0;
                vtg[s][i] = vin[s][i] ? 0.9 : 0.1;
            }
        }
        /* rows 2 and 7: same input as row 0, opposite target */
        for (s = 0; s < VN; ++s) {
            if (s != 2 && s != 7) continue;
            for (i = 0; i < WIDTH; ++i) {
                vin[s][i] = vin[0][i];
                vtg[s][i] = vin[0][i] ? 0.1 : 0.9;
            }
        }
        memset(&net, 0, sizeof net);
        if (btn_init(&net, WIDTH, WIDTH, 2, 32, 0.5, 7u) == 0) {
            status = btn_train_dynamic_checked(&net, &vin[0][0], &vtg[0][0], VN,
                                               20000, 200, 1e-6, 0.01,
                                               &reported);
            loss = whole_set_loss(&net, &vin[0][0], &vtg[0][0], VN, WIDTH,
                                  WIDTH);
            printf("BTN_PLATEAU_HELDOUT status=%d reported=%.17g "
                   "recomputed=%.17g\n", status, reported, loss);
            if (status == BTN_TRAIN_OK)
                check(reported == loss,
                      "a held-out split still reports a reproducible loss");
            check(!(status == BTN_TRAIN_OK && reported < 0.01),
                  "a dataset containing outright contradictions cannot be "
                  "reported as successfully learned with a near-zero loss");
            btn_free(&net);
        }
    }

    /* ---- the split-minimum rule, tested at its boundary -----------------
       Below BTN_TRAIN_MIN_SPLIT_SAMPLES there is NO split and NO validation
       claim -- those rows are trained on like every other row, and calling
       them held-out would be the exact mislabelling this pass exists to fix.
       At or above it the split is real and its rows are never stepped on.

       The boundary is observable, not just documented. Both datasets carry an
       outright contradiction at index 2, which is the first index the mask
       would take (stride = n/(n/5) = 5, offset = 2). Below the floor that row
       is trained on and fights every other row; at the floor it is excluded
       from training entirely, so the optimiser converges on what remains. */
    {
        enum { BELOW = 63, ATFLOOR = 64 };
        static double sin_[ATFLOOR][WIDTH];
        static double stg[ATFLOOR][WIDTH];
        size_t s, i;
        double loss_below, loss_at;
        int status_below, status_at;

        for (s = 0; s < ATFLOOR; ++s) {
            for (i = 0; i < WIDTH; ++i) {
                sin_[s][i] = ((s >> (i % 4)) & 1) ? 1.0 : 0.0;
                stg[s][i] = sin_[s][i] ? 0.9 : 0.1;
            }
        }
        /* index 2: same input as index 0, opposite target */
        for (i = 0; i < WIDTH; ++i) {
            sin_[2][i] = sin_[0][i];
            stg[2][i] = sin_[0][i] ? 0.1 : 0.9;
        }

        memset(&net, 0, sizeof net);
        if (btn_init(&net, WIDTH, WIDTH, 4, 48, 0.5, 3u) == 0) {
            status_below = btn_train_dynamic_checked(&net, &sin_[0][0], &stg[0][0],
                                                     BELOW, 8000, 200, 1e-7, 0.01,
                                                     &loss_below);
            btn_free(&net);
        } else {
            status_below = BTN_TRAIN_INVALID;
            loss_below = -1.0;
        }
        memset(&net, 0, sizeof net);
        if (btn_init(&net, WIDTH, WIDTH, 4, 48, 0.5, 3u) == 0) {
            status_at = btn_train_dynamic_checked(&net, &sin_[0][0], &stg[0][0],
                                                  ATFLOOR, 8000, 200, 1e-7, 0.01,
                                                  &loss_at);
            btn_free(&net);
        } else {
            status_at = BTN_TRAIN_INVALID;
            loss_at = -1.0;
        }
        printf("BTN_SPLIT_RULE below_n=%d status=%d loss=%.17g | "
               "at_floor_n=%d status=%d loss=%.17g\n",
               BELOW, status_below, loss_below, ATFLOOR, status_at, loss_at);

        check(status_below != BTN_TRAIN_INVALID &&
                  status_at != BTN_TRAIN_INVALID,
              "both sides of the split floor train at all");
        /* Below the floor the contradictory row IS trained on, so no run can
           report having reached a 1e-7 target -- there is no net that fits it. */
        check(status_below != BTN_TRAIN_OK,
              "below the split floor every row is trained on, so a dataset with "
              "a contradiction cannot report success");
        /* The whole-set loss must contain the contradiction on BOTH sides: it is
           computed over every row regardless of what was trained on. */
        check(loss_below > 0.0 && loss_at > 0.0,
              "the reported loss covers the whole set on both sides of the floor");
    }

    /* ---- THE exclusion experiment, at n = 64 -----------------------------
       Identical initial net, identical seed, identical everything -- then ONE
       row's target is changed, and the trained weights are compared BYTE FOR
       BYTE against the unmutated run.

       A row whose target reaches the gradient must move some weight. A row
       that is genuinely held out cannot move any. So the set of rows whose
       mutation leaves every weight cell identical IS the set of rows the
       optimiser never stepped on, measured rather than assumed -- and it must
       be exactly the 12 rows the documented mask takes.

       Two configurations, because the exclusion has to hold on both paths the
       trainer can take:

         A. CONVERGED, one window. max_epochs == growth_window, and the target
            is reached on the training rows, so the run snapshots and leaves
            through the success path.
         B. NOT CONVERGED, ten windows, growing every window. min_improvement
            is 2.0, so every window counts as stalled and a neuron is added
            each time; the escape needs BTN_TRAIN_STALLED_GROWTH (16)
            consecutive stalls or exhausted capacity, and ten windows starting
            from eight neurons under a ceiling of 64 reach neither. So this
            path runs the growth machinery and the multi-window SGD loop.

       What this deliberately does NOT claim: that the held-out rows have no
       influence at all. They are the growth, escape and best-net SELECTION
       signal -- that is what a validation split IS FOR -- and a run that
       escapes therefore CAN land somewhere else because of them. The claim
       proved here is the exact one the split has to earn: their targets never
       reach a gradient step. */
    {
        static double ex_in[EX_N][EX_WIDTH];
        static double ex_tg[EX_N][EX_WIDTH];
        static double mutated[EX_N][EX_WIDTH];
        static double base_w[EX_CELLS];
        static double case_w[EX_CELLS];
        static int excluded[EX_N];
        size_t s, i, row, cells = 0, mut_cells = 0, hidden = 0, mut_hidden = 0;
        size_t config, unchanged_rows, changed_rows, expected_rows = 0;
        double base_loss = 0.0, mut_loss = 0.0;
        int base_status, mut_status;

        for (s = 0; s < EX_N; ++s) {
            for (i = 0; i < EX_WIDTH; ++i) {
                ex_in[s][i] = ((s >> (EX_WIDTH - 1 - i)) & 1) ? 1.0 : 0.0;
                ex_tg[s][i] = ex_in[s][i] ? 0.9 : 0.1;
            }
        }

        for (config = 0; config < 2; ++config) {
            /* A: converge inside one window.  B: ten windows, never converge. */
            size_t hidden0 = 8, max_hidden = 64;
            size_t epochs = config == 0 ? 4000 : 5000;
            size_t window = config == 0 ? 4000 : 500;
            double target = config == 0 ? 0.01 : 1e-9;
            double min_improvement = config == 0 ? 0.01 : 2.0;
            const char *label = config == 0 ? "converged" : "growing";

            base_status = run_exclusion_case(&ex_in[0][0], &ex_tg[0][0],
                                             hidden0, max_hidden, epochs,
                                             window, target, min_improvement,
                                             base_w, &cells, &base_loss,
                                             &hidden);
            check(cells <= EX_CELLS, "the weight snapshot buffer holds the net");
            check(base_status ==
                      (config == 0 ? BTN_TRAIN_OK : BTN_TRAIN_PLATEAU_STATUS),
                  config == 0
                      ? "config A reaches its target inside one window, so the "
                        "run leaves through the success path"
                      : "config B does not converge, so the growth path is the "
                        "one under test");

            unchanged_rows = 0;
            changed_rows = 0;
            for (row = 0; row < EX_N; ++row) {
                memcpy(mutated, ex_tg, sizeof ex_tg);
                /* Flip this row's target outright: 0.9 <-> 0.1 on every bit.
                   Nothing else in the dataset moves. */
                for (i = 0; i < EX_WIDTH; ++i) {
                    mutated[row][i] = ex_tg[row][i] > 0.5 ? 0.1 : 0.9;
                }
                mut_status = run_exclusion_case(&ex_in[0][0], &mutated[0][0],
                                                hidden0, max_hidden, epochs,
                                                window, target, min_improvement,
                                                case_w, &mut_cells, &mut_loss,
                                                &mut_hidden);
                excluded[row] =
                    mut_cells == cells &&
                    memcmp(base_w, case_w, cells * sizeof(double)) == 0;
                if (excluded[row]) {
                    unchanged_rows++;
                    /* A row that changed NOTHING about the fit must still
                       change the number the run reports, because the reported
                       loss covers every row. Otherwise the mutation never
                       reached the measurement either, and this test would be
                       proving that nothing happened at all. */
                    check(mut_loss != base_loss,
                          "an excluded row still moves the reported whole-set "
                          "loss -- the mutation is real");
                    check(mut_status == base_status,
                          "an excluded row does not change the run's status");
                    check(mut_hidden == hidden,
                          "an excluded row does not change the width reached");
                } else {
                    changed_rows++;
                }
            }

            /* The measured exclusion set must be exactly the documented mask. */
            expected_rows = 0;
            for (row = 0; row < EX_N; ++row) {
                int want = (row % 5) == 2 && expected_rows < EX_VAL_ROWS;
                if (want) expected_rows++;
                check(excluded[row] == want,
                      want ? "a documented validation row's target never "
                             "reaches a gradient step"
                           : "a training row's target DOES reach the gradient "
                             "-- so the byte comparison can detect an update");
            }
            printf("BTN_EXCLUSION_%s n=%d cells=%zu excluded=%zu trained=%zu "
                   "expected_excluded=%zu status=%d hidden=%zu\n",
                   label, EX_N, cells, unchanged_rows, changed_rows,
                   expected_rows, base_status, hidden);
            check(unchanged_rows == EX_VAL_ROWS,
                  "exactly the 12 masked rows are excluded from every update");
            check(changed_rows == EX_N - EX_VAL_ROWS,
                  "every one of the other 52 rows moves the weights");
        }
    }

    /* ---- property: across seeds, a reported loss is always reproducible --
       The two seeds above are the ones the defect was found with. A rule that
       only holds for the cases it was written against is not a rule. */
    {
        unsigned seeds[] = {1u, 17u, 42u, 91u, 123u, 777u, 2024u, 31337u};
        size_t k, ok_runs = 0, independent_ok = 0;
        for (k = 0; k < sizeof seeds / sizeof seeds[0]; ++k) {
            memset(&net, 0, sizeof net);
            if (btn_init(&net, WIDTH, WIDTH, 1, 32, 0.8, seeds[k]) != 0) continue;
            status = btn_train_dynamic_checked(&net, &inputs[0][0],
                                               &targets[0][0], SAMPLES, 150000,
                                               1000, 0.0008, 0.03, &reported);
            loss = whole_set_loss(&net, &inputs[0][0], &targets[0][0], SAMPLES,
                                  WIDTH, WIDTH);
            check(status == BTN_TRAIN_OK || status == BTN_TRAIN_PLATEAU_STATUS,
                  "every seed reports a documented status");
            if (status == BTN_TRAIN_OK) {
                ok_runs++;
                /* 91 and 123 are the two seeds this file was WRITTEN against.
                   Counting only the others is what makes the bar below a
                   property rather than a restatement of the fixture. */
                if (seeds[k] != 91u && seeds[k] != 123u) independent_ok++;
                check(reported == loss,
                      "for every seed that succeeds, the reported loss is a "
                      "fresh whole-dataset recomputation");
                check(reported <= 0.0008,
                      "a seed reporting success is at or below the target it "
                      "was given");
            } else {
                check(!isfinite(reported) || reported > 0.0008,
                      "a seed that did not succeed does not report a loss "
                      "below the target it failed to reach");
            }
            btn_free(&net);
        }
        printf("BTN_PLATEAU_SEEDS tried=%zu succeeded=%zu independent=%zu\n",
               sizeof seeds / sizeof seeds[0], ok_runs, independent_ok);
        /* The previous form asserted only that each seed reported A DOCUMENTED
           STATUS, which every seed satisfies by plateauing -- a trainer that
           converged for nothing at all would have passed it. At least one seed
           that this file was NOT built around has to actually reach the target
           and report it. */
        check(independent_ok >= 1,
              "at least one seed other than the two this test was written "
              "against converges and reports BTN_TRAIN_OK");
    }

    /* ---- F2b at the caller: the predicate a persisting caller uses -------
       src/legacy/main.c trained, printed the loss, and called btn_save
       unconditionally. The value it would test must refuse the plateau net. */
    {
        static double bad_in[2][WIDTH];
        static double bad_tg[2][WIDTH];
        size_t i;
        double compat;
        for (i = 0; i < WIDTH; i++) {
            bad_in[0][i] = 1.0;
            bad_in[1][i] = 1.0;
            bad_tg[0][i] = 0.9;
            bad_tg[1][i] = 0.1;
        }
        memset(&net, 0, sizeof net);
        if (btn_init(&net, WIDTH, WIDTH, 1, 8, 0.8, 5u) == 0) {
            compat = btn_train_dynamic(&net, &bad_in[0][0], &bad_tg[0][0], 2,
                                       2000, 50, 1e-9, 0.03);
            check(!btn_train_loss_is_success(compat),
                  "the documented success predicate refuses a plateau result, "
                  "so a caller that persists on success cannot persist it");
            btn_free(&net);
        }
        /* ... and accepts an honest one. */
        check(btn_train_loss_is_success(0.0004),
              "the success predicate still accepts an ordinary loss");
        check(!btn_train_loss_is_success(-1.0),
              "the success predicate refuses the argument-error value too");
    }

    if (failures) {
        printf("BTN_TRAIN_PLATEAU_FAIL checks=%d failures=%d\n", checks,
               failures);
        return 1;
    }
    printf("BTN_TRAIN_PLATEAU_PASS checks=%d certifiable=%d/%d status=checked "
           "loss=reproducible heldout=excluded\n", checks, SAMPLES, SAMPLES);
    return 0;
}
