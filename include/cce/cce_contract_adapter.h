#ifndef CCE_CONTRACT_ADAPTER_H
#define CCE_CONTRACT_ADAPTER_H

/* Project a real CCE model into CNET's canonical typed contract runtime.
 * The model remains a modular payload; this facade only owns conversion
 * buffers and optionally the model handle. Certification is not implied:
 * callers must replay a Contract and use registry_add_certified normally.
 */

#include "../nn.h"
#include "cce_model.h"

#ifdef __cplusplus
extern "C" {
#endif

CNET_API int cce_model_init_contract_adapter(
    BinaryTransformNetwork *adapter,
    cce_model *model,
    int owns_model,
    const Port *input_ports,
    size_t input_port_count,
    const Port *output_ports,
    size_t output_port_count,
    unsigned long long behavior_digest,
    size_t cost_hint
);

#ifdef __cplusplus
}
#endif

#endif /* CCE_CONTRACT_ADAPTER_H */
