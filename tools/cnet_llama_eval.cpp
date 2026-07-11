#include "../include/model_runtime.h"
#include "../include/model_probe.h"
#include "../include/cce/cce_qgkp.h"

#include "ggml-backend.h"
#include "llama.h"

#include <chrono>
#include <cctype>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

struct BackendContext {
    uint64_t expected_resource_mask = 0;
    int main_gpu = 0;
    int loads = 0;
    int unloads = 0;
};

struct PromptCase {
    std::string id;
    std::string prompt;
};

struct SampleResult {
    std::string id;
    std::string prompt;
    std::string response;
    int prompt_tokens = 0;
    int generated_tokens = 0;
    int repair_attempts = 0;
    double prompt_ms = 0.0;
    double generation_ms = 0.0;
};

static void usage(const char *argv0) {
    std::fprintf(stderr,
        "usage: %s --model FILE --prompts TSV --output JSONL "
        "[--resource gpu0|gpu1] [--main-gpu N] [--ctx N] "
        "[--max-tokens N] [--temperature F] [--seed N]\n",
        argv0);
}

static std::string json_escape(const std::string &value) {
    std::ostringstream out;
    for (unsigned char ch : value) {
        switch (ch) {
            case '\"': out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\b': out << "\\b"; break;
            case '\f': out << "\\f"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (ch < 0x20) {
                    out << "\\u" << std::hex << std::setw(4)
                        << std::setfill('0') << static_cast<int>(ch)
                        << std::dec << std::setfill(' ');
                } else {
                    out << static_cast<char>(ch);
                }
        }
    }
    return out.str();
}

static std::vector<PromptCase> read_prompts(const std::string &path) {
    std::ifstream input(path);
    std::vector<PromptCase> prompts;
    std::string line;
    if (!input) throw std::runtime_error("cannot open prompts file: " + path);
    while (std::getline(input, line)) {
        if (line.empty() || line[0] == '#') continue;
        const std::size_t separator = line.find('\t');
        if (separator == std::string::npos || separator == 0 ||
            separator + 1 >= line.size()) {
            throw std::runtime_error("prompt line must be id<TAB>prompt");
        }
        prompts.push_back({line.substr(0, separator), line.substr(separator + 1)});
    }
    if (prompts.empty()) throw std::runtime_error("no prompts found");
    return prompts;
}

extern "C" int cnet_llama_load(void *opaque,
                                const CnetModelDescriptor *descriptor,
                                uint64_t resource_mask,
                                void **handle_out,
                                uint64_t *resident_bytes_out) {
    auto *context = static_cast<BackendContext *>(opaque);
    if (!context || !descriptor || !handle_out || !resident_bytes_out ||
        resource_mask != context->expected_resource_mask) {
        return -1;
    }

    llama_model_params params = llama_model_default_params();
    params.n_gpu_layers = -1;
    params.split_mode = LLAMA_SPLIT_MODE_NONE;
    params.main_gpu = context->main_gpu;
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
    context->loads++;
    return 0;
}

extern "C" void cnet_llama_unload(void *opaque, void *handle) {
    auto *context = static_cast<BackendContext *>(opaque);
    if (handle) llama_model_free(static_cast<llama_model *>(handle));
    if (context) context->unloads++;
}

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
    if (needed < 0) {
        return "System: " + system + "\nUser: " + user + "\nAssistant:";
    }
    std::vector<char> buffer(static_cast<std::size_t>(needed) + 1u, '\0');
    int32_t written = llama_chat_apply_template(
        chat_template, messages, 2, true, buffer.data(),
        static_cast<int32_t>(buffer.size()));
    if (written < 0) throw std::runtime_error("chat template application failed");
    /* llama_chat_apply_template() cannot pass Qwen's enable_thinking=false
       template kwarg. Prefill a completed empty reasoning block in the active
       assistant turn so generation starts in final-answer mode. */
    return std::string(buffer.data(), static_cast<std::size_t>(written)) +
           "<think>\n\n</think>\n\n";
}

static std::vector<llama_token> tokenize(const llama_vocab *vocab,
                                         const std::string &text) {
    int32_t count = llama_tokenize(vocab, text.data(),
                                   static_cast<int32_t>(text.size()),
                                   nullptr, 0, true, true);
    if (count == INT32_MIN) throw std::runtime_error("token count overflow");
    if (count < 0) count = -count;
    std::vector<llama_token> tokens(static_cast<std::size_t>(count));
    int32_t written = llama_tokenize(vocab, text.data(),
                                     static_cast<int32_t>(text.size()),
                                     tokens.data(), count, true, true);
    if (written < 0) throw std::runtime_error("tokenization failed");
    tokens.resize(static_cast<std::size_t>(written));
    return tokens;
}

static std::string token_piece(const llama_vocab *vocab, llama_token token) {
    std::vector<char> buffer(256);
    int32_t written = llama_token_to_piece(
        vocab, token, buffer.data(), static_cast<int32_t>(buffer.size()), 0, true);
    if (written < 0) {
        buffer.resize(static_cast<std::size_t>(-written));
        written = llama_token_to_piece(
            vocab, token, buffer.data(), static_cast<int32_t>(buffer.size()), 0, true);
    }
    if (written < 0) throw std::runtime_error("token to piece conversion failed");
    return std::string(buffer.data(), static_cast<std::size_t>(written));
}

static std::string lowercase(std::string value) {
    for (char &ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value;
}

static int count_sentences(const std::string &text) {
    int count = 0;
    bool in_terminal_run = false;
    for (char ch : text) {
        const bool terminal = ch == '.' || ch == '!' || ch == '?';
        if (terminal && !in_terminal_run) count++;
        in_terminal_run = terminal;
    }
    return count;
}

static int count_words(const std::string &text) {
    int count = 0;
    bool in_word = false;
    for (unsigned char ch : text) {
        const bool word = std::isalnum(ch) || ch == '\'' || ch == '-';
        if (word && !in_word) count++;
        in_word = word;
    }
    return count;
}

static std::string constraint_feedback(const PromptCase &test,
                                       const std::string &response) {
    std::vector<std::string> failures;
    const std::string lower = lowercase(response);
    if (response.find("<think>") != std::string::npos &&
        response.find("</think>") == std::string::npos) {
        failures.emplace_back("the reasoning block was not closed");
    }
    if (test.id == "causal_reasoning" && count_sentences(response) != 3) {
        failures.emplace_back("the answer must contain exactly three sentences");
    }
    if (test.id == "constraint_following") {
        std::size_t cursor = 0;
        for (int index = 1; index <= 4; ++index) {
            const std::string marker = std::to_string(index) + ".";
            cursor = response.find(marker, cursor);
            if (cursor == std::string::npos) {
                failures.emplace_back("the answer must contain exactly four numbered steps");
                break;
            }
            cursor += marker.size();
        }
        if (lower.find("heat") != std::string::npos ||
            lower.find("sunlight") != std::string::npos ||
            lower.find("laminat") != std::string::npos) {
            failures.emplace_back("the answer used a prohibited preservation method");
        }
    }
    if (test.id == "narrative_continuity") {
        if (count_words(response) > 140) {
            failures.emplace_back("the story must be 140 words or fewer");
        }
        const std::size_t last_break = response.find_last_of(".!?");
        const std::size_t prior_break = last_break == std::string::npos || last_break == 0
            ? std::string::npos
            : response.find_last_of(".!?", last_break - 1);
        const std::string final_sentence = lowercase(response.substr(
            prior_break == std::string::npos ? 0 : prior_break + 1));
        if (final_sentence.find("map") == std::string::npos &&
            final_sentence.find("without it") == std::string::npos &&
            final_sentence.find("route") == std::string::npos &&
            final_sentence.find("direction") == std::string::npos &&
            final_sentence.find("location") == std::string::npos) {
            failures.emplace_back(
                "the final consequence must explicitly result from losing the map");
        }
    }
    std::ostringstream feedback;
    for (std::size_t i = 0; i < failures.size(); ++i) {
        if (i) feedback << "; ";
        feedback << failures[i];
    }
    return feedback.str();
}

static SampleResult generate_one(llama_model *model,
                                 const PromptCase &test,
                                 uint32_t n_ctx,
                                 int max_tokens,
                                 float temperature,
                                 uint32_t seed) {
    const std::string system =
        "You are a coherent assistant. Output only the final answer without "
        "analysis or chain-of-thought. Follow every explicit constraint and "
        "keep causal details consistent. Numeric limits are hard: when an exact "
        "count is requested, produce exactly that count; when a maximum word "
        "count is requested, target at least 20 percent below it. Do not invent "
        "factual claims.";
    const std::string formatted = apply_chat_template(model, system, test.prompt);
    const llama_vocab *vocab = llama_model_get_vocab(model);
    std::vector<llama_token> prompt_tokens = tokenize(vocab, formatted);
    if (prompt_tokens.empty() || prompt_tokens.size() >= n_ctx) {
        throw std::runtime_error("prompt does not fit context");
    }

    llama_context_params context_params = llama_context_default_params();
    context_params.n_ctx = n_ctx;
    context_params.n_batch = n_ctx;
    context_params.n_ubatch = 512;
    context_params.n_seq_max = 1;
    context_params.n_threads = 8;
    context_params.n_threads_batch = 16;
    context_params.offload_kqv = true;
    context_params.no_perf = false;

    llama_context *context = llama_init_from_model(model, context_params);
    if (!context) throw std::runtime_error("llama context creation failed");

    llama_sampler *sampler = llama_sampler_chain_init(
        llama_sampler_chain_default_params());
    if (!sampler) {
        llama_free(context);
        throw std::runtime_error("sampler creation failed");
    }
    if (temperature <= 0.0f) {
        llama_sampler_chain_add(sampler, llama_sampler_init_greedy());
    } else {
        llama_sampler_chain_add(sampler, llama_sampler_init_top_k(40));
        llama_sampler_chain_add(sampler, llama_sampler_init_top_p(0.90f, 1));
        llama_sampler_chain_add(sampler, llama_sampler_init_min_p(0.05f, 1));
        llama_sampler_chain_add(sampler, llama_sampler_init_temp(temperature));
        llama_sampler_chain_add(sampler, llama_sampler_init_dist(seed));
    }

    SampleResult result;
    result.id = test.id;
    result.prompt = test.prompt;
    result.prompt_tokens = static_cast<int>(prompt_tokens.size());

    const auto prompt_start = std::chrono::steady_clock::now();
    llama_batch batch = llama_batch_get_one(
        prompt_tokens.data(), static_cast<int32_t>(prompt_tokens.size()));
    int decode_result = llama_decode(context, batch);
    const auto prompt_end = std::chrono::steady_clock::now();
    if (decode_result != 0) {
        llama_sampler_free(sampler);
        llama_free(context);
        throw std::runtime_error("prompt decode failed: " +
                                 std::to_string(decode_result));
    }

    const auto generation_start = std::chrono::steady_clock::now();
    for (int index = 0; index < max_tokens; ++index) {
        llama_token token = llama_sampler_sample(sampler, context, -1);
        if (llama_vocab_is_eog(vocab, token)) break;
        result.response += token_piece(vocab, token);
        result.generated_tokens++;
        batch = llama_batch_get_one(&token, 1);
        decode_result = llama_decode(context, batch);
        if (decode_result != 0) {
            llama_sampler_free(sampler);
            llama_free(context);
            throw std::runtime_error("generation decode failed: " +
                                     std::to_string(decode_result));
        }
    }
    const auto generation_end = std::chrono::steady_clock::now();

    result.prompt_ms = std::chrono::duration<double, std::milli>(
        prompt_end - prompt_start).count();
    result.generation_ms = std::chrono::duration<double, std::milli>(
        generation_end - generation_start).count();

    llama_sampler_free(sampler);
    llama_free(context);
    return result;
}

int main(int argc, char **argv) {
    std::string model_path;
    std::string prompts_path;
    std::string output_path;
    std::string resource_name = "gpu1";
    int main_gpu = 0;
    uint32_t n_ctx = 4096;
    int max_tokens = 160;
    float temperature = 0.20f;
    uint32_t seed = 424242;

    try {
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            auto next = [&]() -> std::string {
                if (i + 1 >= argc) throw std::runtime_error("missing value for " + arg);
                return argv[++i];
            };
            if (arg == "--model") model_path = next();
            else if (arg == "--prompts") prompts_path = next();
            else if (arg == "--output") output_path = next();
            else if (arg == "--resource") resource_name = next();
            else if (arg == "--main-gpu") main_gpu = std::stoi(next());
            else if (arg == "--ctx") n_ctx = static_cast<uint32_t>(std::stoul(next()));
            else if (arg == "--max-tokens") max_tokens = std::stoi(next());
            else if (arg == "--temperature") temperature = std::stof(next());
            else if (arg == "--seed") seed = static_cast<uint32_t>(std::stoul(next()));
            else {
                usage(argv[0]);
                return 2;
            }
        }
        if (model_path.empty() || prompts_path.empty() || output_path.empty() ||
            n_ctx < 256 || max_tokens <= 0 || temperature < 0.0f ||
            temperature > 2.0f) {
            usage(argv[0]);
            return 2;
        }

        uint64_t resource_mask;
        if (resource_name == "gpu0") resource_mask = CNET_MODEL_RESOURCE_GPU0;
        else if (resource_name == "gpu1") resource_mask = CNET_MODEL_RESOURCE_GPU1;
        else throw std::runtime_error("resource must be gpu0 or gpu1");

        const std::vector<PromptCase> prompts = read_prompts(prompts_path);
        llama_log_set([](ggml_log_level level, const char *text, void *) {
            if (level >= GGML_LOG_LEVEL_WARN) std::fputs(text, stderr);
        }, nullptr);
        ggml_backend_load_all();
        llama_backend_init();

        std::vector<ggml_backend_dev_t> visible_gpus;
        for (std::size_t i = 0; i < ggml_backend_dev_count(); ++i) {
            ggml_backend_dev_t device = ggml_backend_dev_get(i);
            if (ggml_backend_dev_type(device) == GGML_BACKEND_DEVICE_TYPE_GPU) {
                visible_gpus.push_back(device);
            }
        }
        if (visible_gpus.size() != 1) {
            throw std::runtime_error(
                "expected exactly one visible GPU, found " +
                std::to_string(visible_gpus.size()));
        }
        std::size_t free_bytes = 0;
        std::size_t total_bytes = 0;
        ggml_backend_dev_memory(visible_gpus[0], &free_bytes, &total_bytes);
        std::cout << "CNET_LLAMA_DEVICE name="
                  << ggml_backend_dev_name(visible_gpus[0])
                  << " description=\""
                  << ggml_backend_dev_description(visible_gpus[0])
                  << "\" free_bytes=" << free_bytes
                  << " total_bytes=" << total_bytes << "\n";

        const char *model_id = "qwythos-9b";
        CnetModelDescriptor descriptor{};
        int probe = cnet_model_descriptor_probe(
            &descriptor, model_id, model_path.c_str(),
            "llama.cpp", resource_mask, resource_mask, 1, 0, 0, 0);
        if (probe != CNET_MODEL_OK) throw std::runtime_error("CNET catalog probe failed");
        if (descriptor.model_class != CNET_MODEL_CLASS_DENSE_TRANSFORMER) {
            throw std::runtime_error("CNET refused artifact as a dense model");
        }

        CnetModelBudget budget{resource_mask, 30ull * 1024ull * 1024ull * 1024ull};
        CnetModelManagerOptions options{};
        options.abi_version = CNET_MODEL_RUNTIME_ABI_VERSION;
        options.struct_size = sizeof options;
        options.max_models = 4;
        options.max_backends = 2;
        options.budgets = &budget;
        options.budget_count = 1;

        BackendContext backend_context;
        backend_context.expected_resource_mask = resource_mask;
        backend_context.main_gpu = main_gpu;
        CnetModelBackendSpec backend{};
        backend.abi_version = CNET_MODEL_RUNTIME_ABI_VERSION;
        backend.struct_size = sizeof backend;
        std::snprintf(backend.name, sizeof backend.name, "%s", "llama.cpp");
        backend.context = &backend_context;
        backend.load = cnet_llama_load;
        backend.unload = cnet_llama_unload;

        CnetModelManager *manager = nullptr;
        if (cnet_model_manager_open(&manager, &options) != CNET_MODEL_OK ||
            cnet_model_backend_register(manager, &backend) != CNET_MODEL_OK ||
            cnet_model_catalog_add(manager, &descriptor) != CNET_MODEL_OK) {
            throw std::runtime_error("CNET manager setup failed");
        }

        CnetModelLease lease{};
        const auto load_start = std::chrono::steady_clock::now();
        int acquire = cnet_model_acquire(manager, descriptor.model_id,
                                         resource_mask, &lease);
        const auto load_end = std::chrono::steady_clock::now();
        if (acquire != CNET_MODEL_OK || lease.resource_mask != resource_mask) {
            cnet_model_manager_close(manager);
            throw std::runtime_error("CNET dense acquire failed: " +
                                     std::to_string(acquire));
        }

        auto *model = static_cast<llama_model *>(lease.handle);
        char description[256] = {0};
        llama_model_desc(model, description, sizeof description);
        const double load_ms = std::chrono::duration<double, std::milli>(
            load_end - load_start).count();
        std::cout << "CNET_DENSE_RESIDENT model=" << model_id << " arch="
                  << descriptor.architecture << " quant="
                  << descriptor.quantization << " resource=" << resource_name
                  << " llama=\"" << description << "\" load_ms="
                  << std::fixed << std::setprecision(2) << load_ms << "\n";

        std::ofstream output(output_path, std::ios::trunc);
        if (!output) throw std::runtime_error("cannot open output file");
        std::vector<SampleResult> results;
        for (std::size_t i = 0; i < prompts.size(); ++i) {
            SampleResult result = generate_one(
                model, prompts[i], n_ctx, max_tokens, temperature,
                seed + static_cast<uint32_t>(i));
            const std::string feedback = constraint_feedback(prompts[i], result.response);
            if (!feedback.empty()) {
                PromptCase repair;
                repair.id = prompts[i].id;
                repair.prompt =
                    "Rewrite the draft so it satisfies the original request.\n"
                    "Original request: " + prompts[i].prompt +
                    "\nDraft: " + result.response +
                    "\nValidation failures: " + feedback +
                    "\nReturn only the corrected final answer.";
                SampleResult corrected = generate_one(
                    model, repair, n_ctx, max_tokens, temperature,
                    seed + 1000u + static_cast<uint32_t>(i));
                corrected.id = prompts[i].id;
                corrected.prompt = prompts[i].prompt;
                corrected.prompt_tokens += result.prompt_tokens;
                corrected.generated_tokens += result.generated_tokens;
                corrected.prompt_ms += result.prompt_ms;
                corrected.generation_ms += result.generation_ms;
                corrected.repair_attempts = 1;
                result = std::move(corrected);
            }
            const double tokens_per_second = result.generation_ms > 0.0
                ? (1000.0 * result.generated_tokens / result.generation_ms) : 0.0;
            output << "{\"id\":\"" << json_escape(result.id)
                   << "\",\"prompt\":\"" << json_escape(result.prompt)
                   << "\",\"response\":\"" << json_escape(result.response)
                   << "\",\"prompt_tokens\":" << result.prompt_tokens
                   << ",\"generated_tokens\":" << result.generated_tokens
                   << ",\"repair_attempts\":" << result.repair_attempts
                   << ",\"prompt_ms\":" << std::fixed << std::setprecision(3)
                   << result.prompt_ms << ",\"generation_ms\":"
                   << result.generation_ms << ",\"tokens_per_second\":"
                   << tokens_per_second << "}\n";
            std::cout << "BEGIN_RESPONSE " << result.id << "\n"
                      << result.response << "\nEND_RESPONSE " << result.id << "\n"
                      << "CNET_DENSE_SAMPLE id=" << result.id
                      << " prompt_tokens=" << result.prompt_tokens
                      << " generated_tokens=" << result.generated_tokens
                      << " repair_attempts=" << result.repair_attempts
                      << " tok_s=" << std::setprecision(2) << tokens_per_second
                      << "\n";
            results.push_back(std::move(result));
        }
        output.close();

        CnetModelStats stats{};
        if (cnet_model_stats(manager, descriptor.model_id, &stats) != CNET_MODEL_OK ||
            stats.state != CNET_MODEL_STATE_RESIDENT || stats.loads != 1 ||
            stats.lease_count != 1) {
            throw std::runtime_error("CNET residency stats mismatch");
        }
        if (cnet_model_release(manager, &lease) != CNET_MODEL_OK ||
            cnet_model_manager_close(manager) != CNET_MODEL_OK) {
            throw std::runtime_error("CNET release/close failed");
        }
        manager = nullptr;
        llama_backend_free();

        if (backend_context.loads != 1 || backend_context.unloads != 1) {
            throw std::runtime_error("backend load/unload balance mismatch");
        }
        std::cout << "CNET_LLAMA_EVAL_PASS samples=" << results.size()
                  << " loads=" << backend_context.loads
                  << " unloads=" << backend_context.unloads
                  << " output=" << output_path << "\n";
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "CNET_LLAMA_EVAL_FAIL: %s\n", error.what());
        return 1;
    }
}
