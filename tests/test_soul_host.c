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

    remove("tmp_soul_host.inbox");
    setenv("CNET_GAP_INBOX", "tmp_soul_host.inbox", 1);

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

    {
        long long counts[SOUL_HEALTH_COUNTS];
        int trust = -1, role = -1;
        int n = soul_health_tick(host, counts, SOUL_HEALTH_COUNTS);
        check(n == SOUL_HEALTH_COUNTS,
              "health tick reports the full count vector");
        check(n == SOUL_HEALTH_COUNTS &&
              counts[0] == 1 && counts[1] == 0 && counts[2] == 0 &&
              counts[3] == 0 && counts[4] == 0 && counts[5] == 0 &&
              counts[6] == 0 && counts[7] == 0 && counts[8] == 0,
              "healthy soul: the tick is a proven no-op");
        check(n == SOUL_HEALTH_COUNTS &&
              counts[9] + counts[10] + counts[11] + counts[12] == 1 &&
              counts[11] == 1,
              "trust histogram: the one unit reads certified");
        check(soul_unit_axes(host, unit_name, &trust, &role) == 0 &&
              trust == 2 /* SPECIALIST_TRUST_CERTIFIED */ &&
              role == 0 /* SPECIALIST_ROLE_ACTIVE */,
              "unit axes read certified/active over the ABI");
        check(soul_run(host, unit_name, input, output, 2) == 2,
              "unit still executes after the health tick");
    }

    {
        double req_out[2] = {0.0, 0.0};
        FILE *ib;
        check(soul_request(host, PORT_ONEHOT, 2, 1, "unified_input",
                           PORT_ONEHOT, 2, 1, "unified_goal",
                           input, 2, req_out, 2) == 2 &&
              req_out[0] == expected[0] && req_out[1] == expected[1],
              "explicit-signature request serves through the planner");
        check(soul_request(host, PORT_ONEHOT, 2, 1, "unified_input",
                           PORT_ONEHOT, 2, 1, "unified_goal",
                           NULL, 0, NULL, 0) == 0,
              "capability probe answers plannable without executing");
        check(soul_request(host, PORT_ONEHOT, 4, 1, "novel_symbols",
                           PORT_ONEHOT, 4, 1, "novel_goal",
                           NULL, 0, NULL, 0) == -3,
              "novel goal returns no-plan");
        ib = fopen("tmp_soul_host.inbox", "r");
        check(ib != NULL, "novel goal landed in the gap inbox for the lane");
        if (ib) {
            char line[256] = {0};
            check(fgets(line, sizeof line, ib) != NULL &&
                  strncmp(line, "NO_PLAN ", 8) == 0 &&
                  strstr(line, "novel_goal") != NULL,
                  "inbox line carries the full novel signature");
            fclose(ib);
        }
        remove("tmp_soul_host.inbox");
    }

    soul_close(host);
    contract_free(&contract);
    btn_free(&btn);
    if (getenv("CNET_KEEP_TEST_BASE") == NULL) {
        remove(base_path);
        remove("tmp_soul_host.cnb.tmp");
    }

    /* -- name stability across roster growth (regression) ---------------
       RegistryEntry.name borrows the base's per-unit name buffers; a
       growable row array dangled every earlier borrower when realloc moved
       (hit live at 254 units, 2026-07-11: soul_unit_name(82) returned
       garbage and route_plan missed a sealed unit). 13 units cross the
       first capacity doubling; under ASan realloc always moves. */
    {
        const char *tags2[13] = {"rs00rs00", "rs11rs11", "rs22rs22",
                                 "rs33rs33", "rs44rs44", "rs55rs55",
                                 "rs66rs66", "rs77rs77", "rs88rs88",
                                 "rs99rs99", "rt00rt00", "rt11rt11",
                                 "rt22rt22"};
        CnetBase base2;
        SoulHost *host2 = NULL;
        BinaryTransformNetwork extra[13];
        Contract c2[13];
        int k, ok = 1, reused2 = 0;
        remove("tmp_soul_host2.cnb");
        cnb_init(&base2);
        for (k = 0; k < 13; ++k) {
            char uname[32];
            Port op = port(PORT_ONEHOT, 2, tags2[k]);
            const double *raw2;
            double exp2[2];
            memset(&extra[k], 0, sizeof extra[k]);
            memset(&c2[k], 0, sizeof c2[k]);
            snprintf(uname, sizeof uname, "acq_reg_stab_%02d", k);
            if (btn_init(&extra[k], 2, 2, 2, 2, 0.1, 17u) != 0) { ok = 0; break; }
            extra[k].output_bias[0] = 10.0;
            extra[k].output_bias[1] = -10.0;
            memset(extra[k].hidden_output_weights, 0,
                   extra[k].hidden_count * extra[k].output_count *
                   sizeof(double));
            if (btn_set_ports(&extra[k], in_port, op) != 0) { ok = 0; break; }
            raw2 = btn_forward(&extra[k], input);
            if (!raw2 || port_canonicalize(op, raw2, exp2) != 0 ||
                contract_init_borrowed(&c2[k], uname, &extra[k], input,
                                       exp2, 1) != 0 ||
                cnb_add_unit(&base2, &extra[k], &c2[k], &reused2) != 0) {
                ok = 0;
                break;
            }
        }
        check(ok, "growth fixture seals 13 units");
        check(cnb_save(&base2, "tmp_soul_host2.cnb") == 0, "growth base saves");
        cnb_free(&base2);
        check(soul_open("tmp_soul_host2.cnb", NULL, &host2) == 0 &&
              soul_unit_count(host2) == 13,
              "13-unit roster loads past the capacity doubling");
        ok = 1;
        for (k = 0; k < 13 && ok; ++k) {
            char nm[CNB_NAME_MAX];
            if (soul_unit_name(host2, k, nm, (int)sizeof nm) != 0 ||
                strncmp(nm, "acq_", 4) != 0)
                ok = 0;
        }
        check(ok, "every roster name survives array growth intact");
        check(soul_request(host2, PORT_ONEHOT, 2, 1, "unified_input",
                           PORT_ONEHOT, 2, 1, "rt11rt11",
                           NULL, 0, NULL, 0) == 0,
              "the last-sealed unit is plannable after growth");
        soul_close(host2);
        for (k = 0; k < 13; ++k) {
            contract_free(&c2[k]);
            btn_free(&extra[k]);
        }
        remove("tmp_soul_host2.cnb");
    }

    printf("SOUL_HOST_UNIFIED_%s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
