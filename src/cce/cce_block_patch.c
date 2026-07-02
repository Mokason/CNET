#include "../../include/cce/cce_block_patch.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>

cce_result cce_block_patch_init(cce_block* blk, int patch_size, int stride, int channels) {
    if (!blk || patch_size <= 0 || stride <= 0 || channels <= 0) return CCE_ERR_INVALID_ARG;

    const size_t max_patch_elems = 2147483647u;
    if ((size_t)patch_size > max_patch_elems / (size_t)patch_size) return CCE_ERR_INVALID_ARG;
    size_t patch_area = (size_t)patch_size * (size_t)patch_size;
    if ((size_t)channels > max_patch_elems / patch_area) return CCE_ERR_INVALID_ARG;
    size_t patch_elems_sz = patch_area * (size_t)channels;
    if (patch_elems_sz == 0) return CCE_ERR_INVALID_ARG;
    if (patch_elems_sz > SIZE_MAX / patch_elems_sz) return CCE_ERR_INVALID_ARG;
    if (patch_elems_sz * patch_elems_sz > SIZE_MAX / sizeof(float)) return CCE_ERR_INVALID_ARG;

    memset(blk, 0, sizeof(*blk));  /* ensure all tensors start clean */
    blk->type = CCE_BLOCK_PATCH;
    blk->flags |= CCE_FLAG_PATCH;

    int patch_elems = (int)patch_elems_sz;
    int wshape[2] = {patch_elems, patch_elems};
    cce_result rc = cce_tensor_alloc(&blk->weights, wshape, 2);
    if (rc != CCE_OK) return rc;

    /* Alloc full Adam moments for weights (first + second) */
    rc = cce_tensor_alloc(&blk->momentum_weights, wshape, 2);
    if (rc != CCE_OK) {
        cce_tensor_free(&blk->weights);
        return rc;
    }
    rc = cce_tensor_alloc(&blk->second_moment_w, wshape, 2);
    if (rc != CCE_OK) {
        cce_tensor_free(&blk->momentum_weights);
        cce_tensor_free(&blk->weights);
        return rc;
    }
    cce_tensor_zero(&blk->momentum_weights);
    cce_tensor_zero(&blk->second_moment_w);

    blk->timestep = 0;

    /* bias and momentum_bias / second left zeroed (owns=0 for patch) */

    /* Simple scaled identity for patch processing */
    for (size_t i = 0; i < blk->weights.numel; ++i) {
        blk->weights.data[i] = (i % patch_elems == i / patch_elems) ? 1.0f : 0.0f;
    }

    return CCE_OK;
}

cce_result cce_block_patch_extract(const cce_tensor* image, int h, int w,
                                   cce_tensor* patches_out) {
    if (!image || !patches_out || h <= 0 || w <= 0) return CCE_ERR_INVALID_ARG;

    int patch_size = 2; /* demo fixed */
    if (h < patch_size || w < patch_size) return CCE_ERR_INVALID_ARG;
    size_t patches_y = (size_t)(h - patch_size + 1);
    size_t patches_x = (size_t)(w - patch_size + 1);
    if (patches_y > SIZE_MAX / patches_x) return CCE_ERR_INVALID_ARG;
    size_t num_patches_sz = patches_y * patches_x;
    if (num_patches_sz > (size_t)INT_MAX) return CCE_ERR_INVALID_ARG;
    int num_patches = (int)num_patches_sz;
    int patch_elems = patch_size * patch_size;

    int pshape[2] = {num_patches, patch_elems};
    cce_result rc = cce_tensor_alloc(patches_out, pshape, 2);
    if (rc != CCE_OK) return rc;

    /* Simple row-major patch extraction (assume 1 channel for smoke) */
    int p = 0;
    for (int y = 0; y <= h - patch_size; ++y) {
        for (int x = 0; x <= w - patch_size; ++x) {
            for (int py = 0; py < patch_size; ++py) {
                for (int px = 0; px < patch_size; ++px) {
                    int img_idx = (y + py) * w + (x + px);
                    patches_out->data[p * patch_elems + py * patch_size + px] =
                        ((size_t)img_idx < image->numel) ? image->data[img_idx] : 0.0f;
                }
            }
            p++;
        }
    }
    return CCE_OK;
}
