#ifndef CNET_HELD_MODEL_H
#define CNET_HELD_MODEL_H

/* Process-wide LLM holder for leftover turns.
   Hook (tests), HTTP chat endpoint, or dlopen of libcnet_harness.so. */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CNET_HELD_CONTRACT "held_model_v1"
#define CNET_HELD_TEXT 768

typedef int (*CnetHeldModelHook)(const char *turn, char *out, size_t cap);

void cnet_held_model_set_hook(CnetHeldModelHook hook);

/* Pin an HTTP chat-completions URL. Empty/NULL clears it. */
int cnet_held_model_set_endpoint(const char *url);

/* Pin a GGUF path for in-process harness open (dlopen plugin). */
int cnet_held_model_set_path(const char *gguf_path);

/* Optional HTTP body "model" field (default "held"). MAX requires real id. */
int cnet_held_model_set_name(const char *model_name);

/* 0 = text written. 1 = no holder / empty. */
int cnet_held_model_ask(const char *turn, char *out, size_t cap);

void cnet_held_model_close(void);

#ifdef __cplusplus
}
#endif

#endif /* CNET_HELD_MODEL_H */
