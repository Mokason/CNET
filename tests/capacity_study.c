/*
 * Capacity study: where does a flat single-hidden-layer student stop
 * reaching 100% exact on the decimal-adder scaling family?
 *
 * The family is digits(a) x digits(b) addition with carry-in -- every
 * scale is a real adder, x10 in domain per step:
 *
 *   (1,1)  200 samples   21 -> 5
 *   (2,1)  2,000 samples 31 -> 9
 *   (2,2)  20,000 samples 41 -> 9
 *
 * Students are FIXED width (initial == max hidden: dynamic growth would
 * confound the capacity axis) and train on the canonical hard-target
 * tables a strict teacher would produce. Each run is capped by epochs
 * (the reproducible unit -- exact counts are seeded-deterministic given
 * the epoch count) and by wall-clock seconds (the safety; timings are
 * machine-indicative only). Run from the repo root: make study, or
 * ./capacity_study [seconds-per-run] [--full|--ternary|--compare]
 * [--threshold <t>] [--trials <n>] [--seed-start <seed>] [--train-fraction <p>]
 * to override the defaults.
 *
 * Not part of make test: this program exists to spend a compute budget.
 */
#include "../include/nn.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_N 20000
#define MAX_IN 41
#define MAX_OUT 9
#define DEFAULT_TERNARY_THRESHOLD 0.05
#define DEFAULT_TRAIN_FRACTION 0.80
#define DEFAULT_TRIALS 1
#define DEFAULT_SEED_START 71u

typedef enum {
    RUN_MODE_FULL = 0,
    RUN_MODE_TERNARY = 1,
    RUN_MODE_COMPARE = 2
} RunMode;

typedef struct {
    double sum;
    double sumsq;
} RunningStat;

typedef struct {
    size_t best_at;
    double test_exact;
    double test_loss;
    double secs;
} TrialResult;

typedef struct {
    RunningStat best_at;
    RunningStat test_exact;
    RunningStat test_loss;
    RunningStat secs;
} ModeStats;

/* Build the (da, db) adder table. Inputs: one-hot-10 per digit of a
   (least significant digit first), then b's digits, then the carry bit.
   Targets: hard canonical 4 bits (MSB first) per sum digit (least
   significant first), then the carry-out bit. */
static void build_adder_table(int da, int db,
                             double *inputs, double *targets,
                             size_t *n, size_t *in_w, size_t *out_w) {
    int pow_a = 1, pow_b = 1;
    int sum_digits = (da > db ? da : db);
    int a, b, cin, d, bit;
    size_t row = 0;
    int i;

    for (i = 0; i < da; ++i) {
        pow_a *= 10;
    }
    for (i = 0; i < db; ++i) {
        pow_b *= 10;
    }
    *in_w = (size_t)(10 * (da + db) + 1);
    *out_w = (size_t)(4 * sum_digits + 1);
    *n = (size_t)pow_a * (size_t)pow_b * 2;

    for (a = 0; a < pow_a; ++a) {
        for (b = 0; b < pow_b; ++b) {
            for (cin = 0; cin < 2; ++cin) {
                double *in_row = inputs + row * *in_w;
                double *tg_row = targets + row * *out_w;
                int total = a + b + cin;
                size_t off = 0;
                int v;

                for (i = 0; i < (int)*in_w; ++i) {
                    in_row[i] = 0.0;
                }
                v = a;
                for (d = 0; d < da; ++d) {
                    in_row[off + (size_t)(v % 10)] = 1.0;
                    v /= 10;
                    off += 10;
                }
                v = b;
                for (d = 0; d < db; ++d) {
                    in_row[off + (size_t)(v % 10)] = 1.0;
                    v /= 10;
                    off += 10;
                }
                in_row[off] = (double)cin;

                off = 0;
                v = total;
                for (d = 0; d < sum_digits; ++d) {
                    int digit = v % 10;

                    v /= 10;
                    for (bit = 0; bit < 4; ++bit) {
                        tg_row[off + (size_t)bit] =
                            (double)((digit >> (3 - bit)) & 1);
                    }
                    off += 4;
                }
                tg_row[off] = (double)(v > 0 ? 1 : 0);
                ++row;
            }
        }
    }
}

static void running_add(RunningStat *s, double value) {
    s->sum += value;
    s->sumsq += value * value;
}

static double running_mean(const RunningStat *s, size_t n) {
    return (n == 0) ? 0.0 : (s->sum / (double)n);
}

static double running_std(const RunningStat *s, size_t n) {
    if (n <= 1) {
        return 0.0;
    }
    {
        double mean = running_mean(s, n);
        double var = (s->sumsq / (double)n) - (mean * mean);

        if (var < 0.0) {
            var = 0.0;
        }
        return sqrt(var);
    }
}

/* The executor's bar, per output unit (all ports here are binary):
   unambiguous AND on the right side of 0.5. */
static size_t exact_count(BinaryTransformNetwork *btn,
                         const double *inputs, const double *targets,
                         size_t n, size_t in_w, size_t out_w) {
    size_t exact = 0;
    size_t s, k;

    for (s = 0; s < n; ++s) {
        const double *raw = btn_forward(btn, inputs + s * in_w);
        const double *want = targets + s * out_w;
        int ok = 1;

        for (k = 0; k < out_w; ++k) {
            if ((raw[k] >= 0.25 && raw[k] <= 0.75) ||
                (raw[k] > 0.5) != (want[k] > 0.5)) {
                ok = 0;
                break;
            }
        }
        exact += (size_t)ok;
    }
    return exact;
}

static size_t exact_count_idx(BinaryTransformNetwork *btn,
                             const double *inputs, const double *targets,
                             size_t in_w, size_t out_w,
                             const size_t *indices, size_t n) {
    size_t s, k;
    size_t exact = 0;

    for (s = 0; s < n; ++s) {
        size_t row = indices[s];
        const double *raw = btn_forward(btn, inputs + row * in_w);
        const double *want = targets + row * out_w;
        int ok = 1;

        for (k = 0; k < out_w; ++k) {
            if ((raw[k] >= 0.25 && raw[k] <= 0.75) ||
                (raw[k] > 0.5) != (want[k] > 0.5)) {
                ok = 0;
                break;
            }
        }
        exact += (size_t)ok;
    }
    return exact;
}

static double avg_loss(BinaryTransformNetwork *btn,
                      const double *inputs, const double *targets,
                      size_t n, size_t in_w, size_t out_w) {
    double total = 0.0;
    size_t s, k;

    for (s = 0; s < n; ++s) {
        const double *raw = btn_forward(btn, inputs + s * in_w);
        const double *want = targets + s * out_w;

        for (k = 0; k < out_w; ++k) {
            double d = raw[k] - want[k];

            total += d * d;
        }
    }
    return total / (double)(n * out_w);
}

static double avg_loss_idx(BinaryTransformNetwork *btn,
                          const double *inputs, const double *targets,
                          size_t in_w, size_t out_w,
                          const size_t *indices, size_t n) {
    double total = 0.0;
    size_t s, k;

    for (s = 0; s < n; ++s) {
        size_t row = indices[s];
        const double *raw = btn_forward(btn, inputs + row * in_w);
        const double *want = targets + row * out_w;

        for (k = 0; k < out_w; ++k) {
            double d = raw[k] - want[k];

            total += d * d;
        }
    }
    return total / (double)(n * out_w);
}

static unsigned int split_lcg_next(unsigned int state) {
    return (1103515245u * state + 12345u);
}

static int make_train_test_split(size_t n, double train_fraction,
                                unsigned int split_seed, size_t *work_perm,
                                size_t *train_indices, size_t *test_indices,
                                size_t *train_n, size_t *test_n) {
    size_t i;
    size_t j;
    size_t p;
    unsigned int state;

    if (n == 0 || n > MAX_N || train_indices == NULL || test_indices == NULL ||
        work_perm == NULL || train_n == NULL || test_n == NULL ||
        train_fraction <= 0.0 || train_fraction >= 1.0) {
        return -1;
    }

    for (i = 0; i < n; ++i) {
        work_perm[i] = i;
    }

    state = split_seed;
    for (p = n - 1; p > 0; --p) {
        state = split_lcg_next(state);
        j = (size_t)(state % (unsigned int)(p + 1));
        {
            size_t tmp = work_perm[p];
            work_perm[p] = work_perm[j];
            work_perm[j] = tmp;
        }
    }

    *train_n = (size_t)((double)n * train_fraction + 0.5);
    if (*train_n == 0) {
        *train_n = 1;
    } else if (*train_n >= n) {
        *train_n = n - 1;
    }
    *test_n = n - *train_n;
    if (*test_n == 0) {
        return -1;
    }

    for (i = 0; i < *train_n; ++i) {
        train_indices[i] = work_perm[i];
    }
    for (i = 0; i < *test_n; ++i) {
        test_indices[i] = work_perm[*train_n + i];
    }
    return 0;
}

static int run_single_trial(size_t width, size_t epoch_cap,
                           double seconds_cap, unsigned int seed,
                           const double *train_inputs, size_t train_n,
                           const double *train_targets,
                           const double *inputs, const double *targets,
                           size_t in_w, size_t out_w,
                           const size_t *test_indices, size_t test_n,
                           int want_full, int want_ternary,
                           double ternary_threshold,
                           TrialResult *full_result,
                           TrialResult *ternary_result) {
    BinaryTransformNetwork btn = {0};
    size_t epochs = 0;
    size_t chunk;
    size_t best = 0;
    size_t best_at = 0;
    size_t now;
    clock_t start;
    double secs = 0.0;
    double loss;
    TrialResult local_full = {0};
    TrialResult local_ternary = {0};

    chunk = train_n <= 200 ? 2000 : (train_n <= 2000 ? 500 : 50);

    if (btn_init(&btn, in_w, out_w, width, width, 0.8, seed) != 0) {
        fprintf(stderr, "FAIL: btn_init w=%lu.\n", (unsigned long)width);
        return -1;
    }

    start = clock();
    while (epochs < epoch_cap) {
        size_t step = chunk;

        if (epochs + step > epoch_cap) {
            step = epoch_cap - epochs;
        }
        if (btn_train(&btn, train_inputs, train_targets, train_n, step) != 0) {
            fprintf(stderr, "FAIL: btn_train allocation failure\n");
            btn_free(&btn);
            return -1;
        }
        epochs += step;
        now = exact_count(&btn, train_inputs, train_targets, train_n, in_w, out_w);
        if (now > best) {
            best = now;
            best_at = epochs;
        }
        secs = (double)(clock() - start) / CLOCKS_PER_SEC;
        if (best == train_n || secs >= seconds_cap) {
            break;
        }
    }

    if (want_full) {
        if (btn_set_ternary_inference(&btn, 0, ternary_threshold) != 0) {
            fprintf(stderr, "FAIL: btn_set_ternary_inference disable\n");
            btn_free(&btn);
            return -1;
        }
        now = exact_count_idx(&btn, inputs, targets, in_w, out_w, test_indices,
                             test_n);
        loss = avg_loss_idx(&btn, inputs, targets, in_w, out_w, test_indices,
                            test_n);
        local_full.best_at = best_at;
        local_full.test_exact = (double)now;
        local_full.test_loss = loss;
        local_full.secs = secs;
    }

    if (want_ternary) {
        if (btn_set_ternary_inference(&btn, 1, ternary_threshold) != 0) {
            fprintf(stderr, "FAIL: btn_set_ternary_inference enable\n");
            btn_free(&btn);
            return -1;
        }
        now = exact_count_idx(&btn, inputs, targets, in_w, out_w, test_indices,
                             test_n);
        loss = avg_loss_idx(&btn, inputs, targets, in_w, out_w, test_indices,
                            test_n);
        local_ternary.best_at = best_at;
        local_ternary.test_exact = (double)now;
        local_ternary.test_loss = loss;
        local_ternary.secs = secs;
    }

    if (full_result) {
        *full_result = local_full;
    }
    if (ternary_result) {
        *ternary_result = local_ternary;
    }

    btn_free(&btn);
    return 0;
}

static void print_mode_row(int da, int db, size_t n, size_t width, size_t in_w,
                          size_t out_w, size_t test_n, size_t trials,
                          const char *mode_label, const ModeStats *stats) {
    double best_at_mean = running_mean(&stats->best_at, trials);
    double best_at_std = running_std(&stats->best_at, trials);
    double exact_mean = running_mean(&stats->test_exact, trials);
    double exact_std = running_std(&stats->test_exact, trials);
    double loss_mean = running_mean(&stats->test_loss, trials);
    double loss_std = running_std(&stats->test_loss, trials);
    double secs_mean = running_mean(&stats->secs, trials);
    double secs_std = running_std(&stats->secs, trials);
    char best_buf[64];
    char exact_buf[64];
    char loss_buf[64];
    char secs_buf[64];

    snprintf(best_buf, sizeof(best_buf), "%8.1f+/-%4.1f", best_at_mean,
             best_at_std);
    snprintf(exact_buf, sizeof(exact_buf), "%7.1f+/-%-6.1f/%-4lu",
             exact_mean, exact_std, (unsigned long)test_n);
    snprintf(loss_buf, sizeof(loss_buf), "%.6f+/-%.6f", loss_mean, loss_std);
    snprintf(secs_buf, sizeof(secs_buf), "%6.1f+/-%5.1f", secs_mean,
             secs_std);
    printf("| (%d,%d) | %-12s | %6lu | %4lu | %-15s | %-17s | "
           "%-14s | %-12s | %6lu |\n",
           da, db, mode_label, (unsigned long)n, (unsigned long)width,
           best_buf, exact_buf, loss_buf, secs_buf,
           (unsigned long)(in_w * width + width * out_w));
}

static int run_row(int da, int db, size_t width, size_t epoch_cap,
                   double seconds_cap, double train_fraction,
                   unsigned int seed_start, int trials, double *inputs,
                   double *targets, RunMode mode, double ternary_threshold) {
    size_t n, in_w, out_w;
    size_t train_n = 0;
    size_t test_n = 0;
    size_t *perm_indices = NULL;
    size_t *train_indices = NULL;
    size_t *test_indices = NULL;
    double *train_inputs = NULL;
    double *train_targets = NULL;
    unsigned int seed;
    ModeStats full_stats = {0};
    ModeStats ternary_stats = {0};
    int want_full = (mode != RUN_MODE_TERNARY);
    int want_ternary = (mode != RUN_MODE_FULL);
    int t_idx;

    build_adder_table(da, db, inputs, targets, &n, &in_w, &out_w);
    if (n == 0 || n > MAX_N || in_w == 0 || out_w == 0) {
        fprintf(stderr, "FAIL: build_adder_table generated invalid shape.\n");
        return -1;
    }

    perm_indices = (size_t *)malloc(sizeof(size_t) * n);
    if (perm_indices == NULL) {
        fprintf(stderr, "FAIL: malloc perm.\n");
        return -1;
    }
    train_indices = (size_t *)malloc(sizeof(size_t) * n);
    if (train_indices == NULL) {
        fprintf(stderr, "FAIL: malloc train_indices.\n");
        free(perm_indices);
        return -1;
    }
    test_indices = (size_t *)malloc(sizeof(size_t) * n);
    if (test_indices == NULL) {
        fprintf(stderr, "FAIL: malloc test_indices.\n");
        free(perm_indices);
        free(train_indices);
        return -1;
    }
    train_inputs = (double *)malloc(sizeof(double) * n * in_w);
    if (train_inputs == NULL) {
        fprintf(stderr, "FAIL: malloc train_inputs.\n");
        free(perm_indices);
        free(train_indices);
        free(test_indices);
        return -1;
    }
    train_targets = (double *)malloc(sizeof(double) * n * out_w);
    if (train_targets == NULL) {
        fprintf(stderr, "FAIL: malloc train_targets.\n");
        free(perm_indices);
        free(train_indices);
        free(test_indices);
        free(train_inputs);
        return -1;
    }

    for (t_idx = 0; t_idx < trials; ++t_idx) {
        TrialResult full = {0};
        TrialResult ternary = {0};
        size_t i;
        size_t offset;

        seed = seed_start + (unsigned int)t_idx;

        if (make_train_test_split(n, train_fraction, seed, perm_indices,
                                 train_indices, test_indices, &train_n,
                                 &test_n) != 0) {
            fprintf(stderr, "FAIL: train/test split failed.\n");
            free(perm_indices);
            free(train_indices);
            free(test_indices);
            free(train_inputs);
            free(train_targets);
            return -1;
        }
        for (i = 0; i < train_n; ++i) {
            size_t row;
            double *train_in_row;
            double *train_tg_row;
            const double *src_in_row;
            const double *src_tg_row;

            row = train_indices[i];
            offset = i * in_w;
            train_in_row = train_inputs + offset;
            src_in_row = inputs + row * in_w;
            memcpy(train_in_row, src_in_row, sizeof(double) * in_w);

            offset = i * out_w;
            train_tg_row = train_targets + offset;
            src_tg_row = targets + row * out_w;
            memcpy(train_tg_row, src_tg_row, sizeof(double) * out_w);
        }

        if (run_single_trial(width, epoch_cap, seconds_cap, seed,
                             train_inputs, train_n, train_targets, inputs, targets,
                             in_w, out_w, test_indices, test_n, want_full,
                             want_ternary, ternary_threshold, &full, &ternary) != 0) {
            free(perm_indices);
            free(train_indices);
            free(test_indices);
            free(train_inputs);
            free(train_targets);
            return -1;
        }

        if (want_full) {
            running_add(&full_stats.best_at, (double)full.best_at);
            running_add(&full_stats.test_exact, full.test_exact);
            running_add(&full_stats.test_loss, full.test_loss);
            running_add(&full_stats.secs, full.secs);
        }
        if (want_ternary) {
            running_add(&ternary_stats.best_at, (double)ternary.best_at);
            running_add(&ternary_stats.test_exact, ternary.test_exact);
            running_add(&ternary_stats.test_loss, ternary.test_loss);
            running_add(&ternary_stats.secs, ternary.secs);
        }
    }

    if (want_full) {
        print_mode_row(da, db, n, width, in_w, out_w, test_n, trials,
                       "full", &full_stats);
    }
    if (want_ternary) {
        char ternary_label[24];

        snprintf(ternary_label, sizeof(ternary_label), "ternary(%.3g)",
                 ternary_threshold);
        print_mode_row(da, db, n, width, in_w, out_w, test_n, trials,
                       ternary_label, &ternary_stats);
    }

    free(perm_indices);
    free(train_indices);
    free(test_indices);
    free(train_inputs);
    free(train_targets);
    return 0;
}

static int parse_non_negative_double(const char *text, double *value_out) {
    char *end = NULL;
    double value;

    if (text == NULL || *text == '\0') {
        return -1;
    }
    value = strtod(text, &end);
    if (end == NULL || end == text || *end != '\0' || value < 0.0) {
        return -1;
    }
    *value_out = value;
    return 0;
}

static int parse_positive_int(const char *text, int *value_out) {
    char *end = NULL;
    long value;

    if (text == NULL || *text == '\0') {
        return -1;
    }
    value = strtol(text, &end, 10);
    if (end == NULL || end == text || *end != '\0' || value <= 0 || value > 1000000) {
        return -1;
    }
    *value_out = (int)value;
    return 0;
}

static int parse_non_negative_u32(const char *text, unsigned int *value_out) {
    char *end = NULL;
    unsigned long value;

    if (text == NULL || *text == '\0') {
        return -1;
    }
    value = strtoul(text, &end, 10);
    if (end == NULL || end == text || *end != '\0') {
        return -1;
    }
    if (value > 0xFFFFFFFFu) {
        return -1;
    }
    *value_out = (unsigned int)value;
    return 0;
}

static int parse_train_fraction(const char *text, double *value_out) {
    char *end = NULL;
    double value;

    if (text == NULL || *text == '\0') {
        return -1;
    }
    value = strtod(text, &end);
    if (end == NULL || end == text || *end != '\0' || value <= 0.0 ||
        value >= 1.0) {
        return -1;
    }
    *value_out = value;
    return 0;
}

static void usage(const char *prog_name) {
    fprintf(stderr, "usage: %s [seconds-per-run] [--full|--ternary|--compare]\n",
            prog_name);
    fprintf(stderr, "       [--threshold <t>] [--trials <n>] [--seed-start <seed>]\n");
    fprintf(stderr, "       [--train-fraction <p>]\n");
    fprintf(stderr, "       defaults: cap=240s, threshold=%.3f, train_fraction=%.3f, "
            "trials=%d, seed=%u\n",
            DEFAULT_TERNARY_THRESHOLD, DEFAULT_TRAIN_FRACTION, DEFAULT_TRIALS,
            DEFAULT_SEED_START);
    fprintf(stderr, "       (non-option argument is seconds-per-run)\n");
}

int main(int argc, char **argv) {
    static double inputs[MAX_N * MAX_IN];
    static double targets[MAX_N * MAX_OUT];
    double cap = 240.0;
    RunMode mode = RUN_MODE_FULL;
    double ternary_threshold = DEFAULT_TERNARY_THRESHOLD;
    double train_fraction = DEFAULT_TRAIN_FRACTION;
    int trials = DEFAULT_TRIALS;
    unsigned int seed_start = DEFAULT_SEED_START;
    int i = 1;
    int cap_set = 0;

    for (; i < argc; ++i) {
        if (strcmp(argv[i], "--full") == 0) {
            mode = RUN_MODE_FULL;
        } else if (strcmp(argv[i], "--ternary") == 0) {
            mode = RUN_MODE_TERNARY;
        } else if (strcmp(argv[i], "--compare") == 0) {
            mode = RUN_MODE_COMPARE;
        } else if (strcmp(argv[i], "--help") == 0 ||
                   strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return EXIT_SUCCESS;
        } else if (strcmp(argv[i], "--threshold") == 0) {
            double parsed;
            if (i + 1 >= argc) {
                fprintf(stderr, "FAIL: --threshold needs a value.\n");
                usage(argv[0]);
                return EXIT_FAILURE;
            }
            if (parse_non_negative_double(argv[i + 1], &parsed) != 0) {
                fprintf(stderr, "FAIL: invalid threshold '%s'.\n", argv[i + 1]);
                return EXIT_FAILURE;
            }
            ternary_threshold = parsed;
            ++i;
        } else if (strncmp(argv[i], "--threshold=", 12) == 0) {
            double parsed;
            if (parse_non_negative_double(argv[i] + 12, &parsed) != 0) {
                fprintf(stderr, "FAIL: invalid threshold '%s'.\n",
                        argv[i] + 12);
                return EXIT_FAILURE;
            }
            ternary_threshold = parsed;
        } else if (strcmp(argv[i], "--trials") == 0) {
            int parsed;
            if (i + 1 >= argc) {
                fprintf(stderr, "FAIL: --trials needs a value.\n");
                usage(argv[0]);
                return EXIT_FAILURE;
            }
            if (parse_positive_int(argv[i + 1], &parsed) != 0) {
                fprintf(stderr, "FAIL: invalid trial count '%s'.\n",
                        argv[i + 1]);
                return EXIT_FAILURE;
            }
            trials = parsed;
            ++i;
        } else if (strncmp(argv[i], "--trials=", 9) == 0) {
            int parsed;
            if (parse_positive_int(argv[i] + 9, &parsed) != 0) {
                fprintf(stderr, "FAIL: invalid trial count '%s'.\n", argv[i] + 9);
                return EXIT_FAILURE;
            }
            trials = parsed;
        } else if (strcmp(argv[i], "--seed-start") == 0) {
            unsigned int parsed;
            if (i + 1 >= argc) {
                fprintf(stderr, "FAIL: --seed-start needs a value.\n");
                usage(argv[0]);
                return EXIT_FAILURE;
            }
            if (parse_non_negative_u32(argv[i + 1], &parsed) != 0) {
                fprintf(stderr, "FAIL: invalid seed '%s'.\n", argv[i + 1]);
                return EXIT_FAILURE;
            }
            seed_start = parsed;
            ++i;
        } else if (strncmp(argv[i], "--seed-start=", 13) == 0) {
            unsigned int parsed;
            if (parse_non_negative_u32(argv[i] + 13, &parsed) != 0) {
                fprintf(stderr, "FAIL: invalid seed '%s'.\n", argv[i] + 13);
                return EXIT_FAILURE;
            }
            seed_start = parsed;
        } else if (strcmp(argv[i], "--train-fraction") == 0) {
            double parsed;
            if (i + 1 >= argc) {
                fprintf(stderr, "FAIL: --train-fraction needs a value.\n");
                usage(argv[0]);
                return EXIT_FAILURE;
            }
            if (parse_train_fraction(argv[i + 1], &parsed) != 0) {
                fprintf(stderr, "FAIL: invalid train fraction '%s'.\n",
                        argv[i + 1]);
                return EXIT_FAILURE;
            }
            train_fraction = parsed;
            ++i;
        } else if (strncmp(argv[i], "--train-fraction=", 17) == 0) {
            double parsed;
            if (parse_train_fraction(argv[i] + 17, &parsed) != 0) {
                fprintf(stderr, "FAIL: invalid train fraction '%s'.\n",
                        argv[i] + 17);
                return EXIT_FAILURE;
            }
            train_fraction = parsed;
        } else if (strncmp(argv[i], "--", 2) == 0) {
            fprintf(stderr, "FAIL: unknown option '%s'.\n", argv[i]);
            usage(argv[0]);
            return EXIT_FAILURE;
        } else if (!cap_set && parse_non_negative_double(argv[i], &cap) == 0) {
            if (cap <= 0.0) {
                fprintf(stderr, "FAIL: seconds-per-run must be > 0.\n");
                return EXIT_FAILURE;
            }
            cap_set = 1;
        } else {
            fprintf(stderr, "FAIL: unknown argument '%s'.\n", argv[i]);
            usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    printf("capacity study: fixed-width students on the decimal-adder "
           "family\n");
    printf("(per-run cap: %.0f s base; exact counts reproduce by epoch "
           "count)\n", cap);
    printf("holdout: %.0f%% train, %.0f%% test; trials=%d; train_fraction=%.4f; "
           "seed_start=%u\n",
           train_fraction * 100.0, (1.0 - train_fraction) * 100.0, trials,
           train_fraction, seed_start);
    if (mode == RUN_MODE_FULL) {
        printf("eval mode: full precision\n");
    } else if (mode == RUN_MODE_TERNARY) {
        printf("eval mode: ternary inference, threshold %.4g\n",
               ternary_threshold);
    } else {
        printf("eval mode: comparison (full precision vs ternary inference, "
               "threshold %.4g)\n", ternary_threshold);
    }
    printf("| scale | mode         | n      | w    | epochs@best    | "
           "test exact       | final loss     | secs          | MACs   |\n");
    printf("|-------|--------------|--------|------|----------------|"
           "-----------------|----------------|---------------|--------|\n");

    if (run_row(1, 1, 64, 400000, cap, train_fraction, seed_start, trials,
                inputs, targets,
                mode, ternary_threshold) != 0 ||
        run_row(1, 1, 128, 400000, cap, train_fraction, seed_start, trials,
                inputs, targets,
                mode, ternary_threshold) != 0 ||
        run_row(2, 1, 64, 200000, cap, train_fraction, seed_start, trials,
                inputs, targets,
                mode, ternary_threshold) != 0 ||
        run_row(2, 1, 128, 200000, cap, train_fraction, seed_start, trials,
                inputs, targets,
                mode, ternary_threshold) != 0 ||
        run_row(2, 1, 256, 200000, cap, train_fraction, seed_start, trials,
                inputs, targets,
                mode, ternary_threshold) != 0 ||
        run_row(2, 2, 128, 20000, cap * 1.5, train_fraction, seed_start,
                trials, inputs, targets, mode, ternary_threshold) != 0 ||
        run_row(2, 2, 256, 20000, cap * 1.5, train_fraction, seed_start,
                trials, inputs, targets, mode, ternary_threshold) != 0) {
        return EXIT_FAILURE;
    }

    printf("\nreference: the chunked dec_add2 circuit runs 2 executions "
           "of the 21->5 unit\n");
    printf("           = 2 x (21*128 + 128*5) = 6656 MACs per addition; "
           "a flat w=256\n");
    printf("           student costs 12800 MACs -- past the crossover "
           "before it works.\n");
    return EXIT_SUCCESS;
}
