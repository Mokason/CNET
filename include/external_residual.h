#ifndef CNET_EXTERNAL_RESIDUAL_H
#define CNET_EXTERNAL_RESIDUAL_H

/* E (bigger bet stub): foreign residual backends (e.g. Colibrì MoE streamer)
 * bind as the same oracle-shaped residual used by personal_ai / hybrid Tier C.
 *
 * Not a full Colibrì port — a stable ABI so an out-of-process or dlopen
 * residual can replace residual_gguf without changing serve policy.
 *
 * Gate: hermetic checks in colibri_integrate / external_residual self-test.
 */

#include <stddef.h>

#include "acquire.h"
#include "cnet_export.h"
#include "nn.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char name[64];
    Port in_port;
    Port out_port;
    CnetOracleFn fn;
    void *ctx;
    int bound;
} ExternalResidual;

CNET_API void external_residual_init(ExternalResidual *e);
CNET_API int external_residual_bind(ExternalResidual *e, const char *name,
                                    Port in, Port out, CnetOracleFn fn,
                                    void *ctx);
CNET_API void external_residual_unbind(ExternalResidual *e);

/* Forward to personal_ai residual slot (name defaults to external name). */
CNET_API int personal_ai_bind_external_residual(void *personal_ai,
                                                ExternalResidual *e);

#ifdef __cplusplus
}
#endif

#endif /* CNET_EXTERNAL_RESIDUAL_H */
