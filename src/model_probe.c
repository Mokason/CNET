#include "../include/model_probe.h"
#include "../include/cce/cce_model_catalog.h"

int cnet_model_descriptor_probe(
    CnetModelDescriptor *descriptor_out,
    const char *model_id,
    const char *artifact_path,
    const char *backend_name,
    uint64_t allowed_resource_mask,
    uint64_t preferred_resource_mask,
    uint32_t required_resource_count,
    uint64_t resident_bytes_per_resource,
    uint64_t workspace_bytes,
    uint64_t kv_bytes_per_token) {
    cce_model_info info;
    cce_result result = cce_model_descriptor_probe(
        descriptor_out, &info, model_id, artifact_path, backend_name,
        allowed_resource_mask, preferred_resource_mask,
        required_resource_count, resident_bytes_per_resource,
        workspace_bytes, kv_bytes_per_token);
    switch (result) {
        case CCE_OK:
            return CNET_MODEL_OK;
        case CCE_ERR_INVALID_ARG:
            return CNET_MODEL_INVALID;
        case CCE_ERR_NOT_FOUND:
        case CCE_ERR_IO:
            return CNET_MODEL_NOT_FOUND;
        case CCE_ERR_UNSUPPORTED:
            return CNET_MODEL_UNSUPPORTED;
        default:
            return CNET_MODEL_LOAD_FAILED;
    }
}
