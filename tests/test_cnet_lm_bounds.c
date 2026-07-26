#include <stdio.h>
#include <string.h>

#include "../include/cnet_lm.h"

static int failures;

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s\n", message); \
        failures++; \
    } \
} while (0)

static void test_oversized_artifact_is_rejected(void) {
    const char *path = "tmp_cnet_lm_oversized.btn";
    BinaryTransformNetwork oversized = {0};
    CnetLmModel model;
    int original_input;
    int original_output;

    CHECK(btn_init(&oversized, CNET_LM_MAX_VOCAB + 1, 4, 1, 4, 0.01, 7) == 0,
          "create oversized BTN artifact");
    CHECK(btn_save(&oversized, path) == 0, "save oversized BTN artifact");
    btn_free(&oversized);

    cnet_lm_init(&model);
    original_input = model.input_dim;
    original_output = model.output_dim;
    CHECK(cnet_lm_load(&model, path, NULL) == -1,
          "reject artifact dimension larger than fixed vocabulary storage");
    CHECK(model.input_dim == original_input && model.output_dim == original_output,
          "rejected artifact does not damage the existing model");
    CHECK(model.vocab.size <= CNET_LM_MAX_VOCAB,
          "failed artifact load cannot enlarge vocabulary");
    cnet_lm_free(&model);
    remove(path);
}

static void test_seed_copy_is_bounded_and_alias_safe(void) {
    CnetLmModel model;
    char out[9];
    char alias[9] = "12345678";

    cnet_lm_init(&model);
    model.has_cce = true;
    model.input_dim = 1;
    model.output_dim = 1;

    memset(out, 'X', sizeof(out));
    int len = cnet_lm_generate(&model, "abcdefghijklmnopqrstuvwxyz", out, 8);
    CHECK(len == 7, "oversized seed is truncated to output capacity");
    CHECK(strcmp(out, "abcdefg") == 0, "oversized seed remains terminated");
    CHECK(out[8] == 'X', "oversized seed does not overwrite guard byte");

    len = cnet_lm_generate(&model, alias, alias, 8);
    CHECK(len == 7, "aliased seed respects output capacity");
    CHECK(strcmp(alias, "1234567") == 0, "aliased seed copy is overlap-safe");

    model.has_cce = false;
    cnet_lm_free(&model);
}

int main(void) {
    test_oversized_artifact_is_rejected();
    test_seed_copy_is_bounded_and_alias_safe();
    if (failures != 0) return 1;
    puts("CNET_LM_BOUNDS_PASS");
    return 0;
}
