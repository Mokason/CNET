#ifndef CCE_CASCADE_H
#define CCE_CASCADE_H

#include "cce_block.h"
#include "cce_defs.h"
#include "cce_archive.h"

#ifndef CCE_API
#ifdef CCE_BUILD_DLL
#ifdef _WIN32
#define CCE_API __declspec(dllexport)
#else
#define CCE_API __attribute__((visibility("default")))
#endif
#else
#define CCE_API
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* A cascade is an ordered list of blocks forming one specialist contract */
typedef struct {
    cce_block* blocks;
    int        num_blocks;
    int        max_blocks;
    cce_flags_t flags;
    float       goodness;
    int         freeze_countdown;   /* used by goodness gate for aggressive freezing */
    char       name[64];
    int        exact_tail_length;   /* for HYBRID: # tail blocks using exact BP. -1 = inherit (default 2) */
    cce_diff_mode_t diff_mode;      /* per-cascade override: -1 = inherit from forest/learner */
} cce_cascade;

/* Create empty cascade */
cce_result cce_cascade_init(cce_cascade* cas, int max_blocks);

/* Heap-backed cascade builder API for host bindings. */
CCE_API cce_result cce_cascade_create(cce_cascade** cas, int max_blocks);
CCE_API void cce_cascade_destroy(cce_cascade* cas);
CCE_API cce_result cce_cascade_add_linear(cce_cascade* cas, int input_dim, int output_dim, float init_scale);
CCE_API cce_result cce_cascade_add_linear_head(cce_cascade* cas, int input_dim, int output_dim, float init_scale);
CCE_API cce_result cce_cascade_add_patch(cce_cascade* cas, int patch_size, int stride, int channels);

/* Append a block (takes ownership of the block tensors) */
cce_result cce_cascade_append(cce_cascade* cas, cce_block* blk);

/* Set exact tail length for HYBRID mode (how many last layers use full backprop) */
void cce_cascade_set_exact_tail_length(cce_cascade* cas, int length);

/* Set per-cascade diff mode (-1 to inherit) */
void cce_cascade_set_diff_mode(cce_cascade* cas, cce_diff_mode_t mode);

/* Forward through the whole cascade */
cce_result cce_cascade_forward(const cce_cascade* cas, const cce_tensor* input, cce_tensor* output);

/* Micro-split: duplicate a block and mark both as exploration (ACE style) */
cce_result cce_cascade_micro_split(cce_cascade* cas, int block_idx);

/* Freeze entire cascade (contracts it) */
void cce_cascade_freeze(cce_cascade* cas);

/* Cleanup */
void cce_cascade_free(cce_cascade* cas);

/* Serialize cascade (weights + metadata + per-block state) into archive section.
   Returns offset for later load. Used for forest persistence. */
cce_result cce_cascade_save_to_archive(cce_cascade* cas, cce_archive* ar, const char* name, size_t* out_offset);

/* Reconstruct cascade from archive section (for HOT: owned RAM copy of data; for warm inference can use views in future). */
cce_result cce_cascade_load_from_archive(cce_cascade* cas, cce_archive* ar, size_t offset);

/* ZERO-COPY WARM load: reconstruct a cascade whose block weights/bias are VIEWS
   into the archive's mmap (owns_memory=0), instead of allocating+copying. Reuses
   the same mapped pointer -- no per-recall reload. The result is READ-ONLY
   (every block is flagged FROZEN): use it for inference; train via the owned
   cce_cascade_load_from_archive (copy-on-write to HOT). Only zero-copy where the
   platform/section is mmap-backed and aligned; get_tensor_view falls back to an
   owned copy otherwise (still correct). cce_cascade_free is safe either way
   (views are not freed; copy-fallbacks are). The backing archive must outlive
   the cascade and must NOT be appended-to/remapped while views are alive. */
cce_result cce_cascade_view_from_archive(cce_cascade* cas, cce_archive* ar, size_t offset);

#ifdef __cplusplus
}
#endif

#endif /* CCE_CASCADE_H */
