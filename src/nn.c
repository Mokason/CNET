#include "../include/nn.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Default activation-space deadzone for ternary weight quantization. */
#ifndef BTN_TERNARY_DEFAULT_THRESHOLD
#define BTN_TERNARY_DEFAULT_THRESHOLD 0.05
#endif

static int size_t_mul_overflow(size_t a, size_t b, size_t *out) {
    if (a != 0 && b > (size_t)-1 / a) {
        return -1;
    }
    *out = a * b;
    return 0;
}

static size_t port_total(Port port);
static const char *port_family_token(PortFamily family);
static PortFamily port_family_from_token(const char *token);

static double sigmoid(double x) {
    return 1.0 / (1.0 + exp(-x));
}

static double sigmoid_derivative_from_output(double y) {
    return y * (1.0 - y);
}

static double random_weight(void) {
    return ((double)rand() / (double)RAND_MAX) * 2.0 - 1.0;
}

static double ternary_quantize(double value, double threshold) {
    if (value > threshold) {
        return 1.0;
    }
    if (value < -threshold) {
        return -1.0;
    }
    return 0.0;
}

/* Weights are stored per-hidden-neuron row so the forward/backward inner
   loops over inputs walk contiguous memory. The on-disk format is unaffected:
   save/load iterate through this function. */
static size_t input_hidden_index(const NeuralNetwork *nn, size_t input, size_t hidden) {
    return hidden * nn->input_count + input;
}

static void initialize_hidden_neuron(NeuralNetwork *nn, size_t hidden) {
    size_t input;

    for (input = 0; input < nn->input_count; ++input) {
        nn->input_hidden[input_hidden_index(nn, input, hidden)] = random_weight();
    }

    nn->hidden_bias[hidden] = random_weight();
    nn->hidden_output[hidden] = 0.0;
    nn->hidden_output_weights[hidden] = random_weight();
}

static int nn_add_hidden_neuron(NeuralNetwork *nn) {
    size_t hidden;

    if (nn->hidden_count >= nn->max_hidden_count) {
        return -1;
    }

    hidden = nn->hidden_count;
    initialize_hidden_neuron(nn, hidden);
    if (hidden > 0) {
        nn->hidden_output_weights[hidden] = 0.0;
    }
    nn->hidden_count += 1;
    return 0;
}

int nn_init(
    NeuralNetwork *nn,
    size_t input_count,
    size_t initial_hidden_count,
    size_t max_hidden_count,
    double learning_rate,
    unsigned int seed
) {
    size_t input_hidden_count;
    size_t hidden;

    if (nn == NULL || input_count == 0 || initial_hidden_count == 0 ||
        initial_hidden_count > max_hidden_count || learning_rate <= 0.0) {
        return -1;
    }

    memset(nn, 0, sizeof(*nn));
    nn->input_count = input_count;
    nn->max_hidden_count = max_hidden_count;
    nn->learning_rate = learning_rate;

    if (size_t_mul_overflow(input_count, max_hidden_count, &input_hidden_count) != 0) {
        return -1;
    }

    nn->input_hidden = calloc(input_hidden_count, sizeof(double));
    nn->hidden_bias = calloc(max_hidden_count, sizeof(double));
    nn->hidden_output = calloc(max_hidden_count, sizeof(double));
    nn->hidden_output_weights = calloc(max_hidden_count, sizeof(double));

    if (nn->input_hidden == NULL || nn->hidden_bias == NULL ||
        nn->hidden_output == NULL || nn->hidden_output_weights == NULL) {
        nn_free(nn);
        return -1;
    }

    srand(seed);
    for (hidden = 0; hidden < initial_hidden_count; ++hidden) {
        if (nn_add_hidden_neuron(nn) != 0) {
            nn_free(nn);
            return -1;
        }
    }

    return 0;
}

void nn_free(NeuralNetwork *nn) {
    if (nn == NULL) {
        return;
    }

    free(nn->input_hidden);
    free(nn->hidden_bias);
    free(nn->hidden_output);
    free(nn->hidden_output_weights);
    memset(nn, 0, sizeof(*nn));
}

double nn_forward(NeuralNetwork *nn, const double *inputs) {
    size_t input;
    size_t hidden;
    double output_sum = nn->output_bias;

    for (hidden = 0; hidden < nn->hidden_count; ++hidden) {
        double hidden_sum = nn->hidden_bias[hidden];

        for (input = 0; input < nn->input_count; ++input) {
            hidden_sum += inputs[input] *
                nn->input_hidden[input_hidden_index(nn, input, hidden)];
        }

        nn->hidden_output[hidden] = sigmoid(hidden_sum);
        output_sum += nn->hidden_output[hidden] * nn->hidden_output_weights[hidden];
    }

    nn->last_output = sigmoid(output_sum);
    return nn->last_output;
}

static void nn_train_one(NeuralNetwork *nn, const double *inputs, double target) {
    size_t input;
    size_t hidden;
    double output = nn_forward(nn, inputs);
    double output_error = target - output;
    double output_delta = output_error * sigmoid_derivative_from_output(output);

    /* Single pass per hidden neuron: the backpropagated error reads the
       output-side weight BEFORE it is updated, so no saved copy (and no
       per-sample allocation) is needed. */
    for (hidden = 0; hidden < nn->hidden_count; ++hidden) {
        double hidden_error = output_delta * nn->hidden_output_weights[hidden];
        double hidden_delta =
            hidden_error * sigmoid_derivative_from_output(nn->hidden_output[hidden]);

        for (input = 0; input < nn->input_count; ++input) {
            nn->input_hidden[input_hidden_index(nn, input, hidden)] +=
                nn->learning_rate * hidden_delta * inputs[input];
        }
        nn->hidden_bias[hidden] += nn->learning_rate * hidden_delta;

        nn->hidden_output_weights[hidden] +=
            nn->learning_rate * output_delta * nn->hidden_output[hidden];
    }
    nn->output_bias += nn->learning_rate * output_delta;
}

void nn_train(
    NeuralNetwork *nn,
    const double *inputs,
    const double *targets,
    size_t sample_count,
    size_t epochs
) {
    size_t epoch;
    size_t sample;

    for (epoch = 0; epoch < epochs; ++epoch) {
        for (sample = 0; sample < sample_count; ++sample) {
            nn_train_one(
                nn,
                inputs + (sample * nn->input_count),
                targets[sample]
            );
        }
    }
}

double nn_average_loss(
    NeuralNetwork *nn,
    const double *inputs,
    const double *targets,
    size_t sample_count
) {
    size_t sample;
    double total_loss = 0.0;

    for (sample = 0; sample < sample_count; ++sample) {
        double output = nn_forward(nn, inputs + (sample * nn->input_count));
        double error = targets[sample] - output;
        total_loss += error * error;
    }

    return total_loss / (double)sample_count;
}

double nn_train_dynamic(
    NeuralNetwork *nn,
    const double *inputs,
    const double *targets,
    size_t sample_count,
    size_t max_epochs,
    size_t growth_window,
    double target_loss,
    double min_improvement
) {
    size_t epochs_done = 0;
    double previous_loss = nn_average_loss(nn, inputs, targets, sample_count);

    if (growth_window == 0) {
        growth_window = 1;
    }

    while (epochs_done < max_epochs && previous_loss > target_loss) {
        size_t epochs_to_train = growth_window;
        double current_loss;
        double improvement;
        double relative_improvement;

        if (epochs_done + epochs_to_train > max_epochs) {
            epochs_to_train = max_epochs - epochs_done;
        }

        nn_train(nn, inputs, targets, sample_count, epochs_to_train);
        epochs_done += epochs_to_train;

        current_loss = nn_average_loss(nn, inputs, targets, sample_count);
        improvement = previous_loss - current_loss;
        relative_improvement =
            previous_loss > 0.0 ? improvement / previous_loss : 0.0;

        if (current_loss <= target_loss) {
            return current_loss;
        }

        if (relative_improvement < min_improvement && nn_add_hidden_neuron(nn) == 0) {
            previous_loss = nn_average_loss(nn, inputs, targets, sample_count);
        } else {
            previous_loss = current_loss;
        }
    }

    return previous_loss;
}

int nn_train_dynamic_checked(
    NeuralNetwork *nn,
    const double *inputs,
    const double *targets,
    size_t sample_count,
    size_t max_epochs,
    size_t growth_window,
    double target_loss,
    double min_improvement,
    double *loss_out
) {
    double loss;

    if (loss_out != NULL) *loss_out = BTN_TRAIN_LOSS_FAILED;
    if (nn == NULL || inputs == NULL || targets == NULL || sample_count == 0) {
        return BTN_TRAIN_INVALID;
    }
    (void)nn_train_dynamic(nn, inputs, targets, sample_count, max_epochs,
                           growth_window, target_loss, min_improvement);
    /* Decide the status from a fresh PUBLIC recomputation over the net
       actually handed back. nn_train_dynamic returns `previous_loss`,
       which on the budget-exhausted path is a measurement taken before
       the last neuron was added -- a number describing a net that no
       longer exists. A caller can reproduce THIS one exactly. */
    loss = nn_average_loss(nn, inputs, targets, sample_count);
    if (loss_out != NULL) *loss_out = loss;
    if (!isfinite(loss) || loss < 0.0) {
        if (loss_out != NULL) *loss_out = BTN_TRAIN_LOSS_FAILED;
        return BTN_TRAIN_INVALID;
    }
    /* SUCCESS means one thing here too: the run reached the target it was
       given. Running out of epochs while still descending is not success,
       however healthy the number beside it looks. */
    return loss <= target_loss ? BTN_TRAIN_OK : BTN_TRAIN_PLATEAU_STATUS;
}

int nn_predict_hex_digit(NeuralNetwork *nn, const double bits[4], char *digit) {
    static const char hex_digits[] = "0123456789ABCDEF";
    int value;
    double output;

    if (nn == NULL || bits == NULL || digit == NULL || nn->input_count != 4) {
        return -1;
    }

    output = nn_forward(nn, bits);
    value = (int)floor(output * 16.0);
    if (value < 0) {
        value = 0;
    }
    if (value > 15) {
        value = 15;
    }

    *digit = hex_digits[value];
    return 0;
}

int nn_predict_hex_string(
    NeuralNetwork *nn,
    const char *binary,
    char *hex_output,
    size_t hex_output_size
) {
    size_t binary_len;
    size_t chunk_count;
    size_t padded_len;
    size_t pad_bits;
    size_t chunk;

    if (nn == NULL || binary == NULL || hex_output == NULL || hex_output_size == 0) {
        return -1;
    }

    binary_len = strlen(binary);
    if (binary_len == 0) {
        return -1;
    }

    chunk_count = (binary_len + 3) / 4;
    padded_len = chunk_count * 4;
    pad_bits = padded_len - binary_len;

    if (hex_output_size < chunk_count + 1) {
        return -1;
    }

    for (chunk = 0; chunk < chunk_count; ++chunk) {
        double bits[4];
        size_t bit;

        for (bit = 0; bit < 4; ++bit) {
            size_t padded_index = chunk * 4 + bit;

            if (padded_index < pad_bits) {
                bits[bit] = 0.0;
            } else {
                char c = binary[padded_index - pad_bits];

                if (c != '0' && c != '1') {
                    return -1;
                }

                bits[bit] = c == '1' ? 1.0 : 0.0;
            }
        }

        if (nn_predict_hex_digit(nn, bits, &hex_output[chunk]) != 0) {
            return -1;
        }
    }

    hex_output[chunk_count] = '\0';
    return 0;
}

static int hex_value(char c, int *value) {
    if (c >= '0' && c <= '9') {
        *value = c - '0';
        return 0;
    }
    if (c >= 'A' && c <= 'F') {
        *value = c - 'A' + 10;
        return 0;
    }
    if (c >= 'a' && c <= 'f') {
        *value = c - 'a' + 10;
        return 0;
    }
    return -1;
}

int nn_predict_ascii_char(NeuralNetwork *nn, char high_hex, char low_hex, char *ch) {
    int high;
    int low;
    int printable_index;
    double inputs[2];
    double output;

    if (nn == NULL || ch == NULL || nn->input_count != 2) {
        return -1;
    }
    if (hex_value(high_hex, &high) != 0 || hex_value(low_hex, &low) != 0) {
        return -1;
    }

    inputs[0] = (double)high / 15.0;
    inputs[1] = (double)low / 15.0;
    output = nn_forward(nn, inputs);

    printable_index = (int)floor(output * 95.0);
    if (printable_index < 0) {
        printable_index = 0;
    }
    if (printable_index > 94) {
        printable_index = 94;
    }

    *ch = (char)(printable_index + 32);
    return 0;
}

int nn_predict_ascii_string_from_hex(
    NeuralNetwork *nn,
    const char *hex,
    char *text_output,
    size_t text_output_size
) {
    size_t hex_len;
    size_t text_len;
    size_t index;

    if (nn == NULL || hex == NULL || text_output == NULL || text_output_size == 0) {
        return -1;
    }

    hex_len = strlen(hex);
    if (hex_len == 0 || hex_len % 2 != 0) {
        return -1;
    }

    text_len = hex_len / 2;
    if (text_output_size < text_len + 1) {
        return -1;
    }

    for (index = 0; index < text_len; ++index) {
        if (nn_predict_ascii_char(
                nn,
                hex[index * 2],
                hex[index * 2 + 1],
                &text_output[index]
            ) != 0) {
            return -1;
        }
    }

    text_output[text_len] = '\0';
    return 0;
}

int nn_predict_text_from_binary(
    NeuralNetwork *nibble_nn,
    NeuralNetwork *char_nn,
    const char *binary,
    char *text_output,
    size_t text_output_size
) {
    size_t binary_len;
    size_t hex_len;
    char *hex_output;
    int result;

    if (nibble_nn == NULL || char_nn == NULL || binary == NULL ||
        text_output == NULL || text_output_size == 0) {
        return -1;
    }

    binary_len = strlen(binary);
    if (binary_len == 0 || binary_len % 8 != 0) {
        return -1;
    }

    hex_len = binary_len / 4;
    hex_output = malloc(hex_len + 1);
    if (hex_output == NULL) {
        return -1;
    }

    result = nn_predict_hex_string(nibble_nn, binary, hex_output, hex_len + 1);
    if (result == 0) {
        result = nn_predict_ascii_string_from_hex(
            char_nn,
            hex_output,
            text_output,
            text_output_size
        );
    }

    free(hex_output);
    return result;
}

int nn_save(const NeuralNetwork *nn, const char *path) {
    FILE *file;
    size_t input;
    size_t hidden;

    if (nn == NULL || path == NULL) {
        return -1;
    }

    file = fopen(path, "w");
    if (file == NULL) {
        return -1;
    }

    fprintf(file, "CNET_NN 1\n");
    fprintf(file, "%lu %lu %lu %.17g %.17g\n",
            (unsigned long)nn->input_count,
            (unsigned long)nn->hidden_count,
            (unsigned long)nn->max_hidden_count,
            nn->learning_rate,
            nn->output_bias);

    for (hidden = 0; hidden < nn->hidden_count; ++hidden) {
        fprintf(file, "%.17g %.17g\n",
                nn->hidden_bias[hidden],
                nn->hidden_output_weights[hidden]);
    }

    for (input = 0; input < nn->input_count; ++input) {
        for (hidden = 0; hidden < nn->hidden_count; ++hidden) {
            fprintf(file, "%.17g\n",
                    nn->input_hidden[input_hidden_index(nn, input, hidden)]);
        }
    }

    if (fclose(file) != 0) {
        return -1;
    }

    return 0;
}

int nn_load(NeuralNetwork *nn, const char *path) {
    FILE *file;
    char magic[16];
    int version;
    unsigned long input_count;
    unsigned long hidden_count;
    unsigned long max_hidden_count;
    double learning_rate;
    double output_bias;
    size_t input;
    size_t hidden;

    if (nn == NULL || path == NULL) {
        return -1;
    }

    file = fopen(path, "r");
    if (file == NULL) {
        return -1;
    }

    if (fscanf(file, "%15s %d", magic, &version) != 2 ||
        strcmp(magic, "CNET_NN") != 0 || version != 1) {
        fclose(file);
        return -1;
    }

    if (fscanf(file, "%lu %lu %lu %lf %lf",
               &input_count,
               &hidden_count,
               &max_hidden_count,
               &learning_rate,
               &output_bias) != 5) {
        fclose(file);
        return -1;
    }

    if (nn_init(
            nn,
            (size_t)input_count,
            (size_t)hidden_count,
            (size_t)max_hidden_count,
            learning_rate,
            1u
        ) != 0) {
        fclose(file);
        return -1;
    }

    nn->output_bias = output_bias;

    for (hidden = 0; hidden < nn->hidden_count; ++hidden) {
        if (fscanf(file, "%lf %lf",
                   &nn->hidden_bias[hidden],
                   &nn->hidden_output_weights[hidden]) != 2) {
            nn_free(nn);
            fclose(file);
            return -1;
        }
    }

    for (input = 0; input < nn->input_count; ++input) {
        for (hidden = 0; hidden < nn->hidden_count; ++hidden) {
            if (fscanf(
                    file,
                    "%lf",
                    &nn->input_hidden[input_hidden_index(nn, input, hidden)]
                ) != 1) {
                nn_free(nn);
                fclose(file);
                return -1;
            }
        }
    }

    fclose(file);
    return 0;
}

/* Layouts are chosen so every hot inner loop walks contiguous memory:
   input weights per-hidden-neuron row (forward sums over inputs), output
   weights per-output row (forward sums over hidden). The on-disk format is
   unaffected: save/load iterate through these functions. */
static size_t btn_input_hidden_index(
    const BinaryTransformNetwork *btn,
    size_t input,
    size_t hidden
) {
    return hidden * btn->input_count + input;
}

static size_t btn_hidden_output_index(
    const BinaryTransformNetwork *btn,
    size_t hidden,
    size_t output
) {
    return output * btn->max_hidden_count + hidden;
}

static void btn_initialize_hidden_neuron(BinaryTransformNetwork *btn, size_t hidden) {
    size_t input;
    size_t output;

    for (input = 0; input < btn->input_count; ++input) {
        btn->input_hidden[btn_input_hidden_index(btn, input, hidden)] = random_weight();
    }

    btn->hidden_bias[hidden] = random_weight();
    btn->hidden_output[hidden] = 0.0;

    for (output = 0; output < btn->output_count; ++output) {
        btn->hidden_output_weights[btn_hidden_output_index(btn, hidden, output)] =
            random_weight();
    }
}

/* Scale of a newly grown neuron's output weights. Small enough not to
   disturb the function learned so far, non-zero so the neuron has a
   gradient path at all (see below). */
#define BTN_NEW_NEURON_OUTPUT_SCALE 0.01

static int btn_add_hidden_neuron(BinaryTransformNetwork *btn) {
    size_t hidden;
    size_t output;

    if (btn->hidden_count >= btn->max_hidden_count) {
        return -1;
    }

    hidden = btn->hidden_count;
    btn_initialize_hidden_neuron(btn, hidden);
    if (hidden > 0) {
        /* ROOT CAUSE of the growth plateau, and the reason `combine`
           shipped weights that failed certification.

           These output weights used to be set to exactly 0.0, so that
           adding a neuron would not disturb the function learned so far.
           It also made the neuron useless: the gradient reaching its
           INPUT weights is

               hidden_errors[h] = SUM_o output_deltas[o] * W_ho[h][o]

           which is identically zero while every W_ho[h][o] is zero. The
           new neuron's input weights therefore could not move until its
           output weights had crawled off zero on their own, and stacking
           more neurons behind it could not help. Measured: raising the
           ceiling 64 -> 256 added 49 neurons and left the loss
           BIT-IDENTICAL at 0.008750.

           A small non-zero value restores the gradient path immediately
           while keeping the perturbation to the learned function two
           orders of magnitude below an ordinary weight, which is what
           zeroing was protecting. */
        for (output = 0; output < btn->output_count; ++output) {
            btn->hidden_output_weights[btn_hidden_output_index(btn, hidden, output)] =
                random_weight() * BTN_NEW_NEURON_OUTPUT_SCALE;
        }
    }
    btn->hidden_count += 1;
    return 0;
}

int btn_init(
    BinaryTransformNetwork *btn,
    size_t input_count,
    size_t output_count,
    size_t initial_hidden_count,
    size_t max_hidden_count,
    double learning_rate,
    unsigned int seed
) {
    size_t input_hidden_count;
    size_t hidden_output_count;
    size_t hidden;

    if (btn == NULL || input_count == 0 || output_count == 0 ||
        initial_hidden_count == 0 || initial_hidden_count > max_hidden_count ||
        learning_rate <= 0.0) {
        return -1;
    }

    memset(btn, 0, sizeof(*btn));
    btn->input_count = input_count;
    btn->output_count = output_count;
    btn->max_hidden_count = max_hidden_count;
    btn->learning_rate = learning_rate;
    btn->momentum = 0.0;   /* plain SGD until btn_set_momentum */
    btn->ternary_inference = 0;
    btn->ternary_threshold = BTN_TERNARY_DEFAULT_THRESHOLD;

    /* Default to single untyped (RAW) ports spanning the whole input and
       output. */
    btn->input_ports[0].family = PORT_RAW;
    btn->input_ports[0].field_width = input_count;
    btn->input_ports[0].field_count = 1;
    btn->input_port_count = 1;
    btn->output_ports[0].family = PORT_RAW;
    btn->output_ports[0].field_width = output_count;
    btn->output_ports[0].field_count = 1;
    btn->output_port_count = 1;

    if (size_t_mul_overflow(input_count, max_hidden_count, &input_hidden_count) != 0 ||
        size_t_mul_overflow(max_hidden_count, output_count, &hidden_output_count) != 0) {
        return -1;
    }

    btn->input_hidden = calloc(input_hidden_count, sizeof(double));
    btn->hidden_bias = calloc(max_hidden_count, sizeof(double));
    btn->hidden_output = calloc(max_hidden_count, sizeof(double));
    btn->hidden_output_weights = calloc(hidden_output_count, sizeof(double));
    btn->output_bias = calloc(output_count, sizeof(double));
    btn->last_output = calloc(output_count, sizeof(double));

    if (btn->input_hidden == NULL || btn->hidden_bias == NULL ||
        btn->hidden_output == NULL || btn->hidden_output_weights == NULL ||
        btn->output_bias == NULL || btn->last_output == NULL) {
        btn_free(btn);
        return -1;
    }

    srand(seed);
    for (hidden = 0; hidden < initial_hidden_count; ++hidden) {
        if (btn_add_hidden_neuron(btn) != 0) {
            btn_free(btn);
            return -1;
        }
    }

    return 0;
}

int btn_init_adapter(
    BinaryTransformNetwork *btn,
    size_t input_count,
    size_t output_count,
    const Port *input_ports,
    size_t input_port_count,
    const Port *output_ports,
    size_t output_port_count,
    BtnAdapterForwardFn forward,
    BtnAdapterReleaseFn release,
    void *context,
    unsigned long long behavior_digest,
    size_t cost_hint
) {
    if (btn == NULL || input_count == 0 || output_count == 0 ||
        input_ports == NULL || output_ports == NULL || forward == NULL ||
        behavior_digest == 0) {
        return -1;
    }

    memset(btn, 0, sizeof *btn);
    btn->input_count = input_count;
    btn->output_count = output_count;
    btn->last_output = calloc(output_count, sizeof *btn->last_output);
    if (btn->last_output == NULL ||
        btn_set_io_ports(btn, input_ports, input_port_count,
                         output_ports, output_port_count) != 0) {
        free(btn->last_output);
        memset(btn, 0, sizeof *btn);
        return -1;
    }

    btn->adapter_forward = forward;
    btn->adapter_release = release;
    btn->adapter_context = context;
    btn->adapter_digest = behavior_digest;
    btn->adapter_cost = cost_hint;
    return 0;
}

int btn_is_adapter(const BinaryTransformNetwork *btn) {
    return btn != NULL && btn->adapter_forward != NULL;
}

void btn_free(BinaryTransformNetwork *btn) {
    if (btn == NULL) {
        return;
    }

    if (btn->adapter_release != NULL && btn->adapter_context != NULL) {
        BtnAdapterReleaseFn release = btn->adapter_release;
        void *context = btn->adapter_context;
        btn->adapter_release = NULL;
        btn->adapter_context = NULL;
        release(context);
    }
    free(btn->input_hidden);
    free(btn->hidden_bias);
    free(btn->hidden_output);
    free(btn->hidden_output_weights);
    free(btn->output_bias);
    free(btn->last_output);
    memset(btn, 0, sizeof(*btn));
}

const double *btn_forward(BinaryTransformNetwork *btn, const double *inputs) {
    size_t input;
    size_t hidden;
    size_t output;
    double bias_threshold = btn->ternary_threshold;
    int use_ternary = btn->ternary_inference ? 1 : 0;

    if (btn->adapter_forward != NULL) {
        if (btn->last_output == NULL ||
            btn->adapter_forward(btn->adapter_context,
                                 inputs, btn->input_count,
                                 btn->last_output, btn->output_count) != 0) {
            return NULL;
        }
        return btn->last_output;
    }

    for (hidden = 0; hidden < btn->hidden_count; ++hidden) {
        double hidden_sum = use_ternary ? ternary_quantize(btn->hidden_bias[hidden], bias_threshold) : btn->hidden_bias[hidden];

        for (input = 0; input < btn->input_count; ++input) {
            double w = btn->input_hidden[btn_input_hidden_index(btn, input, hidden)];
            hidden_sum += inputs[input] * (use_ternary ? ternary_quantize(w, bias_threshold) : w);
        }

        btn->hidden_output[hidden] = sigmoid(hidden_sum);
    }

    for (output = 0; output < btn->output_count; ++output) {
        double output_sum = use_ternary ? ternary_quantize(btn->output_bias[output], bias_threshold) : btn->output_bias[output];

        for (hidden = 0; hidden < btn->hidden_count; ++hidden) {
            double w = btn->hidden_output_weights[
                btn_hidden_output_index(btn, hidden, output)];
            output_sum += btn->hidden_output[hidden] * (use_ternary ? ternary_quantize(w, bias_threshold) : w);
        }

        btn->last_output[output] = sigmoid(output_sum);
    }

    return btn->last_output;
}

static const double *btn_forward_full_precision(
    BinaryTransformNetwork *btn,
    const double *inputs
) {
    size_t input;
    size_t hidden;
    size_t output;

    for (hidden = 0; hidden < btn->hidden_count; ++hidden) {
        double hidden_sum = btn->hidden_bias[hidden];

        for (input = 0; input < btn->input_count; ++input) {
            hidden_sum += inputs[input] *
                btn->input_hidden[btn_input_hidden_index(btn, input, hidden)];
        }

        btn->hidden_output[hidden] = sigmoid(hidden_sum);
    }

    for (output = 0; output < btn->output_count; ++output) {
        double output_sum = btn->output_bias[output];

        for (hidden = 0; hidden < btn->hidden_count; ++hidden) {
            output_sum += btn->hidden_output[hidden] *
                btn->hidden_output_weights[btn_hidden_output_index(btn, hidden, output)];
        }

        btn->last_output[output] = sigmoid(output_sum);
    }

    return btn->last_output;
}

int btn_set_ternary_inference(BinaryTransformNetwork *btn, int enabled, double threshold) {
    if (btn == NULL || btn_is_adapter(btn) || threshold < 0.0) {
        return -1;
    }

    btn->ternary_inference = enabled ? 1 : 0;
    btn->ternary_threshold = threshold;
    return 0;
}

static void btn_train_one(
    BinaryTransformNetwork *btn,
    const double *inputs,
    const double *targets,
    double *output_deltas,  /* caller scratch, >= output_count */
    double *hidden_errors   /* caller scratch, >= hidden_count */
) {
    size_t input;
    size_t hidden;
    size_t output;
    const double *outputs = btn_forward_full_precision(btn, inputs);

    for (output = 0; output < btn->output_count; ++output) {
        double error = targets[output] - outputs[output];

        output_deltas[output] =
            error * sigmoid_derivative_from_output(outputs[output]);
    }

    /* Backpropagate through the output weights BEFORE updating them, so no
       saved copy of the weight matrix is needed. */
    for (hidden = 0; hidden < btn->hidden_count; ++hidden) {
        hidden_errors[hidden] = 0.0;
    }
    for (output = 0; output < btn->output_count; ++output) {
        for (hidden = 0; hidden < btn->hidden_count; ++hidden) {
            hidden_errors[hidden] += output_deltas[output] *
                btn->hidden_output_weights[btn_hidden_output_index(btn, hidden, output)];
        }
    }

    for (output = 0; output < btn->output_count; ++output) {
        for (hidden = 0; hidden < btn->hidden_count; ++hidden) {
            btn->hidden_output_weights[btn_hidden_output_index(btn, hidden, output)] +=
                btn->learning_rate * output_deltas[output] * btn->hidden_output[hidden];
        }

        btn->output_bias[output] += btn->learning_rate * output_deltas[output];
    }

    for (hidden = 0; hidden < btn->hidden_count; ++hidden) {
        double hidden_delta =
            hidden_errors[hidden] *
            sigmoid_derivative_from_output(btn->hidden_output[hidden]);

        for (input = 0; input < btn->input_count; ++input) {
            btn->input_hidden[btn_input_hidden_index(btn, input, hidden)] +=
                btn->learning_rate * hidden_delta * inputs[input];
        }

        btn->hidden_bias[hidden] += btn->learning_rate * hidden_delta;
    }
}

int btn_train(
    BinaryTransformNetwork *btn,
    const double *inputs,
    const double *targets,
    size_t sample_count,
    size_t epochs
) {
    size_t epoch;
    size_t sample;
    size_t output_bytes = 0;
    size_t hidden_bytes = 0;
    double *output_deltas = NULL;
    double *hidden_errors = NULL;

    if (btn == NULL || btn_is_adapter(btn) || inputs == NULL || targets == NULL) {
        return -1;
    }
    if (size_t_mul_overflow(btn->output_count, sizeof(*output_deltas), &output_bytes) !=
            0 ||
        size_t_mul_overflow(btn->max_hidden_count, sizeof(*hidden_errors),
                            &hidden_bytes) != 0) {
        return -1;
    }

    output_deltas = malloc(output_bytes);
    hidden_errors = malloc(hidden_bytes);

    if (output_deltas == NULL || hidden_errors == NULL) {
        free(output_deltas);
        free(hidden_errors);
        return -1;
    }

    for (epoch = 0; epoch < epochs; ++epoch) {
        for (sample = 0; sample < sample_count; ++sample) {
            btn_train_one(
                btn,
                inputs + (sample * btn->input_count),
                targets + (sample * btn->output_count),
                output_deltas,
                hidden_errors
            );
        }
    }

    free(output_deltas);
    free(hidden_errors);
    return 0;
}

int btn_set_momentum(BinaryTransformNetwork *btn, double momentum) {
    if (btn == NULL || btn_is_adapter(btn)) return -1;
    if (momentum < 0.0) momentum = 0.0;
    if (momentum > 0.999) momentum = 0.999;
    btn->momentum = momentum;
    return 0;
}

/* ---- CNET_TRAIN_FAST=1 (default OFF): faster plain-SGD training step -----
   Opt-in acceleration of btn_train_dynamic's hot loop (the gap lane's ~100 s
   per-gap teach is ~90% this loop's dense hidden<->output work). Three
   mechanical changes, none of which alters what any floating-point
   accumulator sums or in what order it sums it:
     1. Row blocking: the two forward reductions walk 4 independent output
        elements per pass — 4 independent dependency chains instead of one
        latency-bound chain; each element still receives its terms in the
        same ascending index order as the plain loops.
     2. Exact-zero input skip: exemplar rows are one-hot in the lane, and a
        skipped term is an exact +/-0.0 addition. Under round-to-nearest an
        accumulator seeded from a nonzero bias/weight can never be -0.0 (a
        sum with a nonzero term only rounds to +0.0), and x + (+/-0.0) == x
        for every x except -0.0, so dropping the terms is byte-identical.
     3. OpenMP worksharing across INDEPENDENT elements (rows of the weight
        matrices) in _OPENMP builds only: disjoint writes, static schedule,
        each element's chain stays whole inside one thread — results are
        independent of thread count.
   Byte-identity of the trained weights against the knob-off path is an
   executable gate (`make teach_fast`). Momentum runs (a CLOSED negative;
   btn->momentum != 0) always take the plain path. */
static int btn_train_fast_on(void) {
    const char *e = getenv("CNET_TRAIN_FAST");
    return e != NULL && e[0] == '1';
}

static const double *btn_forward_fast(BinaryTransformNetwork *btn,
                                      const double *inputs) {
    const size_t hidden_count = btn->hidden_count;
    const size_t input_count = btn->input_count;
    const size_t output_count = btn->output_count;
    const size_t stride = btn->max_hidden_count;
    const long hidden_blocks = (long)((hidden_count + 3) / 4);
    const long output_blocks = (long)((output_count + 3) / 4);
    long block;

#ifdef _OPENMP
#pragma omp parallel for schedule(static) default(none) \
    shared(btn, inputs, hidden_count, input_count, hidden_blocks)
#endif
    for (block = 0; block < hidden_blocks; ++block) {
        size_t h0 = (size_t)block * 4;
        size_t rows = hidden_count - h0 < 4 ? hidden_count - h0 : 4;
        const double *w0 = btn->input_hidden + h0 * input_count;
        double acc[4];
        size_t r, i;
        for (r = 0; r < rows; ++r) {
            acc[r] = btn->hidden_bias[h0 + r];
        }
        if (rows == 4) {
            const double *w1 = w0 + input_count;
            const double *w2 = w1 + input_count;
            const double *w3 = w2 + input_count;
            for (i = 0; i < input_count; ++i) {
                double x = inputs[i];
                if (x == 0.0) continue;
                acc[0] += x * w0[i];
                acc[1] += x * w1[i];
                acc[2] += x * w2[i];
                acc[3] += x * w3[i];
            }
        } else {
            for (i = 0; i < input_count; ++i) {
                double x = inputs[i];
                if (x == 0.0) continue;
                for (r = 0; r < rows; ++r) {
                    acc[r] += x * w0[r * input_count + i];
                }
            }
        }
        for (r = 0; r < rows; ++r) {
            btn->hidden_output[h0 + r] = sigmoid(acc[r]);
        }
    }

#ifdef _OPENMP
#pragma omp parallel for schedule(static) default(none) \
    shared(btn, hidden_count, output_count, stride, output_blocks)
#endif
    for (block = 0; block < output_blocks; ++block) {
        size_t o0 = (size_t)block * 4;
        size_t rows = output_count - o0 < 4 ? output_count - o0 : 4;
        const double *w0 = btn->hidden_output_weights + o0 * stride;
        const double *hout = btn->hidden_output;
        double acc[4];
        size_t r, h;
        for (r = 0; r < rows; ++r) {
            acc[r] = btn->output_bias[o0 + r];
        }
        if (rows == 4) {
            const double *w1 = w0 + stride;
            const double *w2 = w1 + stride;
            const double *w3 = w2 + stride;
            for (h = 0; h < hidden_count; ++h) {
                double x = hout[h];
                acc[0] += x * w0[h];
                acc[1] += x * w1[h];
                acc[2] += x * w2[h];
                acc[3] += x * w3[h];
            }
        } else {
            for (h = 0; h < hidden_count; ++h) {
                double x = hout[h];
                for (r = 0; r < rows; ++r) {
                    acc[r] += x * w0[r * stride + h];
                }
            }
        }
        for (r = 0; r < rows; ++r) {
            btn->last_output[o0 + r] = sigmoid(acc[r]);
        }
    }

    return btn->last_output;
}

/* One plain-SGD epoch (every sample: forward + backprop + update), same
   arithmetic and same per-accumulator order as the momentum==0 branch of
   the plain loop below. hidden_errors[h] accumulates in ascending output
   order there too (o-outer/h-inner feeds each h once per o), so the
   h-chunked loop here sums the identical sequence. The forward is inlined
   rather than calling btn_forward_fast because the whole epoch runs inside
   ONE parallel region: per-sample fork/join costs more than the small
   phases themselves (measured 8-thread regression), while the per-phase
   `omp for` barriers below are cheap. Every phase workshares across
   disjoint elements, so results are independent of thread count. */
static void btn_train_fast_epoch(BinaryTransformNetwork *btn,
                                 const double *inputs,
                                 const double *targets,
                                 size_t sample_count,
                                 double adaptive_lr,
                                 double *output_deltas,
                                 double *hidden_errors) {
    const size_t hidden_count = btn->hidden_count;
    const size_t input_count = btn->input_count;
    const size_t output_count = btn->output_count;
    const size_t stride = btn->max_hidden_count;
    const long hidden_blocks = (long)((hidden_count + 3) / 4);
    const long output_blocks = (long)((output_count + 3) / 4);
    const long hidden_chunks = (long)((hidden_count + 63) / 64);

#ifdef _OPENMP
#pragma omp parallel default(none) \
    shared(btn, inputs, targets, sample_count, adaptive_lr, output_deltas, \
           hidden_errors, hidden_count, input_count, output_count, stride, \
           hidden_blocks, output_blocks, hidden_chunks)
#endif
    {
        size_t sample;
        long block;

        for (sample = 0; sample < sample_count; ++sample) {
            const double *in = inputs + sample * input_count;
            const double *tg = targets + sample * output_count;

            /* 1. forward input->hidden (blocked chains, zero-skip) */
#ifdef _OPENMP
#pragma omp for schedule(static)
#endif
            for (block = 0; block < hidden_blocks; ++block) {
                size_t h0 = (size_t)block * 4;
                size_t rows = hidden_count - h0 < 4 ? hidden_count - h0 : 4;
                const double *w0 = btn->input_hidden + h0 * input_count;
                double acc[4];
                size_t r, i;
                for (r = 0; r < rows; ++r) {
                    acc[r] = btn->hidden_bias[h0 + r];
                }
                if (rows == 4) {
                    const double *w1 = w0 + input_count;
                    const double *w2 = w1 + input_count;
                    const double *w3 = w2 + input_count;
                    for (i = 0; i < input_count; ++i) {
                        double x = in[i];
                        if (x == 0.0) continue;
                        acc[0] += x * w0[i];
                        acc[1] += x * w1[i];
                        acc[2] += x * w2[i];
                        acc[3] += x * w3[i];
                    }
                } else {
                    for (i = 0; i < input_count; ++i) {
                        double x = in[i];
                        if (x == 0.0) continue;
                        for (r = 0; r < rows; ++r) {
                            acc[r] += x * w0[r * input_count + i];
                        }
                    }
                }
                for (r = 0; r < rows; ++r) {
                    btn->hidden_output[h0 + r] = sigmoid(acc[r]);
                }
            }

            /* 2. forward hidden->output, fused with the output deltas
               (deltas are elementwise in the forward's own output) */
#ifdef _OPENMP
#pragma omp for schedule(static)
#endif
            for (block = 0; block < output_blocks; ++block) {
                size_t o0 = (size_t)block * 4;
                size_t rows = output_count - o0 < 4 ? output_count - o0 : 4;
                const double *w0 = btn->hidden_output_weights + o0 * stride;
                const double *hout = btn->hidden_output;
                double acc[4];
                size_t r, h;
                for (r = 0; r < rows; ++r) {
                    acc[r] = btn->output_bias[o0 + r];
                }
                if (rows == 4) {
                    const double *w1 = w0 + stride;
                    const double *w2 = w1 + stride;
                    const double *w3 = w2 + stride;
                    for (h = 0; h < hidden_count; ++h) {
                        double x = hout[h];
                        acc[0] += x * w0[h];
                        acc[1] += x * w1[h];
                        acc[2] += x * w2[h];
                        acc[3] += x * w3[h];
                    }
                } else {
                    for (h = 0; h < hidden_count; ++h) {
                        double x = hout[h];
                        for (r = 0; r < rows; ++r) {
                            acc[r] += x * w0[r * stride + h];
                        }
                    }
                }
                for (r = 0; r < rows; ++r) {
                    double out = sigmoid(acc[r]);
                    double error = tg[o0 + r] - out;
                    btn->last_output[o0 + r] = out;
                    output_deltas[o0 + r] =
                        error * sigmoid_derivative_from_output(out);
                }
            }

            /* 3. hidden_errors backprop (reads the pre-update weights) */
#ifdef _OPENMP
#pragma omp for schedule(static)
#endif
            for (block = 0; block < hidden_chunks; ++block) {
                size_t h0 = (size_t)block * 64;
                size_t hn = hidden_count - h0 < 64 ? hidden_count - h0 : 64;
                double *he = hidden_errors + h0;
                size_t o, h;
                for (h = 0; h < hn; ++h) {
                    he[h] = 0.0;
                }
                for (o = 0; o < output_count; ++o) {
                    const double d = output_deltas[o];
                    const double *row =
                        btn->hidden_output_weights + o * stride + h0;
                    for (h = 0; h < hn; ++h) {
                        he[h] += d * row[h];
                    }
                }
            }

            /* 4a. hidden->output update; nowait: 4b touches disjoint
               arrays, and 4b's barrier covers both before the next
               sample's forward reads any weight */
#ifdef _OPENMP
#pragma omp for schedule(static) nowait
#endif
            for (block = 0; block < (long)output_count; ++block) {
                const double d = output_deltas[block];
                const double *hout = btn->hidden_output;
                double *row =
                    btn->hidden_output_weights + (size_t)block * stride;
                size_t h;
                for (h = 0; h < hidden_count; ++h) {
                    row[h] += adaptive_lr * (d * hout[h]);
                }
                btn->output_bias[block] += adaptive_lr * d;
            }

            /* 4b. input->hidden update (zero-skip) */
#ifdef _OPENMP
#pragma omp for schedule(static)
#endif
            for (block = 0; block < (long)hidden_count; ++block) {
                double hidden_delta =
                    hidden_errors[block] *
                    sigmoid_derivative_from_output(btn->hidden_output[block]);
                double *row =
                    btn->input_hidden + (size_t)block * input_count;
                size_t i;
                btn->hidden_bias[block] += adaptive_lr * hidden_delta;
                for (i = 0; i < input_count; ++i) {
                    double x = in[i];
                    if (x == 0.0) continue;
                    row[i] += adaptive_lr * (hidden_delta * x);
                }
            }
        }
    }
}

/* ---- plateau escape ------------------------------------------------
 *
 * Dynamic growth had exactly one lever -- add a neuron -- and a net that
 * has settled into a local minimum does not leave it because capacity
 * appeared. Measured on the `combine` primitive: raising the ceiling from
 * 64 to 256 let the trainer add 49 more neurons and left the loss
 * BIT-IDENTICAL at 0.008750, certifying 240/256, while seed 123u reached
 * 0.002188 and certified 256/256. Capacity was never the constraint.
 *
 * So when growth is exhausted and a window buys nothing, perturb the
 * weights and keep going. The best net ever seen is snapshotted and
 * restored at the end, so a perturbation can never make the returned net
 * worse than the one it started from -- which is what makes this an escape
 * rather than a gamble. Perturbation draws from the same rand() stream the
 * caller seeded, so a run stays reproducible for a given seed.
 */
#define BTN_TRAIN_STUCK_WINDOWS 64
/* Consecutive windows that bought less than min_improvement WHILE a
   neuron was still being added each time. Past this, growth is not the
   answer and the escape engages even though the ceiling is not reached. */
#define BTN_TRAIN_STALLED_GROWTH 16
/* Windows a freshly restarted net is left alone to actually train.

   Without this the restart is self-defeating: the fresh draw is worse
   than the best-so-far by construction, so nothing resets the stuck
   counter, and the next window shakes it and the one after restarts it
   again -- it never accumulates the consecutive epochs it needs. Measured:
   `increment` reaches MSE 0.0 at ANY fixed width, and 1000 epochs (one
   window) is nowhere near enough to get there from a random start. */
#define BTN_TRAIN_SETTLE_WINDOWS 24
/* `adaptive_lr` was set once from btn->learning_rate and then never
   touched -- adaptive in name only. A constant step of 0.8 cannot settle
   into a narrow optimum; it oscillates across it, which is why every
   demo primitive missed the target loss it asked for (measured on
   pristine HEAD: combine 0.00172 against 0.0008, split 0.00188 against
   0.0008, increment 0.00175 against 0.0015) while the code reported the
   number without ever comparing it to the target.

   A window that bought less than min_improvement now shortens the step.
   The floor keeps it from freezing, and an escape restores mobility
   because a restarted net needs to move again. */
#define BTN_TRAIN_LR_DECAY 0.7
#define BTN_TRAIN_LR_FLOOR 1e-3
/* BTN_NEW_NEURON_OUTPUT_SCALE is defined above btn_add_hidden_neuron,
   which is where it is used. */
/* Smallest sample count that gets a held-out split (see the comment at
   the split itself). Below this the growth signal uses the training
   loss and no validation claim is made. */
#define BTN_TRAIN_MIN_SPLIT_SAMPLES 64
#define BTN_TRAIN_SHAKE 0.45
#define BTN_TRAIN_PLATEAU (-2.0)

static size_t btn_weight_cells(const BinaryTransformNetwork *btn) {
    return btn->input_count * btn->max_hidden_count +
           btn->max_hidden_count * btn->output_count +
           btn->max_hidden_count + btn->output_count;
}

static void btn_weights_copy(double *dst, const BinaryTransformNetwork *btn) {
    size_t ih = btn->input_count * btn->max_hidden_count;
    size_t ho = btn->max_hidden_count * btn->output_count;
    memcpy(dst, btn->input_hidden, ih * sizeof(double));
    memcpy(dst + ih, btn->hidden_output_weights, ho * sizeof(double));
    memcpy(dst + ih + ho, btn->hidden_bias,
           btn->max_hidden_count * sizeof(double));
    memcpy(dst + ih + ho + btn->max_hidden_count, btn->output_bias,
           btn->output_count * sizeof(double));
}

static void btn_weights_restore(BinaryTransformNetwork *btn, const double *src) {
    size_t ih = btn->input_count * btn->max_hidden_count;
    size_t ho = btn->max_hidden_count * btn->output_count;
    memcpy(btn->input_hidden, src, ih * sizeof(double));
    memcpy(btn->hidden_output_weights, src + ih, ho * sizeof(double));
    memcpy(btn->hidden_bias, src + ih + ho,
           btn->max_hidden_count * sizeof(double));
    memcpy(btn->output_bias, src + ih + ho + btn->max_hidden_count,
           btn->output_count * sizeof(double));
}

/* Bounded additive perturbation of every live weight. */
static void btn_weights_shake(BinaryTransformNetwork *btn, double amount) {
    size_t hidden, input, output;
    for (hidden = 0; hidden < btn->hidden_count; ++hidden) {
        for (input = 0; input < btn->input_count; ++input) {
            btn->input_hidden[btn_input_hidden_index(btn, input, hidden)] +=
                random_weight() * amount;
        }
        btn->hidden_bias[hidden] += random_weight() * amount;
        for (output = 0; output < btn->output_count; ++output) {
            btn->hidden_output_weights[btn_hidden_output_index(btn, hidden, output)] +=
                random_weight() * amount;
        }
    }
    for (output = 0; output < btn->output_count; ++output) {
        btn->output_bias[output] += random_weight() * amount;
    }
}

/* The whole-dataset loss a CALLER can reproduce: mean squared error over
   EVERY sample, through the public btn_forward. The trainer used to report
   an internal blend of a training loss (which secretly included the
   held-out rows) and a validation loss (which added those same rows a
   second time), so the number it returned matched no computation the
   caller could perform. A measurement nobody can reproduce is not one. */
static double btn_whole_set_loss(
    BinaryTransformNetwork *btn,
    const double *inputs,
    const double *targets,
    size_t sample_count
) {
    double total = 0.0;
    size_t sample, output;
    for (sample = 0; sample < sample_count; ++sample) {
        const double *out = btn_forward(btn, inputs + sample * btn->input_count);
        if (out == NULL) return BTN_TRAIN_LOSS_FAILED;
        for (output = 0; output < btn->output_count; ++output) {
            double error = targets[sample * btn->output_count + output] - out[output];
            total += error * error;
        }
    }
    return total / (double)(sample_count * btn->output_count);
}

int btn_train_loss_is_success(double loss) {
    /* Finite and non-negative. BTN_TRAIN_LOSS_FAILED is +inf, so it fails
       the finiteness test; the old -1.0 and -2.0 encodings fail the sign
       test. NaN fails both, which is why this is a predicate and not an
       inequality a caller writes by hand. */
    return isfinite(loss) && loss >= 0.0;
}

static int btn_train_dynamic_core(
    BinaryTransformNetwork *btn,
    const double *inputs,
    const double *targets,
    size_t sample_count,
    size_t max_epochs,
    size_t growth_window,
    double target_loss,
    double min_improvement,
    int allow_split,
    double *loss_out
) {
    size_t target_val_sample_count = 0;
    size_t val_sample_count = 0;
    size_t train_sample_count = sample_count;
    size_t epochs_done = 0;
    size_t sample;
    size_t epoch;
    size_t output;
    size_t hidden;
    size_t input;
    char *validation_mask = NULL;
    double *output_deltas = NULL;
    double *hidden_errors = NULL;
    double previous_loss;
    double ema_loss = 0.0;
    double ema_prev;
    double improvement;
    double relative_improvement;
    const double ema_alpha = 0.2;
    double validation_loss = 0.0;
    double train_loss = 0.0;
    size_t stride;
    size_t validation_stride_offset;
    double adaptive_lr;
    double mom = 0.0;
    double *v_ho = NULL, *v_ob = NULL, *v_ih = NULL, *v_hb = NULL;
    int fast = 0;
    size_t stuck_windows = 0;
    size_t stalled_growth = 0;
    size_t settle_windows = 0;
    size_t escapes = 0;
    double *best_weights = NULL;
    double best_loss = -1.0;
    size_t best_hidden = 0;
    int converged = 0;


    if (loss_out != NULL) *loss_out = BTN_TRAIN_LOSS_FAILED;
    if (btn == NULL || btn_is_adapter(btn) || inputs == NULL || targets == NULL || sample_count == 0) {
        return BTN_TRAIN_INVALID;
    }
    if (growth_window == 0) {
        growth_window = 1;
    }

    /* A split is only made when holding rows out can mean something.

       Once the held-out rows are GENUINELY held out -- never stepped on,
       never counted in the training loss -- taking a fifth of a 16-row
       set removes three rows of what is, for a CNET primitive, a
       specification rather than a sample of some larger population.
       `hex_value` maps 16 characters to 16 arbitrary values; there is no
       structure in it to generalise from, so three unseen rows are three
       rows the unit cannot possibly certify. Measured: hex_value fell to
       13/16 and increment to 15/16 with a fifth held out, while the
       256-row primitives were unaffected (combine 256/256).

       Below the floor there is no split and no validation claim: the
       growth signal falls back to the training loss, which is what it
       effectively was before -- the difference is that the code now says
       so instead of computing a `validation_loss` over rows it had
       already fitted. Above the floor the split is real. 64 is the
       smallest set where a fifth is a dozen rows, enough to be a signal
       rather than noise. */
    if (allow_split && sample_count >= BTN_TRAIN_MIN_SPLIT_SAMPLES) {
        target_val_sample_count = sample_count / 5;
        if (target_val_sample_count == 0) {
            target_val_sample_count = 1;
        }
    }
    if (target_val_sample_count >= sample_count) {
        target_val_sample_count = sample_count - 1;
    }

    if (target_val_sample_count > 0) {
        validation_mask = calloc(sample_count, sizeof(*validation_mask));
        if (validation_mask == NULL) {
            return BTN_TRAIN_INVALID;
        }

        stride = sample_count / target_val_sample_count;
        if (stride == 0) {
            stride = 1;
        }
        validation_stride_offset = stride / 2;
        for (sample = 0; sample < sample_count && val_sample_count < target_val_sample_count;
             ++sample) {
            if ((sample % stride) == validation_stride_offset) {
                validation_mask[sample] = 1;
                ++val_sample_count;
            }
        }
    }

    /* The held-out rows are HELD OUT. They were previously walked by the
       SGD loop and counted in train_sample_count, so the "validation" loss
       measured rows the optimiser had already fitted -- a split that
       measured nothing and could not detect overfitting by construction. */
    train_sample_count = sample_count - val_sample_count;
    if (train_sample_count == 0) {
        /* Degenerate split: train on everything rather than on nothing. */
        train_sample_count = sample_count;
        val_sample_count = 0;
    }

    output_deltas = calloc(btn->output_count, sizeof(*output_deltas));
    hidden_errors = calloc(btn->max_hidden_count, sizeof(*hidden_errors));
    if (output_deltas == NULL || hidden_errors == NULL) {
        goto fail;
    }

    /* Heavy-ball momentum (btn->momentum != 0): one velocity per weight/bias,
       max-sized and zeroed so grown neurons start at rest. mom == 0 leaves the
       buffers NULL and the updates fall to the exact plain-SGD path below. */
    mom = btn->momentum;
    if (mom != 0.0) {
        v_ho = calloc(btn->output_count * btn->max_hidden_count, sizeof(double));
        v_ob = calloc(btn->output_count, sizeof(double));
        v_ih = calloc(btn->max_hidden_count * btn->input_count, sizeof(double));
        v_hb = calloc(btn->max_hidden_count, sizeof(double));
        if (v_ho == NULL || v_ob == NULL || v_ih == NULL || v_hb == NULL) {
            free(v_ho); free(v_ob); free(v_ih); free(v_hb);
            v_ho = v_ob = v_ih = v_hb = NULL;
            mom = 0.0;   /* OOM: fall back to plain SGD, same correctness */
        }
    }

    /* CNET_TRAIN_FAST: byte-identical fast step, plain SGD only (see the
       comment block above btn_train_fast_on). */
    /* btn_train_fast_epoch walks every sample and takes no mask, so it
       cannot hold rows out. Correctness outranks an opt-in speed knob:
       with a split present, take the exact loop that honours the mask. */
    fast = (mom == 0.0) && btn_train_fast_on() && val_sample_count == 0;

    previous_loss = 0.0;
    for (sample = 0; sample < sample_count; ++sample) {
        const double *train_input;
        const double *train_target;
        const double *outputs;
        if (validation_mask != NULL && validation_mask[sample]) {
            continue;
        }
        train_input = inputs + (sample * btn->input_count);
        train_target = targets + (sample * btn->output_count);
        outputs = fast
            ? btn_forward_fast(btn, train_input)
            : btn_forward_full_precision(btn, train_input);

        for (output = 0; output < btn->output_count; ++output) {
            double error = train_target[output] - outputs[output];
            previous_loss += error * error;
        }
    }
    previous_loss /= (double)(train_sample_count * btn->output_count);
    if (val_sample_count > 0) {
        ema_loss = 0.0;
        for (sample = 0; sample < sample_count; ++sample) {
            if (!validation_mask[sample]) {
                continue;
            }
            const double *val_input = inputs + (sample * btn->input_count);
            const double *val_target = targets + (sample * btn->output_count);
            const double *outputs = fast
                ? btn_forward_fast(btn, val_input)
                : btn_forward_full_precision(btn, val_input);

            for (output = 0; output < btn->output_count; ++output) {
                double error = val_target[output] - outputs[output];
                ema_loss += error * error;
            }
        }
        ema_loss /= (double)(val_sample_count * btn->output_count);
    } else {
        ema_loss = previous_loss;
    }
    adaptive_lr = btn->learning_rate;

    while (epochs_done < max_epochs && previous_loss > target_loss) {
        size_t epochs_to_train = growth_window;

        if (epochs_done + epochs_to_train > max_epochs) {
            epochs_to_train = max_epochs - epochs_done;
        }

        for (epoch = 0; epoch < epochs_to_train; ++epoch) {
            if (fast) {
                btn_train_fast_epoch(btn, inputs, targets, sample_count,
                                     adaptive_lr, output_deltas,
                                     hidden_errors);
                continue;
            }
            for (sample = 0; sample < sample_count; ++sample) {
                const double *train_input;
                const double *train_target;
                const double *outputs;

                /* Never take a gradient step on a held-out row. */
                if (validation_mask != NULL && validation_mask[sample]) {
                    continue;
                }
                train_input = inputs + sample * btn->input_count;
                train_target = targets + sample * btn->output_count;
                outputs = btn_forward_full_precision(btn, train_input);
                for (output = 0; output < btn->output_count; ++output) {
                    double error = train_target[output] - outputs[output];
                    output_deltas[output] =
                        error * sigmoid_derivative_from_output(outputs[output]);
                }
                for (hidden = 0; hidden < btn->hidden_count; ++hidden) {
                    hidden_errors[hidden] = 0.0;
                }
                for (output = 0; output < btn->output_count; ++output) {
                    for (hidden = 0; hidden < btn->hidden_count; ++hidden) {
                        hidden_errors[hidden] +=
                            output_deltas[output] *
                            btn->hidden_output_weights[
                                btn_hidden_output_index(btn, hidden, output)];
                    }
                }

                for (output = 0; output < btn->output_count; ++output) {
                    for (hidden = 0; hidden < btn->hidden_count; ++hidden) {
                        size_t idx = btn_hidden_output_index(btn, hidden, output);
                        double grad = output_deltas[output] *
                                      btn->hidden_output[hidden];
                        if (v_ho) {
                            v_ho[idx] = mom * v_ho[idx] + grad;
                            btn->hidden_output_weights[idx] += adaptive_lr * v_ho[idx];
                        } else {
                            btn->hidden_output_weights[idx] += adaptive_lr * grad;
                        }
                    }
                    if (v_ob) {
                        v_ob[output] = mom * v_ob[output] + output_deltas[output];
                        btn->output_bias[output] += adaptive_lr * v_ob[output];
                    } else {
                        btn->output_bias[output] += adaptive_lr * output_deltas[output];
                    }
                }

                for (hidden = 0; hidden < btn->hidden_count; ++hidden) {
                    double hidden_delta =
                        hidden_errors[hidden] *
                        sigmoid_derivative_from_output(btn->hidden_output[hidden]);
                    if (v_hb) {
                        v_hb[hidden] = mom * v_hb[hidden] + hidden_delta;
                        btn->hidden_bias[hidden] += adaptive_lr * v_hb[hidden];
                    } else {
                        btn->hidden_bias[hidden] += adaptive_lr * hidden_delta;
                    }

                    for (input = 0; input < btn->input_count; ++input) {
                        size_t idx = btn_input_hidden_index(btn, input, hidden);
                        double grad = hidden_delta * train_input[input];
                        if (v_ih) {
                            v_ih[idx] = mom * v_ih[idx] + grad;
                            btn->input_hidden[idx] += adaptive_lr * v_ih[idx];
                        } else {
                            btn->input_hidden[idx] += adaptive_lr * grad;
                        }
                    }
                }
            }
        }
        epochs_done += epochs_to_train;

        train_loss = 0.0;
        for (sample = 0; sample < sample_count; ++sample) {
            const double *train_input;
            const double *train_target;
            const double *outputs;
            if (validation_mask != NULL && validation_mask[sample]) {
                continue;
            }
            train_input = inputs + (sample * btn->input_count);
            train_target = targets + (sample * btn->output_count);
            outputs = fast
                ? btn_forward_fast(btn, train_input)
                : btn_forward_full_precision(btn, train_input);

            for (output = 0; output < btn->output_count; ++output) {
                double error = train_target[output] - outputs[output];
                train_loss += error * error;
            }
        }
        train_loss /= (double)(train_sample_count * btn->output_count);
        if (train_loss <= target_loss) {
            /* SNAPSHOT THE WINNER before leaving. Jumping straight to
               cleanup let it restore an older, worse net over the one that
               had just succeeded, and left a stale stuck counter to report
               a plateau for a run that reached its target. */
            converged = 1;
            stuck_windows = 0;
            if (best_weights == NULL) {
                best_weights = malloc(btn_weight_cells(btn) * sizeof(double));
            }
            if (best_weights != NULL) {
                btn_weights_copy(best_weights, btn);
                best_hidden = btn->hidden_count;
                best_loss = train_loss;
            }
            previous_loss = train_loss;
            goto done;
        }

        if (val_sample_count > 0) {
            validation_loss = 0.0;
            for (sample = 0; sample < sample_count; ++sample) {
                if (!validation_mask[sample]) {
                    continue;
                }
                const double *val_input = inputs + (sample * btn->input_count);
                const double *val_target = targets + (sample * btn->output_count);
                const double *outputs = fast
                    ? btn_forward_fast(btn, val_input)
                    : btn_forward_full_precision(btn, val_input);

                for (output = 0; output < btn->output_count; ++output) {
                    double error = val_target[output] - outputs[output];
                    validation_loss += error * error;
                }
            }
            validation_loss /= (double)(val_sample_count * btn->output_count);
        } else {
            validation_loss = train_loss;
        }

        ema_prev = ema_loss;
        ema_loss = ema_alpha * validation_loss + (1.0 - ema_alpha) * ema_loss;
        improvement = ema_prev - ema_loss;
        relative_improvement =
            ema_prev > 0.0 ? improvement / ema_prev : 0.0;
        /* Keep the best net ever seen, so nothing below can lose ground.
           Scored on the WHOLE sample set, not the held-out slice: with 16
           exemplars the slice is three rows, and picking a net by three
           rows demonstrably returns one that is worse on the other
           thirteen -- which is what certification replays. The stuck
           counter resets only on a MEANINGFUL gain, because a run creeping
           down by a thousandth of a percent per window is still stuck and
           resetting on those never lets the escape escalate. */
        {
        /* Computed directly over every sample in one pass. The old blend
           weighted train_loss and validation_loss -- but train_loss then
           covered ALL rows including the held-out ones, so those rows were
           counted twice and the "whole-set" number was not one. */
        double full_loss = btn_whole_set_loss(btn, inputs, targets,
                                              sample_count);
        if (best_loss < 0.0 || full_loss < best_loss) {
            int meaningful = best_loss < 0.0 ||
                full_loss < best_loss * (1.0 - min_improvement);
            if (best_weights == NULL) {
                best_weights = malloc(btn_weight_cells(btn) * sizeof(double));
            }
            if (best_weights != NULL) {
                btn_weights_copy(best_weights, btn);
                best_loss = full_loss;
                best_hidden = btn->hidden_count;
                if (meaningful) {
                    stuck_windows = 0;
                    stalled_growth = 0;
                }
            }
        }
        }

        if (settle_windows > 0) {
            /* A freshly restarted net is training. Leave it alone. */
            --settle_windows;
        } else if (relative_improvement < min_improvement) {
            /* Shorten the step before doing anything more drastic. */
            if (adaptive_lr > btn->learning_rate * BTN_TRAIN_LR_FLOOR) {
                adaptive_lr *= BTN_TRAIN_LR_DECAY;
            }
            /* Engage only for a run that has NOT met the target it was
               given. A run already at or below its target has nothing to
               escape from, and perturbing it can only cost exemplars:
               lower MSE is not the same as more exemplars certifying, and
               a converged 16-exemplar primitive measurably loses rows to
               a restart it did not need. Those runs take the path below
               unchanged, and return exactly what they used to. */
            /* Growth that is not paying for itself is still a plateau.

               The escape used to be reachable only once
               btn_add_hidden_neuron FAILED, i.e. once the ceiling was
               reached. A run with headroom therefore spent its whole
               budget adding neurons that bought nothing and never tried
               anything else: `increment` (ceiling 128) ran 160000 epochs,
               grew to 91 neurons and finished at 0.00337 against a target
               of 0.0015, with the escape never once engaged.

               So count consecutive stalled windows even while growth is
               still available, and once enough of them have gone by with
               nothing to show, escape as well as grow. */
            int growth_failed = btn_add_hidden_neuron(btn) != 0;
            if (!growth_failed) {
                ++stalled_growth;
            }
            if ((growth_failed ||
                 stalled_growth >= BTN_TRAIN_STALLED_GROWTH) &&
                best_loss > target_loss) {
                stalled_growth = 0;
                /* Growth is exhausted and this window bought too little to
                   be worth a neuron -- which is the plateau condition the
                   outer test already states. More
                   capacity cannot reach past a local minimum, so escalate:
                   perturb every weight, and every fourth stuck window
                   re-draw a slice of the hidden layer outright. The
                   snapshot above is what is returned if none of it helps,
                   so escalating costs nothing. */
                ++stuck_windows;
                ++escapes;
                /* A perturbed or restarted net has to be able to move
                   again, so give the step back. */
                adaptive_lr = btn->learning_rate;
                if (best_weights != NULL) {
                    /* Restart on EVERY escape, not every fourth.

                       Gating on `stuck_windows % 4` meant the restart
                       depended on a counter that only advances when an
                       escape happens, so a run whose escapes are rare
                       never restarted at all: traced on `increment`,
                       stuck_windows reached 3 across the whole 160000
                       epochs and the full-width restart -- the thing that
                       actually works -- fired zero times, while the loss
                       sat at 0.002749 from epoch 110000 onward.

                       Restarting every time is safe here for two reasons
                       that did not hold before: the best net is kept and
                       returned unless the fresh draw beats it, and the
                       fresh draw is given BTN_TRAIN_SETTLE_WINDOWS to
                       train before anything perturbs it again. */
                    if (1) {
                        /* Restart the WHOLE hidden layer at the width
                           reached, rather than re-drawing half of it
                           around the best net found.

                           Growing one neuron at a time from a single
                           unit is what puts this net in a bad basin in
                           the first place, and a redraw anchored on the
                           best keeps it there: measured on pristine HEAD,
                           every demo primitive missed the target loss it
                           asked for -- combine 0.00172 against 0.0008.
                           The same architecture initialised at full width
                           and trained plainly reaches 0.0000132 in 500
                           epochs, sixty times better than the target. The
                           width was never the problem; the schedule that
                           arrived at it was.

                           This can cost nothing, because best_weights is
                           kept and is what gets returned unless the fresh
                           draw beats it. */
                        size_t k;
                        for (k = 0; k < btn->hidden_count; ++k) {
                            btn_initialize_hidden_neuron(btn, k);
                        }
                        for (k = 0; k < btn->output_count; ++k) {
                            btn->output_bias[k] = random_weight();
                        }
                        settle_windows = BTN_TRAIN_SETTLE_WINDOWS;
                    } else {
                        btn_weights_shake(btn, BTN_TRAIN_SHAKE);
                    }
                }
            }
        }
        previous_loss = train_loss;
    }

done:
    free(output_deltas);
    free(hidden_errors);
    free(validation_mask);
    free(v_ho); free(v_ob); free(v_ih); free(v_hb);
    /* Return the best net seen, not the last one trained: a perturbation
       that did not pay must cost nothing. Only for runs that actually
       perturbed -- a run that never escaped returns exactly what it
       always did. */
    if (best_weights != NULL) {
        /* `converged` runs already hold the winning weights, and restoring
           over them is exactly the defect this guards: a run that crossed
           its target could be handed back an older, worse net. */
        if (!converged && escapes > 0 && best_loss >= 0.0 && best_hidden > 0) {
            btn->hidden_count = best_hidden;
            btn_weights_restore(btn, best_weights);
        }
        free(best_weights);
        best_weights = NULL;
    }
    /* A run that never escaped -- growth exhausted, perturbation tried, and
       no new best for many consecutive windows -- must SAY so. Returning a
       plausible loss let a caller persist an uncertifiable net with no
       signal that anything had gone wrong, which is exactly how `combine`
       shipped weights that failed certification while the demo printed a
       healthy-looking number. */
    /* loss_out is ALWAYS the whole-set loss of the net actually handed
       back, through the public forward, whatever the status. A caller
       that wants a whole-set bar checks it; a caller that wants to know
       whether the run did what it was asked reads the status. */
    if (loss_out != NULL) {
        *loss_out = btn_whole_set_loss(btn, inputs, targets, sample_count);
    }
    /* SUCCESS means one thing: the run reached the target it was given,
       on the data it was allowed to train on. Anything else -- stuck on a
       plateau, or simply out of epochs -- did NOT do what it was asked,
       and reporting that as success is how an uncertifiable net gets
       persisted with a healthy-looking number beside it.

       The held-out rows are not part of this question. They were never
       trained on, so requiring the WHOLE-set loss to meet the target
       would mean the split could only ever be reported as failure. What
       they are for is the growth and escape signal above; what they cost
       is that loss_out (whole set) can legitimately exceed target_loss on
       a successful run. That is a real generalisation gap, reported
       rather than hidden. */
    return converged ? BTN_TRAIN_OK : BTN_TRAIN_PLATEAU_STATUS;

fail:
    free(output_deltas);
    free(hidden_errors);
    free(validation_mask);
    free(v_ho); free(v_ob); free(v_ih); free(v_hb);
    free(best_weights);
    return BTN_TRAIN_INVALID;
}

int btn_train_dynamic_checked(
    BinaryTransformNetwork *btn,
    const double *inputs,
    const double *targets,
    size_t sample_count,
    size_t max_epochs,
    size_t growth_window,
    double target_loss,
    double min_improvement,
    double *loss_out
) {
    return btn_train_dynamic_core(btn, inputs, targets, sample_count,
                                  max_epochs, growth_window, target_loss,
                                  min_improvement, 1, loss_out);
}

double btn_train_dynamic(
    BinaryTransformNetwork *btn,
    const double *inputs,
    const double *targets,
    size_t sample_count,
    size_t max_epochs,
    size_t growth_window,
    double target_loss,
    double min_improvement
) {
    double loss = BTN_TRAIN_LOSS_FAILED;
    int status = btn_train_dynamic_core(btn, inputs, targets, sample_count,
                                        max_epochs, growth_window,
                                        target_loss, min_improvement, 1,
                                        &loss);
    /* Failure must not be representable as a good loss. The previous
       encodings were -2.0 and -1.0, and every threshold check in this tree
       is `loss <= bar`, which a negative satisfies. +inf satisfies no
       finite bar and prints as `inf`. */
    if (status != BTN_TRAIN_OK) return BTN_TRAIN_LOSS_FAILED;
    return loss;
}

double btn_train_dynamic_spec(
    BinaryTransformNetwork *btn,
    const double *inputs,
    const double *targets,
    size_t sample_count,
    size_t max_epochs,
    size_t growth_window,
    double target_loss,
    double min_improvement
) {
    double loss = BTN_TRAIN_LOSS_FAILED;
    int status = btn_train_dynamic_core(btn, inputs, targets, sample_count,
                                        max_epochs, growth_window,
                                        target_loss, min_improvement, 0,
                                        &loss);
    if (status != BTN_TRAIN_OK) return BTN_TRAIN_LOSS_FAILED;
    return loss;
}

int btn_predict_bits(
    BinaryTransformNetwork *btn,
    const double *inputs,
    char *bits_output,
    size_t bits_output_size
) {
    size_t output;
    const double *outputs;

    if (btn == NULL || inputs == NULL || bits_output == NULL ||
        bits_output_size < btn->output_count + 1) {
        return -1;
    }

    outputs = btn_forward(btn, inputs);
    for (output = 0; output < btn->output_count; ++output) {
        bits_output[output] = outputs[output] >= 0.5 ? '1' : '0';
    }
    bits_output[btn->output_count] = '\0';

    return 0;
}

int btn_predict_hex_symbol_value(
    BinaryTransformNetwork *btn,
    char hex_symbol,
    char *bits_output,
    size_t bits_output_size
) {
    double inputs[16];
    int value;
    size_t index;

    if (btn == NULL || btn->input_count != 16 || btn->output_count != 4 ||
        bits_output == NULL) {
        return -1;
    }
    if (hex_value(hex_symbol, &value) != 0) {
        return -1;
    }

    for (index = 0; index < 16; ++index) {
        inputs[index] = index == (size_t)value ? 1.0 : 0.0;
    }

    return btn_predict_bits(btn, inputs, bits_output, bits_output_size);
}

static int word_symbol_index(char c) {
    if (c >= 'A' && c <= 'Z') {
        return c - 'A' + 1;
    }
    return 0;
}

static int encode_word_input(const char *word, double *inputs, size_t max_word_len) {
    size_t position;
    size_t symbol;
    size_t word_len;

    if (word == NULL || inputs == NULL) {
        return -1;
    }

    word_len = strlen(word);
    if (word_len > max_word_len) {
        return -1;
    }

    for (position = 0; position < max_word_len; ++position) {
        int active = 0;

        if (position < word_len) {
            active = word_symbol_index(word[position]);
        }

        for (symbol = 0; symbol < 27; ++symbol) {
            inputs[position * 27 + symbol] =
                symbol == (size_t)active ? 1.0 : 0.0;
        }
    }

    return 0;
}

int btn_predict_word_token(
    BinaryTransformNetwork *btn,
    const char *word,
    const char **vocabulary,
    size_t vocabulary_count,
    size_t max_word_len,
    char *word_output,
    size_t word_output_size
) {
    double *inputs;
    const double *outputs;
    size_t output;
    size_t best_index = 0;
    double best_value;

    if (btn == NULL || word == NULL || vocabulary == NULL || vocabulary_count == 0 ||
        word_output == NULL || btn->input_count != max_word_len * 27 ||
        btn->output_count != vocabulary_count) {
        return -1;
    }

    inputs = calloc(btn->input_count, sizeof(*inputs));
    if (inputs == NULL) {
        return -1;
    }

    if (encode_word_input(word, inputs, max_word_len) != 0) {
        free(inputs);
        return -1;
    }

    outputs = btn_forward(btn, inputs);
    best_value = outputs[0];

    for (output = 1; output < btn->output_count; ++output) {
        if (outputs[output] > best_value) {
            best_value = outputs[output];
            best_index = output;
        }
    }

    if (strlen(vocabulary[best_index]) + 1 > word_output_size) {
        free(inputs);
        return -1;
    }

    strcpy(word_output, vocabulary[best_index]);
    free(inputs);
    return 0;
}

int nn_predict_word_from_hex(
    NeuralNetwork *char_nn,
    BinaryTransformNetwork *word_nn,
    const char *hex,
    const char **vocabulary,
    size_t vocabulary_count,
    size_t max_word_len,
    char *word_output,
    size_t word_output_size
) {
    size_t hex_len;
    size_t text_len;
    char *text;
    int result;

    if (char_nn == NULL || word_nn == NULL || hex == NULL ||
        vocabulary == NULL || word_output == NULL) {
        return -1;
    }

    hex_len = strlen(hex);
    if (hex_len == 0 || hex_len % 2 != 0) {
        return -1;
    }

    text_len = hex_len / 2;
    if (text_len > max_word_len) {
        return -1;
    }

    text = malloc(text_len + 1);
    if (text == NULL) {
        return -1;
    }

    result = nn_predict_ascii_string_from_hex(char_nn, hex, text, text_len + 1);
    if (result == 0) {
        result = btn_predict_word_token(
            word_nn,
            text,
            vocabulary,
            vocabulary_count,
            max_word_len,
            word_output,
            word_output_size
        );
    }

    free(text);
    return result;
}

static int encode_hex_pair_input(char high_hex, char low_hex, double inputs[32]) {
    int high;
    int low;
    size_t index;

    if (hex_value(high_hex, &high) != 0 || hex_value(low_hex, &low) != 0) {
        return -1;
    }

    for (index = 0; index < 32; ++index) {
        inputs[index] = 0.0;
    }

    inputs[high] = 1.0;
    inputs[16 + low] = 1.0;
    return 0;
}

int btn_predict_ascii_string_from_hex_pairs(
    BinaryTransformNetwork *char_nn,
    const char *hex,
    char *text_output,
    size_t text_output_size
) {
    size_t hex_len;
    size_t text_len;
    size_t index;

    if (char_nn == NULL || hex == NULL || text_output == NULL ||
        text_output_size == 0 || char_nn->input_count != 32 ||
        char_nn->output_count != 7) {
        return -1;
    }

    hex_len = strlen(hex);
    if (hex_len == 0 || hex_len % 2 != 0) {
        return -1;
    }

    text_len = hex_len / 2;
    if (text_output_size < text_len + 1) {
        return -1;
    }

    for (index = 0; index < text_len; ++index) {
        double inputs[32];
        const double *outputs;
        int ch = 0;
        size_t bit;

        if (encode_hex_pair_input(hex[index * 2], hex[index * 2 + 1], inputs) != 0) {
            return -1;
        }

        outputs = btn_forward(char_nn, inputs);
        for (bit = 0; bit < 7; ++bit) {
            if (outputs[bit] >= 0.5) {
                ch |= 1 << (6 - bit);
            }
        }

        text_output[index] = (char)ch;
    }

    text_output[text_len] = '\0';
    return 0;
}

int btn_predict_word_from_hex(
    BinaryTransformNetwork *char_nn,
    BinaryTransformNetwork *word_nn,
    const char *hex,
    const char **vocabulary,
    size_t vocabulary_count,
    size_t max_word_len,
    char *word_output,
    size_t word_output_size
) {
    size_t hex_len;
    size_t text_len;
    char *text;
    int result;

    if (char_nn == NULL || word_nn == NULL || hex == NULL ||
        vocabulary == NULL || word_output == NULL) {
        return -1;
    }

    hex_len = strlen(hex);
    if (hex_len == 0 || hex_len % 2 != 0) {
        return -1;
    }

    text_len = hex_len / 2;
    if (text_len > max_word_len) {
        return -1;
    }

    text = malloc(text_len + 1);
    if (text == NULL) {
        return -1;
    }

    result = btn_predict_ascii_string_from_hex_pairs(char_nn, hex, text, text_len + 1);
    if (result == 0) {
        result = btn_predict_word_token(
            word_nn,
            text,
            vocabulary,
            vocabulary_count,
            max_word_len,
            word_output,
            word_output_size
        );
    }

    free(text);
    return result;
}

int btn_predict_raw_binary_word_token(
    BinaryTransformNetwork *btn,
    const char *binary,
    const char **vocabulary,
    size_t vocabulary_count,
    char *word_output,
    size_t word_output_size
) {
    double *inputs;
    const double *outputs;
    size_t binary_len;
    size_t index;
    size_t output;
    size_t best_index = 0;
    double best_value;

    if (btn == NULL || binary == NULL || vocabulary == NULL ||
        vocabulary_count == 0 || word_output == NULL ||
        btn->output_count != vocabulary_count) {
        return -1;
    }

    binary_len = strlen(binary);
    if (binary_len == 0 || binary_len > btn->input_count) {
        return -1;
    }

    inputs = calloc(btn->input_count, sizeof(*inputs));
    if (inputs == NULL) {
        return -1;
    }

    for (index = 0; index < binary_len; ++index) {
        if (binary[index] != '0' && binary[index] != '1') {
            free(inputs);
            return -1;
        }

        inputs[index] = binary[index] == '1' ? 1.0 : 0.0;
    }

    outputs = btn_forward(btn, inputs);
    best_value = outputs[0];

    for (output = 1; output < btn->output_count; ++output) {
        if (outputs[output] > best_value) {
            best_value = outputs[output];
            best_index = output;
        }
    }

    if (strlen(vocabulary[best_index]) + 1 > word_output_size) {
        free(inputs);
        return -1;
    }

    strcpy(word_output, vocabulary[best_index]);
    free(inputs);
    return 0;
}

int btn_save(const BinaryTransformNetwork *btn, const char *path) {
    FILE *file;
    size_t input;
    size_t hidden;
    size_t output;

    if (btn == NULL || path == NULL) {
        return -1;
    }

    file = fopen(path, "w");
    if (file == NULL) {
        return -1;
    }

    fprintf(file, "CNET_BTN 5\n");
    fprintf(file, "%lu %lu %lu %lu %.17g\n",
            (unsigned long)btn->input_count,
            (unsigned long)btn->output_count,
            (unsigned long)btn->hidden_count,
            (unsigned long)btn->max_hidden_count,
            btn->learning_rate);
    fprintf(file, "INPUTS %lu\n", (unsigned long)btn->input_port_count);
    for (input = 0; input < btn->input_port_count; ++input) {
        fprintf(file, "PORT_IN %s %lu %lu %s\n",
                port_family_token(btn->input_ports[input].family),
                (unsigned long)btn->input_ports[input].field_width,
                (unsigned long)btn->input_ports[input].field_count,
                btn->input_ports[input].tag[0] != '\0'
                    ? btn->input_ports[input].tag : "-");
    }
    fprintf(file, "OUTPUTS %lu\n", (unsigned long)btn->output_port_count);
    for (output = 0; output < btn->output_port_count; ++output) {
        fprintf(file, "PORT_OUT %s %lu %lu %s\n",
                port_family_token(btn->output_ports[output].family),
                (unsigned long)btn->output_ports[output].field_width,
                (unsigned long)btn->output_ports[output].field_count,
                btn->output_ports[output].tag[0] != '\0'
                    ? btn->output_ports[output].tag : "-");
    }

    for (output = 0; output < btn->output_count; ++output) {
        fprintf(file, "%.17g\n", btn->output_bias[output]);
    }

    for (hidden = 0; hidden < btn->hidden_count; ++hidden) {
        fprintf(file, "%.17g\n", btn->hidden_bias[hidden]);
    }

    for (input = 0; input < btn->input_count; ++input) {
        for (hidden = 0; hidden < btn->hidden_count; ++hidden) {
            fprintf(file, "%.17g\n",
                    btn->input_hidden[btn_input_hidden_index(btn, input, hidden)]);
        }
    }

    for (hidden = 0; hidden < btn->hidden_count; ++hidden) {
        for (output = 0; output < btn->output_count; ++output) {
            fprintf(file, "%.17g\n",
                    btn->hidden_output_weights[
                        btn_hidden_output_index(btn, hidden, output)
                    ]);
        }
    }

    if (fclose(file) != 0) {
        return -1;
    }

    return 0;
}

static size_t port_total(Port port) {
    return port.field_width * port.field_count;
}

int btn_save_stats(const BinaryTransformNetwork *btn, const char *path) {
    FILE *file;

    if (btn == NULL || path == NULL) {
        return -1;
    }

    file = fopen(path, "w");
    if (file == NULL) {
        return -1;
    }

    fprintf(file, "CNET_STATS 1\n");
#if defined(_WIN32) || defined(__WIN32__) || defined(__MINGW32__)
    fprintf(file, "%lu %lu\n",
            atomic_load_explicit(&btn->output_successes, memory_order_relaxed),
            atomic_load_explicit(&btn->output_failures, memory_order_relaxed));
#else
    fprintf(file, "%lu %lu\n", btn->output_successes, btn->output_failures);
#endif

    if (fclose(file) != 0) {
        return -1;
    }
    return 0;
}

/* ---- frozen init (txt replacement) --------------------------------------- */

/* Initialize a BTN directly from const data produced by the freeze generator.
   This is the fast path with zero file I/O and zero text parsing.
   Ports must already be filled in the FrozenBTNData (the generator copies
   them from the committed contract or the original save).
   The weight arrays in data must be laid out in the exact same order as
   btn_save uses (output_bias, hidden_bias, input->hidden, hidden->output).
   Returns 0 on success. */
int btn_init_frozen(BinaryTransformNetwork *btn, const FrozenBTNData *data) {
    size_t i, h, o;
    size_t ih_idx, ho_idx;

    if (btn == NULL || data == NULL) {
        return -1;
    }
    if (data->input_count == 0 || data->output_count == 0 || data->hidden_count == 0) {
        return -1;
    }

    /* Allocate using the normal init path (this sets up all the internal
       pointers and the flat counts). We pass initial == max == hidden so
       it does not grow. lr/seed are irrelevant for a frozen primitive (no
       training), but btn_init rejects learning_rate <= 0, so pass a valid
       placeholder; inference is unaffected (weights are overwritten below). */
    if (btn_init(btn, data->input_count, data->output_count,
                 data->hidden_count, data->hidden_count, 0.1, 0u) != 0) {
        return -1;
    }

    /* Copy ports (the generator ensures they match the committed contract). */
    if (data->input_port_count > 0 && data->input_port_count <= BTN_MAX_INPUT_PORTS) {
        btn->input_port_count = data->input_port_count;
        for (i = 0; i < data->input_port_count; ++i) {
            btn->input_ports[i] = data->input_ports[i];
        }
    }
    if (data->output_port_count > 0 && data->output_port_count <= BTN_MAX_OUTPUT_PORTS) {
        btn->output_port_count = data->output_port_count;
        for (i = 0; i < data->output_port_count; ++i) {
            btn->output_ports[i] = data->output_ports[i];
        }
    }

    /* Copy the four weight regions exactly as the text loader would have. */
    for (o = 0; o < data->output_count; ++o) {
        btn->output_bias[o] = data->output_bias[o];
    }
    for (h = 0; h < data->hidden_count; ++h) {
        btn->hidden_bias[h] = data->hidden_bias[h];
    }

    ih_idx = 0;
    for (i = 0; i < data->input_count; ++i) {
        for (h = 0; h < data->hidden_count; ++h) {
            btn->input_hidden[btn_input_hidden_index(btn, i, h)] =
                data->input_hidden[ih_idx++];
        }
    }

    ho_idx = 0;
    for (h = 0; h < data->hidden_count; ++h) {
        for (o = 0; o < data->output_count; ++o) {
            btn->hidden_output_weights[btn_hidden_output_index(btn, h, o)] =
                data->hidden_output_weights[ho_idx++];
        }
    }

    /* Reliability counters start at zero for frozen committed primitives
       (evidence comes from runtime or is seeded by consolidation). */
#if defined(_WIN32) || defined(__WIN32__) || defined(__MINGW32__)
    atomic_store_explicit(&btn->output_successes, 0, memory_order_relaxed);
    atomic_store_explicit(&btn->output_failures, 0, memory_order_relaxed);
#else
    btn->output_successes = 0;
    btn->output_failures = 0;
#endif

    return 0;
}

int btn_load_stats(BinaryTransformNetwork *btn, const char *path) {
    FILE *file;
    char magic[16];
    int version;
    unsigned long successes;
    unsigned long failures;

    if (btn == NULL || path == NULL) {
        return -1;
    }

    file = fopen(path, "r");
    if (file == NULL) {
        return -1;
    }

    if (fscanf(file, "%15s %d", magic, &version) != 2 ||
        strcmp(magic, "CNET_STATS") != 0 || version != 1 ||
        fscanf(file, "%lu %lu", &successes, &failures) != 2) {
        fclose(file);
        return -1;
    }

#if defined(_WIN32) || defined(__WIN32__) || defined(__MINGW32__)
    atomic_store_explicit(&btn->output_successes, successes, memory_order_relaxed);
    atomic_store_explicit(&btn->output_failures, failures, memory_order_relaxed);
#else
    btn->output_successes = successes;
    btn->output_failures = failures;
#endif
    fclose(file);
    return 0;
}

double btn_reliability(const BinaryTransformNetwork *btn) {
    if (btn == NULL) {
        return 0.0;
    }
#if defined(_WIN32) || defined(__WIN32__) || defined(__MINGW32__)
    unsigned long s = atomic_load_explicit(&btn->output_successes, memory_order_relaxed);
    unsigned long f = atomic_load_explicit(&btn->output_failures, memory_order_relaxed);
#else
    unsigned long s = btn->output_successes;
    unsigned long f = btn->output_failures;
#endif
    return (double)(s + 1) / (double)(s + f + 2);
}

int port_set_tag(Port *port, const char *tag) {
    size_t i;

    if (port == NULL || tag == NULL) {
        return -1;
    }
    /* Tags are atoms over [A-Za-z0-9_]: single tokens the text weight format
       can round-trip, and '-' stays free as the untagged file sentinel. */
    for (i = 0; tag[i] != '\0'; ++i) {
        char c = tag[i];
        int valid = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') || c == '_';

        if (!valid || i >= PORT_TAG_MAX - 1) {
            return -1;
        }
    }
    for (i = 0; tag[i] != '\0'; ++i) {
        port->tag[i] = tag[i];
    }
    port->tag[i] = '\0';
    return 0;
}

static const char *port_family_token(PortFamily family) {
    switch (family) {
        case PORT_ONEHOT:     return "onehot";
        case PORT_BINARY_MSB: return "binary_msb";
        case PORT_BINARY_LSB: return "binary_lsb";
        case PORT_EVIDENCE:   return "evidence";
        case PORT_CONCEPT:    return "concept";
        case PORT_RAW:
        default:              return "raw";
    }
}

static PortFamily port_family_from_token(const char *token) {
    if (strcmp(token, "onehot") == 0) {
        return PORT_ONEHOT;
    }
    if (strcmp(token, "binary_msb") == 0) {
        return PORT_BINARY_MSB;
    }
    if (strcmp(token, "binary_lsb") == 0) {
        return PORT_BINARY_LSB;
    }
    if (strcmp(token, "evidence") == 0) {
        return PORT_EVIDENCE;
    }
    if (strcmp(token, "concept") == 0) {
        return PORT_CONCEPT;
    }
    return PORT_RAW;
}

int port_canonicalize(Port port, const double *raw, double *clean) {
    size_t total;
    size_t field;
    size_t i;

    if (raw == NULL || clean == NULL) {
        return -1;
    }

    total = port_total(port);

    switch (port.family) {
        case PORT_RAW:
            for (i = 0; i < total; ++i) {
                clean[i] = raw[i];
            }
            return 0;

        case PORT_BINARY_MSB:
        case PORT_BINARY_LSB:
            for (i = 0; i < total; ++i) {
                clean[i] = raw[i] >= 0.5 ? 1.0 : 0.0;
            }
            return 0;

        case PORT_ONEHOT:
            for (field = 0; field < port.field_count; ++field) {
                size_t base = field * port.field_width;
                size_t best = 0;
                size_t k;

                for (k = 1; k < port.field_width; ++k) {
                    if (raw[base + k] > raw[base + best]) {
                        best = k;
                    }
                }
                for (k = 0; k < port.field_width; ++k) {
                    clean[base + k] = (k == best) ? 1.0 : 0.0;
                }
            }
            return 0;

        case PORT_EVIDENCE:
            /* 5B: finite distrib for late binding. Keep soft probs; re-normalize top support for clean handoff. */
            {
                double s = 0.0;
                for (i = 0; i < total; ++i) {
                    clean[i] = raw[i] < 0 ? 0 : raw[i];
                    s += clean[i];
                }
                if (s > 1e-9) for (i=0; i<total; ++i) clean[i] /= s;
                else for (i=0; i<total; ++i) clean[i] = 0.0;
            }
            return 0;

        case PORT_CONCEPT:
            /* 6A: higher discrete concept; canonicalize like onehot over concept symbols */
            for (field = 0; field < port.field_count; ++field) {
                size_t base = field * port.field_width;
                size_t best = 0;
                size_t k;
                for (k = 1; k < port.field_width; ++k) {
                    if (raw[base + k] > raw[base + best]) best = k;
                }
                for (k = 0; k < port.field_width; ++k) {
                    clean[base + k] = (k == best) ? 1.0 : 0.0;
                }
            }
            return 0;

        default:
            return -1;
    }
}

int btn_set_io_ports(
    BinaryTransformNetwork *btn,
    const Port *input_ports,
    size_t n_in,
    const Port *output_ports,
    size_t n_out
) {
    size_t i;
    size_t in_total = 0;
    size_t out_total = 0;

    if (btn == NULL || input_ports == NULL || output_ports == NULL ||
        n_in == 0 || n_in > BTN_MAX_INPUT_PORTS ||
        n_out == 0 || n_out > BTN_MAX_OUTPUT_PORTS) {
        return -1;
    }

    for (i = 0; i < n_in; ++i) {
        if (input_ports[i].field_width == 0 ||
            input_ports[i].field_count == 0) {
            return -1;
        }
        in_total += port_total(input_ports[i]);
    }
    for (i = 0; i < n_out; ++i) {
        if (output_ports[i].field_width == 0 ||
            output_ports[i].field_count == 0) {
            return -1;
        }
        out_total += port_total(output_ports[i]);
    }
    if (in_total != btn->input_count || out_total != btn->output_count) {
        return -1;
    }

    for (i = 0; i < n_in; ++i) {
        btn->input_ports[i] = input_ports[i];
    }
    btn->input_port_count = n_in;
    for (i = 0; i < n_out; ++i) {
        btn->output_ports[i] = output_ports[i];
    }
    btn->output_port_count = n_out;
    return 0;
}

int btn_set_input_ports(
    BinaryTransformNetwork *btn,
    const Port *input_ports,
    size_t n,
    Port output_port
) {
    return btn_set_io_ports(btn, input_ports, n, &output_port, 1);
}

int btn_set_ports(
    BinaryTransformNetwork *btn,
    Port input_port,
    Port output_port
) {
    return btn_set_input_ports(btn, &input_port, 1, output_port);
}

int port_compatible(Port producer_output, Port consumer_input) {
    /* Meaning gate: when both sides carry a semantic tag, the tags must
       agree -- regardless of representation. An untagged port is a wildcard
       (gradual adoption; mirrors RAW's absence-of-information fallback). */
    if (producer_output.tag[0] != '\0' && consumer_input.tag[0] != '\0' &&
        strcmp(producer_output.tag, consumer_input.tag) != 0) {
        return 0;
    }

    if (producer_output.family == PORT_RAW || consumer_input.family == PORT_RAW) {
        /* Cannot verify encoding; fall back to total-width equality. */
        return port_total(producer_output) == port_total(consumer_input) ? 1 : 0;
    }

    /* 5B/6A: exact family match for EVIDENCE (distrib late-bind) and CONCEPT (abstract discrete) */
    if ((producer_output.family == PORT_EVIDENCE || producer_output.family == PORT_CONCEPT) &&
        producer_output.family != consumer_input.family) {
        /* strict for now; allow tag-mediated abstraction elsewhere (contracts) */
        return 0;
    }

    return (producer_output.family == consumer_input.family &&
            producer_output.field_width == consumer_input.field_width &&
            producer_output.field_count == consumer_input.field_count) ? 1 : 0;
}

int port_validate(Port port, const double *values) {
    size_t field;
    size_t i;

    if (values == NULL) {
        return 0;
    }

    switch (port.family) {
        case PORT_RAW:
            return 1;

        case PORT_ONEHOT:
            for (field = 0; field < port.field_count; ++field) {
                size_t hot = 0;

                for (i = 0; i < port.field_width; ++i) {
                    if (values[field * port.field_width + i] > 0.5) {
                        ++hot;
                    }
                }
                if (hot != 1) {
                    return 0;
                }
            }
            return 1;

        case PORT_BINARY_MSB:
        case PORT_BINARY_LSB:
            for (i = 0; i < port_total(port); ++i) {
                if (values[i] >= 0.25 && values[i] <= 0.75) {
                    return 0;
                }
            }
            return 1;

        case PORT_EVIDENCE:
            /* 5B: soft distrib evidence. Non-neg, roughly sums to ~1.0 (tolerance for fp) */
            {
                double s = 0.0;
                for (i = 0; i < port_total(port); ++i) {
                    if (values[i] < -1e-9) return 0;
                    s += values[i];
                }
                if (s < 0.5 || s > 1.5) return 0;  /* loose for demo use */
                return 1;
            }

        case PORT_CONCEPT:
            /* 6A: discrete concept symbols, validate as onehot */
            for (field = 0; field < port.field_count; ++field) {
                size_t hot = 0;
                for (i = 0; i < port.field_width; ++i) {
                    if (values[field * port.field_width + i] > 0.5) ++hot;
                }
                if (hot != 1) return 0;
            }
            return 1;

        default:
            return 0;
    }
}

int port_margin(Port port, const double *values, double *out_margin) {
    if (values == NULL || out_margin == NULL) {
        return -1;
    }
    switch (port.family) {
        case PORT_RAW:
            *out_margin = 1.0;  /* no canonicalization boundary */
            return 0;

        case PORT_BINARY_MSB:
        case PORT_BINARY_LSB: {
            size_t i;
            size_t total = port_total(port);
            double m = 0.5;
            for (i = 0; i < total; ++i) {
                double d = values[i] - 0.5;
                if (d < 0.0) d = -d;
                if (d < m) m = d;
            }
            *out_margin = m;
            return 0;
        }

        case PORT_ONEHOT: {
            size_t field;
            double m = 1.0;
            for (field = 0; field < port.field_count; ++field) {
                size_t base = field * port.field_width;
                double gap;
                if (port.field_width < 2) {
                    gap = 1.0;  /* a single option never competes */
                } else {
                    double top1 = values[base];
                    double top2 = values[base + 1];
                    size_t i;
                    if (top2 > top1) {
                        double t = top1; top1 = top2; top2 = t;
                    }
                    for (i = 2; i < port.field_width; ++i) {
                        double v = values[base + i];
                        if (v > top1) { top2 = top1; top1 = v; }
                        else if (v > top2) { top2 = v; }
                    }
                    gap = top1 - top2;
                }
                if (gap < m) m = gap;
            }
            *out_margin = m;
            return 0;
        }

        case PORT_EVIDENCE:
            /* 5B: margin on distrib = top1-top2 on evidence weights (late binding headroom) */
            {
                double top1=values[0], top2=0.0; size_t i; size_t tot = port_total(port);
                if (tot > 1 && values[1] > top1) { top2 = top1; top1 = values[1]; } else if (tot>1) top2=values[1];
                for (i=2; i<tot; ++i) {
                    if (values[i] > top1) { top2=top1; top1=values[i]; }
                    else if (values[i] > top2) top2 = values[i];
                }
                *out_margin = (top1 - top2);
                return 0;
            }

        case PORT_CONCEPT:
            /* 6A: same as onehot gap for discrete concept symbols */
            {
                size_t field;
                double m = 1.0;
                for (field = 0; field < port.field_count; ++field) {
                    size_t base = field * port.field_width;
                    double gap;
                    if (port.field_width < 2) gap = 1.0;
                    else {
                        double top1 = values[base], top2 = values[base+1];
                        size_t ii;
                        if (top2 > top1) { double t=top1; top1=top2; top2=t; }
                        for (ii=2; ii<port.field_width; ++ii) {
                            double v = values[base+ii];
                            if (v>top1){top2=top1;top1=v;} else if (v>top2) top2=v;
                        }
                        gap = top1-top2;
                    }
                    if (gap < m) m = gap;
                }
                *out_margin = m;
                return 0;
            }

        default:
            return -1;
    }
}

int btn_load(BinaryTransformNetwork *btn, const char *path) {
    FILE *file;
    char magic[16];
    int version;
    unsigned long input_count;
    unsigned long output_count;
    unsigned long hidden_count;
    unsigned long max_hidden_count;
    double learning_rate;
    size_t input;
    size_t hidden;
    size_t output;

    if (btn == NULL || path == NULL) {
        return -1;
    }

    file = fopen(path, "r");
    if (file == NULL) {
        return -1;
    }

    if (fscanf(file, "%15s %d", magic, &version) != 2 ||
        strcmp(magic, "CNET_BTN") != 0 ||
        version < 1 || version > 5) {
        fclose(file);
        return -1;
    }

    if (fscanf(file, "%lu %lu %lu %lu %lf",
               &input_count,
               &output_count,
               &hidden_count,
               &max_hidden_count,
               &learning_rate) != 5) {
        fclose(file);
        return -1;
    }

    if (btn_init(
            btn,
            (size_t)input_count,
            (size_t)output_count,
            (size_t)hidden_count,
            (size_t)max_hidden_count,
            learning_rate,
            1u
        ) != 0) {
        fclose(file);
        return -1;
    }

    /* btn_init set one RAW input port; versions 2+ override the ports.
       v2 has a single PORT_IN; v3 prefixes an "INPUTS n" count; v4 appends a
       semantic tag token to each port line ("-" = untagged). */
    if (version >= 2) {
        char keyword[16];
        char out_family[16];
        char tag_token[PORT_TAG_MAX];
        unsigned long out_field_width;
        unsigned long out_field_count;
        unsigned long port_count = 1;
        unsigned long pi;

        if (version >= 3) {
            if (fscanf(file, "%15s %lu", keyword, &port_count) != 2 ||
                strcmp(keyword, "INPUTS") != 0 ||
                port_count == 0 || port_count > BTN_MAX_INPUT_PORTS) {
                btn_free(btn);
                fclose(file);
                return -1;
            }
        }

        for (pi = 0; pi < port_count; ++pi) {
            char in_family[16];
            unsigned long in_field_width;
            unsigned long in_field_count;

            if (fscanf(file, "%15s %15s %lu %lu",
                       keyword, in_family, &in_field_width, &in_field_count) != 4 ||
                strcmp(keyword, "PORT_IN") != 0) {
                btn_free(btn);
                fclose(file);
                return -1;
            }
            btn->input_ports[pi].family = port_family_from_token(in_family);
            btn->input_ports[pi].field_width = (size_t)in_field_width;
            btn->input_ports[pi].field_count = (size_t)in_field_count;
            if (version >= 4) {
                if (fscanf(file, "%31s", tag_token) != 1 ||
                    (strcmp(tag_token, "-") != 0 &&
                     port_set_tag(&btn->input_ports[pi], tag_token) != 0)) {
                    btn_free(btn);
                    fclose(file);
                    return -1;
                }
            }
        }
        btn->input_port_count = (size_t)port_count;

        /* v2-v4 carry exactly one PORT_OUT; v5 prefixes an "OUTPUTS m"
           count, mirroring INPUTS. */
        {
            unsigned long out_port_count = 1;
            unsigned long po;

            if (version >= 5) {
                if (fscanf(file, "%15s %lu", keyword, &out_port_count) != 2 ||
                    strcmp(keyword, "OUTPUTS") != 0 ||
                    out_port_count == 0 ||
                    out_port_count > BTN_MAX_OUTPUT_PORTS) {
                    btn_free(btn);
                    fclose(file);
                    return -1;
                }
            }

            for (po = 0; po < out_port_count; ++po) {
                if (fscanf(file, "%15s %15s %lu %lu",
                           keyword, out_family, &out_field_width,
                           &out_field_count) != 4 ||
                    strcmp(keyword, "PORT_OUT") != 0) {
                    btn_free(btn);
                    fclose(file);
                    return -1;
                }
                btn->output_ports[po].family =
                    port_family_from_token(out_family);
                btn->output_ports[po].field_width = (size_t)out_field_width;
                btn->output_ports[po].field_count = (size_t)out_field_count;
                if (version >= 4) {
                    if (fscanf(file, "%31s", tag_token) != 1 ||
                        (strcmp(tag_token, "-") != 0 &&
                         port_set_tag(&btn->output_ports[po],
                                      tag_token) != 0)) {
                        btn_free(btn);
                        fclose(file);
                        return -1;
                    }
                }
            }
            btn->output_port_count = (size_t)out_port_count;
        }
    }

    for (output = 0; output < btn->output_count; ++output) {
        if (fscanf(file, "%lf", &btn->output_bias[output]) != 1) {
            btn_free(btn);
            fclose(file);
            return -1;
        }
    }

    for (hidden = 0; hidden < btn->hidden_count; ++hidden) {
        if (fscanf(file, "%lf", &btn->hidden_bias[hidden]) != 1) {
            btn_free(btn);
            fclose(file);
            return -1;
        }
    }

    for (input = 0; input < btn->input_count; ++input) {
        for (hidden = 0; hidden < btn->hidden_count; ++hidden) {
            if (fscanf(
                    file,
                    "%lf",
                    &btn->input_hidden[btn_input_hidden_index(btn, input, hidden)]
                ) != 1) {
                btn_free(btn);
                fclose(file);
                return -1;
            }
        }
    }

    for (hidden = 0; hidden < btn->hidden_count; ++hidden) {
        for (output = 0; output < btn->output_count; ++output) {
            if (fscanf(
                    file,
                    "%lf",
                    &btn->hidden_output_weights[
                        btn_hidden_output_index(btn, hidden, output)
                    ]
                ) != 1) {
                btn_free(btn);
                fclose(file);
                return -1;
            }
        }
    }

    fclose(file);
    return 0;
}
