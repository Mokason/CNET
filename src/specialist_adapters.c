#include "../include/specialist_adapters.h"

#include <stdint.h>
#include <stdlib.h>

/* The wrapper owns only this small projection context. The OracleEntry and its
   backend context retain their existing registry-defined lifetime. */
typedef struct {
    OracleEntry *entry;
    size_t input_count;
    size_t output_count;
} CnetOracleAdapter;

static int oracle_adapter_forward(void *context,
                                  const double *input, size_t input_count,
                                  double *output, size_t output_count) {
    CnetOracleAdapter *adapter = (CnetOracleAdapter*)context;
    OracleEntry *entry;
    CnetOracleResult result;

    if (!adapter || !input || !output ||
        input_count != adapter->input_count ||
        output_count != adapter->output_count) {
        return -1;
    }
    entry = adapter->entry;
    if (!entry || (!entry->fn && !entry->fn_v2)) return -1;
    return cnet_oracle_invoke(entry, input, input_count,
                              output, output_count, &result) == CNET_ORACLE_ANSWER
           ? 0 : -1;
}

static void oracle_adapter_release(void *context) {
    free(context);
}

int cnet_oracle_init_contract_adapter(BinaryTransformNetwork *btn,
                                      OracleEntry *entry,
                                      uint64_t behavior_digest,
                                      size_t cost_hint) {
    CnetOracleAdapter *adapter;
    size_t input_count;
    size_t output_count;
    int rc;

    if (!btn || !entry || (!entry->fn && !entry->fn_v2) || behavior_digest == 0 ||
        (entry->behavior_digest != 0 && entry->behavior_digest != behavior_digest) ||
        entry->input_port.field_width == 0 ||
        entry->input_port.field_count == 0 ||
        entry->output_port.field_width == 0 ||
        entry->output_port.field_count == 0 ||
        entry->input_port.field_width > SIZE_MAX / entry->input_port.field_count ||
        entry->output_port.field_width > SIZE_MAX / entry->output_port.field_count) {
        return -1;
    }

    input_count = entry->input_port.field_width * entry->input_port.field_count;
    output_count = entry->output_port.field_width * entry->output_port.field_count;
    adapter = (CnetOracleAdapter*)calloc(1, sizeof *adapter);
    if (!adapter) return -1;
    adapter->entry = entry;
    adapter->input_count = input_count;
    adapter->output_count = output_count;

    rc = btn_init_adapter(btn, input_count, output_count,
                          &entry->input_port, 1,
                          &entry->output_port, 1,
                          oracle_adapter_forward, oracle_adapter_release,
                          adapter, behavior_digest, cost_hint);
    if (rc != 0) free(adapter);
    return rc;
}
