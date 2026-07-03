#include "../../include/cce/cce_block.h"
#include "../../include/cce/cce_gpu.h"
#include "../../include/cce/cce_trit_lut.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

static float sigmoid(float x) { return 1.0f / (1.0f + expf(-x)); }

cce_result cce_block_init_linear(cce_block* blk, int in_dim, int out_dim, float lr) {
    (void)lr; /* stored on higher level for now */
    if (!blk) return CCE_ERR_INVALID_ARG;

    memset(blk, 0, sizeof(*blk));
    blk->type = CCE_BLOCK_LINEAR;

    int wshape[2] = {in_dim, out_dim};
    cce_result rc = cce_tensor_alloc(&blk->weights, wshape, 2);
    if (rc != CCE_OK) return rc;

    int bshape[1] = {out_dim};
    rc = cce_tensor_alloc(&blk->bias, bshape, 1);
    if (rc != CCE_OK) {
        cce_tensor_free(&blk->weights);
        return rc;
    }

    /* Alloc Adam moments (first + second, same shape, zeroed) */
    rc = cce_tensor_alloc(&blk->momentum_weights, wshape, 2);
    if (rc != CCE_OK) {
        cce_tensor_free(&blk->bias);
        cce_tensor_free(&blk->weights);
        return rc;
    }
    rc = cce_tensor_alloc(&blk->momentum_bias, bshape, 1);
    if (rc != CCE_OK) {
        cce_tensor_free(&blk->momentum_weights);
        cce_tensor_free(&blk->bias);
        cce_tensor_free(&blk->weights);
        return rc;
    }
    rc = cce_tensor_alloc(&blk->second_moment_w, wshape, 2);
    if (rc != CCE_OK) {
        cce_tensor_free(&blk->momentum_bias);
        cce_tensor_free(&blk->momentum_weights);
        cce_tensor_free(&blk->bias);
        cce_tensor_free(&blk->weights);
        return rc;
    }
    rc = cce_tensor_alloc(&blk->second_moment_b, bshape, 1);
    if (rc != CCE_OK) {
        cce_tensor_free(&blk->second_moment_w);
        cce_tensor_free(&blk->momentum_bias);
        cce_tensor_free(&blk->momentum_weights);
        cce_tensor_free(&blk->bias);
        cce_tensor_free(&blk->weights);
        return rc;
    }
    cce_tensor_zero(&blk->momentum_weights);
    cce_tensor_zero(&blk->momentum_bias);
    cce_tensor_zero(&blk->second_moment_w);
    cce_tensor_zero(&blk->second_moment_b);

    /* Xavier-ish init */
    float scale = 1.0f / sqrtf((float)in_dim);
    for (size_t i = 0; i < blk->weights.numel; ++i) {
        blk->weights.data[i] = ((float)rand() / RAND_MAX * 2 - 1) * scale;
    }
    for (size_t i = 0; i < blk->bias.numel; ++i) {
        blk->bias.data[i] = 0.0f;
    }

    blk->goodness = 0.5f;
    blk->freeze_countdown = 0;
    blk->timestep = 0;

    /* Zero device tensors (will be allocated on first GPU use) */
    memset(&blk->d_weights, 0, sizeof(cce_tensor));
    memset(&blk->d_bias, 0, sizeof(cce_tensor));
    memset(&blk->d_momentum_weights, 0, sizeof(cce_tensor));
    memset(&blk->d_momentum_bias, 0, sizeof(cce_tensor));
    memset(&blk->d_second_moment_w, 0, sizeof(cce_tensor));
    memset(&blk->d_second_moment_b, 0, sizeof(cce_tensor));

    return CCE_OK;
}

cce_result cce_block_forward(const cce_block* blk, const cce_tensor* input, cce_tensor* output) {
    if (!blk || !input || !output) return CCE_ERR_INVALID_ARG;
    if (blk->type == CCE_BLOCK_PATCH) {
        /* Basic patch forward: for demo, copy or apply simple transform (weights are identity-ish) */
        /* Here we assume input is already patch-flattened; simple mat-vec */
        int in_dim  = blk->weights.shape[0];
        int out_dim = blk->weights.shape[1];
        if (input->numel != (size_t)in_dim || output->numel != (size_t)out_dim) return CCE_ERR_INVALID_ARG;
        for (int o = 0; o < out_dim; ++o) {
            float sum = 0.0f;
            for (int i = 0; i < in_dim; ++i) sum += input->data[i] * blk->weights.data[i*out_dim + o];
            output->data[o] = sum;  /* linear for patch demo, no sigmoid */
        }
        return CCE_OK;
    }
    if (blk->type != CCE_BLOCK_LINEAR && blk->type != CCE_BLOCK_LINEAR_HEAD) return CCE_ERR_UNSUPPORTED;

    int in_dim  = blk->weights.shape[0];
    int out_dim = blk->weights.shape[1];

    if (input->numel != (size_t)in_dim || output->numel != (size_t)out_dim)
        return CCE_ERR_INVALID_ARG;

    /* Try GPU acceleration if available (CUDA on 4070 is excellent here) */
    /* For proper resident, use sync, here we do per call upload for simplicity */
    extern cce_result cce_gpu_try_matmul(const cce_tensor* a, const cce_tensor* b, cce_tensor* c);
    bool is_head = (blk->type == CCE_BLOCK_LINEAR_HEAD);

    /* trit-packed ternary path (1.6-bit): decode each row segment into a tiny
       per-tile int8 scratch (one 8-byte overlapping store per packed byte via
       cce_trit_lut), then run the SAME vectorizable mul-add as the int8 path.
       The decoded codes are exactly the w_q codes pack_trits consumed and each
       output o still accumulates a*code over i ascending -> bit-identical to
       the int8 ternary path. Tiles are 510 wide (a multiple of 5) so every
       tile starts byte-aligned in the packed rows. */
    if (blk->w_trit && blk->w_scale) {
        #pragma omp parallel for schedule(static) default(none) \
                shared(output, blk, input, in_dim, out_dim, is_head, cce_trit_lut) if(out_dim >= 4096)
        for (int ob = 0; ob < out_dim; ob += 510) {
            int oe = (ob + 510 < out_dim) ? ob + 510 : out_dim;
            int w = oe - ob;
            int nb = (w + 4) / 5;             /* packed bytes covering the tile */
            int8_t codes[510 + 8];            /* +8: the last decode store overlaps */
            float* out = &output->data[ob];
            for (int o = 0; o < w; ++o) out[o] = 0.0f;
            for (int i = 0; i < in_dim; ++i) {
                const float a = input->data[i];
                const uint8_t* p = &blk->w_trit[(size_t)i * blk->w_trit_bpr + (size_t)(ob / 5)];
                for (int b = 0; b < nb; ++b)
                    memcpy(&codes[b * 5], cce_trit_lut[p[b]], 8);
                for (int o = 0; o < w; ++o) out[o] += a * (float)codes[o];
            }
            for (int o = 0; o < w; ++o) {
                float v = blk->bias.data[ob + o] + blk->w_scale[ob + o] * out[o];
                out[o] = is_head ? v : sigmoid(v);
            }
        }
        return CCE_OK;
    }

    /* int8 weight-only PTQ path: out[o] = bias[o] + scale[o]*Sum_i in[i]*w_q[i*out+o].
       int8 weights are 1/4 the bytes -> less memory traffic on the bandwidth-bound
       head. Same tiled/vectorized/threaded structure as the float path. */
    if (blk->w_q && blk->w_scale) {
        #pragma omp parallel for schedule(static) default(none) \
                shared(output, blk, input, in_dim, out_dim, is_head) if(out_dim >= 4096)
        for (int ob = 0; ob < out_dim; ob += 512) {
            int oe = (ob + 512 < out_dim) ? ob + 512 : out_dim;
            for (int o = ob; o < oe; ++o) output->data[o] = 0.0f;
            for (int i = 0; i < in_dim; ++i) {
                const float a = input->data[i];
                const int8_t* qrow = &blk->w_q[(size_t)i * out_dim];
                for (int o = ob; o < oe; ++o) output->data[o] += a * (float)qrow[o];
            }
            for (int o = ob; o < oe; ++o) {
                float v = blk->bias.data[o] + blk->w_scale[o] * output->data[o];
                output->data[o] = is_head ? v : sigmoid(v);
            }
        }
        return CCE_OK;
    }

    if (in_dim > 32 || out_dim > 32) {  /* worth it for larger layers */
        cce_tensor lin_out;
        int oshape[2] = {1, out_dim};
        if (cce_tensor_alloc(&lin_out, oshape, 2) == CCE_OK) {
            /* build row vec view */
            if (cce_gpu_try_matmul(input, &blk->weights, &lin_out) == CCE_OK) {
                for (int o = 0; o < out_dim; ++o) {
                    float v = lin_out.data[o] + blk->bias.data[o];
                    output->data[o] = is_head ? v : sigmoid(v);
                }
                cce_tensor_free(&lin_out);
                return CCE_OK;
            }
            cce_tensor_free(&lin_out);
        }
    }

    /* Direct mat-vec fallback.
       i-outer / o-inner so the inner loop streams contiguous memory: both
       weights[i*out_dim + o] and output[o] are unit-stride in o, which the
       compiler auto-vectorizes (SSE/AVX). The original o-outer loop made the
       inner reduction stride by out_dim through `weights`, defeating SIMD.
       Per-output accumulation order (i = 0..in_dim-1) is unchanged, so the
       result is bit-identical to the previous version. */
    /* Output is partitioned into 512-wide tiles. Each tile owns a disjoint range
       of output[o], so the tiles are independent and parallelize cleanly; the
       inner o-loop stays unit-stride (vectorizes). Per-output summation order is
       unchanged -> bit-identical. OpenMP only kicks in for large layers (the
       50520-wide head); the pragma is a no-op without -fopenmp. */
    #pragma omp parallel for schedule(static) default(none) \
            shared(output, blk, input, in_dim, out_dim) if(out_dim >= 4096)
    for (int ob = 0; ob < out_dim; ob += 512) {
        int oe = (ob + 512 < out_dim) ? ob + 512 : out_dim;
        for (int o = ob; o < oe; ++o) output->data[o] = blk->bias.data[o];
        for (int i = 0; i < in_dim; ++i) {
            const float a = input->data[i];
            const float* wrow = &blk->weights.data[(size_t)i * out_dim];
            for (int o = ob; o < oe; ++o) output->data[o] += a * wrow[o];
        }
    }
    if (!is_head) {
        for (int o = 0; o < out_dim; ++o) output->data[o] = sigmoid(output->data[o]);
    }
    return CCE_OK;
}

cce_result cce_block_local_learn(cce_block* blk, const cce_tensor* input, const cce_tensor* target, float lr) {
    if (!blk || blk->flags & CCE_FLAG_FROZEN) return CCE_ERR_FROZEN;
    (void)input; (void)target; (void)lr;
    /* Placeholder for DFA / target-prop in real CCE */
    return CCE_OK;
}

void cce_block_freeze(cce_block* blk) {
    if (blk) blk->flags |= CCE_FLAG_FROZEN;
}

cce_result cce_block_quantize_int8(cce_block* blk) {
    if (!blk) return CCE_ERR_INVALID_ARG;
    if (blk->type != CCE_BLOCK_LINEAR && blk->type != CCE_BLOCK_LINEAR_HEAD)
        return CCE_ERR_UNSUPPORTED;
    if (!blk->weights.data || blk->weights.ndim != 2) return CCE_ERR_INVALID_ARG;
    if (blk->w_q) return CCE_OK;  /* already quantized */

    int in_dim  = blk->weights.shape[0];
    int out_dim = blk->weights.shape[1];
    int8_t* q  = (int8_t*)malloc((size_t)in_dim * out_dim * sizeof(int8_t));
    float*  sc = (float*)calloc((size_t)out_dim, sizeof(float));
    if (!q || !sc) { free(q); free(sc); return CCE_ERR_OOM; }

    /* per-output-channel (column o) abs-max -> symmetric scale */
    for (int i = 0; i < in_dim; ++i) {
        const float* wr = &blk->weights.data[(size_t)i * out_dim];
        for (int o = 0; o < out_dim; ++o) { float a = fabsf(wr[o]); if (a > sc[o]) sc[o] = a; }
    }
    for (int o = 0; o < out_dim; ++o) sc[o] = (sc[o] > 0.0f) ? sc[o] / 127.0f : 1.0f;

    for (int i = 0; i < in_dim; ++i) {
        const float* wr = &blk->weights.data[(size_t)i * out_dim];
        int8_t* qr = &q[(size_t)i * out_dim];
        for (int o = 0; o < out_dim; ++o) {
            long c = lroundf(wr[o] / sc[o]);
            if (c >  127) c =  127;
            if (c < -127) c = -127;
            qr[o] = (int8_t)c;
        }
    }
    blk->w_q = q;
    blk->w_scale = sc;
    return CCE_OK;
}

/* BitNet b1.58 ternary quantization (POST-TRAINING): per-output-channel absmean
   scale gamma, weights -> RoundClip(W/gamma, -1, 1) in {-1,0,+1}. Reuses the int8
   storage + forward path (codes are just -1/0/1). NOTE: applied post-hoc to a model
   trained in full precision this loses quality — BitNet's accuracy needs
   quantization-aware training (shadow weights + STE). This is the inference path. */
cce_result cce_block_quantize_ternary(cce_block* blk) {
    if (!blk) return CCE_ERR_INVALID_ARG;
    if (blk->type != CCE_BLOCK_LINEAR && blk->type != CCE_BLOCK_LINEAR_HEAD)
        return CCE_ERR_UNSUPPORTED;
    if (!blk->weights.data || blk->weights.ndim != 2) return CCE_ERR_INVALID_ARG;
    if (blk->w_q) return CCE_OK;

    int in_dim  = blk->weights.shape[0];
    int out_dim = blk->weights.shape[1];
    int8_t* q  = (int8_t*)malloc((size_t)in_dim * out_dim * sizeof(int8_t));
    float*  sc = (float*)calloc((size_t)out_dim, sizeof(float));
    if (!q || !sc) { free(q); free(sc); return CCE_ERR_OOM; }

    /* per-output-channel absmean gamma */
    for (int i = 0; i < in_dim; ++i) {
        const float* wr = &blk->weights.data[(size_t)i * out_dim];
        for (int o = 0; o < out_dim; ++o) sc[o] += fabsf(wr[o]);
    }
    for (int o = 0; o < out_dim; ++o) { sc[o] /= (float)in_dim; if (sc[o] <= 0.0f) sc[o] = 1.0f; }

    /* RoundClip(W/gamma, -1, 1) */
    for (int i = 0; i < in_dim; ++i) {
        const float* wr = &blk->weights.data[(size_t)i * out_dim];
        int8_t* qr = &q[(size_t)i * out_dim];
        for (int o = 0; o < out_dim; ++o) {
            long c = lroundf(wr[o] / sc[o]);
            if (c >  1) c =  1;
            if (c < -1) c = -1;
            qr[o] = (int8_t)c;
        }
    }
    blk->w_q = q;
    blk->w_scale = sc;
    return CCE_OK;
}

cce_result cce_block_pack_trits(cce_block* blk) {
    if (!blk || !blk->w_q || !blk->w_scale || blk->weights.ndim != 2) return CCE_ERR_INVALID_ARG;
    if (blk->w_trit) return CCE_OK;
    int in_dim  = blk->weights.shape[0];
    int out_dim = blk->weights.shape[1];
    int bpr = (out_dim + 4) / 5;
    uint8_t* t = (uint8_t*)malloc((size_t)in_dim * bpr);
    if (!t) return CCE_ERR_OOM;
    for (int i = 0; i < in_dim; ++i) {
        const int8_t* qr = &blk->w_q[(size_t)i * out_dim];
        uint8_t* tr = &t[(size_t)i * bpr];
        int o = 0;
        while (o < out_dim) {
            int b = 0, mul = 1;
            for (int k = 0; k < 5; ++k) {
                int code = (o < out_dim) ? (int)qr[o] : 0;   /* codes are {-1,0,+1} */
                if (o < out_dim) o++;
                b += (code + 1) * mul; mul *= 3;
            }
            *tr++ = (uint8_t)b;
        }
    }
    blk->w_trit = t;
    blk->w_trit_bpr = bpr;
    free(blk->w_q); blk->w_q = NULL;   /* trit is now the storage; no int8 codes */
    return CCE_OK;
}

void cce_block_free(cce_block* blk) {
    if (!blk) return;
    free(blk->w_q);     blk->w_q = NULL;
    free(blk->w_trit);  blk->w_trit = NULL;
    free(blk->w_scale); blk->w_scale = NULL;
    cce_tensor_free(&blk->weights);
    cce_tensor_free(&blk->bias);
    cce_tensor_free(&blk->momentum_weights);
    cce_tensor_free(&blk->momentum_bias);
    cce_tensor_free(&blk->second_moment_w);
    cce_tensor_free(&blk->second_moment_b);

    /* Free device copies if any */
    if (blk->d_weights.data) cce_gpu_free_device(&blk->d_weights);
    if (blk->d_bias.data) cce_gpu_free_device(&blk->d_bias);
    if (blk->d_momentum_weights.data) cce_gpu_free_device(&blk->d_momentum_weights);
    if (blk->d_momentum_bias.data) cce_gpu_free_device(&blk->d_momentum_bias);
    if (blk->d_second_moment_w.data) cce_gpu_free_device(&blk->d_second_moment_w);
    if (blk->d_second_moment_b.data) cce_gpu_free_device(&blk->d_second_moment_b);

    memset(blk, 0, sizeof(*blk));
}
