#include <stdio.h>
#include <string.h>

#include "../include/acquire.h"
#include "../include/contract/contract.h"
#include "../include/router.h"
#include "../include/specialist_adapters.h"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (cond) printf("  ok: %s\n", (msg)); \
    else { printf("  FAIL: %s\n", (msg)); ++failures; } \
} while (0)

static int invert_oracle(const double *in, double *out, void *ctx) {
    (void)ctx;
    out[0] = in[0] > 0.5 ? 0.0 : 1.0;
    return 0;
}

static int abstain_oracle(const double *in, double *out, void *ctx) {
    (void)in; (void)out; (void)ctx;
    return 1;
}

typedef struct { CnetOracleValidity validity; } V2Fixture;

static int invert_oracle_v2(const double *in, size_t in_count,
                            double *out, size_t out_count,
                            CnetOracleResult *result, void *ctx) {
    (void)ctx;
    if (in_count != 1 || out_count != 1) return -1;
    memset(result, 0, sizeof *result);
    result->abi_version = CNET_ORACLE_ABI_VERSION;
    result->struct_size = (uint32_t)sizeof *result;
    result->status = CNET_ORACLE_ANSWER;
    result->confidence = 0.9;
    result->evidence_digest = 0xabcdu;
    out[0] = in[0] > 0.5 ? 0.0 : 1.0;
    return 0;
}

static CnetOracleValidity validate_v2(
    const double *in, size_t in_count,
    const double *out, size_t out_count,
    const CnetOracleResult *result, void *ctx) {
    V2Fixture *fixture = (V2Fixture *)ctx;
    (void)in; (void)in_count; (void)out; (void)out_count; (void)result;
    return fixture->validity;
}

static Port bit_port(const char *tag) {
    Port p;
    memset(&p, 0, sizeof p);
    p.family = PORT_BINARY_MSB;
    p.field_width = 1;
    p.field_count = 1;
    snprintf(p.tag, sizeof p.tag, "%s", tag);
    return p;
}

int main(void) {
    OracleRegistry oracles;
    BinaryTransformNetwork adapter;
    PrimitiveRegistry registry;
    Contract contract;
    double inputs[2] = {0.0, 1.0};
    double targets[2] = {1.0, 0.0};
    const double *out;
    V2Fixture v2_fixture;
    CnetOracleIdentity identity;

    printf("== oracle specialist adapter ==\n");
    memset(&oracles, 0, sizeof oracles);
    memset(&adapter, 0, sizeof adapter);
    memset(&contract, 0, sizeof contract);
    registry_init(&registry);

    CHECK(acquire_oracle_register(&oracles, "invert_ref", bit_port("bit"),
                                  bit_port("not_bit"), invert_oracle, NULL) == 0,
          "oracle registers");
    CHECK(cnet_oracle_init_contract_adapter(&adapter, &oracles.entries[0],
                                            0x4f5241434c450001ULL, 1) == 0,
          "oracle projects into the universal specialist ABI");
    CHECK(contract_init_borrowed(&contract, "invert_ref", &adapter,
                                 inputs, targets, 2) == 0,
          "typed contract binds to oracle adapter");
    CHECK(registry_add_certified(&registry, &adapter, "invert_ref", &contract) == 0,
          "oracle adapter certifies through the canonical registry");

    out = btn_forward(&adapter, inputs);
    CHECK(out != NULL && out[0] == 1.0, "adapter executes the oracle");
    CHECK(oracles.entries[0].calls >= 3 && oracles.entries[0].rejects == 0,
          "oracle evidence counters include certification and direct execution");

    contract_free(&contract);
    registry_free(&registry);
    btn_free(&adapter);

    memset(&adapter, 0, sizeof adapter);
    memset(&oracles, 0, sizeof oracles);
    CHECK(acquire_oracle_register(&oracles, "abstain_ref", bit_port("bit"),
                                  bit_port("not_bit"), abstain_oracle, NULL) == 0,
          "abstaining oracle registers");
    CHECK(cnet_oracle_init_contract_adapter(&adapter, &oracles.entries[0],
                                            0x4f5241434c450002ULL, 1) == 0,
          "abstaining oracle projects");
    CHECK(btn_forward(&adapter, inputs) == NULL &&
          oracles.entries[0].abstains == 1,
          "oracle abstention becomes canonical execution refusal");
    btn_free(&adapter);

    memset(&adapter, 0, sizeof adapter);
    memset(&oracles, 0, sizeof oracles);
    memset(&identity, 0, sizeof identity);
    identity.abi_version = CNET_ORACLE_ABI_VERSION;
    identity.struct_size = (uint32_t)sizeof identity;
    identity.artifact_digest = 0x1111u;
    identity.contract_digest = 0x2222u;
    v2_fixture.validity = CNET_ORACLE_VALID;
    CHECK(acquire_oracle_register_v2(&oracles, "invert_v2", bit_port("bit"),
                                     bit_port("not_bit"), invert_oracle_v2,
                                     validate_v2, &identity, &v2_fixture) == 0,
          "evidence-carrying oracle registers");
    CHECK(cnet_oracle_init_contract_adapter(&adapter, &oracles.entries[0],
                                            oracles.entries[0].behavior_digest, 1) == 0,
          "v2 oracle projects into the same specialist ABI");
    out = btn_forward(&adapter, inputs);
    CHECK(out && out[0] == 1.0 &&
          oracles.entries[0].last_result.evidence_digest == 0xabcdu,
          "runtime execution preserves v2 evidence metadata");
    v2_fixture.validity = CNET_ORACLE_INVALID;
    CHECK(btn_forward(&adapter, inputs) == NULL &&
          oracles.entries[0].last_result.status == CNET_ORACLE_INVALID_OUTPUT,
          "semantic invalidity becomes governed runtime refusal");
    btn_free(&adapter);

    printf("ORACLE_CONTRACT_ADAPTER_%s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
