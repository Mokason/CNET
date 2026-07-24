#ifndef CCE_MOE_TRAIN_H
#define CCE_MOE_TRAIN_H

/* Sparse MoE language-model training for CNET.
 *
 * A tiny but genuine Mixture-of-Experts LM trained end to end with explicit
 * gradients (the router softmax, top-k gating, per-expert FFNs, the load-balance
 * auxiliary loss, and the next-token cross-entropy head all backprop by hand;
 * finite-difference gradient-checked). It is the sparse FFN sublayer that gives
 * MoE transformers their capacity, exercised as an LM:
 *
 *   ctx tokens --embed--> h = mean(context embeddings)          [d_model]
 *   router: g = softmax(h @ Wr)                                 [n_expert]
 *   pick top_k experts; expert_e(h) = relu(h @ W1_e) @ W2_e     [d_model]
 *   z = h + sum_{e in top-k} g_e * expert_e(h)   (residual MoE)
 *   logits = z @ Wh + bh ; loss = CE(logits, next) + lb_coef * aux
 *
 * Optimizer: Adam (per-parameter moments). No attention — the context pool is a
 * cheap stand-in — so this is the MoE mechanism itself, not a full transformer.
 */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int   vocab;       /* V   */
    int   d_model;     /* d   */
    int   ctx;         /* context tokens pooled into h (>=1) */
    int   n_expert;    /* E   */
    int   top_k;       /* K   experts fired per token (1..E) */
    int   d_ff;        /* per-expert hidden width */
    float lb_coef;     /* load-balance aux loss weight (0 = off) */
    float weight_decay;/* decoupled AdamW weight decay (0 = off) */
    unsigned seed;
} cce_moe_cfg;

typedef struct cce_moe cce_moe;

cce_moe *cce_moe_create(const cce_moe_cfg *cfg);
void     cce_moe_free(cce_moe *m);

/* Forward (+ optional backward) over N examples. tokens[n*ctx + c] are the ctx
 * context token ids; targets[n] is the next token. Fills *ce (mean cross-entropy,
 * nats), *aux (load-balance loss), *balance (max_e load / mean load; 1.0 = perfect
 * balance). Returns ce + lb_coef*aux. With do_backward, accumulates grads into
 * the model (call cce_moe_zero_grad first, cce_moe_adam after). */
double cce_moe_batch(cce_moe *m, const int *tokens, const int *targets, int N,
                     int do_backward, double *ce, double *aux, double *balance);

void cce_moe_zero_grad(cce_moe *m);
void cce_moe_adam(cce_moe *m, float lr, int step);

/* Fraction of tokens (over the last cce_moe_batch) whose top-k included expert e. */
void cce_moe_expert_usage(const cce_moe *m, float *usage /* [n_expert] */);

/* Max relative error between analytic and central finite-difference gradients on
 * (tokens,targets,N). < ~1e-3 means the hand-written backward is correct. */
double cce_moe_grad_check(cce_moe *m, const int *tokens, const int *targets, int N);

#ifdef __cplusplus
}
#endif

#endif /* CCE_MOE_TRAIN_H */
