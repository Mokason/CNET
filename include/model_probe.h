#ifndef CNET_MODEL_PROBE_H
#define CNET_MODEL_PROBE_H

#include "model_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Backend-neutral, header-only model probe for hosts that also include external
 * GGUF consumers such as llama.cpp. This intentionally hides CCE's GGUF types
 * so their enum names cannot collide with ggml/gguf.h. */
CNET_API int cnet_model_descriptor_probe(
    CnetModelDescriptor *descriptor_out,
    const char *model_id,
    const char *artifact_path,
    const char *backend_name,
    uint64_t allowed_resource_mask,
    uint64_t preferred_resource_mask,
    uint32_t required_resource_count,
    uint64_t resident_bytes_per_resource,
    uint64_t workspace_bytes,
    uint64_t kv_bytes_per_token);

#ifdef __cplusplus
}
#endif

#endif
