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

static int btn_add_hidden_neuron(BinaryTransformNetwork *btn) {
    size_t hidden;
    size_t output;

    if (btn->hidden_count >= btn->max_hidden_count) {
        return -1;
    }

    hidden = btn->hidden_count;
    btn_initialize_hidden_neuron(btn, hidden);
    if (hidden > 0) {
        for (output = 0; output < btn->output_count; ++output) {
            btn->hidden_output_weights[btn_hidden_output_index(btn, hidden, output)] = 0.0;
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

void btn_free(BinaryTransformNetwork *btn) {
    if (btn == NULL) {
        return;
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
    if (btn == NULL || threshold < 0.0) {
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

    if (btn == NULL || inputs == NULL || targets == NULL) {
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


    if (btn == NULL || inputs == NULL || targets == NULL || sample_count == 0) {
        return -1.0;
    }
    if (growth_window == 0) {
        growth_window = 1;
    }

    if (sample_count > 1) {
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
            return -1.0;
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

    train_sample_count = sample_count;

    output_deltas = calloc(btn->output_count, sizeof(*output_deltas));
    hidden_errors = calloc(btn->max_hidden_count, sizeof(*hidden_errors));
    if (output_deltas == NULL || hidden_errors == NULL) {
        goto fail;
    }

    previous_loss = 0.0;
    for (sample = 0; sample < sample_count; ++sample) {
        const double *train_input = inputs + (sample * btn->input_count);
        const double *train_target = targets + (sample * btn->output_count);
        const double *outputs = btn_forward_full_precision(btn, train_input);

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
            const double *outputs = btn_forward_full_precision(btn, val_input);

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
            for (sample = 0; sample < sample_count; ++sample) {
                const double *train_input = inputs + sample * btn->input_count;
                const double *train_target = targets + sample * btn->output_count;
                const double *outputs;

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
                        btn->hidden_output_weights[idx] += adaptive_lr * grad;
                    }
                    btn->output_bias[output] += adaptive_lr * output_deltas[output];
                }

                for (hidden = 0; hidden < btn->hidden_count; ++hidden) {
                    double hidden_delta =
                        hidden_errors[hidden] *
                        sigmoid_derivative_from_output(btn->hidden_output[hidden]);
                    btn->hidden_bias[hidden] += adaptive_lr * hidden_delta;

                    for (input = 0; input < btn->input_count; ++input) {
                        size_t idx = btn_input_hidden_index(btn, input, hidden);
                        double grad = hidden_delta * train_input[input];
                        btn->input_hidden[idx] += adaptive_lr * grad;
                    }
                }
            }
        }
        epochs_done += epochs_to_train;

        train_loss = 0.0;
        for (sample = 0; sample < sample_count; ++sample) {
            const double *train_input = inputs + (sample * btn->input_count);
            const double *train_target = targets + (sample * btn->output_count);
            const double *outputs = btn_forward_full_precision(btn, train_input);

            for (output = 0; output < btn->output_count; ++output) {
                double error = train_target[output] - outputs[output];
                train_loss += error * error;
            }
        }
        train_loss /= (double)(train_sample_count * btn->output_count);
        if (train_loss <= target_loss) {
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
                const double *outputs = btn_forward_full_precision(btn, val_input);

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
        if (relative_improvement < min_improvement &&
            btn_add_hidden_neuron(btn) == 0) {
            previous_loss = train_loss;
        } else {
            previous_loss = train_loss;
        }
    }

done:
    free(output_deltas);
    free(hidden_errors);
    free(validation_mask);
    return previous_loss;

fail:
    free(output_deltas);
    free(hidden_errors);
    free(validation_mask);
    return -1.0;
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
