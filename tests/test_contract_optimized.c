#include "../include/nn.h"
#include "../include/contract/contract.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static int failures;

#define CHECK(cond, desc) do { \
    if (cond) printf("  ok   %s\n", desc); \
    else { printf("  FAIL %s\n", desc); ++failures; } \
} while (0)

typedef struct {
    double value;
    size_t calls;
} FixedAdapter;

static Port bit_port(const char *tag) {
    Port p;
    memset(&p, 0, sizeof p);
    p.family = PORT_BINARY_MSB;
    p.field_width = 1;
    p.field_count = 1;
    if (tag != NULL) (void)port_set_tag(&p, tag);
    return p;
}

static Port evidence_port(const char *tag) {
    Port p;
    memset(&p, 0, sizeof p);
    p.family = PORT_EVIDENCE;
    p.field_width = 3;
    p.field_count = 1;
    if (tag != NULL) (void)port_set_tag(&p, tag);
    return p;
}

static int evidence_forward(void *context, const double *input,
                            size_t input_count, double *output,
                            size_t output_count) {
    size_t i;
    (void)context;
    if (input == NULL || output == NULL || input_count != 3 || output_count != 3) {
        return -1;
    }
    for (i = 0; i < 3; ++i) output[i] = input[i];
    return 0;
}

static int fixed_forward(void *context, const double *input, size_t input_count,
                         double *output, size_t output_count) {
    FixedAdapter *fixed = (FixedAdapter *)context;
    if (fixed == NULL || input == NULL || input_count != 1 ||
        output == NULL || output_count != 1) return -1;
    ++fixed->calls;
    output[0] = fixed->value;
    return 0;
}

static void test_frozen_descriptor_refuses_invalid_shape(void) {
    Contract out;
    Contract before;
    FrozenContractData fd;
    Port inputs[BTN_MAX_INPUT_PORTS + 1];
    Port output = bit_port("result");
    double x = 0.0;
    double y = 1.0;
    size_t i;
    char long_name[CONTRACT_NAME_MAX + 8];

    memset(&out, 0xA5, sizeof out);
    before = out;
    memset(&fd, 0, sizeof fd);
    for (i = 0; i < BTN_MAX_INPUT_PORTS + 1; ++i) inputs[i] = bit_port("bit");
    fd.name = "oversized_contract";
    fd.input_port_count = BTN_MAX_INPUT_PORTS + 1;
    fd.output_port_count = 1;
    fd.input_ports = inputs;
    fd.output_ports = &output;
    fd.inputs = &x;
    fd.outputs = &y;
    fd.exemplar_count = 1;

    CHECK(contract_init_frozen(&out, &fd) == -1,
          "oversized frozen input-port count is refused");
    CHECK(memcmp(&out, &before, sizeof out) == 0,
          "refused frozen descriptor leaves destination untouched");

    memset(long_name, 'a', sizeof long_name);
    long_name[sizeof long_name - 1] = '\0';
    fd.name = long_name;
    fd.input_port_count = 1;
    CHECK(contract_init_frozen(&out, &fd) == -1,
          "overlong frozen contract name is refused, not truncated");

    fd.name = "ambiguous_frozen";
    x = 0.5;
    CHECK(contract_init_frozen(&out, &fd) == -1,
          "ambiguous frozen exemplar is refused");
    CHECK(memcmp(&out, &before, sizeof out) == 0,
          "all refused frozen forms preserve the destination");
}

static void test_borrowed_refuses_ambiguous_exemplars(void) {
    BinaryTransformNetwork adapter;
    Contract out;
    Contract before;
    FixedAdapter fixed = {0.99, 0};
    Port port = bit_port("bit");
    double good_input = 0.0;
    double good_target = 1.0;
    double ambiguous = 0.5;

    memset(&adapter, 0, sizeof adapter);
    CHECK(btn_init_adapter(&adapter, 1, 1, &port, 1, &port, 1,
                           fixed_forward, NULL, &fixed, 0xB0220ULL, 1) == 0,
          "borrowed-validation adapter initialized");

    memset(&out, 0xA5, sizeof out);
    before = out;
    CHECK(contract_init_borrowed(&out, "bad_input", &adapter,
                                 &ambiguous, &good_target, 1) == -1,
          "ambiguous borrowed input is refused");
    CHECK(memcmp(&out, &before, sizeof out) == 0,
          "refused borrowed input leaves destination untouched");

    CHECK(contract_init_borrowed(&out, "bad_target", &adapter,
                                 &good_input, &ambiguous, 1) == -1,
          "ambiguous borrowed target is refused");
    btn_free(&adapter);
}

static void test_evidence_contract_roundtrip(void) {
    static const char *path = "logs/contract_evidence_roundtrip.contract";
    BinaryTransformNetwork adapter;
    Contract contract;
    Contract loaded;
    Contract untouched;
    Port port = evidence_port("belief");
    double values[3] = {
        0.12345678901234566,
        0.23456789012345678,
        0.64197532086419756
    };
    double nonfinite[3] = {NAN, 0.25, 0.75};
    unsigned long long before_digest;

    memset(&adapter, 0, sizeof adapter);
    memset(&contract, 0, sizeof contract);
    memset(&loaded, 0, sizeof loaded);
    CHECK(btn_init_adapter(&adapter, 3, 3, &port, 1, &port, 1,
                           evidence_forward, NULL, NULL,
                           0xE71DEACEULL, 3) == 0,
          "evidence adapter initialized");
    CHECK(contract_init_borrowed(&contract, "evidence_roundtrip", &adapter,
                                 values, values, 1) == 0,
          "soft EVIDENCE contract initialized");
    before_digest = contract_content_digest(&contract);
    CHECK(contract_save(&contract, path) == 0,
          "soft EVIDENCE contract saved");
    CHECK(contract_load(&loaded, path) == 0,
          "soft EVIDENCE contract loads after save");
    CHECK(loaded.seal_verified == 1 &&
          contract_content_digest(&loaded) == before_digest,
          "soft EVIDENCE round-trip is bit-identical and sealed");

    memset(&untouched, 0xA5, sizeof untouched);
    CHECK(contract_init_borrowed(&untouched, "nonfinite_evidence", &adapter,
                                 nonfinite, values, 1) == -1,
          "non-finite EVIDENCE exemplar is refused");

    contract_free(&loaded);
    btn_free(&adapter);
    remove(path);
}

static void test_margin_quality_and_single_replay(void) {
    BinaryTransformNetwork active;
    BinaryTransformNetwork candidate;
    Contract contract;
    FixedAdapter active_ctx = {0.76, 0};
    FixedAdapter candidate_ctx = {0.99, 0};
    Port input_port = bit_port("input");
    Port output_port = bit_port("result");
    double input = 0.0;
    double target = 1.0;
    int better;

    memset(&active, 0, sizeof active);
    memset(&candidate, 0, sizeof candidate);
    memset(&contract, 0, sizeof contract);
    CHECK(btn_init_adapter(&active, 1, 1, &input_port, 1, &output_port, 1,
                           fixed_forward, NULL, &active_ctx, 0xA11CEULL, 1) == 0,
          "active adapter initialized");
    CHECK(btn_init_adapter(&candidate, 1, 1, &input_port, 1, &output_port, 1,
                           fixed_forward, NULL, &candidate_ctx, 0xCAAD1ULL, 1) == 0,
          "candidate adapter initialized");
    CHECK(contract_init_borrowed(&contract, "margin_quality", &candidate,
                                 &input, &target, 1) == 0,
          "quality contract initialized");

    contract_cache_reset();
    better = contract_better_if(&contract, &active, &candidate);
    CHECK(better == 1,
          "higher-margin certified candidate is preferred");
    CHECK(active_ctx.calls == 1 && candidate_ctx.calls == 1,
          "candidate comparison uses one certification replay per model");

    {
        const size_t iterations = 2000;
        size_t i;
        clock_t start;
        double elapsed_ms;
        active_ctx.calls = 0;
        candidate_ctx.calls = 0;
        start = clock();
        for (i = 0; i < iterations; ++i) {
            contract_cache_reset();
            if (contract_better_if(&contract, &active, &candidate) != 1) {
                ++failures;
                break;
            }
        }
        elapsed_ms = (double)(clock() - start) * 1000.0 / CLOCKS_PER_SEC;
        printf("CONTRACT_OPT_BENCH iterations=%zu forwards=%zu elapsed_ms=%.3f\n",
               iterations, active_ctx.calls + candidate_ctx.calls, elapsed_ms);
        CHECK(active_ctx.calls + candidate_ctx.calls == iterations * 2,
              "promotion benchmark performs exactly two forwards per comparison");
    }

    btn_free(&active);
    btn_free(&candidate);
}

int main(void) {
    test_frozen_descriptor_refuses_invalid_shape();
    test_borrowed_refuses_ambiguous_exemplars();
    test_evidence_contract_roundtrip();
    test_margin_quality_and_single_replay();
    if (failures == 0) {
        printf("CONTRACT_OPTIMIZED_PASS\n");
        return 0;
    }
    printf("CONTRACT_OPTIMIZED_FAIL failures=%d\n", failures);
    return 1;
}
