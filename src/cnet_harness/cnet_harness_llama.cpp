/*
 * cnet_harness_llama.cpp — llama.cpp backend for the CNET harness plugin.
 *
 * Provides strong overrides of harness_backend_open/generate/close. Reuses the
 * proven load/unload + chat template + sampler patterns from
 * tools/cnet_llama_eval.cpp without altering that tool.
 *
 * Only linked into libcnet_harness.so, not into the hermetic contract test.
 * AMD/ROCm only; no CUDA-specific claims.
 */
#include "cnet_harness_private.h"

extern "C" {
#include "../../include/cce/cce_qgkp.h"
#include "../../include/model_runtime.h"
#include "../../include/model_probe.h"
}

#include "ggml-backend.h"
#include "llama.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct BackendContext {
    uint64_t expected_resource_mask = 0;
    int main_gpu = 0;
    int loads = 0;
    int unloads = 0;
};

struct BackendState {
    BackendContext load_ctx;
    llama_model *model = nullptr;
    llama_context *context = nullptr;
    llama_sampler *sampler = nullptr;
    /* The sampler is owned per-generate call so it can be rebuilt each time
     * AICIMO picks a different profile. */
    bool backend_inited = false;
};

extern "C" int cnet_harness__llama_load(void *opaque,
                                         const CnetModelDescriptor *descriptor,
                                         uint64_t resource_mask,
                                         void **handle_out,
                                         uint64_t *resident_bytes_out) {
    auto *ctx = static_cast<BackendContext *>(opaque);
    if (!ctx || !descriptor || !handle_out || !resident_bytes_out ||
        resource_mask != ctx->expected_resource_mask) {
        return -1;
    }

    llama_model_params params = llama_model_default_params();
    params.n_gpu_layers = -1;
    params.split_mode = LLAMA_SPLIT_MODE_NONE;
    params.main_gpu = ctx->main_gpu;
    params.use_mmap = true;
    params.use_mlock = false;
    params.check_tensors = false;

    std::string load_path = descriptor->artifact_path;
    if (std::strcmp(descriptor->format, "qwen2-pack") == 0) {
        cce_qgkp_info qgkp{};
        if (cce_qgkp_inspect(descriptor->artifact_path, &qgkp) != CCE_OK ||
            qgkp.version != CCE_QGKP_VERSION_ENVELOPE) {
            return -1;
        }
        load_path += ".gguf.cache";
        if (cce_qgkp_materialize_gguf(descriptor->artifact_path,
                                      load_path.c_str()) != CCE_OK) {
            return -1;
        }
    }

    llama_model *model = llama_model_load_from_file(load_path.c_str(), params);
    if (!model) return -1;

    *handle_out = model;
    *resident_bytes_out = llama_model_size(model);
    ctx->loads++;
    return 0;
}

extern "C" void cnet_harness__llama_unload(void *opaque, void *handle) {
    auto *ctx = static_cast<BackendContext *>(opaque);
    if (handle) llama_model_free(static_cast<llama_model *>(handle));
    if (ctx) ctx->unloads++;
}

/* Small helpers (kept local to avoid a broad refactor of cnet_llama_eval). */
static std::string apply_chat_template(llama_model *model,
                                       const std::string &system,
                                       const std::string &user) {
    const std::string user_no_think = user + "\n/no_think";
    const llama_chat_message messages[] = {
        {"system", system.c_str()},
        {"user", user_no_think.c_str()},
    };
    const char *chat_template = llama_model_chat_template(model, nullptr);
    int32_t needed = llama_chat_apply_template(
        chat_template, messages, 2, true, nullptr, 0);
    if (needed < 0) return "System: " + system + "\nUser: " + user + "\nAssistant:";
    std::vector<char> buffer(static_cast<std::size_t>(needed) + 1u, '\0');
    int32_t written = llama_chat_apply_template(
        chat_template, messages, 2, true, buffer.data(),
        static_cast<int32_t>(buffer.size()));
    if (written < 0) return std::string();
    return std::string(buffer.data(), static_cast<std::size_t>(written)) +
           "<think>\n\n</think>\n\n";
}

static std::vector<llama_token> tokenize_all(const llama_vocab *vocab,
                                              const std::string &text) {
    int32_t count = llama_tokenize(vocab, text.data(),
                                   static_cast<int32_t>(text.size()),
                                   nullptr, 0, true, true);
    if (count < 0) count = -count;
    std::vector<llama_token> tokens(static_cast<std::size_t>(count));
    int32_t written = llama_tokenize(vocab, text.data(),
                                     static_cast<int32_t>(text.size()),
                                     tokens.data(), count, true, true);
    if (written < 0) return {};
    tokens.resize(static_cast<std::size_t>(written));
    return tokens;
}

static std::string token_piece(const llama_vocab *vocab, llama_token tok) {
    std::vector<char> buffer(256);
    int32_t written = llama_token_to_piece(
        vocab, tok, buffer.data(), static_cast<int32_t>(buffer.size()), 0, true);
    if (written < 0) {
        buffer.resize(static_cast<std::size_t>(-written));
        written = llama_token_to_piece(
            vocab, tok, buffer.data(), static_cast<int32_t>(buffer.size()), 0, true);
    }
    if (written < 0) return {};
    return std::string(buffer.data(), static_cast<std::size_t>(written));
}

static llama_sampler *build_sampler(CnetHarnessSamplingMode mode, uint32_t seed) {
    llama_sampler *chain = llama_sampler_chain_init(
        llama_sampler_chain_default_params());
    if (!chain) return nullptr;
    switch (mode) {
        case CNET_HARNESS_SAMPLING_DETERMINISTIC:
            llama_sampler_chain_add(chain, llama_sampler_init_greedy());
            break;
        case CNET_HARNESS_SAMPLING_FOCUSED:
            llama_sampler_chain_add(chain, llama_sampler_init_top_k(40));
            llama_sampler_chain_add(chain, llama_sampler_init_top_p(0.85f, 1));
            llama_sampler_chain_add(chain, llama_sampler_init_min_p(0.05f, 1));
            llama_sampler_chain_add(chain, llama_sampler_init_temp(0.30f));
            llama_sampler_chain_add(chain, llama_sampler_init_dist(seed));
            break;
        case CNET_HARNESS_SAMPLING_BALANCED:
        case CNET_HARNESS_SAMPLING_AUTO:
            llama_sampler_chain_add(chain, llama_sampler_init_top_k(40));
            llama_sampler_chain_add(chain, llama_sampler_init_top_p(0.90f, 1));
            llama_sampler_chain_add(chain, llama_sampler_init_min_p(0.05f, 1));
            llama_sampler_chain_add(chain, llama_sampler_init_temp(0.70f));
            llama_sampler_chain_add(chain, llama_sampler_init_dist(seed));
            break;
        case CNET_HARNESS_SAMPLING_EXPLORATORY:
            llama_sampler_chain_add(chain, llama_sampler_init_top_k(80));
            llama_sampler_chain_add(chain, llama_sampler_init_top_p(0.95f, 1));
            llama_sampler_chain_add(chain, llama_sampler_init_min_p(0.03f, 1));
            llama_sampler_chain_add(chain, llama_sampler_init_temp(0.95f));
            llama_sampler_chain_add(chain, llama_sampler_init_dist(seed));
            break;
    }
    return chain;
}

}  /* namespace */

extern "C" {

int harness_backend_open(struct CnetHarnessSession *session) {
    if (!session) return CNET_HARNESS_ERR_INTERNAL;
    if (session->backend_state) return CNET_HARNESS_ERR_STATE;

    BackendState *state = nullptr;
    try {
        state = new BackendState();
    } catch (...) {
        return CNET_HARNESS_ERR_INTERNAL;
    }
    state->load_ctx.expected_resource_mask = session->config_copy.resource_mask;
    state->load_ctx.main_gpu = session->config_copy.main_gpu;

    ggml_backend_load_all();
    llama_backend_init();
    state->backend_inited = true;

    CnetModelDescriptor descriptor{};
    if (cnet_model_descriptor_probe(
            &descriptor,
            session->config_copy.model_id,
            session->config_copy.model_path,
            "llama.cpp",
            session->config_copy.resource_mask,
            session->config_copy.resource_mask,
            1, 0, 0, 0) != CNET_MODEL_OK) {
        llama_backend_free();
        delete state;
        return CNET_HARNESS_ERR_MODEL_LOAD;
    }
    if (descriptor.model_class != CNET_MODEL_CLASS_DENSE_TRANSFORMER) {
        llama_backend_free();
        delete state;
        return CNET_HARNESS_ERR_MODEL_LOAD;
    }

    CnetModelBudget budget{session->config_copy.resource_mask,
                           session->config_copy.budget_bytes};
    CnetModelManagerOptions options{};
    options.abi_version = CNET_MODEL_RUNTIME_ABI_VERSION;
    options.struct_size = sizeof options;
    options.max_models = 4;
    options.max_backends = 2;
    options.budgets = &budget;
    options.budget_count = 1;

    CnetModelBackendSpec backend{};
    backend.abi_version = CNET_MODEL_RUNTIME_ABI_VERSION;
    backend.struct_size = sizeof backend;
    std::snprintf(backend.name, sizeof backend.name, "%s", "llama.cpp");
    backend.context = &state->load_ctx;
    backend.load = cnet_harness__llama_load;
    backend.unload = cnet_harness__llama_unload;

    if (cnet_model_manager_open(&session->manager, &options) != CNET_MODEL_OK ||
        cnet_model_backend_register(session->manager, &backend) != CNET_MODEL_OK ||
        cnet_model_catalog_add(session->manager, &descriptor) != CNET_MODEL_OK) {
        if (session->manager) cnet_model_manager_close(session->manager);
        session->manager = nullptr;
        llama_backend_free();
        delete state;
        return CNET_HARNESS_ERR_MODEL_LOAD;
    }

    if (cnet_model_acquire(session->manager, descriptor.model_id,
                           session->config_copy.resource_mask,
                           &session->lease) != CNET_MODEL_OK ||
        session->lease.resource_mask != session->config_copy.resource_mask) {
        cnet_model_manager_close(session->manager);
        session->manager = nullptr;
        llama_backend_free();
        delete state;
        return CNET_HARNESS_ERR_MODEL_LOAD;
    }
    session->lease_held = 1;

    state->model = static_cast<llama_model *>(session->lease.handle);

    llama_context_params ctx_params = llama_context_default_params();
    ctx_params.n_ctx = session->config_copy.n_ctx;
    ctx_params.n_batch = session->config_copy.n_batch;
    ctx_params.n_ubatch = 512;
    ctx_params.n_seq_max = 1;
    ctx_params.n_threads = session->config_copy.n_threads;
    ctx_params.n_threads_batch = session->config_copy.n_threads;
    ctx_params.offload_kqv = true;
    ctx_params.no_perf = false;

    state->context = llama_init_from_model(state->model, ctx_params);
    if (!state->context) {
        cnet_model_release(session->manager, &session->lease);
        session->lease_held = 0;
        cnet_model_manager_close(session->manager);
        session->manager = nullptr;
        llama_backend_free();
        delete state;
        return CNET_HARNESS_ERR_BACKEND;
    }

    session->backend_state = state;
    return CNET_HARNESS_OK;
}

int harness_backend_generate(struct CnetHarnessSession *session,
                              const CnetHarnessGenerateOptions *options,
                              CnetHarnessSamplingMode effective,
                              CnetHarnessGeneration *generation) {
    if (!session || !options || !generation) return CNET_HARNESS_ERR_INTERNAL;
    auto *state = static_cast<BackendState *>(session->backend_state);
    if (!state || !state->model || !state->context) return CNET_HARNESS_ERR_STATE;

    const std::string system_text = options->system ? options->system : "";
    const std::string user_text = options->user;
    const std::string formatted = apply_chat_template(
        state->model, system_text, user_text);
    if (formatted.empty()) return CNET_HARNESS_ERR_BACKEND;

    const llama_vocab *vocab = llama_model_get_vocab(state->model);
    std::vector<llama_token> prompt_tokens = tokenize_all(vocab, formatted);
    if (prompt_tokens.empty() ||
        prompt_tokens.size() >= session->config_copy.n_ctx) {
        return CNET_HARNESS_ERR_BACKEND;
    }

    llama_memory_clear(llama_get_memory(state->context), true);

    llama_sampler *sampler = build_sampler(effective, options->seed);
    if (!sampler) return CNET_HARNESS_ERR_BACKEND;

    const auto prompt_start = std::chrono::steady_clock::now();
    llama_batch batch = llama_batch_get_one(
        prompt_tokens.data(), static_cast<int32_t>(prompt_tokens.size()));
    int decode_rc = llama_decode(state->context, batch);
    const auto prompt_end = std::chrono::steady_clock::now();
    if (decode_rc != 0) {
        llama_sampler_free(sampler);
        return CNET_HARNESS_ERR_BACKEND;
    }

    std::string response;
    uint32_t generated = 0;
    const auto gen_start = std::chrono::steady_clock::now();
    for (uint32_t i = 0; i < options->max_tokens; ++i) {
        llama_token tok = llama_sampler_sample(sampler, state->context, -1);
        if (llama_vocab_is_eog(vocab, tok)) break;
        response += token_piece(vocab, tok);
        generated++;
        batch = llama_batch_get_one(&tok, 1);
        decode_rc = llama_decode(state->context, batch);
        if (decode_rc != 0) {
            llama_sampler_free(sampler);
            return CNET_HARNESS_ERR_BACKEND;
        }
    }
    const auto gen_end = std::chrono::steady_clock::now();
    llama_sampler_free(sampler);

    char *text = static_cast<char *>(std::malloc(response.size() + 1u));
    if (!text) return CNET_HARNESS_ERR_INTERNAL;
    std::memcpy(text, response.data(), response.size());
    text[response.size()] = '\0';

    generation->text = text;
    generation->prompt_tokens = static_cast<uint32_t>(prompt_tokens.size());
    generation->generated_tokens = generated;
    generation->prompt_ms = std::chrono::duration<double, std::milli>(
        prompt_end - prompt_start).count();
    generation->generation_ms = std::chrono::duration<double, std::milli>(
        gen_end - gen_start).count();
    return CNET_HARNESS_OK;
}

void harness_backend_close(struct CnetHarnessSession *session) {
    if (!session) return;
    auto *state = static_cast<BackendState *>(session->backend_state);
    if (!state) return;
    if (state->context) {
        llama_free(state->context);
        state->context = nullptr;
    }
    /* state->model is owned by the manager lease; do not free directly. */
    if (state->backend_inited) {
        llama_backend_free();
        state->backend_inited = false;
    }
    delete state;
    session->backend_state = nullptr;
}

}  /* extern "C" */
