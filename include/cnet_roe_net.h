/* ROE live miss backends — untrusted until shell verify. */
#ifndef CNET_ROE_NET_H
#define CNET_ROE_NET_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char llm_url[512];   /* e.g. http://127.0.0.1:11434/api/generate */
    char llm_model[128]; /* e.g. deepseek-v4-flash:cloud or qwen2.5:7b */
    char lookup_url[512]; /* duckduckgo instant answer base */
    long timeout_ms;
    int enable_llm;
    int enable_lookup;
    int think; /* 1 allow thinking; default 0 (needed for Ollama cloud V4 flash) */
    char last_err[256];
    uint64_t n_llm_ok, n_llm_fail;
    uint64_t n_lookup_ok, n_lookup_fail;
    uint64_t tokens_prompt_est, tokens_completion_est;
} RoeNet;

void roe_net_init(RoeNet *N);
/* Env:
 *   ROE_LIVE=1
 *   ROE_LLM_URL     default http://127.0.0.1:11434/api/generate (Ollama)
 *   ROE_LLM_MODEL   e.g. deepseek-v4-flash:cloud  or qwen2.5:7b
 *   ROE_LLM_THINK   0|1 (default 0; cloud V4 flash returns empty if think left on)
 *   ROE_LOOKUP_URL, ROE_TIMEOUT_MS
 * No DEEPSEEK_API_KEY — use Ollama cloud models via local Ollama daemon.
 */
void roe_net_from_env(RoeNet *N);

/* Ollama /api/generate. Returns 0 and fills answer. UNTRUSTED. */
int roe_net_llm(RoeNet *N, const char *prompt, char *answer, size_t answer_cap,
                uint64_t *tokens_est);

/* DuckDuckGo instant answer (or configured URL with %s). UNTRUSTED. */
int roe_net_lookup(RoeNet *N, const char *query, char *snippet, size_t cap,
                   uint64_t *tokens_est);

#ifdef __cplusplus
}
#endif

#endif
