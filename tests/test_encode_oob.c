/*
 * Regression test for the past-the-terminator out-of-bounds read in
 * encode_word_input (nn.c). ASan caught the original bug on Linux; this
 * machine's MinGW has no ASan runtime, so instead of relying on a fault we
 * pin the *semantic* invariant the bug violated:
 *
 *   Every position at or after the word's terminator must encode the padding
 *   token (symbol 0), regardless of what bytes sit past the terminator.
 *
 * The fixed encoder honours this by construction (it never indexes past
 * word_len). To prove these assertions actually distinguish fixed-from-broken,
 * we keep a copy of the ORIGINAL logic and show it leaks post-terminator
 * bytes into the encoding. The shared input buffer is fully initialised, so
 * the buggy variant reads defined memory here (no UB in the test itself) while
 * still exhibiting the broken semantics.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Pulls in the real, static encode_word_input + word_symbol_index. */
#include "../src/nn.c"

/* The pre-fix logic, kept only to give the assertions teeth. */
static int encode_word_input_buggy(const char *word, double *inputs, size_t max_word_len) {
    size_t position;
    size_t symbol;

    if (word == NULL || inputs == NULL || strlen(word) > max_word_len) {
        return -1;
    }

    for (position = 0; position < max_word_len; ++position) {
        int active = 0;

        if (word[position] != '\0') {
            active = word_symbol_index(word[position]);
        }

        for (symbol = 0; symbol < 27; ++symbol) {
            inputs[position * 27 + symbol] = symbol == (size_t)active ? 1.0 : 0.0;
        }
    }

    return 0;
}

static int is_padding_at(const double *enc, size_t pos) {
    size_t s;

    if (enc[pos * 27 + 0] != 1.0) {
        return 0;
    }
    for (s = 1; s < 27; ++s) {
        if (enc[pos * 27 + s] != 0.0) {
            return 0;
        }
    }
    return 1;
}

int run_test_encode_oob(void) {
    /* "AB" + terminator + poison uppercase bytes. word_len == 2,
       max_word_len == 5. Bytes 3..5 are valid memory so the buggy variant is
       observable rather than a crash. */
    char buf[8];
    double enc_fixed[5 * 27];
    double enc_buggy[5 * 27];
    const size_t max_word_len = 5;

    memset(buf, 0, sizeof(buf));
    buf[0] = 'A';
    buf[1] = 'B';
    buf[2] = '\0';
    buf[3] = 'X';
    buf[4] = 'Y';
    buf[5] = 'Z';

    assert(encode_word_input(buf, enc_fixed, max_word_len) == 0);
    assert(encode_word_input_buggy(buf, enc_buggy, max_word_len) == 0);

    /* The fix: positions at/after the terminator are padding, period. */
    assert(is_padding_at(enc_fixed, 0) == 0); /* 'A' is a real symbol */
    assert(enc_fixed[0 * 27 + word_symbol_index('A')] == 1.0);
    assert(enc_fixed[1 * 27 + word_symbol_index('B')] == 1.0);
    assert(is_padding_at(enc_fixed, 2));
    assert(is_padding_at(enc_fixed, 3));
    assert(is_padding_at(enc_fixed, 4));

    /* Teeth: the OLD logic leaked buf[3]='X', buf[4]='Y' into the encoding,
       so positions 3 and 4 were NOT padding. If a future edit reverts the fix,
       enc_fixed would look like enc_buggy and the asserts above would fire. */
    assert(!is_padding_at(enc_buggy, 3));
    assert(!is_padding_at(enc_buggy, 4));
    assert(enc_buggy[3 * 27 + word_symbol_index('X')] == 1.0);
    assert(enc_buggy[4 * 27 + word_symbol_index('Y')] == 1.0);

    printf("OOB regression PASS: fixed encoder pads past terminator; "
           "old logic leaked bytes %c,%c into positions 3,4.\n", buf[3], buf[4]);
    return 0;
}
