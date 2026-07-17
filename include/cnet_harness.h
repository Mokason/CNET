/*
 * cnet_harness.h — versioned C ABI for the .NET-owned CNET inference harness.
 *
 * This is the ONLY symbol boundary the managed .NET layer touches. It hides
 * llama.cpp behind an opaque session and reuses the CNET catalog / residency
 * manager and the AICIMO router owned by cnet.so.
 *
 * All structs crossing this ABI carry (abi_version, struct_size) as their
 * first two uint32_t fields. Any input struct that does not match
 * CNET_HARNESS_ABI_VERSION or the current sizeof is rejected before any
 * backend or model state is created.
 *
 * The first tracer is synchronous, non-streaming. Cancellation and token
 * streaming are documented in docs/cnet_dotnet_inference_harness.md but are
 * explicitly not implemented in this ABI.
 */
#ifndef CNET_HARNESS_H
#define CNET_HARNESS_H

#include <stddef.h>
#include <stdint.h>

#include "cnet_export.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CNET_HARNESS_ABI_VERSION 1u

/* Result codes. Non-negative = success (only OK is non-negative). */
typedef enum {
    CNET_HARNESS_OK             =  0,
    CNET_HARNESS_ERR_INVALID    = -1,  /* bad abi_version, struct_size, args */
    CNET_HARNESS_ERR_MODEL_LOAD = -2,  /* catalog/probe/acquire failed        */
    CNET_HARNESS_ERR_BACKEND    = -3,  /* llama.cpp context / decode failed   */
    CNET_HARNESS_ERR_STATE      = -4,  /* misuse of an already-closed session */
    CNET_HARNESS_ERR_INTERNAL   = -5   /* unexpected non-recoverable failure  */
} CnetHarnessStatus;

/* Sampling profile identifiers. AUTO means "let AICIMO decide"; any other
 * value is an explicit caller override reported back in the result. */
typedef enum {
    CNET_HARNESS_SAMPLING_AUTO           = 0,
    CNET_HARNESS_SAMPLING_DETERMINISTIC  = 1,
    CNET_HARNESS_SAMPLING_FOCUSED        = 2,
    CNET_HARNESS_SAMPLING_BALANCED       = 3,
    CNET_HARNESS_SAMPLING_EXPLORATORY    = 4
} CnetHarnessSamplingMode;

/* Named single-bit resource selectors. Callers pass exactly one of these as
 * config.resource_mask; multi-bit or unknown masks are rejected. Values
 * mirror include/model_runtime.h's CnetModelResource; kept here so the ABI
 * has no transitive header dependency. */
typedef enum {
    CNET_HARNESS_RESOURCE_CPU  = 1u << 0,
    CNET_HARNESS_RESOURCE_GPU0 = 1u << 1,
    CNET_HARNESS_RESOURCE_GPU1 = 1u << 2,
    CNET_HARNESS_RESOURCE_GPU2 = 1u << 3,
    CNET_HARNESS_RESOURCE_GPU3 = 1u << 4
} CnetHarnessResource;

#define CNET_HARNESS_RESOURCE_MASK_KNOWN (   \
    CNET_HARNESS_RESOURCE_CPU  |             \
    CNET_HARNESS_RESOURCE_GPU0 |             \
    CNET_HARNESS_RESOURCE_GPU1 |             \
    CNET_HARNESS_RESOURCE_GPU2 |             \
    CNET_HARNESS_RESOURCE_GPU3)

/* Opaque session. Callers only ever hold a pointer to it. */
typedef struct CnetHarnessSession CnetHarnessSession;

typedef struct {
    uint32_t abi_version;      /* = CNET_HARNESS_ABI_VERSION */
    uint32_t struct_size;      /* = sizeof(CnetHarnessConfig) */
    const char *model_id;      /* required, UTF-8, non-empty */
    const char *model_path;    /* required, GGUF path        */
    uint64_t resource_mask;    /* CNET_MODEL_RESOURCE_GPU* bitmask */
    uint64_t budget_bytes;     /* resident budget, must be > 0     */
    int32_t main_gpu;          /* llama.cpp main_gpu index         */
    uint32_t n_ctx;            /* context tokens                   */
    uint32_t n_batch;          /* llama.cpp n_batch                */
    uint32_t n_threads;        /* generation threads               */
    uint32_t aicimo_num_ops;   /* >= 4                             */
    uint32_t aicimo_base_dim;  /* >= 8                             */
} CnetHarnessConfig;

typedef struct {
    uint32_t abi_version;
    uint32_t struct_size;
    const char *system;                    /* optional (may be NULL)         */
    const char *user;                      /* required, UTF-8                */
    const char *role;                      /* required, UTF-8, feeds AICIMO  */
    uint32_t max_tokens;
    uint32_t seed;
    CnetHarnessSamplingMode sampling;      /* AUTO honors AICIMO             */
} CnetHarnessGenerateOptions;

typedef struct {
    uint32_t abi_version;
    uint32_t struct_size;
    char *text;                            /* heap, UTF-8, freed by _free    */
    uint32_t prompt_tokens;
    uint32_t generated_tokens;
    double prompt_ms;
    double generation_ms;
    uint32_t selected_adapter;
    float route_uncertainty;               /* [0, 1]                          */
    CnetHarnessSamplingMode effective_sampling;
    int32_t aicimo_override;               /* 1 if caller override in effect  */
    /* Actual numeric sampling parameters applied by the backend, resolved
     * from the effective_sampling profile via one central table.
     * Deterministic honestly reports 0.0f / 1.0f / 0 / 0.0f. */
    float effective_temperature;
    float effective_top_p;
    uint32_t effective_top_k;
    float effective_min_p;
} CnetHarnessGeneration;

/* Lifecycle. Ownership: sessions and generations are heap-owned by the
 * plugin; callers must free them exactly through the paired free/close call.
 * Both free/close accept NULL. Access to one session must be serialized by
 * native callers; separate sessions may be used concurrently. */
CNET_API int cnet_harness_open(const CnetHarnessConfig *config,
                                CnetHarnessSession **session_out);

CNET_API int cnet_harness_generate(CnetHarnessSession *session,
                                    const CnetHarnessGenerateOptions *options,
                                    CnetHarnessGeneration **generation_out);

CNET_API void cnet_harness_generation_free(CnetHarnessGeneration *generation);

CNET_API int cnet_harness_close(CnetHarnessSession *session);

/* Static, never-NULL error string. Unknown codes return "unknown". */
CNET_API const char *cnet_harness_error_string(int status);

/* Test/introspection surface: perform a real role-biased AICIMO decision
 * against this session's persistent router and return the metadata the plugin
 * would apply to a subsequent generate() call. Does NOT touch llama.cpp.
 * Available so the contract test can exercise AICIMO without a GGUF. */
typedef struct {
    uint32_t abi_version;
    uint32_t struct_size;
    uint32_t selected_adapter;
    float route_uncertainty;
    CnetHarnessSamplingMode effective_sampling;
    /* Actual numeric sampling parameters that would be applied to a
     * generation with this route. Same source of truth as
     * CnetHarnessGeneration.effective_*. */
    float effective_temperature;
    float effective_top_p;
    uint32_t effective_top_k;
    float effective_min_p;
} CnetHarnessRouteInfo;

CNET_API int cnet_harness_probe_route(CnetHarnessSession *session,
                                       const char *role,
                                       CnetHarnessSamplingMode override_mode,
                                       CnetHarnessRouteInfo *info_out);

#ifdef __cplusplus
}
#endif

#endif /* CNET_HARNESS_H */
