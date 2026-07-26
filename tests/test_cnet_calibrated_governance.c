#include "cnet_calibrated_governance.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;

static void check(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
}

int main(void) {
    char path[] = "/tmp/cnet-governance-XXXXXX";
    char json[1024];
    CnetRoutePolicy policies[4];
    CnetClaimBinding binding;
    const CnetRoutePolicy *policy;
    const char *refs[] = {"evidence://memory/fact-17",
                          "evidence://eval/held-out-4"};
    size_t policy_count = 0;
    int fd = mkstemp(path);
    FILE *file;

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
    if (failures) return 1;
    puts("CALIBRATED_GOVERNANCE_PASS metric=1.000 abstention=fail_closed "
         "claims=provenance_bound");
    return 0;
}
