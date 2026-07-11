/* Universal specialist tracer bullet.
 * A callback-backed implementation must enter the same typed contract,
 * certification, registry, planner, executor, evidence, and lifecycle path as
 * a matrix BTN. The callback is runtime-only and must refuse CNU persistence.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/nn.h"
#include "../include/router.h"
#include "../include/contract/contract.h"
#include "../include/contract/unit.h"

static int failures;
static int releases;

typedef struct {
    int calls;
} IdentityContext;

static void check(int ok, const char *name) {
    printf("  %-62s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static Port make_port(const char *tag) {
    Port p;
    memset(&p, 0, sizeof p);
    p.family = PORT_ONEHOT;
    p.field_width = 2;
    p.field_count = 1;
    snprintf(p.tag, sizeof p.tag, "%s", tag);
    return p;
}

static int identity_forward(void *opaque,
                            const double *input, size_t input_count,
                            double *output, size_t output_count) {
    IdentityContext *ctx = (IdentityContext *)opaque;
    if (!ctx || !input || !output || input_count != 2 || output_count != 2) return -1;
    ctx->calls++;
    output[0] = input[0];
    output[1] = input[1];
    return 0;
}

static void identity_release(void *opaque) {
    releases++;
    free(opaque);
}

int main(void) {
    BinaryTransformNetwork adapter;
    PrimitiveRegistry registry;
    RoutePlan plan;
    Contract contract;
    IdentityContext *context = (IdentityContext *)calloc(1, sizeof *context);
    Port input_port = make_port("adapter_input");
    Port output_port = make_port("adapter_output");
    const double input[2] = {0.0, 1.0};
    const double expected[2] = {0.0, 1.0};
    double output[2] = {0.0, 0.0};
    unsigned char *serialized = NULL;
    size_t serialized_len = 0;

    memset(&adapter, 0, sizeof adapter);
    memset(&contract, 0, sizeof contract);
    memset(&plan, 0, sizeof plan);

    printf("== unified runtime: contract adapter ==\n");
    check(context != NULL, "adapter context allocates");
    check(btn_init_adapter(&adapter,
                           2, 2,
                           &input_port, 1,
                           &output_port, 1,
                           identity_forward,
                           identity_release,
                           context,
                           0x554e494649454431ULL,
                           2) == 0,
          "runtime adapter initializes with typed ports and digest");
    check(btn_is_adapter(&adapter),
          "adapter identity is explicit");
    check(btn_forward(&adapter, input) != NULL &&
          adapter.last_output[0] == expected[0] &&
          adapter.last_output[1] == expected[1],
          "normal btn_forward dispatches through adapter callback");
    check(btn_train(&adapter, input, expected, 1, 1) < 0,
          "matrix training refuses runtime adapter");

    check(contract_init_borrowed(&contract, "external_identity", &adapter,
                                 input, expected, 1) == 0,
          "normal contract binds adapter");
    check(btn_certify(&adapter, &contract, NULL) == 0,
          "normal certification replays adapter behavior");
    check(unit_save_mem(&adapter, &contract, &serialized, &serialized_len) < 0 &&
          serialized == NULL && serialized_len == 0,
          "runtime adapter refuses fake CNU persistence");

    registry_init(&registry);
    check(registry_add_certified(&registry, &adapter, "external_identity",
                                 &contract) == 0,
          "certified adapter enters canonical registry");
    registry.require_certified = 1;
    check(route_plan(&registry, input_port, output_port, &plan) == 0 &&
          plan.length == 1,
          "canonical planner discovers adapter");
    plan.strict = 1;
    check(route_execute(&plan, input, 2, output, 2) == 0 &&
          output[0] == expected[0] && output[1] == expected[1],
          "canonical executor runs adapter with typed handoffs");
    check(btn_reliability(&adapter) > 0.5,
          "adapter accrues normal execution evidence");

    registry_free(&registry);
    contract_free(&contract);
    btn_free(&adapter);
    check(releases == 1,
          "adapter context releases exactly once");

    printf("UNIFIED_ADAPTER_%s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
