/* Self-contained acceptance test for the public soul_host ABI.
 * Builds a real sealed CNB, opens it through the host, enumerates the
 * certification-replayed registry, and proves named execution and routing
 * share one live evidence stream.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/soul_host.h"
#include "../include/base.h"
#include "../include/contract/contract.h"
#include "../include/nn.h"

static int failures;

static void check(int ok, const char *name) {
    printf("  %-58s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static Port port(PortFamily family, size_t width, const char *tag) {
    Port p;
    memset(&p, 0, sizeof p);
    p.family = family;
    p.field_width = width;
    p.field_count = 1;
    snprintf(p.tag, sizeof p.tag, "%s", tag);
    return p;
}

int main(void) {
    const char *base_path = "tmp_soul_host.cnb";
    const char *unit_name = "acq_unified_goal";
    const double input[2] = {1.0, 0.0};
    double expected[2] = {0.0, 0.0};
    double output[2] = {0.0, 0.0};
    BinaryTransformNetwork btn;
    Contract contract;
    CnetBase base;
    SoulHost *host = NULL;
    Port in_port = port(PORT_ONEHOT, 2, "unified_input");
    Port out_port = port(PORT_ONEHOT, 2, "unified_goal");
    CnetOracleIdentity oracle_identity;
    const double *raw;
    char name[CNB_NAME_MAX];
    int reused = 0;

    memset(&btn, 0, sizeof btn);
    memset(&contract, 0, sizeof contract);
    remove(base_path);
    remove("tmp_soul_host.cnb.tmp");

    printf("== soul_host: canonical certified runtime ==\n");
    check(btn_init(&btn, 2, 2, 2, 2, 0.1, 17u) == 0,
          "matrix primitive initializes");
    /* Deterministic valid ONEHOT output without a training loop. */
    btn.output_bias[0] = 10.0;
    btn.output_bias[1] = -10.0;
    memset(btn.hidden_output_weights, 0,
           btn.hidden_count * btn.output_count * sizeof(double));
    check(btn_set_ports(&btn, in_port, out_port) == 0,
          "typed contract ports attach");
    raw = btn_forward(&btn, input);
    check(raw != NULL && port_canonicalize(out_port, raw, expected) == 0,
          "reference exemplar canonicalizes");
    check(contract_init_borrowed(&contract, unit_name, &btn,
                                 input, expected, 1) == 0,
          "contract captures real exemplar");
    check(btn_certify(&btn, &contract, NULL) == 0,
          "unit certifies before persistence");

    cnb_init(&base);
    check(cnb_add_unit(&base, &btn, &contract, &reused) == 0 && !reused,
          "unit enters unified base");
    memset(&oracle_identity, 0, sizeof oracle_identity);
    oracle_identity.abi_version = CNET_ORACLE_ABI_VERSION;
    oracle_identity.struct_size = (uint32_t)sizeof oracle_identity;
    oracle_identity.artifact_digest = 0x1010u;
    oracle_identity.contract_digest = 0x2020u;
    oracle_identity.config_digest = 0x3030u;
    oracle_identity.retrieval_snapshot_digest = 0x4040u;
    oracle_identity.toolchain_digest = 0x5050u;
    check(cnb_add_oracle_desc_v2(&base, "unified_teacher", "builtin",
                                 in_port, out_port, &oracle_identity) == 0,
          "evidence-carrying Oracle descriptor enters unified base");
    check(cnb_save(&base, base_path) == 0,
          "unified base saves atomically");
    cnb_free(&base);

    check(soul_open(base_path, NULL, &host) == 0 && host != NULL,
          "host opens and replays certification");
    check(soul_unit_count(host) == 1,
          "host enumerates certified registry count");
    memset(name, 0, sizeof name);
    check(soul_unit_name(host, 0, name, (int)sizeof name) == 0 &&
          strcmp(name, unit_name) == 0,
          "host enumerates canonical unit name");
    check(soul_unit_name(host, 1, name, (int)sizeof name) < 0,
          "host refuses out-of-range enumeration");

    check(soul_oracle_count(host) == 1,
          "host enumerates native Oracle descriptors");
    {
        char oracle_name[CNB_NAME_MAX] = {0};
        char oracle_kind[CNB_NAME_MAX] = {0};
        uint64_t behavior = 0, artifact = 0, contract_digest = 0;
        uint64_t config_digest = 0, retrieval = 0, toolchain = 0;
        check(soul_oracle_name(host, 0, oracle_name, (int)sizeof oracle_name) == 0 &&
              strcmp(oracle_name, "unified_teacher") == 0,
              "host projects authoritative Oracle name");
        check(soul_oracle_kind(host, 0, oracle_kind, (int)sizeof oracle_kind) == 0 &&
              strcmp(oracle_kind, "builtin") == 0,
              "host projects authoritative Oracle kind");
        check(soul_oracle_identity(host, 0, &behavior, &artifact, &contract_digest,
                                   &config_digest, &retrieval, &toolchain) == 0 &&
              behavior == cnet_oracle_identity_digest(&oracle_identity) &&
              artifact == oracle_identity.artifact_digest &&
              contract_digest == oracle_identity.contract_digest &&
              config_digest == oracle_identity.config_digest &&
              retrieval == oracle_identity.retrieval_snapshot_digest &&
              toolchain == oracle_identity.toolchain_digest,
              "host projects complete bounded Oracle identity");
    }

    {
        int in_total = 0, out_total = 0;
        check(soul_unit_dims(host, unit_name, &in_total, &out_total) == 0 &&
              in_total == 2 && out_total == 2,
              "dimensions come from live registry entry");
    }
    check(soul_run(host, unit_name, input, output, 2) == 2,
          "named execution uses live registry entry");
    check(soul_route(host, "unified_goal", input, 2, output, 2) == 2 &&
          output[0] == expected[0] && output[1] == expected[1],
          "typed route executes canonical primitive");
    check(soul_unit_reliability_milli(host, unit_name) > 500,
          "named execution and route update one evidence stream");

    soul_close(host);
    contract_free(&contract);
    btn_free(&btn);
    if (getenv("CNET_KEEP_TEST_BASE") == NULL) {
        remove(base_path);
        remove("tmp_soul_host.cnb.tmp");
    }

    printf("SOUL_HOST_UNIFIED_%s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
