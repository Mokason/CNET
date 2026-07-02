#ifndef CCE_BLOCK_H
#define CCE_BLOCK_H

#include "cce_tensor.h"
#include "cce_defs.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Block types that fit the contractual paradigm */
typedef enum {
    CCE_BLOCK_LINEAR = 0,      /* squasher / dense */
    CCE_BLOCK_PATCH  = 1,      /* spatial patch contract */
    CCE_BLOCK_DEPTHWISE = 2,
    CCE_BLOCK_LINEAR_HEAD = 3, /* final linear (no sigmoid) for logits / classifiers */
} cce_block_type_t;

typedef struct {
    cce_block_type_t type;
    cce_tensor       weights;
    cce_tensor       bias;
    cce_tensor       momentum_weights;   /* first moment for Adam */
    cce_tensor       momentum_bias;
    cce_tensor       second_moment_w;    /* second moment for Adam */
    cce_tensor       second_moment_b;
    cce_flags_t      flags;
    float            goodness;     /* last measured goodness */
    int              freeze_countdown;
    int              timestep;       /* per-block Adam timestep for bias correction */

    /* Device-side copies for GPU (allocated lazily when GPU active) */
    cce_tensor       d_weights;
    cce_tensor       d_bias;
    cce_tensor       d_momentum_weights;
    cce_tensor       d_momentum_bias;
    cce_tensor       d_second_moment_w;
    cce_tensor       d_second_moment_b;

    /* Optional int8 weight-only PTQ (both NULL unless quantized). When set,
       cce_block_forward uses an int8 mat-vec:
         out[o] = bias[o] + w_scale[o] * Sum_i in[i] * w_q[i*out_dim + o].
       w_q keeps the same [in,out] row-major layout as `weights`. */
    int8_t*          w_q;       /* [in_dim*out_dim] int8 codes */
    float*           w_scale;   /* [out_dim] per-output-channel scale */

    /* trit-packed ternary (5 codes/byte, base-3) — inference-only 1.6-bit storage.
       When set, replaces w_q (which is freed): forward reads trits directly. */
    uint8_t*         w_trit;    /* [in_dim * ceil(out_dim/5)] */
    int              w_trit_bpr;/* bytes per input row = ceil(out_dim/5) */
} cce_block;

/* Initialize a linear (squasher) block */
cce_result cce_block_init_linear(cce_block* blk, int in_dim, int out_dim, float lr);

/* Forward pass (inplace friendly view) */
cce_result cce_block_forward(const cce_block* blk, const cce_tensor* input, cce_tensor* output);

/* Quantize this linear/linear-head block's weights to int8 (per-output-channel,
   symmetric). Weight-only PTQ: activations stay float. After this, forward uses
   the int8 path. No-op if already quantized; FP32 weights are left in place. */
cce_result cce_block_quantize_int8(cce_block* blk);

/* BitNet b1.58 ternary PTQ: per-output-channel absmean scale, weights in {-1,0,+1}.
   Reuses the int8 forward path. Post-hoc only — recovering quality needs QAT. */
cce_result cce_block_quantize_ternary(cce_block* blk);

/* Pack a ternary block's int8 codes into 1.6-bit trits (5/byte), freeing the int8
   codes. Forward then reads trits directly. Requires w_q (ternary) set. Bit-exact
   with the int8 ternary forward. */
cce_result cce_block_pack_trits(cce_block* blk);

/* Local learning step (for unfrozen blocks) */
cce_result cce_block_local_learn(cce_block* blk, const cce_tensor* input, const cce_tensor* target, float lr);

/* Freeze a block (stops learning, marks for contract) */
void cce_block_freeze(cce_block* blk);

/* Free owned tensors */
void cce_block_free(cce_block* blk);

#ifdef __cplusplus
}
#endif

#endif /* CCE_BLOCK_H */
