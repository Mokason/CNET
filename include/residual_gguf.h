#ifndef CNET_RESIDUAL_GGUF_H
#define CNET_RESIDUAL_GGUF_H

/* Real-world Tier C residual: local GGUF transformer as open-ended generator.
 *
 * Bound into personal_ai via personal_ai_bind_residual_gguf() or auto-open
 * when CNET_RESIDUAL_GGUF is set.
 *
 * Port contract (window mode — default for personal AI):
 *   in  : ONEHOT[W]  (or RAW of length W) selecting token win[i]
 *   out : ONEHOT[W]  argmax next-token restricted to the same window
 *         (finite, composable; not full-vocab free prose — that stays
 *         behind a wider window or future token-stream API)
 *
 * Env:
 *   CNET_RESIDUAL_GGUF=/path/model.gguf   auto-bind on personal_ai_open
 *   CNET_RESIDUAL_WINDOW=/path/ids.txt    window file (flagship convention)
 *   CNET_ORACLE_INT8=1                   memory diet for large GGUFs
 *   CNET_RESIDUAL_SESSION_KV=1           chat path: grow KV (not bit-stable)
 *   CNET_PILOT=1                         research: record next-slot hints
 *
 * Gate: make residual_gguf → RESIDUAL_GGUF_PASS
 * Real smoke: make residual_gguf_real (requires CNET_RESIDUAL_GGUF)
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

typedef struct ResidualGguf ResidualGguf;

/* Load GGUF as residual. window_path NULL → synthetic contiguous [base,base+W).
   window_n is used only for synthetic window (default 32 if 0). */
CNET_API int residual_gguf_open(ResidualGguf **out, const char *gguf_path,
                                const char *window_path, int synthetic_n);

CNET_API void residual_gguf_close(ResidualGguf *r);

/* Oracle-shaped callback for hybrid residual / personal_ai_bind_residual. */
CNET_API int residual_gguf_oracle(const double *in, double *out, void *ctx);

/* Port templates matching the bound window width. */
CNET_API Port residual_gguf_input_port(const ResidualGguf *r);
CNET_API Port residual_gguf_output_port(const ResidualGguf *r);

CNET_API int residual_gguf_window_n(const ResidualGguf *r);
CNET_API int residual_gguf_vocab(const ResidualGguf *r);
CNET_API const int *residual_gguf_window_ids(const ResidualGguf *r);

/* P4: reset session KV (always safe; mining path also resets per probe). */
CNET_API void residual_gguf_session_reset(ResidualGguf *r);
/* 1 if session-KV mode is active. */
CNET_API int residual_gguf_session_mode(const ResidualGguf *r);
/* P5 research: pilot hints recorded this residual (0 if PILOT off). */
CNET_API uint64_t residual_gguf_pilot_recorded(const ResidualGguf *r);

/* Bind into PersonalAi as Tier C residual (takes ownership of *r? no —
   caller keeps ResidualGguf* alive for PersonalAi lifetime). */
CNET_API int personal_ai_bind_residual_gguf(PersonalAi *ai, ResidualGguf *r,
                                            const char *name);

/* If CNET_RESIDUAL_GGUF is set, open and bind residual on *ai.
   Returns 0 bound, 1 skipped (env unset), <0 error. Stores handle in *owned
   for the caller to close after personal_ai_close (may be NULL to leak into
   process — tests pass owned). */
CNET_API int personal_ai_auto_residual_gguf(PersonalAi *ai,
                                            ResidualGguf **owned);

#ifdef __cplusplus
}
#endif

#endif /* CNET_RESIDUAL_GGUF_H */
