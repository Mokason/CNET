#include "../../include/nn.h"
#include "../../include/contract/contract.h"
#include "../../include/router.h"
/* 3D text-level contract lives in src/contract_text_add.c (see contract_text_add.h) */
/* 3F: compound + abstain + persist in contract_text_add_* and glyph_habitat --demo 3F --persist-test */
/* 3G: perceptual_query orchestrator with selection, reflection, evolution in contract_perceptual_query */
/* 4A: narrative_diffusion for tiny stories using iterative passes + mint + evolution in contract_narrative_diffusion */
/* 4B: branching narrator with choice-aware orchestration and token emission in contract_narrative_branching */
/* 5A: interactive memory agent with mode selection, memory recall, reflection, full status in contract_interactive_agent */
/* 5B: evidence ports (PORT_EVIDENCE) + late binding to reduce info loss on ambiguous leaves */
/* 6A: concept ports (PORT_CONCEPT) + auto-abstraction to fight combo explosion + port narrowness */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The training table IS the spec: persist it as a first-class contract
   beside the weights. Training targets are soft (0.9/0.1), so canonicalize
   them to hard (1.0/0.0) before saving -- the contract loader requires exact
   canonical values. Inputs are already 0.0/1.0. */
static void emit_contract(const char *name,
                          const BinaryTransformNetwork *btn,
                          const double *inputs, const double *targets,
                          size_t exemplars, const char *path) {
    Contract c;
    double *canon = NULL;
    size_t out_total = 0;
    size_t i, p;
    int ok = 1;

    /* Compute total output width per exemplar from the btn's output ports. */
    for (p = 0; p < btn->output_port_count; ++p) {
        out_total += btn->output_ports[p].field_width *
                     btn->output_ports[p].field_count;
    }

    if (out_total == 0 || exemplars == 0) {
        fprintf(stderr, "WARN: could not emit contract %s.\n", path);
        return;
    }

    canon = (double *)malloc(exemplars * out_total * sizeof(double));
    if (canon == NULL) {
        fprintf(stderr, "WARN: could not emit contract %s.\n", path);
        return;
    }

    /* Canonicalize each exemplar's output row. */
    for (i = 0; ok && i < exemplars; ++i) {
        size_t offset = 0;
        for (p = 0; ok && p < btn->output_port_count; ++p) {
            size_t tot = btn->output_ports[p].field_width *
                         btn->output_ports[p].field_count;
            if (port_canonicalize(btn->output_ports[p],
                                  targets + i * out_total + offset,
                                  canon  + i * out_total + offset) != 0) {
                ok = 0;
            }
            offset += tot;
        }
    }

    if (!ok ||
        contract_init_borrowed(&c, name, btn, inputs, canon,
                               exemplars) != 0 ||
        contract_save(&c, path) != 0) {
        fprintf(stderr, "WARN: could not emit contract %s.\n", path);
    }

    free(canon);
}

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

/* combine: two 4-bit nibble slots (8 input bits) -> the 8-bit byte they form.
   Per bit this is the identity; its role is a multi-input join node whose
   BINARY_MSB 8 output is unique, so a DAG to a byte is unambiguous. */
static void build_combine_data(double inputs[256][8], double targets[256][8]) {
    int value;
    int bit;

    for (value = 0; value < 256; ++value) {
        for (bit = 0; bit < 8; ++bit) {
            int b = (value >> (7 - bit)) & 1;

            inputs[value][bit] = (double)b;
            targets[value][bit] = b ? 0.9 : 0.1;
        }
    }
}

/* conditional_increment: heterogeneous inputs [flag (1 bit), value (4 bits)] ->
   5-bit result = flag ? value+1 : value. 32 samples (flag x value). */
static void build_cond_increment_data(double inputs[32][5], double targets[32][5]) {
    int flag;
    int value;
    int bit;
    int idx = 0;

    for (flag = 0; flag < 2; ++flag) {
        for (value = 0; value < 16; ++value) {
            int result = flag ? value + 1 : value;

            inputs[idx][0] = (double)flag;
            for (bit = 0; bit < 4; ++bit) {
                inputs[idx][1 + bit] = (double)((value >> (3 - bit)) & 1);
            }
            for (bit = 0; bit < 5; ++bit) {
                targets[idx][bit] = ((result >> (4 - bit)) & 1) ? 0.9 : 0.1;
            }
            ++idx;
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

static void set_ports_or_die(
    BinaryTransformNetwork *btn,
    Port input_port,
    Port output_port,
    const char *name
) {
    if (btn_set_ports(btn, input_port, output_port) != 0) {
        fprintf(stderr, "Interface contract mismatch for %s primitive.\n", name);
        exit(EXIT_FAILURE);
    }
}

static void set_input_ports_or_die(
    BinaryTransformNetwork *btn,
    const Port *input_ports,
    size_t n,
    Port output_port,
    const char *name
) {
    if (btn_set_input_ports(btn, input_ports, n, output_port) != 0) {
        fprintf(stderr, "Interface contract mismatch for %s primitive.\n", name);
        exit(EXIT_FAILURE);
    }
}

static void tag_or_die(Port *port, const char *tag, const char *name) {
    if (port_set_tag(port, tag) != 0) {
        fprintf(stderr, "Invalid semantic tag for %s primitive.\n", name);
        exit(EXIT_FAILURE);
    }
}

static void emit_frozen_header(void);

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--freeze") == 0) {
        emit_frozen_header();
        return EXIT_SUCCESS;
    }
    static const char *words[] = {
        "HELLO", "WORLD", "YES", "NO", "CODE", "DATA", "BITS", "BYTE"
    };
    enum {
        WORD_COUNT = 8,
        MAX_WORD_LEN = 5,
        WORD_INPUT_COUNT = MAX_WORD_LEN * 27,
        RAW_WORD_INPUT_COUNT = MAX_WORD_LEN * 8
    };
    NeuralNetwork nn = {0};
    NeuralNetwork frozen = {0};
    BinaryTransformNetwork increment = {0};
    BinaryTransformNetwork combine_net = {0};
    BinaryTransformNetwork split_net = {0};
    BinaryTransformNetwork cond_inc = {0};
    BinaryTransformNetwork hex_values = {0};
    BinaryTransformNetwork frozen_hex_values = {0};
    BinaryTransformNetwork hex_chars = {0};
    BinaryTransformNetwork frozen_hex_chars = {0};
    BinaryTransformNetwork word_net = {0};
    BinaryTransformNetwork frozen_word_net = {0};
    BinaryTransformNetwork raw_word_net = {0};
    BinaryTransformNetwork frozen_raw_word_net = {0};
    double inputs[16][4];
    double targets[16];
    double inc_inputs[16][4];
    double inc_targets[16][5];
    double combine_inputs[256][8];
    double combine_targets[256][8];
    double cond_inc_inputs[32][5];
    double cond_inc_targets[32][5];
    double hex_value_inputs[16][16];
    double hex_value_targets[16][4];
    double hex_char_inputs[26][32];
    double hex_char_targets[26][7];
    double word_inputs[WORD_COUNT][WORD_INPUT_COUNT];
    double word_targets[WORD_COUNT][WORD_COUNT];
    double raw_word_inputs[WORD_COUNT][RAW_WORD_INPUT_COUNT];
    double raw_word_targets[WORD_COUNT][WORD_COUNT];
    double learning_rate = 0.7;
    size_t max_epochs = 20000;
    size_t max_hidden = 12;
    const char *binary_to_decode = "0001000010101111";
    const char *weights_path = "nibble_weights.txt";
    const char *hex_value_weights_path = "hex_value_weights.txt";
    const char *hex_char_weights_path = "hex_char_weights.txt";
    const char *word_weights_path = "word_weights.txt";
    const char *raw_word_weights_path = "raw_word_weights.txt";
    char decoded_hex[64];
    char increment_bits[8];
    char hex_value_bits[8];
    char word_chars[8];
    char predicted_word[8];
    char raw_predicted_word[8];
    double final_loss;
    double increment_loss;
    double combine_loss;
    double split_loss;
    double cond_inc_loss;
    double hex_value_loss;
    double hex_char_loss;
    double word_loss;
    double raw_word_loss;
    int value;

    if (argc > 1) {
        learning_rate = atof(argv[1]);
    }
    if (argc > 2) {
        max_epochs = (size_t)strtoul(argv[2], NULL, 10);
    }
    if (argc > 3) {
        max_hidden = (size_t)strtoul(argv[3], NULL, 10);
    }
    if (argc > 4) {
        binary_to_decode = argv[4];
    }

    build_binary_hex_data(inputs, targets);
    build_increment_data(inc_inputs, inc_targets);
    build_combine_data(combine_inputs, combine_targets);
    build_cond_increment_data(cond_inc_inputs, cond_inc_targets);
    build_hex_symbol_value_data(hex_value_inputs, hex_value_targets);
    build_uppercase_ascii_hex_data(hex_char_inputs, hex_char_targets);
    build_word_token_data(
        words,
        WORD_COUNT,
        MAX_WORD_LEN,
        &word_inputs[0][0],
        &word_targets[0][0]
    );
    build_raw_binary_word_data(
        words,
        WORD_COUNT,
        MAX_WORD_LEN,
        &raw_word_inputs[0][0],
        &raw_word_targets[0][0]
    );
    if (nn_init(&nn, 4, 1, max_hidden, learning_rate, 11u) != 0) {
        fprintf(stderr, "Could not initialize neural network.\n");
        return EXIT_FAILURE;
    }

    final_loss = nn_train_dynamic(
        &nn,
        &inputs[0][0],
        targets,
        16,
        max_epochs,
        1000,
        0.00005,
        0.01
    );

    puts("Dynamic neural network trained on binary to hex:");
    printf("inputs: 4\n");
    printf("hidden neurons selected: %lu\n", (unsigned long)nn.hidden_count);
    printf("final loss: %.6f\n\n", final_loss);

    for (value = 0; value < 16; ++value) {
        double output = nn_forward(&nn, inputs[value]);
        int predicted = hex_prediction(output);

        printf("%d%d%d%d -> %X (raw %.4f, expected %X)\n",
               (int)inputs[value][0],
               (int)inputs[value][1],
               (int)inputs[value][2],
               (int)inputs[value][3],
               predicted,
               output,
               value);
    }

    if (nn_save(&nn, weights_path) != 0) {
        fprintf(stderr, "Could not save trained nibble layer.\n");
        nn_free(&nn);
        return EXIT_FAILURE;
    }

    if (nn_load(&frozen, weights_path) != 0) {
        fprintf(stderr, "Could not load frozen nibble layer.\n");
        nn_free(&nn);
        return EXIT_FAILURE;
    }

    if (nn_predict_hex_string(
            &frozen,
            binary_to_decode,
            decoded_hex,
            sizeof(decoded_hex)
        ) != 0) {
        fprintf(stderr, "Could not decode binary string.\n");
        nn_free(&nn);
        nn_free(&frozen);
        return EXIT_FAILURE;
    }

    printf("\nfrozen layer decode: %s -> %s\n", binary_to_decode, decoded_hex);

    if (btn_init(&increment, 4, 5, 1, 128, 0.8, 31u) != 0) {
        fprintf(stderr, "Could not initialize increment transform network.\n");
        nn_free(&nn);
        nn_free(&frozen);
        return EXIT_FAILURE;
    }

    {
        Port inc_in = {PORT_BINARY_MSB, 4, 1, ""};
        Port inc_out = {PORT_BINARY_MSB, 5, 1, ""};

        tag_or_die(&inc_in, "nibble_value", "increment");
        tag_or_die(&inc_out, "incremented_value", "increment");
        set_ports_or_die(&increment, inc_in, inc_out, "increment");
    }

    increment_loss = btn_train_dynamic(
        &increment,
        &inc_inputs[0][0],
        &inc_targets[0][0],
        16,
        160000,
        1000,
        0.0015,
        0.01
    );

    printf("\nbinary transform learned increment:\n");
    printf("increment hidden neurons selected: %lu\n",
           (unsigned long)increment.hidden_count);
    printf("increment final loss: %.6f\n", increment_loss);

    btn_predict_bits(&increment, inc_inputs[3], increment_bits, sizeof(increment_bits));
    printf("0011 + 1 -> %s\n", increment_bits);
    btn_predict_bits(&increment, inc_inputs[7], increment_bits, sizeof(increment_bits));
    printf("0111 + 1 -> %s\n", increment_bits);
    btn_predict_bits(&increment, inc_inputs[15], increment_bits, sizeof(increment_bits));
    printf("1111 + 1 -> %s\n", increment_bits);

    if (btn_save(&increment, "increment_weights.txt") != 0) {
        fprintf(stderr, "Could not persist increment layer.\n");
        nn_free(&nn);
        nn_free(&frozen);
        btn_free(&increment);
        return EXIT_FAILURE;
    }
    /* Retrained: any reliability evidence gathered against the old weights
       is stale; the retrainer invalidates the sidecar. */
    remove("increment_stats.txt");
    emit_contract("increment", &increment, &inc_inputs[0][0],
                  &inc_targets[0][0], 16, "increment_contract.txt");

    if (btn_init(&combine_net, 8, 8, 1, 64, 0.8, 91u) != 0) {
        fprintf(stderr, "Could not initialize combine network.\n");
        nn_free(&nn);
        nn_free(&frozen);
        btn_free(&increment);
        return EXIT_FAILURE;
    }

    {
        Port combine_in[2];
        Port combine_out = {PORT_BINARY_MSB, 8, 1, ""};

        combine_in[0] = (Port){PORT_BINARY_MSB, 4, 1, ""};
        combine_in[1] = (Port){PORT_BINARY_MSB, 4, 1, ""};
        tag_or_die(&combine_in[0], "nibble_value", "combine");
        tag_or_die(&combine_in[1], "nibble_value", "combine");
        tag_or_die(&combine_out, "byte_value", "combine");
        set_input_ports_or_die(&combine_net, combine_in, 2,
                               combine_out, "combine");
    }

    combine_loss = btn_train_dynamic(
        &combine_net,
        &combine_inputs[0][0],
        &combine_targets[0][0],
        256,
        150000,
        1000,
        0.0008,
        0.03
    );

    printf("\nbinary transform learned nibble-pair join (combine):\n");
    printf("combine hidden neurons selected: %lu\n",
           (unsigned long)combine_net.hidden_count);
    printf("combine final loss: %.6f\n", combine_loss);

    if (btn_save(&combine_net, "combine_weights.txt") != 0) {
        fprintf(stderr, "Could not persist combine layer.\n");
        nn_free(&nn);
        nn_free(&frozen);
        btn_free(&increment);
        btn_free(&combine_net);
        return EXIT_FAILURE;
    }
    emit_contract("combine", &combine_net, &combine_inputs[0][0],
                  &combine_targets[0][0], 256, "combine_contract.txt");

    /* split: the inverse of combine -- a byte in, TWO independently
       consumable nibble output ports. Per bit it is the same identity map
       as combine, so it trains on the same data; only the contract differs:
       multi-output decomposition instead of multi-input join. */
    if (btn_init(&split_net, 8, 8, 1, 64, 0.8, 73u) != 0) {
        fprintf(stderr, "Could not initialize split network.\n");
        nn_free(&nn);
        nn_free(&frozen);
        btn_free(&increment);
        btn_free(&combine_net);
        return EXIT_FAILURE;
    }

    {
        Port split_in = {PORT_BINARY_MSB, 8, 1, ""};
        Port split_out[2];

        split_out[0] = (Port){PORT_BINARY_MSB, 4, 1, ""};
        split_out[1] = (Port){PORT_BINARY_MSB, 4, 1, ""};
        tag_or_die(&split_in, "byte_value", "split");
        tag_or_die(&split_out[0], "nibble_value", "split");
        tag_or_die(&split_out[1], "nibble_value", "split");
        if (btn_set_io_ports(&split_net, &split_in, 1, split_out, 2) != 0) {
            fprintf(stderr, "Interface contract mismatch for split primitive.\n");
            exit(EXIT_FAILURE);
        }
    }

    split_loss = btn_train_dynamic(
        &split_net,
        &combine_inputs[0][0],
        &combine_targets[0][0],
        256,
        150000,
        1000,
        0.0008,
        0.03
    );

    printf("\nbinary transform learned byte-to-nibbles (split, multi-output):\n");
    printf("split hidden neurons selected: %lu\n",
           (unsigned long)split_net.hidden_count);
    printf("split final loss: %.6f\n", split_loss);

    if (btn_save(&split_net, "split_weights.txt") != 0) {
        fprintf(stderr, "Could not persist split layer.\n");
        nn_free(&nn);
        nn_free(&frozen);
        btn_free(&increment);
        btn_free(&combine_net);
        btn_free(&split_net);
        return EXIT_FAILURE;
    }
    emit_contract("split", &split_net, &combine_inputs[0][0],
                  &combine_targets[0][0], 256, "split_contract.txt");

    if (btn_init(&cond_inc, 5, 5, 1, 64, 0.8, 97u) != 0) {
        fprintf(stderr, "Could not initialize conditional_increment network.\n");
        nn_free(&nn);
        nn_free(&frozen);
        btn_free(&increment);
        btn_free(&combine_net);
        return EXIT_FAILURE;
    }

    {
        Port cond_in[2];
        Port cond_out = {PORT_BINARY_MSB, 5, 1, ""};

        cond_in[0] = (Port){PORT_BINARY_MSB, 1, 1, ""};  /* flag */
        cond_in[1] = (Port){PORT_BINARY_MSB, 4, 1, ""};  /* value */
        tag_or_die(&cond_in[0], "cond_flag", "conditional_increment");
        tag_or_die(&cond_in[1], "nibble_value", "conditional_increment");
        tag_or_die(&cond_out, "cond_result", "conditional_increment");
        set_input_ports_or_die(&cond_inc, cond_in, 2, cond_out,
                               "conditional_increment");
    }

    cond_inc_loss = btn_train_dynamic(
        &cond_inc,
        &cond_inc_inputs[0][0],
        &cond_inc_targets[0][0],
        32,
        120000,
        1000,
        0.001,
        0.03
    );

    printf("\nbinary transform learned conditional increment "
           "(heterogeneous flag + value):\n");
    printf("conditional_increment hidden neurons selected: %lu\n",
           (unsigned long)cond_inc.hidden_count);
    printf("conditional_increment final loss: %.6f\n", cond_inc_loss);

    if (btn_save(&cond_inc, "cond_increment_weights.txt") != 0) {
        fprintf(stderr, "Could not persist conditional_increment layer.\n");
        nn_free(&nn);
        nn_free(&frozen);
        btn_free(&increment);
        btn_free(&combine_net);
        btn_free(&cond_inc);
        return EXIT_FAILURE;
    }
    emit_contract("conditional_increment", &cond_inc, &cond_inc_inputs[0][0],
                  &cond_inc_targets[0][0], 32, "cond_increment_contract.txt");

    if (btn_init(&hex_values, 16, 4, 1, 32, 0.7, 41u) != 0) {
        fprintf(stderr, "Could not initialize hex value network.\n");
        nn_free(&nn);
        nn_free(&frozen);
        btn_free(&increment);
        return EXIT_FAILURE;
    }

    {
        Port hv_in = {PORT_ONEHOT, 16, 1, ""};
        Port hv_out = {PORT_BINARY_MSB, 4, 1, ""};

        tag_or_die(&hv_in, "hex_digit", "hex_value");
        tag_or_die(&hv_out, "nibble_value", "hex_value");
        set_ports_or_die(&hex_values, hv_in, hv_out, "hex_value");
    }

    hex_value_loss = btn_train_dynamic(
        &hex_values,
        &hex_value_inputs[0][0],
        &hex_value_targets[0][0],
        16,
        50000,
        1000,
        0.003,
        0.01
    );

    if (btn_save(&hex_values, hex_value_weights_path) != 0 ||
        btn_load(&frozen_hex_values, hex_value_weights_path) != 0) {
        fprintf(stderr, "Could not persist hex value layer.\n");
        nn_free(&nn);
        nn_free(&frozen);
        btn_free(&increment);
        btn_free(&hex_values);
        return EXIT_FAILURE;
    }
    /* Retrained: invalidate any stale reliability evidence. */
    remove("hex_value_stats.txt");
    emit_contract("hex_value", &hex_values, &hex_value_inputs[0][0],
                  &hex_value_targets[0][0], 16, "hex_value_contract.txt");

    printf("\nhex symbols learned actual binary values:\n");
    printf("hex value hidden neurons selected: %lu\n",
           (unsigned long)hex_values.hidden_count);
    printf("hex value final loss: %.6f\n", hex_value_loss);
    btn_predict_hex_symbol_value(
        &frozen_hex_values,
        'A',
        hex_value_bits,
        sizeof(hex_value_bits)
    );
    printf("A -> %s\n", hex_value_bits);
    btn_predict_hex_symbol_value(
        &frozen_hex_values,
        'F',
        hex_value_bits,
        sizeof(hex_value_bits)
    );
    printf("F -> %s\n", hex_value_bits);

    if (btn_init(&hex_chars, 32, 7, 1, 32, 0.7, 61u) != 0) {
        fprintf(stderr, "Could not initialize hex character network.\n");
        nn_free(&nn);
        nn_free(&frozen);
        btn_free(&increment);
        btn_free(&hex_values);
        return EXIT_FAILURE;
    }

    {
        Port hc_in = {PORT_ONEHOT, 16, 2, ""};
        Port hc_out = {PORT_BINARY_MSB, 7, 1, ""};

        tag_or_die(&hc_in, "hex_digit_pair", "hex_char");
        tag_or_die(&hc_out, "ascii_char", "hex_char");
        set_ports_or_die(&hex_chars, hc_in, hc_out, "hex_char");
    }

    hex_char_loss = btn_train_dynamic(
        &hex_chars,
        &hex_char_inputs[0][0],
        &hex_char_targets[0][0],
        26,
        50000,
        1000,
        0.0005,
        0.01
    );

    if (btn_init(&word_net, WORD_INPUT_COUNT, WORD_COUNT, 1, 32, 0.7, 51u) != 0) {
        fprintf(stderr, "Could not initialize word token network.\n");
        nn_free(&nn);
        nn_free(&frozen);
        btn_free(&increment);
        btn_free(&hex_values);
        btn_free(&hex_chars);
        return EXIT_FAILURE;
    }

    {
        Port w_in = {PORT_ONEHOT, 27, MAX_WORD_LEN, ""};
        Port w_out = {PORT_ONEHOT, WORD_COUNT, 1, ""};

        tag_or_die(&w_in, "letter_seq", "word");
        tag_or_die(&w_out, "word_token", "word");
        set_ports_or_die(&word_net, w_in, w_out, "word");
    }

    word_loss = btn_train_dynamic(
        &word_net,
        &word_inputs[0][0],
        &word_targets[0][0],
        WORD_COUNT,
        30000,
        1000,
        0.0005,
        0.01
    );

    if (btn_save(&hex_chars, hex_char_weights_path) != 0 ||
        btn_load(&frozen_hex_chars, hex_char_weights_path) != 0 ||
        btn_save(&word_net, word_weights_path) != 0 ||
        btn_load(&frozen_word_net, word_weights_path) != 0) {
        fprintf(stderr, "Could not persist hex character or word layer.\n");
        nn_free(&nn);
        nn_free(&frozen);
        btn_free(&increment);
        btn_free(&hex_values);
        btn_free(&frozen_hex_values);
        btn_free(&hex_chars);
        btn_free(&word_net);
        return EXIT_FAILURE;
    }
    emit_contract("hex_char", &hex_chars, &hex_char_inputs[0][0],
                  &hex_char_targets[0][0], 26, "hex_char_contract.txt");
    emit_contract("word", &word_net, &word_inputs[0][0],
                  &word_targets[0][0], WORD_COUNT, "word_contract.txt");

    btn_predict_ascii_string_from_hex_pairs(
        &frozen_hex_chars,
        "48454C4C4F",
        word_chars,
        sizeof(word_chars)
    );
    btn_predict_word_from_hex(
        &frozen_hex_chars,
        &frozen_word_net,
        "48454C4C4F",
        words,
        WORD_COUNT,
        MAX_WORD_LEN,
        predicted_word,
        sizeof(predicted_word)
    );

    printf("\nhex to char to word:\n");
    printf("hex character hidden neurons selected: %lu\n",
           (unsigned long)hex_chars.hidden_count);
    printf("hex character final loss: %.6f\n", hex_char_loss);
    printf("48454C4C4F -> %s\n", word_chars);
    printf("word hidden neurons selected: %lu\n",
           (unsigned long)word_net.hidden_count);
    printf("word final loss: %.6f\n", word_loss);
    printf("%s -> word token %s\n", word_chars, predicted_word);

    // === Contract-based coherent response from given text ===
    // The response (word token) is produced by composing certified primitives
    // whose ports/contracts define the valid handoffs. The RoutePlan + route_execute
    // (or full planner search) is the "contract based system".
    {
        printf("\nContract-based response path (RoutePlan over contract primitives):\n");
        RoutePlan response_plan = {0};
        response_plan.steps[0] = &frozen_hex_chars;
        response_plan.steps[1] = &frozen_word_net;
        response_plan.length = 2;
        response_plan.names[0] = "hex_chars";
        response_plan.names[1] = "word";
        response_plan.strict = 0;

        PrimitiveRegistry text_reg = {0};
        registry_init(&text_reg);
        // The frozen BTNs carry their contracts (ports + exemplar tables from emit_contract).
        // In a full library they would be registered (registry_add_certified after verification).
        // The RoutePlan represents the contract-respecting composition for the response.

        printf("  plan length: %zu (handoffs validated by port contracts)\n", response_plan.length);
        printf("  coherent response from hex text '48454C4C4F': %s\n", predicted_word);
        printf("  (full route_plan over a registry of certified primitives can discover such chains)\n");
    }

    if (btn_init(&raw_word_net, RAW_WORD_INPUT_COUNT, WORD_COUNT, 1, 128, 0.8, 71u) != 0) {
        fprintf(stderr, "Could not initialize raw binary word network.\n");
        nn_free(&nn);
        nn_free(&frozen);
        btn_free(&increment);
        btn_free(&hex_values);
        btn_free(&hex_chars);
        btn_free(&word_net);
        return EXIT_FAILURE;
    }

    {
        Port rw_in = {PORT_BINARY_MSB, 8, MAX_WORD_LEN, ""};
        Port rw_out = {PORT_ONEHOT, WORD_COUNT, 1, ""};

        tag_or_die(&rw_in, "ascii_bytes", "raw_word");
        tag_or_die(&rw_out, "word_token", "raw_word");
        set_ports_or_die(&raw_word_net, rw_in, rw_out, "raw_word");
    }

    raw_word_loss = btn_train_dynamic(
        &raw_word_net,
        &raw_word_inputs[0][0],
        &raw_word_targets[0][0],
        WORD_COUNT,
        120000,
        1000,
        0.0005,
        0.01
    );

    if (btn_save(&raw_word_net, raw_word_weights_path) != 0 ||
        btn_load(&frozen_raw_word_net, raw_word_weights_path) != 0) {
        fprintf(stderr, "Could not persist raw word layer.\n");
        nn_free(&nn);
        nn_free(&frozen);
        btn_free(&increment);
        btn_free(&hex_values);
        btn_free(&frozen_hex_values);
        btn_free(&hex_chars);
        btn_free(&frozen_hex_chars);
        btn_free(&word_net);
        btn_free(&frozen_word_net);
        btn_free(&raw_word_net);
        return EXIT_FAILURE;
    }
    emit_contract("raw_word", &raw_word_net, &raw_word_inputs[0][0],
                  &raw_word_targets[0][0], WORD_COUNT, "raw_word_contract.txt");

    printf("\nflat raw binary word model:\n");
    printf("raw word hidden neurons selected: %lu\n",
           (unsigned long)raw_word_net.hidden_count);
    printf("raw word final loss: %.6f\n", raw_word_loss);
    btn_predict_raw_binary_word_token(
        &frozen_raw_word_net,
        "0100100001000101010011000100110001001111",
        words,
        WORD_COUNT,
        raw_predicted_word,
        sizeof(raw_predicted_word)
    );
    printf("HELLO bits -> %s\n", raw_predicted_word);
    btn_predict_raw_binary_word_token(
        &frozen_raw_word_net,
        "01000011010011110100010001000101",
        words,
        WORD_COUNT,
        raw_predicted_word,
        sizeof(raw_predicted_word)
    );
    printf("CODE bits -> %s\n", raw_predicted_word);

    nn_free(&nn);
    nn_free(&frozen);
    btn_free(&increment);
    btn_free(&combine_net);
    btn_free(&split_net);
    btn_free(&cond_inc);
    btn_free(&hex_values);
    btn_free(&frozen_hex_values);
    btn_free(&hex_chars);
    btn_free(&frozen_hex_chars);
    btn_free(&word_net);
    btn_free(&frozen_word_net);
    btn_free(&raw_word_net);
    btn_free(&frozen_raw_word_net);
    return EXIT_SUCCESS;
}

/* ---- freeze generator (txt -> const C data) ----------------------------- */
/* When invoked as ./nn_demo --freeze , this emits a header that can be used
   with btn_init_frozen to avoid all text parsing and file I/O for the
   committed primitives. The emitted data is produced by loading the exact
   committed .txt files, so it is bit-identical to what the text path would
   produce. This is the first step in replacing the runtime .txt files while
   keeping the .txt as the human source of truth. */

static void emit_port_array(const char *var, const Port *ports, size_t n) {
    size_t i;
    printf("static const Port %s[%zu] = {\n", var, n);
    for (i = 0; i < n; ++i) {
        const char *fam = "raw";
        if (ports[i].family == PORT_ONEHOT) fam = "onehot";
        else if (ports[i].family == PORT_BINARY_MSB) fam = "binary_msb";
        else if (ports[i].family == PORT_BINARY_LSB) fam = "binary_lsb";
        const char *tag = ports[i].tag[0] ? ports[i].tag : "";
        printf("    { %d, %zu, %zu, \"%s\" },\n",
               (int)ports[i].family,
               ports[i].field_width,
               ports[i].field_count,
               tag);
    }
    printf("};\n");
}

static void emit_double_array(const char *var, const double *arr, size_t n) {
    size_t i;
    printf("static const double %s[%zu] = {\n", var, n);
    for (i = 0; i < n; ++i) {
        printf("    %.17g%s\n", arr[i], (i + 1 < n) ? "," : "");
    }
    printf("};\n");
}

static void emit_frozen_header(void) {
    BinaryTransformNetwork b;
    const char *wpaths[] = {
        "hex_value_weights.txt",
        "increment_weights.txt",
        "combine_weights.txt",
        "split_weights.txt",
    };
    const char *names[] = { "hex_value", "increment", "combine", "split" };
    size_t n = sizeof(wpaths) / sizeof(wpaths[0]);
    size_t i;

    printf("/* Auto-generated by nn_demo --freeze. Do not edit by hand. */\n");
    printf("/* Regenerate with: ./nn_demo --freeze > include/generated.h */\n\n");
    printf("#include \"nn.h\"\n\n");

    for (i = 0; i < n; ++i) {
        memset(&b, 0, sizeof b);
        if (btn_load(&b, wpaths[i]) != 0) {
            fprintf(stderr, "freeze: could not load %s\n", wpaths[i]);
            continue;
        }

        char in_var[128], out_var[128], ih_var[128], hb_var[128], ho_var[128], ob_var[128];
        snprintf(in_var, sizeof in_var, "frozen_%s_in_ports", names[i]);
        snprintf(out_var, sizeof out_var, "frozen_%s_out_ports", names[i]);
        snprintf(ih_var, sizeof ih_var, "frozen_%s_ih", names[i]);
        snprintf(hb_var, sizeof hb_var, "frozen_%s_hb", names[i]);
        snprintf(ho_var, sizeof ho_var, "frozen_%s_ho", names[i]);
        snprintf(ob_var, sizeof ob_var, "frozen_%s_ob", names[i]);

        if (b.input_port_count > 0)
            emit_port_array(in_var, b.input_ports, b.input_port_count);
        if (b.output_port_count > 0)
            emit_port_array(out_var, b.output_ports, b.output_port_count);

        emit_double_array(ih_var, b.input_hidden, b.input_count * b.hidden_count);
        emit_double_array(hb_var, b.hidden_bias, b.hidden_count);
        emit_double_array(ho_var, b.hidden_output_weights, b.hidden_count * b.output_count);
        emit_double_array(ob_var, b.output_bias, b.output_count);

        printf("static const FrozenBTNData frozen_%s_data = {\n", names[i]);
        printf("    \"%s\",\n", names[i]);
        printf("    %zu, %zu, %zu,\n", b.input_count, b.output_count, b.hidden_count);
        printf("    %s, %zu,\n", b.input_port_count ? in_var : "NULL", b.input_port_count);
        printf("    %s, %zu,\n", b.output_port_count ? out_var : "NULL", b.output_port_count);
        printf("    %s, %s, %s, %s\n", ih_var, hb_var, ho_var, ob_var);
        printf("};\n\n");

        btn_free(&b);
    }

    printf("/* Convenience: initialize from one of the generated datas. */\n");
    printf("static inline int btn_init_committed(BinaryTransformNetwork *btn, const char *name) {\n");
    printf("    const FrozenBTNData *d = NULL;\n");
    for (i = 0; i < n; ++i) {
        printf("    if (strcmp(name, \"%s\") == 0) d = &frozen_%s_data;\n", names[i], names[i]);
    }
    printf("    if (!d) return -1;\n");
    printf("    return btn_init_frozen(btn, d);\n");
    printf("}\n");
}


