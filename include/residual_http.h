#ifndef CNET_RESIDUAL_HTTP_H
#define CNET_RESIDUAL_HTTP_H

/* Tier-C residual via OpenAI-compatible / llama.cpp HTTP (Bonsai Q1, etc.).
 *
 * Native cce_gguf cannot open PrismML Q1_0 GGUFs. This adapter keeps the same
 * window one-hot residual contract as residual_gguf:
 *   in  : ONEHOT[W] over window slots
 *   out : ONEHOT[W] next-slot (best token among window via /completion probs)
 *
 * Env:
 *   CNET_RESIDUAL_HTTP=http://127.0.0.1:8080   base URL (no trailing slash)
 *   CNET_RESIDUAL_WINDOW=/path/ids.txt         window ids (required for real)
 *   CNET_RESIDUAL_HTTP_TIMEOUT_MS=30000
 *
 * Gate: make residual_http → RESIDUAL_HTTP_PASS
 */

#include <stddef.h>
#include <stdint.h>

#include "acquire.h"
#include "cnet_export.h"
#include "nn.h"
#include "personal_ai.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ResidualHttp ResidualHttp;

CNET_API int residual_http_open(ResidualHttp **out, const char *base_url,
                                const char *window_path, int synthetic_n);

CNET_API void residual_http_close(ResidualHttp *r);

CNET_API int residual_http_oracle(const double *in, double *out, void *ctx);

CNET_API int residual_http_window_n(const ResidualHttp *r);
CNET_API const int *residual_http_window_ids(const ResidualHttp *r);

CNET_API Port residual_http_input_port(const ResidualHttp *r);
CNET_API Port residual_http_output_port(const ResidualHttp *r);

/* Health: GET {base}/v1/models — 0 ok, <0 fail. */
CNET_API int residual_http_ping(const ResidualHttp *r);

CNET_API int personal_ai_bind_residual_http(PersonalAi *ai, ResidualHttp *r,
                                            const char *name);

/* Auto-bind when CNET_RESIDUAL_HTTP set. Returns 1 if unset, 0 ok, <0 fail. */
CNET_API int personal_ai_auto_residual_http(PersonalAi *ai,
                                            ResidualHttp **owned);

#ifdef __cplusplus
}
#endif

#endif /* CNET_RESIDUAL_HTTP_H */
