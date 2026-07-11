#include "../../include/cce/cce_contract_adapter.h"

#include <limits.h>
#include <stdlib.h>

/* Contract boundaries use double because the legacy planner/executor does;
   CCE stays float internally. Conversion is explicit and contained here. */
typedef struct {
    cce_model *model;
    int owns_model;
    float *input;
    float *output;
    size_t input_count;
    size_t output_count;
} CceContractAdapter;

static int ports_total(const Port *ports, size_t count, size_t *total_out) {
    size_t total = 0;
    size_t i;
    if (!ports || count == 0 || !total_out) return -1;
    for (i = 0; i < count; ++i) {
        size_t fields;
        if (ports[i].field_width == 0 || ports[i].field_count == 0 ||
            ports[i].field_width > (size_t)-1 / ports[i].field_count) return -1;
        fields = ports[i].field_width * ports[i].field_count;
        if (total > (size_t)-1 - fields) return -1;
        total += fields;
    }
    if (total == 0 || total > (size_t)INT_MAX) return -1;
    *total_out = total;
    return 0;
}

static int cce_contract_forward(void *opaque,
                                const double *input, size_t input_count,
                                double *output, size_t output_count) {
    CceContractAdapter *ctx = (CceContractAdapter *)opaque;
    int actual = 0;
    size_t i;
    if (!ctx || !ctx->model || !input || !output ||
        input_count != ctx->input_count || output_count != ctx->output_count) return -1;
    for (i = 0; i < input_count; ++i) ctx->input[i] = (float)input[i];
    if (cce_model_forward(ctx->model, ctx->input, (int)input_count,
                          ctx->output, (int)output_count, &actual) != CCE_OK ||
        actual != (int)output_count) return -1;
    for (i = 0; i < output_count; ++i) output[i] = (double)ctx->output[i];
    return 0;
}

static void cce_contract_release(void *opaque) {
    CceContractAdapter *ctx = (CceContractAdapter *)opaque;
    if (!ctx) return;
    if (ctx->owns_model && ctx->model) cce_model_destroy(ctx->model);
    free(ctx->input);
    free(ctx->output);
    free(ctx);
}

int cce_model_init_contract_adapter(
    BinaryTransformNetwork *adapter,
    cce_model *model,
    int owns_model,
    const Port *input_ports,
    size_t input_port_count,
    const Port *output_ports,
    size_t output_port_count,
    unsigned long long behavior_digest,
    size_t cost_hint
) {
    CceContractAdapter *ctx;
    size_t input_count, output_count;
    if (!adapter || !model || behavior_digest == 0 ||
        ports_total(input_ports, input_port_count, &input_count) != 0 ||
        ports_total(output_ports, output_port_count, &output_count) != 0) return -1;

    ctx = (CceContractAdapter *)calloc(1, sizeof *ctx);
    if (!ctx) return -1;
    ctx->input = (float *)malloc(input_count * sizeof *ctx->input);
    ctx->output = (float *)malloc(output_count * sizeof *ctx->output);
    if (!ctx->input || !ctx->output) {
        free(ctx->input);
        free(ctx->output);
        free(ctx);
        return -1;
    }
    ctx->model = model;
    ctx->owns_model = owns_model ? 1 : 0;
    ctx->input_count = input_count;
    ctx->output_count = output_count;

    if (btn_init_adapter(adapter,
                         input_count, output_count,
                         input_ports, input_port_count,
                         output_ports, output_port_count,
                         cce_contract_forward, cce_contract_release, ctx,
                         behavior_digest, cost_hint) != 0) {
        /* Ownership transfers only when btn_init_adapter succeeds. */
        free(ctx->input);
        free(ctx->output);
        free(ctx);
        return -1;
    }
    return 0;
}
