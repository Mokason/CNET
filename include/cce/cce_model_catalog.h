#ifndef CCE_MODEL_CATALOG_H
#define CCE_MODEL_CATALOG_H

#include "cce_detect.h"
#include "../model_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Probe model metadata without loading tensor payloads and project the result
 * into CNET's backend-neutral catalog descriptor. resident_bytes_per_resource
 * is a conservative backend-supplied upper bound including its required
 * runtime reserve; zero uses artifact bytes as a mmap-style default. */
CNET_API cce_result cce_model_descriptor_probe(
    CnetModelDescriptor *descriptor_out,
    cce_model_info *info_out,
    const char *model_id,
    const char *artifact_path,
    const char *backend_name,
    uint64_t allowed_resource_mask,
    uint64_t preferred_resource_mask,
    uint32_t required_resource_count,
    uint64_t resident_bytes_per_resource,
    uint64_t workspace_bytes,
    uint64_t kv_bytes_per_token
);

#ifdef __cplusplus
}
#endif

#endif /* CCE_MODEL_CATALOG_H */
