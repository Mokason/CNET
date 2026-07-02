#include "../include/nn.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void build_binary_hex_data(double inputs[16][4], double targets[16]) {
    int value;
    int bit;

    for (value = 0; value < 16; ++value) {
        for (bit = 0; bit < 4; ++bit) {
            inputs[value][bit] = (double)((value >> (3 - bit)) & 1);
        }
        targets[value] = ((double)value + 0.5) / 16.0;
    }
}

static void build_increment_data(double inputs[16][4], double targets[16][5]) {
    int value;
    int bit;

    for (value = 0; value < 16; ++value) {
        int incremented = value + 1;

        for (bit = 0; bit < 4; ++bit) {
            inputs[value][bit] = (double)((value >> (3 - bit)) & 1);
        }

        for (bit = 0; bit < 5; ++bit) {
            targets[value][bit] =
                ((incremented >> (4 - bit)) & 1) ? 0.9 : 0.1;
        }
    }
}

static void build_hex_symbol_value_data(double inputs[16][16], double targets[16][4]) {
    int value;
    int index;
    int bit;

    for (value = 0; value < 16; ++value) {
        for (index = 0; index < 16; ++index) {
            inputs[value][index] = index == value ? 1.0 : 0.0;
        }

        for (bit = 0; bit < 4; ++bit) {
            targets[value][bit] = ((value >> (3 - bit)) & 1) ? 0.9 : 0.1;
        }
    }
}

static void build_uppercase_ascii_hex_data(double inputs[26][32], double targets[26][7]) {
    int ch;
    int bit;

    for (ch = 'A'; ch <= 'Z'; ++ch) {
        int index = ch - 'A';
        int high = (ch >> 4) & 0xF;
        int low = ch & 0xF;

        memset(inputs[index], 0, 32 * sizeof(inputs[index][0]));
        inputs[index][high] = 1.0;
        inputs[index][16 + low] = 1.0;

        for (bit = 0; bit < 7; ++bit) {
            targets[index][bit] = ((ch >> (6 - bit)) & 1) ? 0.9 : 0.1;
        }
    }
}

static int word_symbol_index(char c) {
    if (c >= 'A' && c <= 'Z') {
        return c - 'A' + 1;
    }
    return 0;
}

static void encode_word_input(const char *word, double *input, size_t max_word_len) {
    size_t position;
    size_t symbol;
    size_t word_len = strlen(word);

    for (position = 0; position < max_word_len; ++position) {
        int active = 0;

        if (position < word_len) {
            active = word_symbol_index(word[position]);
        }

        for (symbol = 0; symbol < 27; ++symbol) {
            input[position * 27 + symbol] = symbol == (size_t)active ? 1.0 : 0.0;
        }
    }
}

static void build_word_token_data(
    const char **words,
    size_t word_count,
    size_t max_word_len,
    double *inputs,
    double *targets
) {
    size_t word;
    size_t output;

    for (word = 0; word < word_count; ++word) {
        encode_word_input(words[word], inputs + (word * max_word_len * 27), max_word_len);

        for (output = 0; output < word_count; ++output) {
            targets[word * word_count + output] = output == word ? 0.9 : 0.1;
        }
    }
}

static void encode_raw_binary_word_input(
    const char *word,
    double *input,
    size_t max_word_len
) {
    size_t position;
    int bit;
    size_t word_len = strlen(word);

    for (position = 0; position < max_word_len; ++position) {
        unsigned char ch = 0;

        if (position < word_len) {
            ch = (unsigned char)word[position];
        }

        for (bit = 0; bit < 8; ++bit) {
            input[position * 8 + bit] = (double)((ch >> (7 - bit)) & 1);
        }
    }
}

static void build_raw_binary_word_data(
    const char **words,
    size_t word_count,
    size_t max_word_len,
    double *inputs,
    double *targets
) {
    size_t word;
    size_t output;

    for (word = 0; word < word_count; ++word) {
        encode_raw_binary_word_input(
            words[word],
            inputs + (word * max_word_len * 8),
            max_word_len
        );

        for (output = 0; output < word_count; ++output) {
            targets[word * word_count + output] = output == word ? 0.9 : 0.1;
        }
    }
}

static int hex_prediction(double output) {
    int value = (int)floor(output * 16.0);

    if (value < 0) {
        return 0;
    }
    if (value > 15) {
        return 15;
    }
    return value;
}

static double logit(double y) {
    return log(y / (1.0 - y));
}

static void test_scalar_ascii_decoder_uses_bucket_index_without_offset(void) {
    NeuralNetwork chars;
    char ch = '\0';
    int index = 'A' - 32;

    assert(nn_init(&chars, 2, 1, 1, 0.7, 23u) == 0);
    chars.input_hidden[0] = 0.0;
    chars.input_hidden[1] = 0.0;
    chars.hidden_bias[0] = 0.0;
    chars.hidden_output_weights[0] = 0.0;
    chars.output_bias = logit(((double)index + 0.25) / 95.0);

    assert(nn_predict_ascii_char(&chars, '4', '1', &ch) == 0);
    assert(ch == 'A');

    nn_free(&chars);
}

static void test_forward_output_range(void) {
    NeuralNetwork nn;
    double input[4] = {0.0, 0.0, 0.0, 1.0};
    double output = 0.0;

    assert(nn_init(&nn, 4, 1, 8, 0.5, 1234u) == 0);
    output = nn_forward(&nn, input);

    assert(output > 0.0);
    assert(output < 1.0);
    nn_free(&nn);
}

static void test_dynamic_training_creates_hidden_neurons(void) {
    NeuralNetwork nn;
    double inputs[16][4];
    double targets[16];

    build_binary_hex_data(inputs, targets);
    assert(nn_init(&nn, 4, 1, 8, 0.7, 7u) == 0);

    nn_train_dynamic(&nn, &inputs[0][0], targets, 16, 12000, 1000, 0.0001, 0.01);

    assert(nn.hidden_count > 1);
    assert(nn.hidden_count <= nn.max_hidden_count);
    nn_free(&nn);
}

static void test_dynamic_training_learns_binary_hex_values(void) {
    NeuralNetwork nn;
    double inputs[16][4];
    double targets[16];
    int value;

    build_binary_hex_data(inputs, targets);
    assert(nn_init(&nn, 4, 1, 12, 0.7, 11u) == 0);

    nn_train_dynamic(&nn, &inputs[0][0], targets, 16, 30000, 1000, 0.00005, 0.01);

    for (value = 0; value < 16; ++value) {
        int predicted = hex_prediction(nn_forward(&nn, inputs[value]));
        assert(predicted == value);
    }

    nn_free(&nn);
}

static void test_frozen_network_predicts_split_binary_string(void) {
    NeuralNetwork nn;
    double inputs[16][4];
    double targets[16];
    char output[8];

    build_binary_hex_data(inputs, targets);
    assert(nn_init(&nn, 4, 1, 12, 0.7, 11u) == 0);
    nn_train_dynamic(&nn, &inputs[0][0], targets, 16, 30000, 1000, 0.00005, 0.01);

    assert(nn_predict_hex_string(&nn, "000100001111", output, sizeof(output)) == 0);
    assert(strcmp(output, "10F") == 0);

    assert(nn_predict_hex_string(&nn, "1", output, sizeof(output)) == 0);
    assert(strcmp(output, "1") == 0);

    assert(nn_predict_hex_string(&nn, "10000", output, sizeof(output)) == 0);
    assert(strcmp(output, "10") == 0);

    nn_free(&nn);
}

static void test_saved_network_loads_as_frozen_layer(void) {
    NeuralNetwork trained;
    NeuralNetwork loaded;
    double inputs[16][4];
    double targets[16];
    char output[8];
    const char *path = "test_nibble_weights.txt";

    build_binary_hex_data(inputs, targets);
    assert(nn_init(&trained, 4, 1, 12, 0.7, 11u) == 0);
    nn_train_dynamic(&trained, &inputs[0][0], targets, 16, 30000, 1000, 0.00005, 0.01);

    assert(nn_save(&trained, path) == 0);
    assert(nn_load(&loaded, path) == 0);
    assert(loaded.hidden_count == trained.hidden_count);

    assert(nn_predict_hex_string(&loaded, "10101111", output, sizeof(output)) == 0);
    assert(strcmp(output, "AF") == 0);

    nn_free(&trained);
    nn_free(&loaded);
    remove(path);
}

static void test_binary_transform_learns_increment_with_carry(void) {
    BinaryTransformNetwork inc;
    double inputs[16][4];
    double targets[16][5];
    char output[8];

    build_increment_data(inputs, targets);
    assert(btn_init(&inc, 4, 5, 1, 64, 0.8, 31u) == 0);
    btn_train_dynamic(&inc, &inputs[0][0], &targets[0][0], 16, 160000, 1000, 0.0002, 0.01);

    assert(btn_predict_bits(&inc, inputs[3], output, sizeof(output)) == 0);
    assert(strcmp(output, "00100") == 0);

    assert(btn_predict_bits(&inc, inputs[7], output, sizeof(output)) == 0);
    assert(strcmp(output, "01000") == 0);

    assert(btn_predict_bits(&inc, inputs[15], output, sizeof(output)) == 0);
    assert(strcmp(output, "10000") == 0);

    assert(inc.hidden_count > 1);
    btn_free(&inc);
}

static void test_hex_symbols_learn_actual_binary_values(void) {
    BinaryTransformNetwork hex_values;
    double inputs[16][16];
    double targets[16][4];
    char output[8];

    build_hex_symbol_value_data(inputs, targets);
    assert(btn_init(&hex_values, 16, 4, 1, 16, 0.7, 41u) == 0);
    btn_train_dynamic(&hex_values, &inputs[0][0], &targets[0][0], 16, 50000, 1000, 0.0003, 0.01);

    assert(btn_predict_hex_symbol_value(&hex_values, 'A', output, sizeof(output)) == 0);
    assert(strcmp(output, "1010") == 0);

    assert(btn_predict_hex_symbol_value(&hex_values, 'F', output, sizeof(output)) == 0);
    assert(strcmp(output, "1111") == 0);

    btn_free(&hex_values);
}

static void test_binary_transform_network_save_load_reuses_weights(void) {
    BinaryTransformNetwork trained;
    BinaryTransformNetwork loaded;
    double inputs[16][16];
    double targets[16][4];
    char output[8];
    const char *path = "test_hex_value_btn_weights.txt";

    build_hex_symbol_value_data(inputs, targets);
    assert(btn_init(&trained, 16, 4, 1, 16, 0.7, 41u) == 0);
    btn_train_dynamic(
        &trained,
        &inputs[0][0],
        &targets[0][0],
        16,
        50000,
        1000,
        0.0003,
        0.01
    );

    assert(btn_save(&trained, path) == 0);
    assert(btn_load(&loaded, path) == 0);
    assert(loaded.input_count == trained.input_count);
    assert(loaded.output_count == trained.output_count);
    assert(loaded.hidden_count == trained.hidden_count);

    assert(btn_predict_hex_symbol_value(&loaded, 'A', output, sizeof(output)) == 0);
    assert(strcmp(output, "1010") == 0);

    btn_free(&trained);
    btn_free(&loaded);
    remove(path);
}

static void test_hex_to_char_layer_decodes_word_text(void) {
    BinaryTransformNetwork chars;
    double inputs[26][32];
    double targets[26][7];
    char output[8];

    build_uppercase_ascii_hex_data(inputs, targets);
    assert(btn_init(&chars, 32, 7, 1, 32, 0.7, 61u) == 0);
    btn_train_dynamic(&chars, &inputs[0][0], &targets[0][0], 26, 50000, 1000, 0.0005, 0.01);

    assert(btn_predict_ascii_string_from_hex_pairs(&chars, "48454C4C4F", output, sizeof(output)) == 0);
    assert(strcmp(output, "HELLO") == 0);

    btn_free(&chars);
}

static void test_char_layer_learns_word_tokens(void) {
    static const char *words[] = {
        "HELLO", "WORLD", "YES", "NO", "CODE", "DATA", "BITS", "BYTE", "NODE"
    };
    enum { WORD_COUNT = 9, MAX_WORD_LEN = 5, INPUT_COUNT = MAX_WORD_LEN * 27 };
    BinaryTransformNetwork word_net;
    double inputs[WORD_COUNT][INPUT_COUNT];
    double targets[WORD_COUNT][WORD_COUNT];
    char predicted[8];

    build_word_token_data(
        words,
        WORD_COUNT,
        MAX_WORD_LEN,
        &inputs[0][0],
        &targets[0][0]
    );

    assert(btn_init(&word_net, INPUT_COUNT, WORD_COUNT, 1, 32, 0.7, 51u) == 0);
    btn_train_dynamic(
        &word_net,
        &inputs[0][0],
        &targets[0][0],
        WORD_COUNT,
        30000,
        1000,
        0.0005,
        0.01
    );

    assert(btn_predict_word_token(
        &word_net,
        "HELLO",
        words,
        WORD_COUNT,
        MAX_WORD_LEN,
        predicted,
        sizeof(predicted)
    ) == 0);
    assert(strcmp(predicted, "HELLO") == 0);

    assert(btn_predict_word_token(
        &word_net,
        "YES",
        words,
        WORD_COUNT,
        MAX_WORD_LEN,
        predicted,
        sizeof(predicted)
    ) == 0);
    assert(strcmp(predicted, "YES") == 0);

    assert(btn_predict_word_token(
        &word_net,
        "CODE",
        words,
        WORD_COUNT,
        MAX_WORD_LEN,
        predicted,
        sizeof(predicted)
    ) == 0);
    assert(strcmp(predicted, "CODE") == 0);
    assert(btn_predict_word_token(
        &word_net,
        "NODE",
        words,
        WORD_COUNT,
        MAX_WORD_LEN,
        predicted,
        sizeof(predicted)
    ) == 0);
    assert(strcmp(predicted, "NODE") == 0);

    btn_free(&word_net);
}

static void test_hex_to_word_pipeline(void) {
    static const char *words[] = {
        "HELLO", "WORLD", "YES", "NO", "CODE", "DATA", "BITS", "BYTE", "NODE"
    };
    enum { WORD_COUNT = 9, MAX_WORD_LEN = 5, INPUT_COUNT = MAX_WORD_LEN * 27 };
    BinaryTransformNetwork chars;
    BinaryTransformNetwork word_net;
    double char_inputs[26][32];
    double char_targets[26][7];
    double word_inputs[WORD_COUNT][INPUT_COUNT];
    double word_targets[WORD_COUNT][WORD_COUNT];
    char predicted[8];

    build_uppercase_ascii_hex_data(char_inputs, char_targets);
    build_word_token_data(
        words,
        WORD_COUNT,
        MAX_WORD_LEN,
        &word_inputs[0][0],
        &word_targets[0][0]
    );

    assert(btn_init(&chars, 32, 7, 1, 32, 0.7, 61u) == 0);
    btn_train_dynamic(&chars, &char_inputs[0][0], &char_targets[0][0], 26, 50000, 1000, 0.0005, 0.01);

    assert(btn_init(&word_net, INPUT_COUNT, WORD_COUNT, 1, 32, 0.7, 51u) == 0);
    btn_train_dynamic(
        &word_net,
        &word_inputs[0][0],
        &word_targets[0][0],
        WORD_COUNT,
        30000,
        1000,
        0.0005,
        0.01
    );

    assert(btn_predict_word_from_hex(
        &chars,
        &word_net,
        "48454C4C4F",
        words,
        WORD_COUNT,
        MAX_WORD_LEN,
        predicted,
        sizeof(predicted)
    ) == 0);
    assert(strcmp(predicted, "HELLO") == 0);

    assert(btn_predict_word_from_hex(
        &chars,
        &word_net,
        "434F4445",
        words,
        WORD_COUNT,
        MAX_WORD_LEN,
        predicted,
        sizeof(predicted)
    ) == 0);
    assert(strcmp(predicted, "CODE") == 0);
    assert(btn_predict_word_from_hex(
        &chars,
        &word_net,
        "4E4F4445",
        words,
        WORD_COUNT,
        MAX_WORD_LEN,
        predicted,
        sizeof(predicted)
    ) == 0);
    assert(strcmp(predicted, "NODE") == 0);

    btn_free(&chars);
    btn_free(&word_net);
}

static void test_flat_raw_binary_word_model_learns_few_texts(void) {
    static const char *words[] = {
        "HELLO", "WORLD", "YES", "NO", "CODE", "DATA", "BITS", "BYTE", "NODE"
    };
    enum { WORD_COUNT = 9, MAX_WORD_LEN = 5, INPUT_COUNT = MAX_WORD_LEN * 8 };
    BinaryTransformNetwork raw_words;
    double inputs[WORD_COUNT][INPUT_COUNT];
    double targets[WORD_COUNT][WORD_COUNT];
    char predicted[8];

    build_raw_binary_word_data(
        words,
        WORD_COUNT,
        MAX_WORD_LEN,
        &inputs[0][0],
        &targets[0][0]
    );

    assert(btn_init(&raw_words, INPUT_COUNT, WORD_COUNT, 1, 128, 0.8, 71u) == 0);
    btn_train_dynamic(
        &raw_words,
        &inputs[0][0],
        &targets[0][0],
        WORD_COUNT,
        120000,
        1000,
        0.0005,
        0.01
    );

    assert(btn_predict_raw_binary_word_token(
        &raw_words,
        "0100100001000101010011000100110001001111",
        words,
        WORD_COUNT,
        predicted,
        sizeof(predicted)
    ) == 0);
    assert(strcmp(predicted, "HELLO") == 0);

    assert(btn_predict_raw_binary_word_token(
        &raw_words,
        "01000011010011110100010001000101",
        words,
        WORD_COUNT,
        predicted,
        sizeof(predicted)
    ) == 0);
    assert(strcmp(predicted, "CODE") == 0);
    assert(btn_predict_raw_binary_word_token(
        &raw_words,
        "01001110010011110100010001000101",
        words,
        WORD_COUNT,
        predicted,
        sizeof(predicted)
    ) == 0);
    assert(strcmp(predicted, "NODE") == 0);

    btn_free(&raw_words);
}

int run_test_nn(void) {
    test_forward_output_range();
    test_scalar_ascii_decoder_uses_bucket_index_without_offset();
    test_dynamic_training_creates_hidden_neurons();
    test_dynamic_training_learns_binary_hex_values();
    test_frozen_network_predicts_split_binary_string();
    test_saved_network_loads_as_frozen_layer();
    test_binary_transform_learns_increment_with_carry();
    test_hex_symbols_learn_actual_binary_values();
    test_binary_transform_network_save_load_reuses_weights();
    test_hex_to_char_layer_decodes_word_text();
    test_char_layer_learns_word_tokens();
    test_hex_to_word_pipeline();
    test_flat_raw_binary_word_model_learns_few_texts();
    puts("All neural network tests passed.");
    return 0;
}
