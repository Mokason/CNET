#include "cnet_calibrated_governance.h"
#include "cnet_semantic_cortex.h"
#include "cnet_shared_workspace.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void check(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
}

int main(void) {
    CnetSharedWorkspace workspace;
    CnetSemanticCortex cortex;
    CnetWorkspaceEntry recent[8];
    CnetClaimBinding binding;
    const char *evidence[] = {"workspace://cnet-verifier/4"};
    const CnetRoutePolicy policy = {
        "semantic.answer", 0.20, 0.90, 20
    };
    size_t proposals = 0, i;

    check(cnet_workspace_init(&workspace, 8) == 0,
          "shared workspace initializes");
    check(cnet_semantic_cortex_init_hermetic(&cortex, NULL) == 0,
          "semantic cortex initializes");
    check(cnet_semantic_cortex_propose(&cortex,
          "memory consolidation candidate", 3, 1000, &workspace,
          &proposals) == 0 && proposals == 3,
          "semantic cortex publishes candidates");
    check(cnet_workspace_recent(&workspace, recent, 8) == 3,
          "workspace exposes candidates");
    for (i = 0; i < proposals; i++) {
        check(recent[i].trust == CNET_WORKSPACE_UNCERTIFIED,
              "semantic candidate has no answer authority");
    }
    check(cnet_governance_decide(&policy, 0.10, 0.98, 100) ==
          CNET_GOVERNANCE_ABSTAIN,
          "weak semantic margin cannot answer");
    check(cnet_claim_bind("Consolidation is supported.", NULL, 0,
          &binding) != 0, "candidate alone cannot support a claim");

    check(cnet_workspace_push(&workspace, CNET_WORKSPACE_EVIDENCE,
          CNET_WORKSPACE_CERTIFIED, "cnet:verifier",
          "verified consolidation evidence", 0.97, 1010) == 0,
          "CNET verifier publishes certified evidence");
    check(cnet_governance_decide(&policy, 0.40, 0.97, 100) ==
          CNET_GOVERNANCE_ANSWER,
          "verified route clears calibrated thresholds");
    check(cnet_claim_bind("Consolidation is supported.", evidence, 1,
          &binding) == 0, "verified claim binds its evidence");

    if (failures) return 1;
    puts("COGNITIVE_RUNTIME_SMOKE_PASS metric=1.000 "
         "cortex=proposal cnet=authority provenance=required");
    return 0;
}
