#include "cnet_semantic_cortex.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void check(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
}

static int fake_topk(const double *input, double *output,
                     void *context, int k) {
    const size_t width = *(const size_t *)context;
    size_t i;
    (void)input;
    for (i = 0; i < (size_t)k * width; i++) output[i] = 0.0;
    for (i = 0; i < (size_t)k; i++) output[i * width + i + 1] = 1.0;
    return 0;
}

int main(void) {
    CnetSharedWorkspace workspace;
    CnetSemanticCortex cortex;
    CnetWorkspaceEntry entries[8];
    size_t proposals = 0, width = 5;
    const int ids[] = {101, 102, 103, 104, 105};

    check(cnet_workspace_init(&workspace, 8) == 0, "workspace initializes");
    check(cnet_semantic_cortex_init_hermetic(&cortex, NULL) == 0,
          "hermetic cortex initializes");
    check(cnet_semantic_cortex_propose(&cortex,
          "adaptive memory consolidation", 3, 100, &workspace,
          &proposals) == 0, "hermetic proposals succeed");
    check(proposals == 3, "hermetic proposal count is bounded");
    check(cnet_workspace_recent(&workspace, entries, 8) == 3,
          "proposals enter shared workspace");
    check(entries[0].trust == CNET_WORKSPACE_UNCERTIFIED &&
          entries[1].trust == CNET_WORKSPACE_UNCERTIFIED,
          "semantic cortex cannot certify proposals");
    check(strstr(entries[0].source_tag, "semantic_cortex") != NULL,
          "proposal source is explicit");

    cnet_workspace_clear(&workspace);
    check(cnet_semantic_cortex_init_residual_http(
          &cortex, fake_topk, &width, ids, width, NULL) == 0,
          "residual adapter initializes");
    check(cnet_semantic_cortex_propose(&cortex, "held out query", 2,
          200, &workspace, &proposals) == 0,
          "residual-compatible callback proposes");
    check(proposals == 2, "residual proposal count is correct");
    check(cnet_workspace_recent(&workspace, entries, 8) == 2,
          "residual proposals are published");
    check(strcmp(entries[0].text_or_latent_ref,
                 "residual-token:103") == 0,
          "residual window id is preserved");
    check(entries[0].trust == CNET_WORKSPACE_UNCERTIFIED,
          "residual proposal remains uncertified");

    check(cnet_semantic_cortex_propose(&cortex, "", 2, 0,
          &workspace, &proposals) != 0, "empty query rejected");
    check(cnet_semantic_cortex_propose(&cortex, "query", 9, 0,
          &workspace, &proposals) != 0, "oversized top-k rejected");

    if (failures) return 1;
    puts("SEMANTIC_CORTEX_PASS metric=1.000 authority=cnet");
    return 0;
}
