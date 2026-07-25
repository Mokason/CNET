#ifndef CCE_MOE_XF_H
#define CCE_MOE_XF_H

/* Per-token transformer MoE block trainer — the scale-up path from cce_moe_train.
 *
 * One transformer block over a length-`seq` sequence, trained for next-token CE:
 *   x  = embed(tokens) + pos          (learned token + positional embeddings)
 *   x1 = x  + Attention(x)            (single-head, causal)
 *   x2 = x1 + MoE_FFN(x1)             (softmax router, top-k experts, residual)
 *   logits = x2 @ Wh + bh ;  loss = CE + lb_coef*aux
 *
 * Unlike cce_moe_train (which concatenates a fixed context and doesn't scale),
 * this processes each token at d_model and mixes context with attention — the
 * real transformer shape. Every heavy matmul (QKV/O, expert up/down, head, and
 * their backward GEMMs) goes through one dispatch point so it runs on the CPU
 * (reference, for gradient-checking) or the GPU via cce_clgemm (for throughput).
 * Experts are dense-looped for now (no gather/scatter dispatch yet). Explicit
 * forward+backward, Adam(W); finite-difference gradient-checked.
 */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int   vocab;
    int   d_model;
    int   seq;         /* sequence length (tokens per training example) */
    int   n_expert;
    int   top_k;
    int   d_ff;        /* per-expert hidden width */
    float lb_coef;
    float weight_decay;
    unsigned seed;
} cce_moe_xf_cfg;

typedef struct cce_moe_xf cce_moe_xf;

cce_moe_xf *cce_moe_xf_create(const cce_moe_xf_cfg *cfg);
void        cce_moe_xf_free(cce_moe_xf *m);

/* Turn GPU matmuls on (1) / off (0). Returns 1 if the GPU is available and now
 * active, else 0 (stays on CPU). Safe to call anytime. */
int cce_moe_xf_use_gpu(cce_moe_xf *m, int on);
const char *cce_moe_xf_device(const cce_moe_xf *m); /* GPU device string, or "cpu" */

/* One sequence: tokens[0..seq] (seq+1 ids; predict token t+1 from prefix). Fills
 * *ce (mean CE over the seq positions) and *aux (load-balance). With do_backward,
 * accumulates grads. Returns ce + lb_coef*aux. */
double cce_moe_xf_seq(cce_moe_xf *m, const int *tokens, int do_backward, double *ce, double *aux);

void cce_moe_xf_zero_grad(cce_moe_xf *m);
void cce_moe_xf_adam(cce_moe_xf *m, float lr, int step);

/* Directional finite-difference gradient check on one sequence, run on whatever
 * device is currently active (call with GPU on to validate the GPU backward). */
double cce_moe_xf_grad_check(cce_moe_xf *m, const int *tokens);

/* ---- checkpointing --------------------------------------------------------
 * Persist weights AND the Adam moments plus the step counter, so a resumed run
 * continues the same optimisation rather than restarting it: without the
 * moments, every resume re-enters the bias-correction warmup and throws away
 * the accumulated second-moment scale. Gradients are not stored (recomputed).
 *
 * The file records the full cce_moe_xf_cfg and an FNV-1a checksum over the
 * payload; load REFUSES a config mismatch or a truncated/corrupt file rather
 * than silently reinterpreting it. Returns 0 on success, negative on error.
 * cce_moe_xf_load returns NULL on any failure and sets *step_out only on
 * success. */
int cce_moe_xf_save(const cce_moe_xf *m, const char *path, int step);
cce_moe_xf *cce_moe_xf_load(const char *path, int *step_out);

#ifdef __cplusplus
}
#endif

#endif /* CCE_MOE_XF_H */
