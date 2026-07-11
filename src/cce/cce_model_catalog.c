#include "../../include/cce/cce_model_catalog.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static int fits(const char *value, size_t capacity) {
    return value && value[0] != '\0' && strlen(value) < capacity;
}

static uint32_t catalog_count_bits(uint64_t value) {
    uint32_t count = 0;
    while (value != 0) {
        value &= value - 1u;
        count++;
    }
    return count;
}

static CnetModelClass catalog_class(const cce_model_info *info) {
    if (info->is_moe) return CNET_MODEL_CLASS_MOE;
    switch (info->family) {
        case CCE_ARCH_FAMILY_LLAMA:
        case CCE_ARCH_FAMILY_GPT2:
            return CNET_MODEL_CLASS_DENSE_TRANSFORMER;
        case CCE_ARCH_FAMILY_MAMBA:
            return CNET_MODEL_CLASS_SSM;
        default:
            break;
    }
    if (strstr(info->arch, "bert") || strstr(info->arch, "embed"))
        return CNET_MODEL_CLASS_EMBEDDING;
    /* Qwen3.5 GGUFs are hybrid attention + recurrent architectures. They do
       not fit CCE's pure LLAMA or MAMBA runner families, but non-MoE variants
       still have dense FFNs and are valid dense backends (for example via
       llama.cpp). Density and CCE runner availability are separate facts. */
    if (strcmp(info->arch, "qwen35") == 0 ||
        strcmp(info->arch, "qwen3.5") == 0)
        return CNET_MODEL_CLASS_DENSE_TRANSFORMER;
    return CNET_MODEL_CLASS_OTHER;
}

cce_result cce_model_descriptor_probe(
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
) {
    cce_model_info info;
    struct stat file_info;
    CnetModelClass model_class;
    if (!descriptor_out || !info_out ||
        !fits(model_id, CNET_MODEL_ID_MAX) ||
        !fits(artifact_path, CNET_MODEL_PATH_MAX) ||
        !fits(backend_name, CNET_MODEL_BACKEND_NAME_MAX) ||
        allowed_resource_mask == 0 || required_resource_count == 0 ||
        required_resource_count > catalog_count_bits(allowed_resource_mask) ||
        (preferred_resource_mask & ~allowed_resource_mask) != 0)
        return CCE_ERR_INVALID_ARG;
    if (stat(artifact_path, &file_info) != 0 || file_info.st_size <= 0)
        return CCE_ERR_IO;
    if (cce_detect_file(artifact_path, &info) != CCE_OK)
        return CCE_ERR_IO;

    memset(descriptor_out, 0, sizeof *descriptor_out);
    descriptor_out->abi_version = CNET_MODEL_RUNTIME_ABI_VERSION;
    descriptor_out->struct_size = sizeof *descriptor_out;
    snprintf(descriptor_out->model_id, sizeof descriptor_out->model_id, "%s", model_id);
    snprintf(descriptor_out->backend_name, sizeof descriptor_out->backend_name,
             "%s", backend_name);
    snprintf(descriptor_out->architecture, sizeof descriptor_out->architecture,
             "%s", info.arch[0] ? info.arch : cce_detect_family_name(info.family));
    snprintf(descriptor_out->format, sizeof descriptor_out->format,
             "%s", cce_detect_format_name(info.format));
    snprintf(descriptor_out->quantization, sizeof descriptor_out->quantization,
             "%s", info.dtype);
    snprintf(descriptor_out->artifact_path, sizeof descriptor_out->artifact_path,
             "%s", artifact_path);

    model_class = catalog_class(&info);
    descriptor_out->model_class = model_class;
    descriptor_out->capabilities = model_class == CNET_MODEL_CLASS_EMBEDDING ?
        CNET_MODEL_CAP_EMBEDDING : CNET_MODEL_CAP_TEXT_GENERATION;
    descriptor_out->artifact_bytes = (uint64_t)file_info.st_size;
    descriptor_out->resident_bytes_per_resource = resident_bytes_per_resource ?
        resident_bytes_per_resource : (uint64_t)file_info.st_size;
    descriptor_out->workspace_bytes = workspace_bytes;
    descriptor_out->kv_bytes_per_token = kv_bytes_per_token;
    descriptor_out->allowed_resource_mask = allowed_resource_mask;
    descriptor_out->preferred_resource_mask = preferred_resource_mask;
    descriptor_out->required_resource_count = required_resource_count;
    descriptor_out->context_limit = info.ctx_len > 0 ? (uint32_t)info.ctx_len : 0;
    *info_out = info;
    return CCE_OK;
}
