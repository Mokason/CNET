#include "../../include/cce/cce_cascade.h"
#include "../../include/cce/cce_block_patch.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <limits.h>

static int checked_matrix_bytes(uint32_t rows, uint32_t cols, size_t* out_bytes) {
    if (!out_bytes || rows == 0 || cols == 0) return 0;
    size_t r = (size_t)rows;
    size_t c = (size_t)cols;
    if (r > SIZE_MAX / c) return 0;
    size_t elems = r * c;
    if (elems > SIZE_MAX / sizeof(float)) return 0;
    *out_bytes = elems * sizeof(float);
    return 1;
}

static int checked_vector_bytes(uint32_t count, size_t* out_bytes) {
    if (!out_bytes || count == 0) return 0;
#if SIZE_MAX < UINT32_MAX
    if ((size_t)count > SIZE_MAX / sizeof(float)) return 0;
#endif
    *out_bytes = (size_t)count * sizeof(float);
    return 1;
}

static int checked_add_size(size_t* total, size_t add) {
    if (!total) return 0;
    if (*total > SIZE_MAX - add) return 0;
    *total += add;
    return 1;
}

static cce_result cce_block_clone_linear_for_split(cce_block* dst, const cce_block* src, float noise_scale) {
    if (!dst || !src) return CCE_ERR_INVALID_ARG;
    int in_dim = src->weights.shape[0];
    int out_dim = src->weights.shape[1];
    if (in_dim <= 0 || out_dim <= 0) return CCE_ERR_INVALID_ARG;

    cce_result rc = cce_block_init_linear(dst, in_dim, out_dim, 0.0f);
    if (rc != CCE_OK) return rc;

    if (src->weights.numel != dst->weights.numel || src->bias.numel != dst->bias.numel) {
        cce_block_free(dst);
        return CCE_ERR_INVALID_ARG;
    }

    memcpy(dst->weights.data, src->weights.data, src->weights.numel * sizeof(float));
    for (size_t i = 0; i < dst->weights.numel; ++i) {
        float jitter = ((float)rand() / RAND_MAX - 0.5f) * noise_scale;
        dst->weights.data[i] += jitter;
    }

    memcpy(dst->bias.data, src->bias.data, src->bias.numel * sizeof(float));
    for (size_t i = 0; i < dst->bias.numel; ++i) {
        float jitter = ((float)rand() / RAND_MAX - 0.5f) * noise_scale;
        dst->bias.data[i] += jitter;
    }

    dst->type = src->type;
    dst->flags = (src->flags | CCE_FLAG_EXPLORATION) & ~CCE_FLAG_FROZEN;
    dst->goodness = src->goodness;
    dst->freeze_countdown = src->freeze_countdown;
    dst->timestep = 0;
    return CCE_OK;
}

static cce_result cce_block_init_owned_from_archive(cce_block* blk,
                                                   uint32_t type,
                                                   uint32_t in_dim,
                                                   uint32_t out_dim,
                                                   uint32_t w_bytes,
                                                   uint32_t b_bytes) {
    if (!blk || in_dim == 0 || out_dim == 0) return CCE_ERR_INVALID_ARG;
    if (in_dim > (uint32_t)INT_MAX || out_dim > (uint32_t)INT_MAX) return CCE_ERR_INVALID_ARG;
    memset(blk, 0, sizeof(*blk));

    if (type == CCE_BLOCK_PATCH) {
        size_t expected_wb = 0;
        if (!checked_matrix_bytes(in_dim, out_dim, &expected_wb)) return CCE_ERR_INVALID_ARG;
        if (in_dim != out_dim ||
            w_bytes != expected_wb ||
            b_bytes != 0)
            return CCE_ERR_INVALID_ARG;

        int wshape[2] = { (int)in_dim, (int)out_dim };
        cce_result rc = cce_tensor_alloc(&blk->weights, wshape, 2);
        if (rc != CCE_OK)
            return rc;
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
        blk->type = CCE_BLOCK_PATCH;
        blk->flags = CCE_FLAG_PATCH;
        blk->timestep = 0;
        return CCE_OK;
    }

    if (type != CCE_BLOCK_LINEAR && type != CCE_BLOCK_LINEAR_HEAD)
        return CCE_ERR_INVALID_ARG;

    size_t expected_wb = 0;
    size_t expected_bb = 0;
    if (!checked_matrix_bytes(in_dim, out_dim, &expected_wb) ||
        !checked_vector_bytes(out_dim, &expected_bb))
        return CCE_ERR_INVALID_ARG;

    cce_result rc = cce_block_init_linear(blk, (int)in_dim, (int)out_dim, 0.0f);
    if (rc != CCE_OK)
        return rc;
    if (w_bytes != expected_wb || b_bytes != expected_bb) {
        cce_block_free(blk);
        return CCE_ERR_INVALID_ARG;
    }
    blk->type = (cce_block_type_t)type;
    return CCE_OK;
}

cce_result cce_cascade_init(cce_cascade* cas, int max_blocks) {
    if (!cas || max_blocks <= 0) return CCE_ERR_INVALID_ARG;
    memset(cas, 0, sizeof(*cas));
    cas->blocks = (cce_block*)calloc(max_blocks, sizeof(cce_block));
    if (!cas->blocks) return CCE_ERR_OOM;
    cas->max_blocks = max_blocks;
    cas->goodness = 0.5f;
    cas->freeze_countdown = 3;
    cas->exact_tail_length = -1;
    cas->diff_mode = -1;
    return CCE_OK;
}

cce_result cce_cascade_create(cce_cascade** cas_out, int max_blocks) {
    if (!cas_out || max_blocks <= 0) return CCE_ERR_INVALID_ARG;
    *cas_out = NULL;

    cce_cascade* cas = (cce_cascade*)malloc(sizeof(cce_cascade));
    if (!cas) return CCE_ERR_OOM;

    cce_result rc = cce_cascade_init(cas, max_blocks);
    if (rc != CCE_OK) {
        free(cas);
        return rc;
    }

    *cas_out = cas;
    return CCE_OK;
}

void cce_cascade_destroy(cce_cascade* cas) {
    if (!cas) return;
    cce_cascade_free(cas);
    free(cas);
}

cce_result cce_cascade_add_linear(cce_cascade* cas, int input_dim, int output_dim, float init_scale) {
    if (!cas || input_dim <= 0 || output_dim <= 0) return CCE_ERR_INVALID_ARG;

    cce_block blk;
    memset(&blk, 0, sizeof(blk));
    cce_result rc = cce_block_init_linear(&blk, input_dim, output_dim, init_scale);
    if (rc != CCE_OK) return rc;

    rc = cce_cascade_append(cas, &blk);
    if (rc != CCE_OK) {
        cce_block_free(&blk);
        return rc;
    }
    return CCE_OK;
}

cce_result cce_cascade_add_linear_head(cce_cascade* cas, int input_dim, int output_dim, float init_scale) {
    if (!cas || input_dim <= 0 || output_dim <= 0) return CCE_ERR_INVALID_ARG;

    cce_block blk;
    memset(&blk, 0, sizeof(blk));
    cce_result rc = cce_block_init_linear(&blk, input_dim, output_dim, init_scale);
    if (rc != CCE_OK) return rc;
    blk.type = CCE_BLOCK_LINEAR_HEAD;

    rc = cce_cascade_append(cas, &blk);
    if (rc != CCE_OK) {
        cce_block_free(&blk);
        return rc;
    }
    return CCE_OK;
}

cce_result cce_cascade_add_patch(cce_cascade* cas, int patch_size, int stride, int channels) {
    if (!cas || patch_size <= 0 || stride <= 0 || channels <= 0) return CCE_ERR_INVALID_ARG;

    cce_block blk;
    memset(&blk, 0, sizeof(blk));
    cce_result rc = cce_block_patch_init(&blk, patch_size, stride, channels);
    if (rc != CCE_OK) return rc;

    rc = cce_cascade_append(cas, &blk);
    if (rc != CCE_OK) {
        cce_block_free(&blk);
        return rc;
    }
    return CCE_OK;
}

cce_result cce_cascade_append(cce_cascade* cas, cce_block* blk) {
    if (!cas || !blk || cas->num_blocks >= cas->max_blocks) return CCE_ERR_INVALID_ARG;
    cas->blocks[cas->num_blocks] = *blk;  /* move */
    cas->num_blocks++;
    return CCE_OK;
}

void cce_cascade_set_exact_tail_length(cce_cascade* cas, int length) {
    if (cas) cas->exact_tail_length = length;
}

void cce_cascade_set_diff_mode(cce_cascade* cas, cce_diff_mode_t mode) {
    if (cas) cas->diff_mode = mode;
}

cce_result cce_cascade_forward(const cce_cascade* cas, const cce_tensor* input, cce_tensor* output) {
    if (!cas || cas->num_blocks == 0 || !input || !output) return CCE_ERR_INVALID_ARG;

    cce_tensor curr;
    /* Make a copy of input for safety in this simple version */
    int in_nd = input->ndim;
    cce_result ar = cce_tensor_alloc(&curr, input->shape, in_nd);
    if (ar != CCE_OK) return ar;
    memcpy(curr.data, input->data, curr.numel * sizeof(float));

    for (int i = 0; i < cas->num_blocks; ++i) {
        const cce_block* b = &cas->blocks[i];
        int out_dim = b->weights.shape[1];
        int oshape[1] = {out_dim};

        cce_tensor next;
        cce_result nr = cce_tensor_alloc(&next, oshape, 1);
        if (nr != CCE_OK) {
            cce_tensor_free(&curr);
            return nr;
        }

        cce_result r = cce_block_forward(b, &curr, &next);
        if (r != CCE_OK) {
            cce_tensor_free(&next);
            cce_tensor_free(&curr);
            return r;
        }
        cce_tensor_free(&curr);
        curr = next;
    }

    *output = curr;
    return CCE_OK;
}

cce_result cce_cascade_micro_split(cce_cascade* cas, int block_idx) {
    if (!cas || block_idx < 0 || block_idx >= cas->num_blocks) return CCE_ERR_INVALID_ARG;
    if (cas->num_blocks >= cas->max_blocks) {
        return CCE_ERR_INVALID_ARG;
    }

    cce_block clone;
    memset(&clone, 0, sizeof(clone));
    cce_result rc = cce_block_clone_linear_for_split(&clone, &cas->blocks[block_idx], 0.02f);
    if (rc != CCE_OK) {
        return rc;
    }
    rc = cce_cascade_append(cas, &clone);
    if (rc != CCE_OK) {
        cce_block_free(&clone);
        return rc;
    }
    cas->blocks[block_idx].flags |= CCE_FLAG_EXPLORATION;
    return CCE_OK;
}

void cce_cascade_freeze(cce_cascade* cas) {
    if (!cas) return;
    for (int i = 0; i < cas->num_blocks; ++i) {
        cce_block_freeze(&cas->blocks[i]);
    }
    cas->flags |= CCE_FLAG_FROZEN;
    cas->freeze_countdown = -999;  /* strongly frozen */
}

void cce_cascade_free(cce_cascade* cas) {
    if (!cas) return;
    for (int i = 0; i < cas->num_blocks; ++i) {
        cce_block_free(&cas->blocks[i]);
    }
    free(cas->blocks);
    memset(cas, 0, sizeof(*cas));
}

/* === Persistence for deeper forests + archive tiers === */

cce_result cce_cascade_save_to_archive(cce_cascade* cas, cce_archive* ar, const char* name, size_t* out_offset) {
    if (!cas || !ar || cas->num_blocks == 0) return CCE_ERR_INVALID_ARG;

    /* Refuse what the format cannot hold, instead of corrupting:
       - blocks whose FP payload was dropped (oracle int8 mode keeps shape
         metadata with data == NULL) have nothing to persist;
       - the layout stores w_bytes/b_bytes as u32, so a >4 GB tensor (e.g. a
         262k-vocab head at FP32) cannot be represented. Callers already
         tolerate persistence failure (the HOT RAM copy keeps working). */
    for (int bi = 0; bi < cas->num_blocks; bi++) {
        const cce_block* b = &cas->blocks[bi];
        if (b->weights.numel > 0 && !b->weights.data) return CCE_ERR_INVALID_ARG;
        if (b->weights.numel * sizeof(float) > 0xFFFFFFFFull) return CCE_ERR_UNSUPPORTED;
        if (b->bias.numel * sizeof(float) > 0xFFFFFFFFull) return CCE_ERR_UNSUPPORTED;
    }

    /* Simple binary layout:
       [u32 num_blocks]
       for each block:
         u32 type, in_d, out_d
         u32 w_bytes, b_bytes
         [w_bytes weights]
         [b_bytes bias]
         f32 goodness
         u32 flags
         i32 freeze_countdown
    */
    size_t total = sizeof(uint32_t);
    for (int i = 0; i < cas->num_blocks; ++i) {
        cce_block* b = &cas->blocks[i];
        if (b->weights.ndim != 2 || b->weights.shape[0] <= 0 || b->weights.shape[1] <= 0 ||
            !b->weights.data)
            return CCE_ERR_INVALID_ARG;
        if (b->type == CCE_BLOCK_PATCH) {
            if (b->bias.numel != 0 || b->bias.data != NULL)
                return CCE_ERR_INVALID_ARG;
        } else if (b->type == CCE_BLOCK_LINEAR || b->type == CCE_BLOCK_LINEAR_HEAD) {
            if (b->bias.ndim != 1 || b->bias.shape[0] != b->weights.shape[1] || !b->bias.data)
                return CCE_ERR_INVALID_ARG;
        } else {
            return CCE_ERR_INVALID_ARG;
        }
        if (b->weights.numel > SIZE_MAX / sizeof(float) ||
            b->bias.numel > SIZE_MAX / sizeof(float))
            return CCE_ERR_INVALID_ARG;
        size_t wb = b->weights.numel * sizeof(float);
        size_t bb = b->bias.numel * sizeof(float);
        if (wb > UINT32_MAX || bb > UINT32_MAX)
            return CCE_ERR_INVALID_ARG;
        size_t block_total = sizeof(uint32_t)*5 + sizeof(float) + sizeof(uint32_t) + sizeof(int32_t);
        if (!checked_add_size(&block_total, wb) ||
            !checked_add_size(&block_total, bb) ||
            !checked_add_size(&total, block_total))
            return CCE_ERR_INVALID_ARG;
    }

    unsigned char* buf = (unsigned char*)malloc(total);
    if (!buf) return CCE_ERR_OOM;

    size_t pos = 0;
    uint32_t nb = (uint32_t)cas->num_blocks;
    memcpy(buf + pos, &nb, sizeof(uint32_t)); pos += sizeof(uint32_t);

    for (int i = 0; i < cas->num_blocks; ++i) {
        cce_block* b = &cas->blocks[i];
        uint32_t t = (uint32_t)b->type;
        uint32_t id = (uint32_t)b->weights.shape[0];
        uint32_t od = (uint32_t)b->weights.shape[1];
        size_t wb_sz = b->weights.numel * sizeof(float);
        size_t bb_sz = b->bias.numel * sizeof(float);
        if (wb_sz > UINT32_MAX || bb_sz > UINT32_MAX) { free(buf); return CCE_ERR_INVALID_ARG; }
        uint32_t wb = (uint32_t)wb_sz;
        uint32_t bb = (uint32_t)bb_sz;

        memcpy(buf + pos, &t, sizeof(uint32_t)); pos += sizeof(uint32_t);
        memcpy(buf + pos, &id, sizeof(uint32_t)); pos += sizeof(uint32_t);
        memcpy(buf + pos, &od, sizeof(uint32_t)); pos += sizeof(uint32_t);
        memcpy(buf + pos, &wb, sizeof(uint32_t)); pos += sizeof(uint32_t);
        memcpy(buf + pos, &bb, sizeof(uint32_t)); pos += sizeof(uint32_t);

        if (wb > 0) memcpy(buf + pos, b->weights.data, wb);
        pos += wb;
        if (bb > 0) memcpy(buf + pos, b->bias.data, bb);
        pos += bb;

        memcpy(buf + pos, &b->goodness, sizeof(float)); pos += sizeof(float);
        uint32_t fl = b->flags;
        memcpy(buf + pos, &fl, sizeof(uint32_t)); pos += sizeof(uint32_t);
        int32_t fc = (int32_t)b->freeze_countdown;
        memcpy(buf + pos, &fc, sizeof(int32_t)); pos += sizeof(int32_t);
    }

    size_t off = 0;
    cce_result rc = cce_archive_append_section(ar, name ? name : "cascade", buf, total, &off);
    free(buf);
    if (rc == CCE_OK && out_offset) *out_offset = off;
    return rc;
}

cce_result cce_cascade_load_from_archive(cce_cascade* cas, cce_archive* ar, size_t offset) {
    if (!cas || !ar) return CCE_ERR_INVALID_ARG;

    /* Read incrementally with EXACT sizes -- robust for any archive size. (The old
       implementation read a fixed 1MB prefix, but cce_archive_read_raw rejects any
       read past EOF, so it failed on every archive smaller than 1MB. The forest
       never hit it because branches stay HOT and only reload after eviction.)
       Mirrors cce_cascade_view_from_archive but copies into OWNED tensors (HOT). */
    uint32_t nb = 0;
    if (cce_archive_read_raw(ar, offset, &nb, sizeof(uint32_t)) != CCE_OK) return CCE_ERR_IO;
    if (nb == 0 || nb > CCE_MAX_BLOCKS) return CCE_ERR_INVALID_ARG;

    if (cce_cascade_init(cas, (int)nb) != CCE_OK) return CCE_ERR_OOM;

    size_t pos = offset + sizeof(uint32_t);
    for (uint32_t i = 0; i < nb; ++i) {
        uint32_t hdr[5];
        if (cce_archive_read_raw(ar, pos, hdr, sizeof(hdr)) != CCE_OK) break;
        uint32_t t = hdr[0], id = hdr[1], od = hdr[2], wb = hdr[3], bb = hdr[4];
        pos += sizeof(hdr);

        cce_block blk;
        cce_result ir = cce_block_init_owned_from_archive(&blk, t, id, od, wb, bb);
        if (ir != CCE_OK) {
            if (ir == CCE_ERR_OOM) {
                cce_cascade_free(cas);
                return CCE_ERR_OOM;
            }
            break;
        }
        if (cce_archive_read_raw(ar, pos, blk.weights.data, wb) != CCE_OK) { cce_block_free(&blk); break; }
        pos += wb;
        if (bb > 0 && cce_archive_read_raw(ar, pos, blk.bias.data, bb) != CCE_OK) { cce_block_free(&blk); break; }
        pos += bb;

        float g = 0.0f; uint32_t fl = 0; int32_t fc = 0;
        if (cce_archive_read_raw(ar, pos, &g,  sizeof(float)) != CCE_OK) { cce_block_free(&blk); break; }
        pos += sizeof(float);
        if (cce_archive_read_raw(ar, pos, &fl, sizeof(uint32_t)) != CCE_OK) { cce_block_free(&blk); break; }
        pos += sizeof(uint32_t);
        if (cce_archive_read_raw(ar, pos, &fc, sizeof(int32_t)) != CCE_OK) { cce_block_free(&blk); break; }
        pos += sizeof(int32_t);

        blk.goodness = g;
        blk.flags = fl;
        if (blk.type == CCE_BLOCK_PATCH) blk.flags |= CCE_FLAG_PATCH;
        blk.freeze_countdown = (int)fc;

        if (cce_cascade_append(cas, &blk) != CCE_OK) {
            cce_block_free(&blk);
            break;
        }  /* struct copy; owned tensors move to the cascade */
    }

    if (cas->num_blocks != (int)nb) {
        cce_cascade_free(cas);
        return CCE_ERR_IO;
    }
    return CCE_OK;
}

cce_result cce_cascade_view_from_archive(cce_cascade* cas, cce_archive* ar, size_t offset) {
    if (!cas || !ar) return CCE_ERR_INVALID_ARG;

    /* Read only the tiny num_blocks header; the big weight blobs are VIEWED. */
    uint32_t nb = 0;
    if (cce_archive_read_raw(ar, offset, &nb, sizeof(uint32_t)) != CCE_OK) return CCE_ERR_IO;
    if (nb == 0 || nb > CCE_MAX_BLOCKS) return CCE_ERR_INVALID_ARG;

    if (cce_cascade_init(cas, (int)nb) != CCE_OK) return CCE_ERR_OOM;

    /* absolute walk position inside the archive (matches the save layout:
       [u32 nb] then per block: [u32 type,id,od,wb,bb][wb weights][bb bias]
       [f32 goodness][u32 flags][i32 freeze_countdown]) */
    size_t pos = offset + sizeof(uint32_t);

    for (uint32_t i = 0; i < nb; ++i) {
        uint32_t hdr[5];
        if (cce_archive_read_raw(ar, pos, hdr, sizeof(hdr)) != CCE_OK) break;
        uint32_t type = hdr[0], id = hdr[1], od = hdr[2], wb = hdr[3], bb = hdr[4];
        pos += sizeof(hdr);

        size_t w_off = pos;
        size_t b_off = pos + wb;
        size_t tail  = pos + wb + bb;

        if (type != CCE_BLOCK_PATCH && type != CCE_BLOCK_LINEAR && type != CCE_BLOCK_LINEAR_HEAD) break;
        if (id > (uint32_t)INT_MAX || od > (uint32_t)INT_MAX) break;

        /* sanity: declared byte sizes must match the dims we will view */
        size_t expected_wb = 0;
        size_t expected_bb = 0;
        if (type == CCE_BLOCK_PATCH) {
            if (!checked_matrix_bytes(id, od, &expected_wb)) break;
            if (id != od || wb != expected_wb || bb != 0) break;
        } else {
            if (!checked_matrix_bytes(id, od, &expected_wb) ||
                !checked_vector_bytes(od, &expected_bb)) break;
            if (wb != expected_wb || bb != expected_bb) break;
        }

        cce_block blk;
        memset(&blk, 0, sizeof(blk));   /* momentum/second-moment/device tensors stay null (read-only block) */
        blk.type = (cce_block_type_t)type;

        int wshape[2] = { (int)id, (int)od };
        /* zero-copy view into the mmap (falls back to an owned copy if unmapped/misaligned) */
        if (cce_archive_get_tensor_view(ar, w_off, wshape, 2, &blk.weights) != CCE_OK) break;
        if (type != CCE_BLOCK_PATCH) {
            int bshape[1] = { (int)od };
            if (cce_archive_get_tensor_view(ar, b_off, bshape, 1, &blk.bias) != CCE_OK) {
                cce_tensor_free(&blk.weights);
                break;
            }
        }

        float g = 0.0f; uint32_t fl = 0; int32_t fc = 0;
        if (cce_archive_read_raw(ar, tail, &g, sizeof(float)) != CCE_OK ||
            cce_archive_read_raw(ar, tail + sizeof(float), &fl, sizeof(uint32_t)) != CCE_OK ||
            cce_archive_read_raw(ar, tail + sizeof(float) + sizeof(uint32_t), &fc, sizeof(int32_t)) != CCE_OK) {
            cce_tensor_free(&blk.weights);
            cce_tensor_free(&blk.bias);
            break;
        }
        blk.goodness = g;
        blk.flags = fl | CCE_FLAG_FROZEN;   /* a view is read-only */
        if (blk.type == CCE_BLOCK_PATCH) blk.flags |= CCE_FLAG_PATCH;
        blk.freeze_countdown = (int)fc;

        if (cce_cascade_append(cas, &blk) != CCE_OK) {
            cce_block_free(&blk);
            break;
        }  /* struct copy; views carry owns_memory=0 */

        pos = tail + sizeof(float) + sizeof(uint32_t) + sizeof(int32_t);
    }

    if (cas->num_blocks != (int)nb) {
        cce_cascade_free(cas);
        return CCE_ERR_IO;
    }
    cce_cascade_freeze(cas);  /* mark the whole cascade frozen (read-only) */
    return CCE_OK;
}
