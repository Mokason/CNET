#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../include/acquire.h"

static int failures;
#define CHECK(cond, msg) do { \
    if (cond) printf("  ok: %s\n", (msg)); \
    else { printf("  FAIL: %s\n", (msg)); ++failures; } \
} while (0)

typedef struct {
    CnetOracleStatus status;
    CnetOracleValidity validity;
    int malformed_abi;
    double confidence;
    uint64_t evidence_digest;
} OracleFixture;

static Port bit_port(const char *tag) {
    Port p;
    memset(&p, 0, sizeof p);
    p.family = PORT_BINARY_MSB;
    p.field_width = 1;
    p.field_count = 1;
    snprintf(p.tag, sizeof p.tag, "%s", tag);
    return p;
}

static CnetOracleIdentity fixture_identity(void) {
    CnetOracleIdentity id;
    memset(&id, 0, sizeof id);
    id.abi_version = CNET_ORACLE_ABI_VERSION;
    id.struct_size = (uint32_t)sizeof id;
    id.artifact_digest = UINT64_C(0x1111222233334444);
    id.contract_digest = UINT64_C(0x5555666677778888);
    id.config_digest = UINT64_C(0x0102030405060708);
    id.retrieval_snapshot_digest = UINT64_C(0x1020304050607080);
    id.toolchain_digest = UINT64_C(0xa1a2a3a4a5a6a7a8);
    return id;
}

static int oracle_v2(const double *in, size_t in_count,
                     double *out, size_t out_count,
                     CnetOracleResult *result, void *ctx) {
    OracleFixture *f = (OracleFixture *)ctx;
    if (!in || !out || in_count != 1 || out_count != 1 || !result || !f) return -1;
    memset(result, 0, sizeof *result);
    result->abi_version = f->malformed_abi ? 999u : CNET_ORACLE_ABI_VERSION;
    result->struct_size = (uint32_t)sizeof *result;
    result->status = f->status;
    result->confidence = f->confidence;
    result->evidence_digest = f->evidence_digest;
    out[0] = in[0] > 0.5 ? 0.0 : 1.0;
    return 0;
}

static CnetOracleValidity semantic_validator(
    const double *in, size_t in_count,
    const double *out, size_t out_count,
    const CnetOracleResult *result, void *ctx) {
    OracleFixture *f = (OracleFixture *)ctx;
    (void)in; (void)in_count; (void)out; (void)out_count; (void)result;
    return f ? f->validity : CNET_ORACLE_VALIDITY_UNDETERMINED;
}

static int legacy_refuse(const double *in, double *out, void *ctx) {
    (void)in; (void)out; (void)ctx;
    return -1;
}

int main(void) {
    OracleRegistry registry;
    OracleFixture fixture;
    CnetOracleIdentity identity, changed;
    CnetOracleResult result;
    CnetOracleStatus status;
    OracleEntry *entry;
    PrimitiveRegistry primitives;
    AcquireLedger ledger;
    AcquireConfig config;
    double input[1] = {0.0};
    double output[1] = {-1.0};
    uint64_t digest;

    printf("== Oracle v2 evidence lifecycle ==\n");
    memset(&registry, 0, sizeof registry);
    memset(&fixture, 0, sizeof fixture);
    fixture.status = CNET_ORACLE_ANSWER;
    fixture.validity = CNET_ORACLE_VALID;
    fixture.confidence = 0.95;
    fixture.evidence_digest = UINT64_C(0xfeedbeef);
    identity = fixture_identity();

    digest = cnet_oracle_identity_digest(&identity);
    changed = identity;
    changed.retrieval_snapshot_digest++;
    CHECK(digest != 0, "component identity produces a stable non-zero digest");
    CHECK(digest == cnet_oracle_identity_digest(&identity),
          "identity digest is deterministic");
    CHECK(digest != cnet_oracle_identity_digest(&changed),
          "retrieval snapshot participates in behavior identity");

    CHECK(acquire_oracle_register_v2(&registry, "invert_v2",
                                     bit_port("bit"), bit_port("not_bit"),
                                     oracle_v2, semantic_validator,
                                     &identity, &fixture) == 0,
          "v2 oracle registers with bounded identity and semantic validator");
    entry = &registry.entries[0];
    CHECK(entry->behavior_digest == digest,
          "registry binds the complete component identity digest");

    memset(&result, 0, sizeof result);
    status = cnet_oracle_invoke(entry, input, 1, output, 1, &result);
    CHECK(status == CNET_ORACLE_ANSWER && output[0] == 1.0,
          "typed answer passes type and semantic validation");
    CHECK(result.confidence == 0.95 && result.evidence_digest == UINT64_C(0xfeedbeef),
          "confidence and evidence survive invocation");
    CHECK(entry->calls == 1 && entry->status_counts[CNET_ORACLE_ANSWER] == 1,
          "answer evidence is accounted by first-class status");

    fixture.status = CNET_ORACLE_ABSTAIN_AMBIGUOUS;
    status = cnet_oracle_invoke(entry, input, 1, output, 1, &result);
    CHECK(status == CNET_ORACLE_ABSTAIN_AMBIGUOUS && entry->abstains == 1,
          "teacher ambiguity remains distinct abstention evidence");

    fixture.status = CNET_ORACLE_REFUSE_POLICY;
    status = cnet_oracle_invoke(entry, input, 1, output, 1, &result);
    CHECK(status == CNET_ORACLE_REFUSE_POLICY &&
          entry->status_counts[CNET_ORACLE_REFUSE_POLICY] == 1,
          "policy refusal is not collapsed into implementation failure");

    fixture.status = CNET_ORACLE_FAIL_TRANSIENT;
    status = cnet_oracle_invoke(entry, input, 1, output, 1, &result);
    CHECK(status == CNET_ORACLE_FAIL_TRANSIENT &&
          entry->status_counts[CNET_ORACLE_FAIL_TRANSIENT] == 1,
          "transient failure is retry-distinguishable");

    fixture.status = CNET_ORACLE_ANSWER;
    fixture.validity = CNET_ORACLE_VALIDITY_UNDETERMINED;
    status = cnet_oracle_invoke(entry, input, 1, output, 1, &result);
    CHECK(status == CNET_ORACLE_ABSTAIN_UNDETERMINED && entry->abstains == 2,
          "undetermined semantic validity abstains instead of fabricating a label");

    fixture.validity = CNET_ORACLE_INVALID;
    status = cnet_oracle_invoke(entry, input, 1, output, 1, &result);
    CHECK(status == CNET_ORACLE_INVALID_OUTPUT &&
          entry->status_counts[CNET_ORACLE_INVALID_OUTPUT] == 1,
          "semantically invalid typed output is rejected explicitly");

    fixture.validity = CNET_ORACLE_VALID;
    fixture.confidence = 1.5;
    status = cnet_oracle_invoke(entry, input, 1, output, 1, &result);
    CHECK(status == CNET_ORACLE_INVALID_OUTPUT,
          "out-of-range confidence invalidates an answer");

    fixture.confidence = 0.8;
    fixture.malformed_abi = 1;
    status = cnet_oracle_invoke(entry, input, 1, output, 1, &result);
    CHECK(status == CNET_ORACLE_FAIL_PERMANENT,
          "malformed result ABI fails permanently rather than being trusted");

    CHECK(entry->calls == 8 && entry->rejects == 5 && entry->abstains == 2,
          "legacy aggregate counters remain compatible and honest");

    fixture.status = CNET_ORACLE_ANSWER;
    fixture.validity = CNET_ORACLE_VALID;
    fixture.malformed_abi = 0;
    fixture.confidence = 0.95;
    output[0] = -1.0;
    registry_init(&primitives);
    acquire_ledger_init(&ledger);
    acquire_config_defaults(&config);
    CHECK(acquire_execute_or_fallback(&primitives, &ledger, &registry, &config,
                                      bit_port("bit"), bit_port("not_bit"),
                                      input, 1, output, 1) == 0 && output[0] == 1.0,
          "gap fallback uses the governed v2 invocation path");
    CHECK(ledger.count == 1 && ledger.gaps[0].cap_count == 1 &&
          entry->last_result.evidence_digest == UINT64_C(0xfeedbeef),
          "fallback capture remains linked to Oracle evidence");
    config.min_evidence = 2;
    config.evidence_threshold = 1.0;
    config.pilot_count = 0;
    config.init_hidden = 2;
    config.max_hidden = 8;
    config.max_epochs = 8000;
    {
        AcquireReport report;
        memset(&report, 0, sizeof report);
        CHECK(acquire_drain(&primitives, &ledger, &registry, &config, &report) == 0 &&
              report.closed == 1 && primitives.count == 1,
              "v2 Oracle evidence trains and independently certifies a specialist");
    }
    acquire_ledger_free(&ledger);
    registry_free(&primitives);

    memset(&registry, 0, sizeof registry);
    CHECK(acquire_oracle_register(&registry, "legacy_refuse", bit_port("bit"),
                                  bit_port("not_bit"), legacy_refuse, NULL) == 0,
          "v1 oracle registration remains supported");
    status = cnet_oracle_invoke(&registry.entries[0], input, 1, output, 1, &result);
    CHECK(status == CNET_ORACLE_FAIL_PERMANENT &&
          registry.entries[0].rejects == 1,
          "v1 negative return maps into explicit permanent failure");

    printf("ORACLE_V2_%s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
