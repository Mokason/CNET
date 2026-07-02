#ifndef CCE_DEFS_H
#define CCE_DEFS_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Error codes */
typedef enum {
    CCE_OK = 0,
    CCE_ERR_OOM = -1,
    CCE_ERR_INVALID_ARG = -2,
    CCE_ERR_IO = -3,
    CCE_ERR_NOT_FOUND = -4,
    CCE_ERR_FROZEN = -5,
    CCE_ERR_UNSUPPORTED = -6,
} cce_result;

/* Common alignment for tensors (AVX2 friendly + cache line) */
#define CCE_ALIGN 64

/* Helper macros */
#define CCE_MAX_DIMS 8
#define CCE_MAX_BLOCKS 64

/* Flags for contracts / blocks */
typedef uint32_t cce_flags_t;
#define CCE_FLAG_FROZEN          (1u << 0)
#define CCE_FLAG_EXPLORATION     (1u << 1)
#define CCE_FLAG_NOVELTY         (1u << 2)
#define CCE_FLAG_PATCH           (1u << 3)

/* Differentiation modes for cce_learner / cascades / forests.
   LOCAL = core compositional local credit (NoProp/DFA/FF)
   HYBRID = exact on last N tail layers (configurable), local before
   EXACT  = full backprop through cascade (PyTorch-quality partial BP) */
typedef enum {
    CCE_DIFF_LOCAL = 0,
    CCE_DIFF_HYBRID = 1,
    CCE_DIFF_EXACT = 2
} cce_diff_mode_t;

#ifdef __cplusplus
}
#endif

#endif /* CCE_DEFS_H */
