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

    /* Copies of string pointers held by config_copy (heap-owned by session). */
    char *model_id_owned;
    char *model_path_owned;

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

/* Backend hook. Weak default implementations in cnet_harness_core.c let the
 * hermetic contract test link without llama.cpp. The llama TU
 * (cnet_harness_llama.cpp) provides strong overrides. */
int harness_backend_open(struct CnetHarnessSession *session);
int harness_backend_generate(struct CnetHarnessSession *session,
                              const CnetHarnessGenerateOptions *options,
                              CnetHarnessSamplingMode effective,
                              CnetHarnessGeneration *generation);
void harness_backend_close(struct CnetHarnessSession *session);

/* Shared sampling-profile decision, exported for the backend TU. */
CnetHarnessSamplingMode cnet_harness__adapter_to_profile(uint32_t adapter);
CnetHarnessSamplingMode cnet_harness__downgrade_for_uncertainty(
    CnetHarnessSamplingMode profile, float uncertainty);

#ifdef __cplusplus
}
#endif

#endif /* CNET_HARNESS_PRIVATE_H */
