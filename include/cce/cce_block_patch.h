#ifndef CCE_BLOCK_PATCH_H
#define CCE_BLOCK_PATCH_H

#include "cce_block.h"
#include "cce_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Patch block: turns spatial input into patch rows for contract processing */
cce_result cce_block_patch_init(cce_block* blk, int patch_size, int stride, int channels);

cce_result cce_block_patch_extract(const cce_tensor* image, int h, int w,
                                   cce_tensor* patches_out);

#ifdef __cplusplus
}
#endif

#endif /* CCE_BLOCK_PATCH_H */
