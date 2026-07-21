/*
 * test_cnet_evidence_bundle.c — hermetic gate for unit evidence bundles.
 * make evidence_bundle → EVIDENCE_BUNDLE_PASS
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../include/base.h"
#include "../include/contract/contract.h"
#include "../include/cnet_evidence_bundle.h"
#include "../include/nn.h"

static int failures, checks;

static void check(int ok, const char *name) {
    checks++;
    printf("  %-62s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static Port port(PortFamily fam, size_t w, const char *tag) {
    Port p;
    memset(&p, 0, sizeof p);
    p.family = fam;
    p.field_width = w;
    p.field_count = 1;
    snprintf(p.tag, sizeof p.tag, "%s", tag);
    return p;
}

int main(void) {
    const char *base_path = "tmp_evidence_bundle.cnb";
    const char *store_path = "tmp_evidence_bundle.cnb.evidence.jsonl";
    const char *unit = "acq_ev_goal";
    const double input[2] = {1.0, 0.0};
    double expected[2] = {0.0, 0.0};
    BinaryTransformNetwork btn;
    Contract contract;
    CnetBase base;
    CnetEvidenceBundle b, b2, live;
    CnetEvidenceOpts opts;
    char json[2048];
    char defpath[128];
    int reused = 0;
    const double *raw;

    printf("== cnet_evidence_bundle ==\n");
    remove(base_path);
    remove(store_path);

    memset(&btn, 0, sizeof btn);
    memset(&contract, 0, sizeof contract);
    check(btn_init(&btn, 2, 2, 2, 2, 0.1, 3u) == 0, "btn init");
    btn.output_bias[0] = 10.0;
    btn.output_bias[1] = -10.0;
    memset(btn.hidden_output_weights, 0,
           btn.hidden_count * btn.output_count * sizeof(double));
    check(btn_set_ports(&btn, port(PORT_ONEHOT, 2, "ev_in"),
                        port(PORT_ONEHOT, 2, "ev_goal")) == 0,
          "ports");
    raw = btn_forward(&btn, input);
    check(raw && port_canonicalize(btn.output_ports[0], raw, expected) == 0,
          "canonicalize");
    check(contract_init_borrowed(&contract, unit, &btn, input, expected, 1) == 0,
          "contract");
    check(btn_certify(&btn, &contract, NULL) == 0, "certify");

    cnb_init(&base);
    check(cnb_add_unit(&base, &btn, &contract, &reused) == 0 && !reused,
          "cnb add unit");
    check(cnb_save(&base, base_path) == 0, "cnb save");

    memset(&opts, 0, sizeof opts);
    opts.toolchain_digest = 0xabcdu;
    opts.counterfactual_stability = 0.91f;
    opts.rollback_unit = "acq_ev_goal_prev";
    opts.rollback_artifact_digest = 0x1111ull;
    opts.recipe_fp = 0x2222ull;

    check(cnet_evidence_bundle_from_base(&base, unit, &opts, &b) == 0,
          "from_base");
    check(b.behavior_digest != 0, "behavior_digest set");
    check(b.contract_digest != 0, "contract_digest set");
    check(b.dataset_hash != 0, "dataset_hash set");
    check(b.artifact_digest != 0, "artifact_digest set");
    check(cnet_evidence_bundle_is_complete(&b), "bundle complete");
    check(b.counterfactual_stability > 0.9f &&
              b.counterfactual_stability < 0.92f,
          "cf stability recorded");
    check(strcmp(b.rollback_unit, "acq_ev_goal_prev") == 0,
          "rollback unit");
    check(b.rollback_artifact_digest == 0x1111ull, "rollback digest");
    check(b.toolchain_digest == 0xabcdu, "toolchain digest");
    check(b.reliability > 0.0 && b.reliability <= 1.0, "reliability in range");

    check(cnet_evidence_bundle_format_json(&b, json, sizeof json) == 0,
          "format json");
    check(strstr(json, "\"contract_digest\"") &&
              strstr(json, "\"dataset_hash\"") &&
              strstr(json, "artifact_sha256") &&
              strstr(json, "runtime_libs_digest") &&
              strstr(json, "reliability") &&
              strstr(json, "counterfactual_stability") &&
              strstr(json, "rollback_unit"),
          "json has all required fields");

    check(cnet_evidence_store_put(store_path, &b) == 0, "store put");
    check(cnet_evidence_store_get(store_path, unit, &b2) == 0, "store get");
    check(b2.behavior_digest == b.behavior_digest, "round-trip behavior");
    check(b2.contract_digest == b.contract_digest, "round-trip contract");
    check(b2.dataset_hash == b.dataset_hash, "round-trip dataset");
    check(b2.artifact_digest == b.artifact_digest, "round-trip artifact");
    check(memcmp(b2.artifact_sha256, b.artifact_sha256, 32) == 0,
          "round-trip sha256");
    check(b2.counterfactual_stability > 0.9f, "round-trip cf");
    check(strcmp(b2.rollback_unit, "acq_ev_goal_prev") == 0,
          "round-trip rollback");

    check(cnet_evidence_bundle_verify(&base, &b) == 0, "verify matches live");
    b.dataset_hash ^= 1ull;
    check(cnet_evidence_bundle_verify(&base, &b) != 0, "verify catches tamper");
    b.dataset_hash ^= 1ull;

    check(cnet_evidence_record(&base, unit, store_path, &opts) == 0,
          "record helper");
    check(cnet_evidence_store_path_for_base(base_path, defpath,
                                            sizeof defpath) == 0 &&
              strstr(defpath, ".evidence.jsonl"),
          "default store path");

    /* Rebuild without opts still complete. */
    check(cnet_evidence_bundle_from_base(&base, unit, NULL, &live) == 0 &&
              cnet_evidence_bundle_is_complete(&live),
          "live rebuild complete");
    check(cnet_evidence_bundle_from_base(&base, "missing", NULL, &live) != 0,
          "missing unit fails");

    cnb_free(&base);
    contract_free(&contract);
    btn_free(&btn);
    remove(base_path);
    remove(store_path);

    if (failures) {
        printf("EVIDENCE_BUNDLE_FAIL failures=%d checks=%d\n", failures,
               checks);
        return 1;
    }
    printf("EVIDENCE_BUNDLE_PASS checks=%d\n", checks);
    return 0;
}
