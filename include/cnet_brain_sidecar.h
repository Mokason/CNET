/* Opt-in Brain continuous sidecar (pieces.bin).
 * Does NOT enter CNU1 or lower CNET floors. Explicit load path only.
 */
#ifndef CNET_BRAIN_SIDECAR_H
#define CNET_BRAIN_SIDECAR_H

#include <stddef.h>

#include "cnet_export.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct CnetBrainSidecar CnetBrainSidecar;

/* Load Brain pieces.bin (magic CBPC v1). Returns 0 on success.
 * Fail-closed: missing/corrupt/bad magic → non-zero, *out unchanged/NULL. */
CNET_API int cnet_brain_sidecar_load(CnetBrainSidecar **out, const char *pieces_path);

CNET_API void cnet_brain_sidecar_free(CnetBrainSidecar *s);

/* Dims from header. Returns 0 if loaded. */
CNET_API int cnet_brain_sidecar_dims(const CnetBrainSidecar *s, int *in_dim, int *out_dim);

/* Number of pieces (peaks+residuals). */
CNET_API int cnet_brain_sidecar_n_pieces(const CnetBrainSidecar *s);

/* Continuous serve: nearest peak + residual hops (board law, α=0.65 LTM).
 * float API (Brain native). Returns 0 ok, -1 abstain/miss, -2 bad args. */
CNET_API int cnet_brain_sidecar_serve_f(const CnetBrainSidecar *s, const float *x,
                                        int in_dim, float *y, int out_dim);

/* double convenience (copies through float). */
CNET_API int cnet_brain_sidecar_serve(const CnetBrainSidecar *s, const double *x,
                                      size_t in_dim, double *y, size_t out_dim);

/* Opt-in env: if CNET_BRAIN_SIDECAR=/path/to/dir_or_pieces.bin is set and
 * loadable, returns a process-local singleton (not freed by caller).
 * NULL if unset or fail — never invents continuous answers. */
CNET_API const CnetBrainSidecar *cnet_brain_sidecar_env(void);

#ifdef __cplusplus
}
#endif

#endif
