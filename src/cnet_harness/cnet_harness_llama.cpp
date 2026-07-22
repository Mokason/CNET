/*
 * cnet_harness_llama.cpp — llama.cpp backend for the CNET harness plugin.
 *
 * Provides strong overrides of harness_backend_open, harness_backend_generate,
 * harness_backend_prepare_close, and harness_backend_finish_close. Reuses the
 * proven load/unload + chat template + sampler patterns from
 * tools/cnet_llama_eval.cpp without altering that tool.
 *
 * Only linked into libcnet_harness.so, not into the hermetic contract test.
 *
 * Process-global state (llama_backend_init/free, ggml_backend_load_all) is
 * guarded by a mutex + refcount so multiple sessions cannot free global
 * state out from under one another.
 */
#include "cnet_harness_private.h"

extern "C" {
#include "../../include/cce/cce_qgkp.h"
#include "../../include/model_runtime.h"
#include "../../include/model_probe.h"
typedef struct cce_gguf cce_gguf;
cce_result cce_gguf_load(const char *path, cce_gguf **out);
void cce_gguf_free(cce_gguf *gguf);
int cce_gguf_get_n_layer(const cce_gguf *gguf);
}

#include "ggml-backend.h"
#include "llama.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct BackendContext {
    CnetHarnessSession *session = nullptr;
    uint64_t expected_resource_mask = 0;
    int main_gpu = -1;
    int is_cpu = 0;
    int loads = 0;
    int unloads = 0;
    int model_layer_count = 0;
    std::vector<ggml_backend_dev_t> selected_devices;
    std::vector<float> tensor_split;
    std::vector<std::size_t> free_vram_before;
};

struct BackendState {
    BackendContext load_ctx;
    llama_model *model = nullptr;
    llama_context *context = nullptr;
    std::vector<llama_token> cached_prompt_tokens;
    bool holds_global = false;
};

/* Process-global refcount for llama_backend_init/free and ggml_backend_load_all.
 * First acquire initializes; last release finalizes. Protects concurrent
 * sessions from tearing global state down while another session still holds
 * a model handle. */
std::mutex g_global_mutex;
int g_global_refcount = 0;

bool acquire_global_backend() {
    std::lock_guard<std::mutex> lock(g_global_mutex);
    if (g_global_refcount == 0) {
        ggml_backend_load_all();
        llama_backend_init();
    }
    ++g_global_refcount;
    return true;
}

void release_global_backend() {
    std::lock_guard<std::mutex> lock(g_global_mutex);
    if (g_global_refcount <= 0) return;
    --g_global_refcount;
    if (g_global_refcount == 0) {
        llama_backend_free();
    }
}

bool configure_bounded_offload(BackendContext *ctx,
                               const std::string &load_path,
                               llama_model_params *params) {
    if (!ctx || !ctx->session || !params || !ctx->session->offload_enabled) {
        return false;
    }
    const CnetHarnessOffloadPolicy &policy = ctx->session->offload_policy;

    cce_gguf *metadata = nullptr;
    if (cce_gguf_load(load_path.c_str(), &metadata) != CCE_OK || !metadata) {
        std::fprintf(stderr,
            "cnet_harness: bounded offload could not read GGUF metadata\n");
        return false;
    }
    ctx->model_layer_count = cce_gguf_get_n_layer(metadata);
    cce_gguf_free(metadata);
    if (ctx->model_layer_count <= 0 ||
        policy.gpu_layer_count >= ctx->model_layer_count) {
        std::fprintf(stderr,
            "cnet_harness: refusing non-partial offload (%d requested, %d model layers)\n",
            policy.gpu_layer_count, ctx->model_layer_count);
        return false;
    }

    std::vector<ggml_backend_dev_t> gpu_devices;
    const std::size_t backend_count = ggml_backend_dev_count();
    for (std::size_t i = 0; i < backend_count; ++i) {
        ggml_backend_dev_t device = ggml_backend_dev_get(i);
        if (device && ggml_backend_dev_type(device) ==
                GGML_BACKEND_DEVICE_TYPE_GPU) {
            gpu_devices.push_back(device);
        }
    }

    ctx->selected_devices.clear();
    ctx->free_vram_before.clear();
    for (uint32_t i = 0; i < policy.device_count; ++i) {
        const int requested = policy.device_indices[i];
        if (requested < 0 ||
            static_cast<std::size_t>(requested) >= gpu_devices.size()) {
            std::fprintf(stderr,
                "cnet_harness: GPU device index %d unavailable (%zu dedicated GPUs)\n",
                requested, gpu_devices.size());
            return false;
        }
        ggml_backend_dev_t device = gpu_devices[static_cast<std::size_t>(requested)];
        ctx->selected_devices.push_back(device);
        std::size_t free_bytes = 0;
        std::size_t total_bytes = 0;
        ggml_backend_dev_memory(device, &free_bytes, &total_bytes);
        ctx->free_vram_before.push_back(free_bytes);
    }
    ctx->selected_devices.push_back(nullptr);

    ctx->tensor_split.assign(llama_max_devices(), 0.0f);
    bool any_positive = false;
    for (uint32_t i = 0; i < policy.device_count; ++i) {
        ctx->tensor_split[i] = policy.tensor_split[i];
        any_positive = any_positive || policy.tensor_split[i] > 0.0f;
    }
    if (!any_positive) {
        for (uint32_t i = 0; i < policy.device_count; ++i) {
            ctx->tensor_split[i] = 1.0f;
        }
    }

    params->devices = ctx->selected_devices.data();
    params->n_gpu_layers = policy.gpu_layer_count;
    params->main_gpu = 0;
    params->split_mode = policy.device_count == 1u
        ? LLAMA_SPLIT_MODE_NONE
        : LLAMA_SPLIT_MODE_LAYER;
    params->tensor_split = policy.device_count == 1u
        ? nullptr
        : ctx->tensor_split.data();
    return true;
}

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
    if (ctx->is_cpu) {
        /* First-class CPU: no GPU offload, main_gpu ignored. */
        params.n_gpu_layers = 0;
        params.main_gpu = 0;
    } else {
        params.n_gpu_layers = -1;
        params.main_gpu = ctx->main_gpu < 0 ? 0 : ctx->main_gpu;
    }
    params.split_mode = LLAMA_SPLIT_MODE_NONE;
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
    if (ctx->session && ctx->session->offload_enabled &&
        !configure_bounded_offload(ctx, load_path, &params)) {
        return -1;
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

/* Generic chat-template application. No model-specific control tokens are
 * injected. If the model's real template cannot be resolved or applied, a
 * plain "System:/User:/Assistant:" fallback is used. */
static std::string apply_chat_template(llama_model *model,
                                       const std::string &system,
                                       const std::string &user) {
    const llama_chat_message messages[] = {
        {"system", system.c_str()},
        {"user", user.c_str()},
    };
    const char *chat_template = llama_model_chat_template(model, nullptr);
    if (!chat_template) {
        return "System: " + system + "\nUser: " + user + "\nAssistant:";
    }
    int32_t needed = llama_chat_apply_template(
        chat_template, messages, 2, true, nullptr, 0);
    if (needed < 0) {
        return "System: " + system + "\nUser: " + user + "\nAssistant:";
    }
    std::vector<char> buffer(static_cast<std::size_t>(needed) + 1u, '\0');
    int32_t written = llama_chat_apply_template(
        chat_template, messages, 2, true, buffer.data(),
        static_cast<int32_t>(buffer.size()));
    if (written < 0) {
        return "System: " + system + "\nUser: " + user + "\nAssistant:";
    }
    return std::string(buffer.data(), static_cast<std::size_t>(written));
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

static std::size_t common_prompt_prefix(
        const std::vector<llama_token> &left,
        const std::vector<llama_token> &right) {
    const std::size_t limit = left.size() < right.size()
        ? left.size() : right.size();
    std::size_t prefix = 0;
    while (prefix < limit && left[prefix] == right[prefix]) ++prefix;
    return prefix;
}

/* Build a sampler chain from the plugin's central profile-parameter table.
 * The backend never invents its own numbers; it consumes whatever the core
 * decided. Deterministic uses greedy; every other mode composes the same
 * top_k/top_p/min_p/temp chain from the passed-in params. */
static llama_sampler *build_sampler(CnetHarnessSamplingMode mode,
                                     CnetHarnessSamplingParams p,
                                     uint32_t seed) {
    llama_sampler *chain = llama_sampler_chain_init(
        llama_sampler_chain_default_params());
    if (!chain) return nullptr;
    if (mode == CNET_HARNESS_SAMPLING_DETERMINISTIC) {
        llama_sampler_chain_add(chain, llama_sampler_init_greedy());
        return chain;
    }
    if (p.top_k > 0u) {
        llama_sampler_chain_add(chain,
            llama_sampler_init_top_k(static_cast<int32_t>(p.top_k)));
    }
    if (p.top_p > 0.0f && p.top_p < 1.0f) {
        llama_sampler_chain_add(chain, llama_sampler_init_top_p(p.top_p, 1));
    }
    if (p.min_p > 0.0f) {
        llama_sampler_chain_add(chain, llama_sampler_init_min_p(p.min_p, 1));
    }
    llama_sampler_chain_add(chain, llama_sampler_init_temp(p.temperature));
    llama_sampler_chain_add(chain, llama_sampler_init_dist(seed));
    return chain;
}

bool publish_bounded_offload_info(CnetHarnessSession *session,
                                  BackendState *state) {
    if (!session || !state || !session->offload_enabled) return true;
    const CnetHarnessOffloadPolicy &policy = session->offload_policy;
    if (state->load_ctx.model_layer_count <= policy.gpu_layer_count ||
        state->load_ctx.selected_devices.size() <= policy.device_count ||
        state->load_ctx.free_vram_before.size() != policy.device_count) {
        return false;
    }

    CnetHarnessOffloadInfo info = session->offload_info;
    info.applied_gpu_layers = policy.gpu_layer_count;
    info.model_layer_count = state->load_ctx.model_layer_count;
    for (uint32_t i = 0; i < policy.device_count; ++i) {
        std::size_t free_after = 0;
        std::size_t total_bytes = 0;
        ggml_backend_dev_memory(state->load_ctx.selected_devices[i],
                                &free_after, &total_bytes);
        const std::size_t free_before = state->load_ctx.free_vram_before[i];
        const uint64_t delta = free_before > free_after
            ? static_cast<uint64_t>(free_before - free_after)
            : 0u;
        if (policy.max_vram_bytes_per_device > 0u &&
            delta > policy.max_vram_bytes_per_device) {
            std::fprintf(stderr,
                "cnet_harness: GPU %d residency %llu exceeds cap %llu bytes\n",
                policy.device_indices[i],
                static_cast<unsigned long long>(delta),
                static_cast<unsigned long long>(
                    policy.max_vram_bytes_per_device));
            return false;
        }
        info.vram_bytes[i] = delta;
    }
    session->offload_info = info;
    return true;
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
    state->load_ctx.session = session;
    state->load_ctx.expected_resource_mask = session->config_copy.resource_mask;
    state->load_ctx.main_gpu = session->config_copy.main_gpu;
    state->load_ctx.is_cpu =
        (session->config_copy.resource_mask == (uint64_t)CNET_HARNESS_RESOURCE_CPU)
            ? 1 : 0;

    acquire_global_backend();
    state->holds_global = true;

    CnetModelDescriptor descriptor{};
    if (cnet_model_descriptor_probe(
            &descriptor,
            session->config_copy.model_id,
            session->config_copy.model_path,
            "llama.cpp",
            session->config_copy.resource_mask,
            session->config_copy.resource_mask,
            1, 0, 0, 0) != CNET_MODEL_OK) {
        release_global_backend();
        state->holds_global = false;
        delete state;
        return CNET_HARNESS_ERR_MODEL_LOAD;
    }
    if (descriptor.model_class != CNET_MODEL_CLASS_DENSE_TRANSFORMER) {
        release_global_backend();
        state->holds_global = false;
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
        release_global_backend();
        state->holds_global = false;
        delete state;
        return CNET_HARNESS_ERR_MODEL_LOAD;
    }

    if (cnet_model_acquire(session->manager, descriptor.model_id,
                           session->config_copy.resource_mask,
                           &session->lease) != CNET_MODEL_OK ||
        session->lease.resource_mask != session->config_copy.resource_mask) {
        cnet_model_manager_close(session->manager);
        session->manager = nullptr;
        release_global_backend();
        state->holds_global = false;
        delete state;
        return CNET_HARNESS_ERR_MODEL_LOAD;
    }
    session->lease_held = 1;

    state->model = static_cast<llama_model *>(session->lease.handle);

    llama_context_params ctx_params = llama_context_default_params();
    ctx_params.n_ctx = session->config_copy.n_ctx;
    ctx_params.n_batch = session->config_copy.n_batch;
    ctx_params.n_ubatch = session->config_copy.n_batch < 512u
        ? session->config_copy.n_batch : 512u;
    ctx_params.n_seq_max = 1;
    /* Keep recurrent rollback snapshots disabled intentionally. llama.cpp
     * allocates recurrent state as (1 + n_rs_seq) rows per cell and layer, so
     * even a small non-zero value can multiply hybrid-model memory. Pure KV
     * models support seq_rm without snapshots; hybrid models fail seq_rm and
     * take the correctness-preserving full-prefill fallback below. */
    ctx_params.n_rs_seq = 0;
    ctx_params.n_threads = session->config_copy.n_threads;
    ctx_params.n_threads_batch = session->config_copy.n_threads;
    ctx_params.offload_kqv = session->offload_enabled
        ? session->offload_policy.offload_kqv != 0u
        : !state->load_ctx.is_cpu;
    ctx_params.no_perf = false;

    state->context = llama_init_from_model(state->model, ctx_params);
    if (!state->context || !publish_bounded_offload_info(session, state)) {
        if (state->context) {
            llama_free(state->context);
            state->context = nullptr;
        }
        cnet_model_release(session->manager, &session->lease);
        session->lease_held = 0;
        cnet_model_manager_close(session->manager);
        session->manager = nullptr;
        release_global_backend();
        state->holds_global = false;
        delete state;
        return CNET_HARNESS_ERR_BACKEND;
    }

    session->backend_state = state;
    return CNET_HARNESS_OK;
}

int harness_backend_count_tokens(struct CnetHarnessSession *session,
                                  const char *text,
                                  int32_t *count_out) {
    if (!session || !text || !count_out) return CNET_HARNESS_ERR_INTERNAL;
    auto *state = static_cast<BackendState *>(session->backend_state);
    if (!state || !state->model) return CNET_HARNESS_ERR_STATE;

    const std::size_t len = std::strlen(text);
    if (len > static_cast<std::size_t>(INT32_MAX)) return CNET_HARNESS_ERR_INVALID;

    try {
        const llama_vocab *vocab = llama_model_get_vocab(state->model);
        /* add_special=false, parse_special=false: the caller is costing a text
         * fragment, not building a prompt — BOS/EOS and template markup belong
         * to the prompt builder, and special-token markup inside user text
         * must count as the literal text it is. A NULL buffer makes
         * llama_tokenize return the negated required count. */
        int32_t count = llama_tokenize(vocab, text, static_cast<int32_t>(len),
                                       nullptr, 0, false, false);
        if (count < 0) count = -count;
        *count_out = count;
        return CNET_HARNESS_OK;
    } catch (...) {
        return CNET_HARNESS_ERR_INTERNAL;
    }
}

int harness_backend_generate(struct CnetHarnessSession *session,
                              const CnetHarnessGenerateOptions *options,
                              CnetHarnessSamplingMode effective,
                              CnetHarnessSamplingParams params,
                              CnetHarnessGeneration *generation) {
    if (!session || !options || !generation) return CNET_HARNESS_ERR_INTERNAL;
    auto *state = static_cast<BackendState *>(session->backend_state);
    if (!state || !state->model || !state->context) return CNET_HARNESS_ERR_STATE;

    try {
    const std::string system_text = options->system ? options->system : "";
    const std::string user_text = options->user;
    const std::string formatted = apply_chat_template(
        state->model, system_text, user_text);
    if (formatted.empty()) return CNET_HARNESS_ERR_BACKEND;

    const llama_vocab *vocab = llama_model_get_vocab(state->model);
    std::vector<llama_token> prompt_tokens = tokenize_all(vocab, formatted);
    if (prompt_tokens.empty()) {
        return CNET_HARNESS_ERR_BACKEND;
    }
    /* Context bound: reject upfront if the prompt already fills the window,
     * or cap max_tokens so decode never drives us past n_ctx. Discarding a
     * partial response after llama_decode fails would be dishonest. */
    const uint32_t n_ctx = session->config_copy.n_ctx;
    if (prompt_tokens.size() >= n_ctx) {
        return CNET_HARNESS_ERR_INVALID;
    }
    const uint32_t max_new = static_cast<uint32_t>(
        n_ctx - static_cast<uint32_t>(prompt_tokens.size()));
    uint32_t cap_max_tokens = options->max_tokens < max_new
        ? options->max_tokens : max_new;

    std::unique_ptr<llama_sampler, decltype(&llama_sampler_free)> sampler(
        build_sampler(effective, params, options->seed), &llama_sampler_free);
    if (!sampler) return CNET_HARNESS_ERR_BACKEND;

    llama_memory_t memory = llama_get_memory(state->context);
    std::size_t reused_prompt_tokens = 0;
    if (!state->cached_prompt_tokens.empty() && prompt_tokens.size() > 1u) {
        const std::size_t common = common_prompt_prefix(
            state->cached_prompt_tokens, prompt_tokens);
        const std::size_t candidate = common < prompt_tokens.size() - 1u
            ? common : prompt_tokens.size() - 1u;
        if (candidate > 0u && llama_memory_seq_rm(
                memory, 0, static_cast<llama_pos>(candidate), -1)) {
            reused_prompt_tokens = candidate;
        } else {
            llama_memory_clear(memory, false);
        }
    } else {
        llama_memory_clear(memory, false);
    }

    const auto prompt_start = std::chrono::steady_clock::now();
    int decode_rc = 0;
    const std::size_t n_batch = session->config_copy.n_batch;
    for (std::size_t offset = reused_prompt_tokens;
         offset < prompt_tokens.size(); offset += n_batch) {
        const std::size_t remaining = prompt_tokens.size() - offset;
        const std::size_t chunk_size = remaining < n_batch ? remaining : n_batch;
        llama_batch batch = llama_batch_get_one(
            prompt_tokens.data() + offset, static_cast<int32_t>(chunk_size));
        decode_rc = llama_decode(state->context, batch);
        if (decode_rc != 0) break;
    }
    const auto prompt_end = std::chrono::steady_clock::now();
    if (decode_rc != 0) {
        state->cached_prompt_tokens.clear();
        llama_memory_clear(memory, false);
        return CNET_HARNESS_ERR_BACKEND;
    }

    llama_batch batch{};
    std::string response;
    uint32_t generated = 0;
    const auto gen_start = std::chrono::steady_clock::now();
    for (uint32_t i = 0; i < cap_max_tokens; ++i) {
        llama_token tok = llama_sampler_sample(sampler.get(), state->context, -1);
        if (llama_vocab_is_eog(vocab, tok)) break;
        response += token_piece(vocab, tok);
        generated++;
        batch = llama_batch_get_one(&tok, 1);
        decode_rc = llama_decode(state->context, batch);
        if (decode_rc != 0) {
            state->cached_prompt_tokens.clear();
            llama_memory_clear(memory, false);
            return CNET_HARNESS_ERR_BACKEND;
        }
    }
    const auto gen_end = std::chrono::steady_clock::now();
    try {
        state->cached_prompt_tokens = prompt_tokens;
    } catch (...) {
        state->cached_prompt_tokens.clear();
    }

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
    } catch (...) {
        state->cached_prompt_tokens.clear();
        llama_memory_clear(llama_get_memory(state->context), false);
        return CNET_HARNESS_ERR_INTERNAL;
    }
}

void harness_backend_prepare_close(struct CnetHarnessSession *session) {
    if (!session) return;
    auto *state = static_cast<BackendState *>(session->backend_state);
    if (!state) return;
    if (state->context) {
        llama_free(state->context);
        state->context = nullptr;
    }
    /* Do NOT touch the global backend or the model handle here — the model
     * is still leased and other sessions may share global state. */
}

void harness_backend_finish_close(struct CnetHarnessSession *session) {
    if (!session) return;
    auto *state = static_cast<BackendState *>(session->backend_state);
    if (!state) return;
    /* state->model is owned by the manager lease which has already been
     * released and destroyed by the core; do not free directly. */
    if (state->holds_global) {
        release_global_backend();
        state->holds_global = false;
    }
    delete state;
    session->backend_state = nullptr;
}

}  /* extern "C" */
