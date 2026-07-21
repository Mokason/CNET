/* Automatic Hermes→gap→lane learning helpers.
 * CNET_AUTO_LEARN=1 (default on when unset in deploy): freeform capability
 * misses are rewritten into window-teachable NO_PLAN shapes so the gap lane
 * can train/seal BTNs without manual intervention.
 */
#ifndef CNET_AUTO_LEARN_H
#define CNET_AUTO_LEARN_H

#include "router.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 1 when auto-learn rewrite is active (CNET_AUTO_LEARN not "0"). */
CNET_API int cnet_auto_learn_enabled(void);

/* Window width for teachable shapes (CNET_AUTO_LEARN_W, default 256). */
CNET_API size_t cnet_auto_learn_window(void);

/* Default top-k (CNET_AUTO_LEARN_K, default 3, clamp 1..8). */
CNET_API size_t cnet_auto_learn_k(void);

/* True if ports already look like a gap-lane LM-teachable shape
 * (onehot W→onehot W×k, k in 1..8). Tags may still be freeform. */
CNET_API int cnet_auto_learn_shape_ok(Port in, Port goal);

/* Rewrite in/goal into w_cur → tk{id}q{id} teachable form using seed text
 * (or existing goal tag) hashed into the window. No-op if disabled or already
 * tagged tk*q*. Returns 1 if rewritten, 0 if unchanged, -1 on error. */
CNET_API int cnet_auto_learn_make_teachable(Port *in, Port *goal,
                                           const char *seed_text);

/* Note a chat/text interaction as a teachable NO_PLAN into inbox.
 * Returns 0 on success. */
CNET_API int cnet_auto_learn_note_text(const char *inbox_path,
                                       const char *text, size_t k_override);

#ifdef __cplusplus
}
#endif
#endif
