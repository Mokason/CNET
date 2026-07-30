#include "cnet_calibrated_governance.h"
#include "cnet_heldout.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CAPABILITY_ID "calibrated_abstention"

static int failures;

static void check(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
}

/* Map the fixture's expected decision word onto the enum the policy returns.
   An unknown word is a fixture error, never a silent pass. */
static int expected_decision(const char *word, int *out) {
    if (strcmp(word, "abstain") == 0) {
        *out = CNET_GOVERNANCE_ABSTAIN;
        return 0;
    }
    if (strcmp(word, "answer_with_evidence") == 0) {
        *out = CNET_GOVERNANCE_ANSWER;
        return 0;
    }
    return -1;
}

/* Replay one declared held-out decision case THROUGH the fixture: margin,
   reliability, sample count and the expected verdict all come from the file, so
   changing any of them changes what this asserts. */
static void heldout_decision_case(CnetHeldOut *h, const CnetRoutePolicy *policy,
                                  const char *case_id, double margin_fb,
                                  double reliability_fb, double samples_fb,
                                  const char *expected_fb) {
    char word[64];
    double margin, reliability, samples;
    int want = 0, got, ok;
    margin = cnet_heldout_num(h, case_id, "margin", margin_fb);
    reliability = cnet_heldout_num(h, case_id, "reliability", reliability_fb);
    samples = cnet_heldout_num(h, case_id, "samples", samples_fb);
    (void)cnet_heldout_str(h, case_id, "expected", word, sizeof word,
                           expected_fb);
    if (expected_decision(word, &want) != 0) {
        fprintf(stderr, "FAIL: case %s declares unknown expected %s\n", case_id,
                word);
        failures++;
        cnet_heldout_verdict(h, case_id, 0);
        return;
    }
    got = cnet_governance_decide(policy, margin, reliability, (size_t)samples);
    ok = (got == want);
    if (!ok) {
        fprintf(stderr,
                "FAIL: case %s margin=%.3f reliability=%.3f samples=%.0f "
                "expected %s, decided %d\n",
                case_id, margin, reliability, samples, word, got);
        failures++;
    }
    cnet_heldout_verdict(h, case_id, ok);
}

int main(void) {
    char path[] = "/tmp/cnet-governance-XXXXXX";
    char json[1024];
    CnetRoutePolicy policies[4];
    CnetClaimBinding binding;
    CnetHeldOut heldout;
    const CnetRoutePolicy *policy;
    const char *refs[] = {"evidence://memory/fact-17",
                          "evidence://eval/held-out-4"};
    size_t policy_count = 0;
    int fd = mkstemp(path);
    int heldout_rc = cnet_heldout_open(&heldout, CAPABILITY_ID);
    FILE *file;

    if (heldout_rc < 0) {
        fprintf(stderr, "FAIL: declared held-out fixture is unusable\n");
        return 2;
    }

    check(fd >= 0, "temporary policy file opens");
    file = fd >= 0 ? fdopen(fd, "w") : NULL;
    check(file != NULL, "temporary policy stream opens");
    if (file) {
        fputs("{\"routes\":["
              "{\"route_id\":\"memory.answer\","
              "\"minimum_margin\":0.25,"
              "\"minimum_reliability\":0.90,"
              "\"minimum_samples\":20},"
              "{\"route_id\":\"tool.plan\","
              "\"minimum_margin\":0.10,"
              "\"minimum_reliability\":0.80,"
              "\"minimum_samples\":5}]}", file);
        fclose(file);
    }
    check(cnet_governance_load_json(path, policies, 4,
          &policy_count) == 0, "JSON route policies load");
    check(policy_count == 2, "all policies load");
    policy = cnet_governance_find_policy(policies, policy_count,
                                         "memory.answer");
    check(policy != NULL, "route policy is found");
    check(cnet_governance_decide(policy, 0.30, 0.95, 30) ==
          CNET_GOVERNANCE_ANSWER, "well-supported answer is allowed");
    check(cnet_governance_decide(policy, 0.20, 0.95, 30) ==
          CNET_GOVERNANCE_ABSTAIN, "low margin abstains");
    check(cnet_governance_decide(policy, 0.30, 0.50, 30) ==
          CNET_GOVERNANCE_ABSTAIN, "low reliability abstains");
    check(cnet_governance_decide(policy, 0.30, 0.95, 3) ==
          CNET_GOVERNANCE_ABSTAIN, "insufficient samples abstain");
    check(cnet_governance_decide(policy, NAN, 0.95, 30) ==
          CNET_GOVERNANCE_ABSTAIN, "invalid confidence abstains");

    /* --- declared held-out cases -------------------------------------------
       These replay the SAME policy against the numbers and verdicts the fixture
       declares. The hard-coded checks above stay as an independent guard; these
       are what make the certificate causal. */
    heldout_decision_case(&heldout, policy, "low-margin-abstains", 0.20, 0.95,
                          30, "abstain");
    heldout_decision_case(&heldout, policy, "supported-answer", 0.30, 0.95, 30,
                          "answer_with_evidence");
    {
        const char *case_id = "evidence-free-claim";
        char word[64];
        size_t nrefs = cnet_heldout_array_len(&heldout, case_id,
                                              "evidence_refs", 0);
        int bound = cnet_claim_bind("Water freezes at zero Celsius.",
                                    nrefs ? refs : NULL, nrefs, &binding);
        int ok;
        (void)cnet_heldout_str(&heldout, case_id, "expected", word,
                               sizeof word, "claim_rejected");
        if (strcmp(word, "claim_rejected") == 0) ok = (bound != 0);
        else if (strcmp(word, "claim_bound") == 0) ok = (bound == 0);
        else {
            fprintf(stderr, "FAIL: case %s declares unknown expected %s\n",
                    case_id, word);
            failures++;
            ok = 0;
        }
        if (!ok) {
            fprintf(stderr, "FAIL: case %s with %zu evidence refs expected %s, "
                            "cnet_claim_bind returned %d\n",
                    case_id, nrefs, word, bound);
            failures++;
        }
        cnet_heldout_verdict(&heldout, case_id, ok);
    }

    check(cnet_claim_bind("Water freezes at zero Celsius.", NULL, 0,
          &binding) != 0, "claim without evidence is rejected");
    check(cnet_claim_bind("Water freezes at zero Celsius.", refs, 2,
          &binding) == 0, "claim binds to evidence");
    check(binding.binding_digest != 0, "claim binding has digest");
    check(cnet_claim_binding_json(&binding, json, sizeof(json)) > 0,
          "claim binding formats as JSON");
    check(strstr(json, "evidence://eval/held-out-4") != NULL,
          "JSON preserves evidence reference");
    check(strstr(json, "\"binding_digest\":") != NULL,
          "JSON exposes binding digest");

    unlink(path);
    if (cnet_heldout_finish(&heldout) != 0) {
        fprintf(stderr, "FAIL: declared held-out fixture was not honoured\n");
        failures++;
    }
    cnet_heldout_close(&heldout);
    if (failures) return 1;
    puts("CALIBRATED_GOVERNANCE_PASS metric=1.000 abstention=fail_closed "
         "claims=provenance_bound");
    return 0;
}
