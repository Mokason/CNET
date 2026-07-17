/* teach_fast_bench: hermetic profile + A/B + gate for the gap-lane student
 * training hot loop (btn_train_dynamic driven exactly the way the deployed
 * lane drives it).
 *
 * Fixture = the campaign's top-3 unit shape, no model needed:
 *   in  = 256 one-hot (corpus window token)
 *   out = 3 x 256 one-hot (ordered top-3 continuation)
 *   256 exemplars (exhaustive window), deterministic synthetic mapping.
 * Drive = src/acquire.c's adaptive staged loop with the deployed service's
 * knobs (config/cnet-gap-lane.service): max_epochs=20000, CNET_ACQ_STAGES=40
 * (500 epochs/stage), growth_window=200, target_loss=1e-7,
 * min_improvement=1e-9, lr=0.5, seed=42, hidden 128->512, btn_certify
 * between stages, plateau give-up = stages/4.
 *
 * Modes:
 *   run      full teach with the real btn_train_dynamic; honors
 *            CNET_TRAIN_FAST from the environment. Prints wall time, stages,
 *            certify verdict and an FNV-1a digest of the trained weights.
 *   profile  instrumented REPLICA of btn_train_dynamic (same maths, same
 *            rand consumption) with per-phase timers, then the real trainer
 *            on a second student for a bit-identity check of the replica.
 *   gate     OFF run then ON run in one process (same seed; btn_init
 *            reseeds rand), asserts both certify with byte-identical
 *            weights and prints TEACH_FAST_PASS.
 */
#include "../include/nn.h"
#include "../include/contract/contract.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define FIX_W 256          /* window width = input one-hot width */
#define FIX_K 3            /* top-k fields */
#define FIX_IN (FIX_W)
#define FIX_OUT (FIX_W * FIX_K)
#define FIX_N 256          /* exemplars (exhaustive window) */

#define ACQ_HIDDEN 128
#define ACQ_MAXHIDDEN 512
#define ACQ_LR 0.5
#define ACQ_SEED 42u
#define ACQ_EPOCHS 20000
#define ACQ_STAGES 40
#define ACQ_GROWTH 200
#define ACQ_TARGET 1e-7
#define ACQ_MINIMP 1e-9

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* deterministic synthetic top-3 mapping: 3 distinct picks per exemplar */
static void build_fixture(double *inputs, double *targets) {
    size_t i, f;
    memset(inputs, 0, (size_t)FIX_N * FIX_IN * sizeof *inputs);
    memset(targets, 0, (size_t)FIX_N * FIX_OUT * sizeof *targets);
    for (i = 0; i < FIX_N; ++i) {
        unsigned pick[FIX_K];
        inputs[i * FIX_IN + i] = 1.0;
        for (f = 0; f < FIX_K; ++f) {
            unsigned p = (unsigned)((i * 2654435761u + f * 40503u + 12345u)
                                    >> 7) % FIX_W;
            size_t g;
            for (g = 0; g < f; ++g) {
                if (pick[g] == p) { p = (p + 97u) % FIX_W; g = (size_t)-1; }
            }
            pick[f] = p;
            targets[i * FIX_OUT + f * FIX_W + p] = 1.0;
        }
    }
}

static void fixture_ports(Port *in, Port *goal) {
    memset(in, 0, sizeof *in);
    memset(goal, 0, sizeof *goal);
    in->family = PORT_ONEHOT;
    in->field_width = FIX_W;
    in->field_count = 1;
    snprintf(in->tag, PORT_TAG_MAX, "%s", "tfb_tok");
    goal->family = PORT_ONEHOT;
    goal->field_width = FIX_W;
    goal->field_count = FIX_K;
    snprintf(goal->tag, PORT_TAG_MAX, "%s", "tfb_next3");
}

static unsigned long long fnv64(unsigned long long h, const void *p, size_t n) {
    const unsigned char *b = (const unsigned char *)p;
    size_t i;
    if (h == 0) h = 1469598103934665603ULL;
    for (i = 0; i < n; ++i) {
        h ^= b[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static unsigned long long btn_weights_digest(const BinaryTransformNetwork *b) {
    unsigned long long h = 0;
    h = fnv64(h, &b->hidden_count, sizeof b->hidden_count);
    h = fnv64(h, b->input_hidden,
              b->input_count * b->max_hidden_count * sizeof(double));
    h = fnv64(h, b->hidden_bias, b->max_hidden_count * sizeof(double));
    h = fnv64(h, b->hidden_output_weights,
              b->max_hidden_count * b->output_count * sizeof(double));
    h = fnv64(h, b->output_bias, b->output_count * sizeof(double));
    return h;
}

/* ---- staged adaptive drive (mirror of src/acquire.c:1014-1055) ---------- */

typedef double (*stage_train_fn)(BinaryTransformNetwork *btn,
                                 const double *inputs, const double *targets,
                                 size_t n, size_t max_epochs,
                                 size_t growth_window, double target_loss,
                                 double min_improvement, void *ctx);

static double real_stage(BinaryTransformNetwork *btn, const double *inputs,
                         const double *targets, size_t n, size_t max_epochs,
                         size_t growth_window, double target_loss,
                         double min_improvement, void *ctx) {
    (void)ctx;
    return btn_train_dynamic(btn, inputs, targets, n, max_epochs,
                             growth_window, target_loss, min_improvement);
}

/* Returns 0 when the staged drive ended with a certified-exact student. */
static int staged_teach(BinaryTransformNetwork *btn, const double *inputs,
                        const double *targets, stage_train_fn train, void *ctx,
                        size_t *stages_used, double *final_loss,
                        double *cert_sec) {
    size_t s, prev_pass = 0, plateau = 0;
    size_t plateau_limit = ACQ_STAGES / 4;
    int exact = -1;
    Contract probe;
    int have_probe = (contract_init_borrowed(&probe, "tfb_unit", btn, inputs,
                                             targets, FIX_N) == 0);
    if (plateau_limit < 2) plateau_limit = 2;
    *stages_used = 0;
    *final_loss = -1.0;
    if (cert_sec) *cert_sec = 0.0;
    for (s = 0; s < ACQ_STAGES; ++s) {
        CertifyReport crep;
        double t0;
        *final_loss = train(btn, inputs, targets, FIX_N,
                            ACQ_EPOCHS / ACQ_STAGES, ACQ_GROWTH, ACQ_TARGET,
                            ACQ_MINIMP, ctx);
        *stages_used = s + 1;
        if (!have_probe) continue;
        t0 = now_sec();
        exact = btn_certify(btn, &probe, &crep);
        if (cert_sec) *cert_sec += now_sec() - t0;
        if (exact == 0) break;
        if (crep.passed <= prev_pass) {
            if (++plateau >= plateau_limit) break;
        } else {
            plateau = 0;
        }
        prev_pass = crep.passed;
    }
    if (have_probe) contract_free(&probe);
    return exact;
}

/* ---- instrumented replica of btn_train_dynamic (plain-SGD path) ----------
 * Same arithmetic, same order, same rand() consumption as src/nn.c, with
 * clock_gettime around each phase. Bit-identity vs the real trainer is
 * asserted by the caller, so the attribution below is the real loop's. */

typedef struct {
    double fwd_hidden;   /* input->hidden reduction (training forwards) */
    double fwd_output;   /* hidden->output reduction (training forwards) */
    double bp_deltas;    /* output deltas + hidden_errors backprop */
    double upd_ho;       /* hidden->output weight + bias update */
    double upd_ih;       /* input->hidden weight + bias update */
    double loss_eval;    /* initial / per-window train + validation evals */
    size_t epochs;
    size_t samples;
} PhaseProfile;

static double rep_sigmoid(double x) { return 1.0 / (1.0 + exp(-x)); }
static double rep_sigd(double y) { return y * (1.0 - y); }
static double rep_random_weight(void) {
    return ((double)rand() / (double)RAND_MAX) * 2.0 - 1.0;
}

static const double *rep_forward(BinaryTransformNetwork *b,
                                 const double *inputs) {
    size_t input, hidden, output;
    for (hidden = 0; hidden < b->hidden_count; ++hidden) {
        double hidden_sum = b->hidden_bias[hidden];
        const double *row = b->input_hidden + hidden * b->input_count;
        for (input = 0; input < b->input_count; ++input) {
            hidden_sum += inputs[input] * row[input];
        }
        b->hidden_output[hidden] = rep_sigmoid(hidden_sum);
    }
    for (output = 0; output < b->output_count; ++output) {
        double output_sum = b->output_bias[output];
        const double *row = b->hidden_output_weights +
                            output * b->max_hidden_count;
        for (hidden = 0; hidden < b->hidden_count; ++hidden) {
            output_sum += b->hidden_output[hidden] * row[hidden];
        }
        b->last_output[output] = rep_sigmoid(output_sum);
    }
    return b->last_output;
}

static int rep_add_hidden_neuron(BinaryTransformNetwork *b) {
    size_t hidden, input, output;
    if (b->hidden_count >= b->max_hidden_count) return -1;
    hidden = b->hidden_count;
    for (input = 0; input < b->input_count; ++input) {
        b->input_hidden[hidden * b->input_count + input] = rep_random_weight();
    }
    b->hidden_bias[hidden] = rep_random_weight();
    b->hidden_output[hidden] = 0.0;
    for (output = 0; output < b->output_count; ++output) {
        b->hidden_output_weights[output * b->max_hidden_count + hidden] =
            rep_random_weight();
    }
    if (hidden > 0) {
        for (output = 0; output < b->output_count; ++output) {
            b->hidden_output_weights[output * b->max_hidden_count + hidden] =
                0.0;
        }
    }
    b->hidden_count += 1;
    return 0;
}

static double rep_table_loss(BinaryTransformNetwork *b, const double *inputs,
                             const double *targets, size_t sample_count,
                             const char *mask, int mask_val, size_t denom) {
    size_t sample, output;
    double loss = 0.0;
    for (sample = 0; sample < sample_count; ++sample) {
        const double *in = inputs + sample * b->input_count;
        const double *tg = targets + sample * b->output_count;
        const double *out;
        if (mask && mask[sample] != mask_val) continue;
        out = rep_forward(b, in);
        for (output = 0; output < b->output_count; ++output) {
            double e = tg[output] - out[output];
            loss += e * e;
        }
    }
    return loss / (double)(denom * b->output_count);
}

static double rep_train_dynamic(BinaryTransformNetwork *btn,
                                const double *inputs, const double *targets,
                                size_t sample_count, size_t max_epochs,
                                size_t growth_window, double target_loss,
                                double min_improvement, void *ctx) {
    PhaseProfile *pp = (PhaseProfile *)ctx;
    size_t target_val = 0, val_n = 0, epochs_done = 0;
    size_t sample, epoch, output, hidden, input, stride, off;
    char *mask = NULL;
    double *output_deltas, *hidden_errors;
    double previous_loss, ema_loss, ema_prev, improvement, rel;
    const double ema_alpha = 0.2;
    double validation_loss, train_loss, lr;
    double t0, t1;

    if (growth_window == 0) growth_window = 1;
    if (sample_count > 1) {
        target_val = sample_count / 5;
        if (target_val == 0) target_val = 1;
    }
    if (target_val >= sample_count) target_val = sample_count - 1;
    if (target_val > 0) {
        mask = calloc(sample_count, 1);
        if (!mask) return -1.0;
        stride = sample_count / target_val;
        if (stride == 0) stride = 1;
        off = stride / 2;
        for (sample = 0; sample < sample_count && val_n < target_val;
             ++sample) {
            if ((sample % stride) == off) {
                mask[sample] = 1;
                ++val_n;
            }
        }
    }
    output_deltas = calloc(btn->output_count, sizeof *output_deltas);
    hidden_errors = calloc(btn->max_hidden_count, sizeof *hidden_errors);
    if (!output_deltas || !hidden_errors) {
        free(output_deltas); free(hidden_errors); free(mask);
        return -1.0;
    }

    t0 = now_sec();
    previous_loss = rep_table_loss(btn, inputs, targets, sample_count, NULL, 0,
                                   sample_count);
    if (val_n > 0) {
        ema_loss = rep_table_loss(btn, inputs, targets, sample_count, mask, 1,
                                  val_n);
    } else {
        ema_loss = previous_loss;
    }
    pp->loss_eval += now_sec() - t0;
    lr = btn->learning_rate;

    while (epochs_done < max_epochs && previous_loss > target_loss) {
        size_t epochs_to_train = growth_window;
        if (epochs_done + epochs_to_train > max_epochs) {
            epochs_to_train = max_epochs - epochs_done;
        }
        for (epoch = 0; epoch < epochs_to_train; ++epoch) {
            for (sample = 0; sample < sample_count; ++sample) {
                const double *in = inputs + sample * btn->input_count;
                const double *tg = targets + sample * btn->output_count;
                const size_t H = btn->hidden_count;
                const size_t O = btn->output_count;
                const size_t I = btn->input_count;
                const size_t MH = btn->max_hidden_count;

                /* forward, split per layer for attribution */
                t0 = now_sec();
                for (hidden = 0; hidden < H; ++hidden) {
                    double hidden_sum = btn->hidden_bias[hidden];
                    const double *row = btn->input_hidden + hidden * I;
                    for (input = 0; input < I; ++input) {
                        hidden_sum += in[input] * row[input];
                    }
                    btn->hidden_output[hidden] = rep_sigmoid(hidden_sum);
                }
                t1 = now_sec(); pp->fwd_hidden += t1 - t0; t0 = t1;
                for (output = 0; output < O; ++output) {
                    double output_sum = btn->output_bias[output];
                    const double *row = btn->hidden_output_weights +
                                        output * MH;
                    for (hidden = 0; hidden < H; ++hidden) {
                        output_sum += btn->hidden_output[hidden] * row[hidden];
                    }
                    btn->last_output[output] = rep_sigmoid(output_sum);
                }
                t1 = now_sec(); pp->fwd_output += t1 - t0; t0 = t1;

                for (output = 0; output < O; ++output) {
                    double error = tg[output] - btn->last_output[output];
                    output_deltas[output] =
                        error * rep_sigd(btn->last_output[output]);
                }
                for (hidden = 0; hidden < H; ++hidden) {
                    hidden_errors[hidden] = 0.0;
                }
                for (output = 0; output < O; ++output) {
                    const double *row = btn->hidden_output_weights +
                                        output * MH;
                    for (hidden = 0; hidden < H; ++hidden) {
                        hidden_errors[hidden] +=
                            output_deltas[output] * row[hidden];
                    }
                }
                t1 = now_sec(); pp->bp_deltas += t1 - t0; t0 = t1;

                for (output = 0; output < O; ++output) {
                    double *row = btn->hidden_output_weights + output * MH;
                    for (hidden = 0; hidden < H; ++hidden) {
                        double grad = output_deltas[output] *
                                      btn->hidden_output[hidden];
                        row[hidden] += lr * grad;
                    }
                    btn->output_bias[output] += lr * output_deltas[output];
                }
                t1 = now_sec(); pp->upd_ho += t1 - t0; t0 = t1;

                for (hidden = 0; hidden < H; ++hidden) {
                    double hidden_delta =
                        hidden_errors[hidden] *
                        rep_sigd(btn->hidden_output[hidden]);
                    double *row = btn->input_hidden + hidden * I;
                    btn->hidden_bias[hidden] += lr * hidden_delta;
                    for (input = 0; input < I; ++input) {
                        double grad = hidden_delta * in[input];
                        row[input] += lr * grad;
                    }
                }
                pp->upd_ih += now_sec() - t0;
                pp->samples += 1;
            }
        }
        epochs_done += epochs_to_train;
        pp->epochs += epochs_to_train;

        t0 = now_sec();
        train_loss = rep_table_loss(btn, inputs, targets, sample_count, NULL,
                                    0, sample_count);
        pp->loss_eval += now_sec() - t0;
        if (train_loss <= target_loss) {
            previous_loss = train_loss;
            break;
        }
        if (val_n > 0) {
            t0 = now_sec();
            validation_loss = rep_table_loss(btn, inputs, targets,
                                             sample_count, mask, 1, val_n);
            pp->loss_eval += now_sec() - t0;
        } else {
            validation_loss = train_loss;
        }
        ema_prev = ema_loss;
        ema_loss = ema_alpha * validation_loss + (1.0 - ema_alpha) * ema_loss;
        improvement = ema_prev - ema_loss;
        rel = ema_prev > 0.0 ? improvement / ema_prev : 0.0;
        if (rel < min_improvement && rep_add_hidden_neuron(btn) == 0) {
            previous_loss = train_loss;
        } else {
            previous_loss = train_loss;
        }
    }
    free(output_deltas);
    free(hidden_errors);
    free(mask);
    return previous_loss;
}

/* ---- modes -------------------------------------------------------------- */

/* CNET_TFB_HIDDEN (bench-only): initial hidden size, default the campaign's
   128. 512 measures the deployed steady state (real units grow to ~512). */
static BinaryTransformNetwork *fresh_student(void) {
    Port in, goal;
    size_t hidden = ACQ_HIDDEN;
    const char *e = getenv("CNET_TFB_HIDDEN");
    BinaryTransformNetwork *btn = calloc(1, sizeof *btn);
    if (!btn) return NULL;
    if (e && e[0]) {
        long v = atol(e);
        if (v >= 1 && v <= ACQ_MAXHIDDEN) hidden = (size_t)v;
    }
    fixture_ports(&in, &goal);
    if (btn_init(btn, FIX_IN, FIX_OUT, hidden, ACQ_MAXHIDDEN, ACQ_LR,
                 ACQ_SEED) != 0 ||
        btn_set_ports(btn, in, goal) != 0) {
        free(btn);
        return NULL;
    }
    return btn;
}

static int mode_run(const double *inputs, const double *targets) {
    BinaryTransformNetwork *btn = fresh_student();
    size_t stages = 0;
    double loss = -1.0, cert_s = 0.0, t0, wall;
    int exact;
    const char *fast = getenv("CNET_TRAIN_FAST");
    if (!btn) return 2;
    t0 = now_sec();
    exact = staged_teach(btn, inputs, targets, real_stage, NULL, &stages,
                         &loss, &cert_s);
    wall = now_sec() - t0;
    printf("RUN fast=%s wall=%.3fs stages=%zu epochs<=%zu hidden=%zu "
           "final_loss=%.3e certify=%s cert_time=%.3fs digest=%016llx\n",
           (fast && fast[0] == '1') ? "on" : "off", wall, stages,
           stages * (ACQ_EPOCHS / ACQ_STAGES), btn->hidden_count, loss,
           exact == 0 ? "EXACT" : "not-exact", cert_s,
           btn_weights_digest(btn));
    btn_free(btn);
    free(btn);
    return exact == 0 ? 0 : 1;
}

static int mode_profile(const double *inputs, const double *targets) {
    PhaseProfile pp;
    size_t stages = 0;
    double loss = -1.0, cert_s = 0.0, t0, wall, accounted;
    unsigned long long dig_rep, dig_real;
    int exact;
    BinaryTransformNetwork *btn;

    memset(&pp, 0, sizeof pp);

    /* replica first (cold certify cache), timers on */
    btn = fresh_student();
    if (!btn) return 2;
    t0 = now_sec();
    exact = staged_teach(btn, inputs, targets, rep_train_dynamic, &pp,
                         &stages, &loss, &cert_s);
    wall = now_sec() - t0;
    dig_rep = btn_weights_digest(btn);
    btn_free(btn);
    free(btn);

    accounted = pp.fwd_hidden + pp.fwd_output + pp.bp_deltas + pp.upd_ho +
                pp.upd_ih + pp.loss_eval + cert_s;
    printf("PROFILE wall=%.3fs stages=%zu epochs=%zu samples=%zu "
           "certify=%s final_loss=%.3e\n",
           wall, stages, pp.epochs, pp.samples,
           exact == 0 ? "EXACT" : "not-exact", loss);
    printf("PROFILE fwd_hidden=%.3fs (%.1f%%)\n", pp.fwd_hidden,
           100.0 * pp.fwd_hidden / wall);
    printf("PROFILE fwd_output=%.3fs (%.1f%%)\n", pp.fwd_output,
           100.0 * pp.fwd_output / wall);
    printf("PROFILE bp_deltas=%.3fs (%.1f%%)\n", pp.bp_deltas,
           100.0 * pp.bp_deltas / wall);
    printf("PROFILE upd_ho=%.3fs (%.1f%%)\n", pp.upd_ho,
           100.0 * pp.upd_ho / wall);
    printf("PROFILE upd_ih=%.3fs (%.1f%%)\n", pp.upd_ih,
           100.0 * pp.upd_ih / wall);
    printf("PROFILE loss_eval=%.3fs (%.1f%%)\n", pp.loss_eval,
           100.0 * pp.loss_eval / wall);
    printf("PROFILE certify=%.3fs (%.1f%%)\n", cert_s, 100.0 * cert_s / wall);
    printf("PROFILE accounted=%.3fs (%.1f%%)\n", accounted,
           100.0 * accounted / wall);

    /* real trainer on a fresh same-seed student: replica must be
       bit-identical or the attribution above cannot be trusted */
    btn = fresh_student();
    if (!btn) return 2;
    exact = staged_teach(btn, inputs, targets, real_stage, NULL, &stages,
                         &loss, NULL);
    dig_real = btn_weights_digest(btn);
    btn_free(btn);
    free(btn);
    printf("PROFILE replica_bitident=%s (replica=%016llx real=%016llx)\n",
           dig_rep == dig_real ? "yes" : "NO", dig_rep, dig_real);
    return dig_rep == dig_real && exact == 0 ? 0 : 1;
}

/* OFF run then ON run in one process; byte-identity + certified-exact are
   the pass conditions. Timing here is indicative only (the second run's
   between-stage certifies hit the certify cache); A/B wall time comes from
   two separate `run` invocations. */
static int mode_gate(const double *inputs, const double *targets) {
    unsigned long long dig[2];
    double wall[2], loss;
    size_t stages[2];
    int exact[2], i;

    for (i = 0; i < 2; ++i) {
        BinaryTransformNetwork *btn = fresh_student();
        double t0, cert_s;
        if (!btn) return 2;
        if (setenv("CNET_TRAIN_FAST", i == 0 ? "0" : "1", 1) != 0) {
            btn_free(btn); free(btn);
            return 2;
        }
        t0 = now_sec();
        exact[i] = staged_teach(btn, inputs, targets, real_stage, NULL,
                                &stages[i], &loss, &cert_s);
        wall[i] = now_sec() - t0;
        dig[i] = btn_weights_digest(btn);
        btn_free(btn);
        free(btn);
        printf("GATE fast=%s wall=%.3fs stages=%zu certify=%s "
               "digest=%016llx\n",
               i == 0 ? "off" : "on", wall[i], stages[i],
               exact[i] == 0 ? "EXACT" : "not-exact", dig[i]);
    }
    if (exact[0] != 0 || exact[1] != 0) {
        printf("TEACH_FAST_FAIL certification (off=%d on=%d)\n", exact[0],
               exact[1]);
        return 1;
    }
    if (dig[0] != dig[1]) {
        printf("TEACH_FAST_FAIL weights diverge (off=%016llx on=%016llx)\n",
               dig[0], dig[1]);
        return 1;
    }
    if (stages[0] != stages[1]) {
        printf("TEACH_FAST_FAIL stage counts diverge (off=%zu on=%zu)\n",
               stages[0], stages[1]);
        return 1;
    }
    printf("TEACH_FAST_PASS knob-off bit-identity + knob-on byte-identical "
           "certified student (digest=%016llx, stages=%zu, "
           "off=%.3fs on=%.3fs)\n",
           dig[0], stages[0], wall[0], wall[1]);
    return 0;
}

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "run";
    static double inputs[FIX_N * FIX_IN];
    static double targets[FIX_N * FIX_OUT];
    build_fixture(inputs, targets);
    if (strcmp(mode, "run") == 0) return mode_run(inputs, targets);
    if (strcmp(mode, "profile") == 0) return mode_profile(inputs, targets);
    if (strcmp(mode, "gate") == 0) return mode_gate(inputs, targets);
    fprintf(stderr, "usage: %s run|profile|gate\n", argv[0]);
    return 2;
}
