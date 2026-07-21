/*
 * cnet_harness_private.h — plugin-internal session layout and backend hook.
 *
 * Visible only to translation units under src/cnet_harness/. The managed
 * layer and hermetic tests never see this file.
 */
#ifndef CNET_HARNESS_PRIVATE_H
#define CNET_HARNESS_PRIVATE_H

#include "../../include/cnet_harness.h"
#include "../../include/cce/cce_aicimo.h"
#include "../../include/model_runtime.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One session owns exactly one AICIMO router, one manager lease, and one
 * llama-backed context. In hermetic builds the backend fields stay NULL. */
struct CnetHarnessSession {
    uint32_t magic;                    /* CNET_HARNESS_SESSION_MAGIC */
    CnetHarnessConfig config_copy;
    int offload_enabled;
    CnetHarnessOffloadPolicy offload_policy;
    CnetHarnessOffloadInfo offload_info;

    /* Copies of string pointers held by config_copy (heap-owned by session). */
    char *model_id_owned;
    char *model_path_owned;

    /* Optional route-decision JSONL path (from CNET_ROUTE_LOG at open). */
    char *route_log_path;

    /* AICIMO router: one per session, persistent across calls. */
    cce_aicimo_router router;
    int router_ready;

    /* CNET catalog manager: single residency authority. */
    CnetModelManager *manager;
    CnetModelLease lease;
    int lease_held;

    /* Opaque backend state (llama_model + llama_context + sampler chain in the
     * llama TU; NULL in hermetic builds). */
    void *backend_state;
};

#define CNET_HARNESS_SESSION_MAGIC 0xC1E7C0DEu

/* Numeric parameters that a sampling profile resolves to. One source of
 * truth: cnet_harness__profile_params. The backend consumes these; there is
 * no per-backend switch that can drift from what the plugin reports. */
typedef struct {
    float temperature;   /* deterministic reports 0.0f honestly            */
    float top_p;         /* deterministic reports 1.0f (no top-p cut)      */
    uint32_t top_k;      /* deterministic reports 0 (no top-k cut)         */
    float min_p;         /* deterministic reports 0.0f                     */
} CnetHarnessSamplingParams;

CnetHarnessSamplingParams cnet_harness__profile_params(
    CnetHarnessSamplingMode mode);

/* Backend hook contract. The default (fail-closed) implementations in
 * cnet_harness_core.c return ERR_BACKEND. The llama TU
 * (cnet_harness_llama.cpp) provides strong overrides. Tests that need a
 * hermetic route-only path must provide their own strong fakes. */
int harness_backend_open(struct CnetHarnessSession *session);
int harness_backend_generate(struct CnetHarnessSession *session,
                              const CnetHarnessGenerateOptions *options,
                              CnetHarnessSamplingMode effective,
                              CnetHarnessSamplingParams params,
                              CnetHarnessGeneration *generation);
/* Two-phase teardown. prepare_close frees the backend context/sampler while
 * the CNET model lease is still held. finish_close releases global backend
 * state after the core has released the lease and closed the manager. */
void harness_backend_prepare_close(struct CnetHarnessSession *session);
void harness_backend_finish_close(struct CnetHarnessSession *session);

/* Shared sampling-profile decision, exported for the backend TU. */
CnetHarnessSamplingMode cnet_harness__adapter_to_profile(uint32_t adapter);
CnetHarnessSamplingMode cnet_harness__downgrade_for_uncertainty(
    CnetHarnessSamplingMode profile, float uncertainty);

#ifdef __cplusplus
}
#endif

#endif /* CNET_HARNESS_PRIVATE_H */
