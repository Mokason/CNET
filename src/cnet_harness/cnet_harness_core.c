/*
 * cnet_harness_core.c — plugin core (backend-agnostic).
 *
 * Owns the entire versioned C ABI. Delegates model load / decode to a backend
 * translation unit through weakly-linked hooks so that hermetic contract
 * tests can exercise the ABI, AICIMO routing, and lifecycle without linking
 * llama.cpp.
 *
 * The weak defaults fail closed (ERR_BACKEND). Tests that want to exercise
 * the route-only surface must link explicit strong fake hooks; there is no
 * production no-op backend.
 */
#include "cnet_harness_private.h"

#include "../../include/cce/cce_aicimo.h"
#include "../../include/cnet_agent_role.h"
#include "../../include/cnet_route_log.h"
#include "../../include/model_runtime.h"
#include "../../include/model_probe.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ---- weak default backend hooks ---- */

__attribute__((weak))
int harness_backend_open(struct CnetHarnessSession *session) {
    (void)session;
    /* No backend linked in: fail closed. Tests provide strong fakes when
     * they want the route-only surface. */
    return CNET_HARNESS_ERR_BACKEND;
}

__attribute__((weak))
int harness_backend_generate(struct CnetHarnessSession *session,
                              const CnetHarnessGenerateOptions *options,
                              CnetHarnessSamplingMode effective,
                              CnetHarnessSamplingParams params,
                              CnetHarnessGeneration *generation) {
    (void)session; (void)options; (void)effective; (void)params; (void)generation;
    return CNET_HARNESS_ERR_BACKEND;
}

__attribute__((weak))
int harness_backend_count_tokens(struct CnetHarnessSession *session,
                                  const char *text,
                                  int32_t *count_out) {
    (void)session; (void)text; (void)count_out;
    return CNET_HARNESS_ERR_BACKEND;
}

__attribute__((weak))
void harness_backend_prepare_close(struct CnetHarnessSession *session) {
    (void)session;
}

__attribute__((weak))
void harness_backend_finish_close(struct CnetHarnessSession *session) {
    (void)session;
}

/* ---- helpers ---- */

static char *dup_string(const char *src) {
    if (!src) return NULL;
    size_t len = strlen(src);
    char *dst = (char *)malloc(len + 1u);
    if (!dst) return NULL;
    memcpy(dst, src, len + 1u);
    return dst;
}

static double mono_ms_now(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0.0;
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
}

/* Best-effort append; never fails the caller. */
static void emit_route_log(struct CnetHarnessSession *session,
                           const char *event,
                           const char *role,
                           uint32_t selected_expert,
                           float entropy,
                           CnetHarnessSamplingMode profile,
                           int override_used,
                           int outcome_code,
                           double route_latency_ms,
                           double total_latency_ms,
                           uint32_t prompt_tokens,
                           uint32_t generated_tokens) {
    CnetRouteLogEvent ev;
    CnetAgentRolePolicy pol;
    int known;
    if (!session || !session->route_log_path || !session->route_log_path[0])
        return;
    if (!role) role = "";
    known = (cnet_agent_role_resolve(role, &pol) == 0);
    memset(&ev, 0, sizeof ev);
    ev.event = event ? event : "route";
    ev.mechanism = known ? CNET_ROUTE_MECH_AGENT_ROLE : CNET_ROUTE_MECH_ROLE_HASH;
    ev.role = role;
    ev.role_canonical = known ? pol.name : role;
    ev.selected_expert = selected_expert;
    ev.expert_profile = cnet_route_log_profile_name((int)profile);
    ev.entropy = entropy;
    ev.outcome = cnet_route_log_outcome_name(outcome_code);
    ev.outcome_code = outcome_code;
    ev.route_latency_ms = route_latency_ms;
    ev.total_latency_ms = total_latency_ms;
    ev.prompt_tokens = prompt_tokens;
    ev.generated_tokens = generated_tokens;
    ev.cost = cnet_route_log_cost(prompt_tokens, generated_tokens);
    ev.aicimo_override = override_used ? 1 : 0;
    ev.model_id = session->model_id_owned;
    (void)cnet_route_log_append(session->route_log_path, &ev);
}

static int is_single_bit(uint64_t v) {
    return v != 0ull && (v & (v - 1ull)) == 0ull;
}

static int resource_mask_valid(uint64_t mask) {
    if (!is_single_bit(mask)) return 0;
    /* Must match a documented resource bit. */
    return (mask & (uint64_t)CNET_HARNESS_RESOURCE_MASK_KNOWN) == mask;
}

static int validate_config(const CnetHarnessConfig *c) {
    if (!c) return 0;
    if (c->abi_version != CNET_HARNESS_ABI_VERSION) return 0;
    if (c->struct_size != sizeof(*c)) return 0;
    if (!c->model_id || c->model_id[0] == '\0') return 0;
    if (!c->model_path || c->model_path[0] == '\0') return 0;
    if (!resource_mask_valid(c->resource_mask)) return 0;
    if (c->budget_bytes == 0ull) return 0;
    if (c->n_ctx < 256u) return 0;
    if (c->n_batch == 0u || c->n_batch > c->n_ctx) return 0;
    if (c->n_threads == 0u || c->n_threads > 1024u) return 0;
    if (c->aicimo_num_ops < 4u || c->aicimo_num_ops > 256u) return 0;
    if (c->aicimo_base_dim < 8u || c->aicimo_base_dim > 8192u) return 0;
    return 1;
}

static int validate_offload_policy(const CnetHarnessConfig *config,
                                   const CnetHarnessOffloadPolicy *policy) {
    if (!config || !policy) return 0;
    if (config->resource_mask == (uint64_t)CNET_HARNESS_RESOURCE_CPU) return 0;
    if (policy->abi_version != CNET_HARNESS_OFFLOAD_ABI_VERSION) return 0;
    if (policy->struct_size != sizeof(*policy)) return 0;
    if (policy->gpu_layer_count <= 0) return 0;
    if (policy->device_count == 0u ||
        policy->device_count > CNET_HARNESS_MAX_GPU_DEVICES) return 0;
    if (policy->offload_kqv > 1u) return 0;
    if (policy->device_count == 1u) {
        if (policy->split_mode != CNET_HARNESS_SPLIT_NONE) return 0;
    } else if (policy->split_mode != CNET_HARNESS_SPLIT_LAYER) {
        return 0;
    }
    for (uint32_t i = 0; i < policy->device_count; ++i) {
        if (policy->device_indices[i] < 0) return 0;
        if (!isfinite(policy->tensor_split[i]) ||
            policy->tensor_split[i] < 0.0f) return 0;
        for (uint32_t j = 0; j < i; ++j) {
            if (policy->device_indices[i] == policy->device_indices[j]) return 0;
        }
    }
    return 1;
}

static int validate_generate_options(const CnetHarnessGenerateOptions *o) {
    if (!o) return 0;
    if (o->abi_version != CNET_HARNESS_ABI_VERSION) return 0;
    if (o->struct_size != sizeof(*o)) return 0;
    if (!o->user || o->user[0] == '\0') return 0;
    if (!o->role || o->role[0] == '\0') return 0;
    if (o->max_tokens == 0u || o->max_tokens > 65536u) return 0;
    switch (o->sampling) {
        case CNET_HARNESS_SAMPLING_AUTO:
        case CNET_HARNESS_SAMPLING_DETERMINISTIC:
        case CNET_HARNESS_SAMPLING_FOCUSED:
        case CNET_HARNESS_SAMPLING_BALANCED:
        case CNET_HARNESS_SAMPLING_EXPLORATORY:
            break;
        default:
            return 0;
    }
    return 1;
}

CnetHarnessSamplingMode cnet_harness__adapter_to_profile(uint32_t adapter) {
    switch (adapter % 4u) {
        case 0u: return CNET_HARNESS_SAMPLING_DETERMINISTIC;
        case 1u: return CNET_HARNESS_SAMPLING_FOCUSED;
        case 2u: return CNET_HARNESS_SAMPLING_BALANCED;
        default: return CNET_HARNESS_SAMPLING_EXPLORATORY;
    }
}

CnetHarnessSamplingMode cnet_harness__downgrade_for_uncertainty(
    CnetHarnessSamplingMode profile, float uncertainty) {
    if (uncertainty < 0.85f) return profile;
    switch (profile) {
        case CNET_HARNESS_SAMPLING_EXPLORATORY: return CNET_HARNESS_SAMPLING_BALANCED;
        case CNET_HARNESS_SAMPLING_BALANCED:    return CNET_HARNESS_SAMPLING_FOCUSED;
        case CNET_HARNESS_SAMPLING_FOCUSED:     return CNET_HARNESS_SAMPLING_DETERMINISTIC;
        default: return profile;
    }
}

/* Single source of truth for the numeric parameters each profile applies.
 * Deterministic reports honest zeros (no cut, no temperature). */
CnetHarnessSamplingParams cnet_harness__profile_params(
    CnetHarnessSamplingMode mode) {
    CnetHarnessSamplingParams p = {0.0f, 1.0f, 0u, 0.0f};
    switch (mode) {
        case CNET_HARNESS_SAMPLING_DETERMINISTIC:
            p.temperature = 0.0f; p.top_p = 1.0f; p.top_k = 0u; p.min_p = 0.0f;
            break;
        case CNET_HARNESS_SAMPLING_FOCUSED:
            p.temperature = 0.30f; p.top_p = 0.85f; p.top_k = 40u; p.min_p = 0.05f;
            break;
        case CNET_HARNESS_SAMPLING_BALANCED:
        case CNET_HARNESS_SAMPLING_AUTO:
            p.temperature = 0.70f; p.top_p = 0.90f; p.top_k = 40u; p.min_p = 0.05f;
            break;
        case CNET_HARNESS_SAMPLING_EXPLORATORY:
            p.temperature = 0.95f; p.top_p = 0.95f; p.top_k = 80u; p.min_p = 0.03f;
            break;
    }
    return p;
}

/* ---- ABI ---- */

const char *cnet_harness_error_string(int status) {
    switch (status) {
        case CNET_HARNESS_OK:              return "ok";
        case CNET_HARNESS_ERR_INVALID:     return "invalid argument";
        case CNET_HARNESS_ERR_MODEL_LOAD:  return "model load failed";
        case CNET_HARNESS_ERR_BACKEND:     return "backend failure";
        case CNET_HARNESS_ERR_STATE:       return "invalid session state";
        case CNET_HARNESS_ERR_INTERNAL:    return "internal error";
        default:                            return "unknown";
    }
}

static void session_release(struct CnetHarnessSession *session) {
    if (!session) return;
    /* Two-phase backend teardown. Phase one frees per-session backend state
     * (context, sampler) while the model lease is still held; phase two
     * runs only after the lease is released and the manager is closed, so
     * global backend state cannot vanish while another session still owns
     * a model handle. */
    harness_backend_prepare_close(session);
    if (session->lease_held && session->manager) {
        cnet_model_release(session->manager, &session->lease);
        session->lease_held = 0;
    }
    if (session->manager) {
        cnet_model_manager_close(session->manager);
        session->manager = NULL;
    }
    harness_backend_finish_close(session);
    if (session->router_ready) {
        cce_aicimo_router_free(&session->router);
        session->router_ready = 0;
    }
    free(session->model_id_owned);
    free(session->model_path_owned);
    free(session->route_log_path);
    session->magic = 0u;
    free(session);
}

static int cnet_harness_open_internal(
        const CnetHarnessConfig *config,
        const CnetHarnessOffloadPolicy *policy,
        CnetHarnessSession **session_out) {
    if (!session_out) return CNET_HARNESS_ERR_INVALID;
    *session_out = NULL;
    if (!validate_config(config)) return CNET_HARNESS_ERR_INVALID;
    if (policy && !validate_offload_policy(config, policy)) {
        return CNET_HARNESS_ERR_INVALID;
    }

    if (access(config->model_path, R_OK) != 0) {
        return CNET_HARNESS_ERR_MODEL_LOAD;
    }

    struct CnetHarnessSession *session = (struct CnetHarnessSession *)
        calloc(1, sizeof(*session));
    if (!session) return CNET_HARNESS_ERR_INTERNAL;

    session->magic = CNET_HARNESS_SESSION_MAGIC;
    session->config_copy = *config;
    if (policy) {
        session->offload_enabled = 1;
        session->offload_policy = *policy;
        session->offload_info.abi_version = CNET_HARNESS_OFFLOAD_ABI_VERSION;
        session->offload_info.struct_size =
            (uint32_t)sizeof(session->offload_info);
        session->offload_info.requested_gpu_layers = policy->gpu_layer_count;
        session->offload_info.device_count = policy->device_count;
        session->offload_info.split_mode = policy->split_mode;
        session->offload_info.offload_kqv = policy->offload_kqv;
        for (uint32_t i = 0; i < policy->device_count; ++i) {
            session->offload_info.device_indices[i] = policy->device_indices[i];
        }
    }
    session->model_id_owned = dup_string(config->model_id);
    session->model_path_owned = dup_string(config->model_path);
    {
        const char *logp = cnet_route_log_path_from_env();
        session->route_log_path = logp ? dup_string(logp) : NULL;
        if (logp && !session->route_log_path) {
            session_release(session);
            return CNET_HARNESS_ERR_INTERNAL;
        }
    }
    if (!session->model_id_owned || !session->model_path_owned) {
        session_release(session);
        return CNET_HARNESS_ERR_INTERNAL;
    }
    /* Re-point config_copy strings to session-owned storage so the caller's
     * lifetime does not survive open(). */
    session->config_copy.model_id = session->model_id_owned;
    session->config_copy.model_path = session->model_path_owned;

    if (cce_aicimo_router_init(&session->router,
                                (size_t)config->aicimo_num_ops,
                                (size_t)config->aicimo_base_dim) != CCE_OK) {
        session_release(session);
        return CNET_HARNESS_ERR_INTERNAL;
    }
    session->router_ready = 1;

    int rc = harness_backend_open(session);
    if (rc != CNET_HARNESS_OK) {
        session_release(session);
        return rc;
    }

    *session_out = session;
    return CNET_HARNESS_OK;
}

int cnet_harness_open(const CnetHarnessConfig *config,
                      CnetHarnessSession **session_out) {
    return cnet_harness_open_internal(config, NULL, session_out);
}

int cnet_harness_open_with_offload(const CnetHarnessConfig *config,
                                   const CnetHarnessOffloadPolicy *policy,
                                   CnetHarnessSession **session_out) {
    if (!policy) {
        if (session_out) *session_out = NULL;
        return CNET_HARNESS_ERR_INVALID;
    }
    return cnet_harness_open_internal(config, policy, session_out);
}

int cnet_harness_get_offload_info(CnetHarnessSession *session,
                                  CnetHarnessOffloadInfo *info_out) {
    if (!session || session->magic != CNET_HARNESS_SESSION_MAGIC) {
        return CNET_HARNESS_ERR_STATE;
    }
    if (!info_out ||
        info_out->abi_version != CNET_HARNESS_OFFLOAD_ABI_VERSION ||
        info_out->struct_size != sizeof(*info_out)) {
        return CNET_HARNESS_ERR_INVALID;
    }
    if (!session->offload_enabled) return CNET_HARNESS_ERR_STATE;
    *info_out = session->offload_info;
    return CNET_HARNESS_OK;
}

int cnet_harness_close(CnetHarnessSession *session) {
    if (!session) return CNET_HARNESS_OK;
    if (session->magic != CNET_HARNESS_SESSION_MAGIC) return CNET_HARNESS_ERR_STATE;
    session_release(session);
    return CNET_HARNESS_OK;
}

void cnet_harness_generation_free(CnetHarnessGeneration *generation) {
    if (!generation) return;
    if (generation->text) free(generation->text);
    free(generation);
}

/* Perform one AICIMO decision against this session's persistent router and
 * translate it into a sampling profile. Known agent roles (cnet_agent_role)
 * route on their canonical name and, under AUTO sampling, start from the
 * role policy's preferred profile (uncertainty may still downgrade). Explicit
 * sampling overrides always win. Free-form roles keep the adapter→profile map. */
static int route_and_profile(struct CnetHarnessSession *session,
                              const char *role,
                              CnetHarnessSamplingMode override_mode,
                              uint32_t *adapter_out,
                              float *uncertainty_out,
                              CnetHarnessSamplingMode *effective_out,
                              int *override_used_out) {
    if (!session || session->magic != CNET_HARNESS_SESSION_MAGIC) {
        return CNET_HARNESS_ERR_STATE;
    }
    if (!role || role[0] == '\0') return CNET_HARNESS_ERR_INVALID;
    if (!session->router_ready) return CNET_HARNESS_ERR_STATE;

    CnetAgentRolePolicy agent_pol;
    int known_agent = (cnet_agent_role_resolve(role, &agent_pol) == 0);
    const char *route_role = known_agent ? agent_pol.name : role;

    size_t dim = (size_t)session->config_copy.aicimo_base_dim;
    float *scratch_in  = (float *)calloc(dim, sizeof(float));
    float *scratch_out = (float *)calloc(dim, sizeof(float));
    if (!scratch_in || !scratch_out) {
        free(scratch_in); free(scratch_out);
        return CNET_HARNESS_ERR_INTERNAL;
    }
    /* Deterministic zero input: adapter selection depends on role bias +
     * strength matrix, not on the input vector. */
    size_t selected = 0;
    float uncertainty = 1.0f;
    cce_result rc = cce_aicimo_route_decision(&session->router,
                                               scratch_in, dim,
                                               scratch_out, dim,
                                               route_role, &selected,
                                               &uncertainty);
    free(scratch_in);
    free(scratch_out);
    if (rc != CCE_OK) return CNET_HARNESS_ERR_INTERNAL;

    CnetHarnessSamplingMode profile;
    if (known_agent) {
        /* CnetAgentSamplingPref values match CnetHarnessSamplingMode 1..4. */
        profile = (CnetHarnessSamplingMode)agent_pol.preferred_sampling;
    } else {
        profile = cnet_harness__adapter_to_profile((uint32_t)selected);
    }
    profile = cnet_harness__downgrade_for_uncertainty(profile, uncertainty);

    int override_used = 0;
    if (override_mode != CNET_HARNESS_SAMPLING_AUTO) {
        profile = override_mode;
        override_used = 1;
    }

    if (adapter_out)     *adapter_out     = (uint32_t)selected;
    if (uncertainty_out) *uncertainty_out = uncertainty;
    if (effective_out)   *effective_out   = profile;
    if (override_used_out) *override_used_out = override_used;
    return CNET_HARNESS_OK;
}

int cnet_harness_count_tokens(CnetHarnessSession *session,
                               const char *text,
                               int32_t *count_out) {
    if (!count_out) return CNET_HARNESS_ERR_INVALID;
    *count_out = 0;
    if (!session || session->magic != CNET_HARNESS_SESSION_MAGIC) {
        return CNET_HARNESS_ERR_STATE;
    }
    if (!text) return CNET_HARNESS_ERR_INVALID;
    if (text[0] == '\0') return CNET_HARNESS_OK;   /* empty counts as 0 */
    return harness_backend_count_tokens(session, text, count_out);
}

int cnet_harness_probe_route(CnetHarnessSession *session,
                              const char *role,
                              CnetHarnessSamplingMode override_mode,
                              CnetHarnessRouteInfo *info_out) {
    if (!info_out) return CNET_HARNESS_ERR_INVALID;
    switch (override_mode) {
        case CNET_HARNESS_SAMPLING_AUTO:
        case CNET_HARNESS_SAMPLING_DETERMINISTIC:
        case CNET_HARNESS_SAMPLING_FOCUSED:
        case CNET_HARNESS_SAMPLING_BALANCED:
        case CNET_HARNESS_SAMPLING_EXPLORATORY:
            break;
        default:
            return CNET_HARNESS_ERR_INVALID;
    }
    if (info_out->abi_version != CNET_HARNESS_ABI_VERSION ||
        info_out->struct_size != sizeof(*info_out)) {
        return CNET_HARNESS_ERR_INVALID;
    }

    uint32_t adapter = 0;
    float unc = 1.0f;
    CnetHarnessSamplingMode profile = CNET_HARNESS_SAMPLING_DETERMINISTIC;
    int override_used = 0;
    double t0 = mono_ms_now();
    int rc = route_and_profile(session, role, override_mode,
                                &adapter, &unc, &profile, &override_used);
    double route_ms = mono_ms_now() - t0;
    if (rc != CNET_HARNESS_OK) {
        emit_route_log(session, "probe_route", role, 0, 1.0f,
                       CNET_HARNESS_SAMPLING_AUTO, 0, rc, route_ms, route_ms,
                       0, 0);
        return rc;
    }

    CnetHarnessSamplingParams params = cnet_harness__profile_params(profile);
    info_out->selected_adapter = adapter;
    info_out->route_uncertainty = unc;
    info_out->effective_sampling = profile;
    info_out->effective_temperature = params.temperature;
    info_out->effective_top_p = params.top_p;
    info_out->effective_top_k = params.top_k;
    info_out->effective_min_p = params.min_p;

    emit_route_log(session, "probe_route", role, adapter, unc, profile,
                   override_used, CNET_HARNESS_OK, route_ms, route_ms, 0, 0);
    return CNET_HARNESS_OK;
}

int cnet_harness_generate(CnetHarnessSession *session,
                           const CnetHarnessGenerateOptions *options,
                           CnetHarnessGeneration **generation_out) {
    if (!generation_out) return CNET_HARNESS_ERR_INVALID;
    *generation_out = NULL;
    if (!session || session->magic != CNET_HARNESS_SESSION_MAGIC) {
        return CNET_HARNESS_ERR_STATE;
    }
    if (!validate_generate_options(options)) return CNET_HARNESS_ERR_INVALID;

    uint32_t adapter = 0;
    float unc = 1.0f;
    CnetHarnessSamplingMode profile = CNET_HARNESS_SAMPLING_DETERMINISTIC;
    int override_used = 0;
    double t_all0 = mono_ms_now();
    double t_route0 = t_all0;
    int rc = route_and_profile(session, options->role, options->sampling,
                                &adapter, &unc, &profile, &override_used);
    double route_ms = mono_ms_now() - t_route0;
    if (rc != CNET_HARNESS_OK) {
        emit_route_log(session, "generate", options->role, 0, 1.0f,
                       CNET_HARNESS_SAMPLING_AUTO, 0, rc, route_ms,
                       mono_ms_now() - t_all0, 0, 0);
        return rc;
    }

    CnetHarnessGeneration *gen = (CnetHarnessGeneration *)
        calloc(1, sizeof(*gen));
    if (!gen) {
        emit_route_log(session, "generate", options->role, adapter, unc,
                       profile, override_used, CNET_HARNESS_ERR_INTERNAL,
                       route_ms, mono_ms_now() - t_all0, 0, 0);
        return CNET_HARNESS_ERR_INTERNAL;
    }
    gen->abi_version = CNET_HARNESS_ABI_VERSION;
    gen->struct_size = (uint32_t)sizeof(*gen);
    gen->selected_adapter = adapter;
    gen->route_uncertainty = unc;
    gen->effective_sampling = profile;
    gen->aicimo_override = override_used;

    CnetHarnessSamplingParams params = cnet_harness__profile_params(profile);
    gen->effective_temperature = params.temperature;
    gen->effective_top_p = params.top_p;
    gen->effective_top_k = params.top_k;
    gen->effective_min_p = params.min_p;

    /* Known agent roles: prepend the policy system fragment and route on the
     * canonical role name so backends see a stable stance. */
    CnetHarnessGenerateOptions opts = *options;
    char *composed_system = NULL;
    CnetAgentRolePolicy agent_pol;
    if (cnet_agent_role_resolve(options->role, &agent_pol) == 0) {
        composed_system = cnet_agent_role_compose_system(&agent_pol,
                                                         options->system);
        if (!composed_system) {
            cnet_harness_generation_free(gen);
            emit_route_log(session, "generate", options->role, adapter, unc,
                           profile, override_used, CNET_HARNESS_ERR_INTERNAL,
                           route_ms, mono_ms_now() - t_all0, 0, 0);
            return CNET_HARNESS_ERR_INTERNAL;
        }
        opts.system = composed_system;
        opts.role = agent_pol.name;
    }

    int brc = harness_backend_generate(session, &opts, profile, params, gen);
    free(composed_system);
    {
        double total_ms = mono_ms_now() - t_all0;
        uint32_t pt = (brc == CNET_HARNESS_OK) ? gen->prompt_tokens : 0u;
        uint32_t gt = (brc == CNET_HARNESS_OK) ? gen->generated_tokens : 0u;
        /* Prefer backend-reported wall times when present. */
        if (brc == CNET_HARNESS_OK &&
            (gen->prompt_ms > 0.0 || gen->generation_ms > 0.0)) {
            total_ms = route_ms + gen->prompt_ms + gen->generation_ms;
        }
        emit_route_log(session, "generate", options->role, adapter, unc,
                       profile, override_used, brc, route_ms, total_ms, pt, gt);
    }
    if (brc != CNET_HARNESS_OK) {
        cnet_harness_generation_free(gen);
        return brc;
    }

    *generation_out = gen;
    return CNET_HARNESS_OK;
}
