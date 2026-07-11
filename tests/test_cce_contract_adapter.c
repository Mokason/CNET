/* Real CCE -> certified contract planner integration.
 * The CCE model remains modular; the adapter only projects its stable model
 * identity and typed boundary into the canonical CNET runtime.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/nn.h"
#include "../include/router.h"
#include "../include/contract/contract.h"
#include "../include/cce/cce_tensor.h"
#include "../include/cce/cce_block.h"
#include "../include/cce/cce_cascade.h"
#include "../include/cce/cce_forest.h"
#include "../include/cce/cce_model.h"
#include "../include/cce/cce_contract_adapter.h"

static int failures;

static void check(int ok, const char *name) {
    printf("  %-62s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static Port raw_port(size_t width, const char *tag) {
    Port p;
    memset(&p, 0, sizeof p);
    p.family = PORT_RAW;
    p.field_width = width;
    p.field_count = 1;
    snprintf(p.tag, sizeof p.tag, "%s", tag);
    return p;
}

static cce_model *build_model(const char *forest_path, cce_forest **forest_out) {
    cce_cascade cascade;
    cce_block first, head;
    cce_forest *forest = NULL;
    cce_model *model = NULL;

    memset(&cascade, 0, sizeof cascade);
    memset(&first, 0, sizeof first);
    memset(&head, 0, sizeof head);
    cce_cascade_init(&cascade, 4);
    if (cce_block_init_linear(&first, 2, 3, 0.01f) != CCE_OK ||
        cce_block_init_linear(&head, 3, 2, 0.01f) != CCE_OK) return NULL;
    head.type = CCE_BLOCK_LINEAR_HEAD;
    if (cce_cascade_append(&cascade, &first) != CCE_OK ||
        cce_cascade_append(&cascade, &head) != CCE_OK) return NULL;

    remove(forest_path);
    if (cce_forest_open(&forest, forest_path, 4) != CCE_OK ||
        cce_forest_add_branch(forest, &cascade, "contracted-cce") != CCE_OK ||
        cce_model_create(&model, "contracted-cce-model") != CCE_OK ||
        cce_model_add_forest(model, forest, "primary") != CCE_OK) return NULL;
    *forest_out = forest;
    return model;
}

int main(void) {
    const char *forest_path = "tmp_unified_cce.cce";
    cce_forest *forest = NULL;
    cce_model *model = build_model(forest_path, &forest);
    BinaryTransformNetwork adapter;
    Contract contract;
    PrimitiveRegistry registry;
    RoutePlan plan;
    Port input_port = raw_port(2, "cce_contract_input");
    Port output_port = raw_port(2, "cce_contract_output");
    const double input[2] = {0.25, -0.5};
    float cce_input[2] = {0.25f, -0.5f};
    float direct[2] = {0.0f, 0.0f};
    double expected[2] = {0.0, 0.0};
    double routed[2] = {0.0, 0.0};
    int direct_dim = 0;

    memset(&adapter, 0, sizeof adapter);
    memset(&contract, 0, sizeof contract);
    memset(&plan, 0, sizeof plan);

    printf("== unified runtime: real CCE model adapter ==\n");
    check(model != NULL && forest != NULL, "real CCE model builds");
    check(model != NULL &&
          cce_model_forward(model, cce_input, 2, direct, 2, &direct_dim) == CCE_OK &&
          direct_dim == 2,
          "direct CCE model forward succeeds");
    expected[0] = (double)direct[0];
    expected[1] = (double)direct[1];

    check(cce_model_init_contract_adapter(&adapter, model, 0,
                                          &input_port, 1,
                                          &output_port, 1,
                                          0x4343454d4f44454cULL,
                                          12) == 0,
          "CCE model projects into universal adapter");
    check(contract_init_borrowed(&contract, "cce_contract_model", &adapter,
                                 input, expected, 1) == 0,
          "real CCE output becomes contract exemplar");
    check(btn_certify(&adapter, &contract, NULL) == 0,
          "normal certification replays real CCE forward");

    registry_init(&registry);
    check(registry_add_certified(&registry, &adapter, "cce_contract_model",
                                 &contract) == 0,
          "certified CCE adapter enters canonical registry");
    registry.require_certified = 1;
    check(route_plan(&registry, input_port, output_port, &plan) == 0 &&
          plan.length == 1,
          "canonical planner discovers real CCE model");
    plan.strict = 1;
    check(route_execute(&plan, input, 2, routed, 2) == 0 &&
          fabs(routed[0] - expected[0]) < 1e-12 &&
          fabs(routed[1] - expected[1]) < 1e-12,
          "canonical executor matches direct CCE output");

    registry_free(&registry);
    contract_free(&contract);
    btn_free(&adapter);
    if (model) cce_model_destroy(model);
    if (forest) cce_forest_close(forest);
    remove(forest_path);

    printf("UNIFIED_CCE_ADAPTER_%s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
