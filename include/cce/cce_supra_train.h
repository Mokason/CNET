#ifndef CCE_SUPRA_TRAIN_H
#define CCE_SUPRA_TRAIN_H

/* Supra QAT trainer: the transformer backward the quality phase is gated on
 * (docs/superpowers/specs/2026-06-28-supra-qat-scope.md, steps 7-10).
 *
 * Hand-rolled reverse pass over a GPT-shaped stack mirroring the verified
 * cce_supra_gpt_forward math (LayerNorm -> fused QKV -> causal softmax
 * attention -> proj -> residual -> LayerNorm -> MLP up -> tanh-GELU -> down
 * -> residual; final LN -> linear head), following the PROVEN cce_wordlm
 * recipe: FP shadow weights + per-output absmean ternary forward + STE
 * backward (identity into the shadow), double-accumulation dots, Adam.
 *
 * Scale orientation matches cce_block_quantize_ternary (per-OUTPUT column
 * absmean, weights [in][out]) so a trained shadow exports straight into the
 * existing ternary/packed pipeline.
 *
 * Tensor policy (spec section 6): qkv/proj/mlp/head/tok_emb are QAT
 * candidates (per-group knobs); pos_emb is FROZEN FP; LayerNorm params and
 * biases stay FP but DO train.
 *
 * Gate: tests/test_supra_train.c — hermetic tiny-model gradcheck (central
 * differences over every parameter group) + QAT smoke (loss decreases,
 * QAT beats post-hoc on FP-argmax recovery). No model files needed.
 */

#include "cce_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cce_supra_train cce_supra_train;

typedef struct {
    int n_layer;      /* transformer blocks */
    int n_embd;       /* hidden size D */
    int n_head;       /* attention heads (D % n_head == 0) */
    int mlp_hidden;   /* MLP up width (e.g. 4*D) */
    int vocab;        /* vocabulary size */
    int block_size;   /* max sequence length T */
    unsigned seed;    /* deterministic init */
    /* QAT knobs: 1 = ternary forward + STE for that group, 0 = plain FP */
    int qat_qkv, qat_proj, qat_mlp, qat_head, qat_emb;
} cce_supra_train_config;

/* Create with random (seeded) weights — enough for the hermetic gate. */
cce_supra_train* cce_supra_train_create(const cce_supra_train_config* cfg);
void cce_supra_train_free(cce_supra_train* t);

/* Forward only: logits[vocab] at the LAST position (ternary where QAT). */
cce_result cce_supra_train_logits(cce_supra_train* t, const int* tokens, int T,
                                  float* logits /*[vocab]*/);

/* One training step on one sequence: forward (ternary where QAT), loss at the
 * last position, full backward, Adam update on every trainable group.
 * teacher_probs != NULL -> soft-KD cross-entropy vs that distribution
 * (the T=1 recipe the head milestones proved); NULL -> hard CE on target.
 * Returns the loss (double). */
double cce_supra_train_step(cce_supra_train* t, const int* tokens, int T,
                            const float* teacher_probs /*[vocab] or NULL*/,
                            int target, float lr);

/* Analytic-vs-numerical gradient check (FP mode; run with all qat_* = 0).
 * Central differences over n_samples params drawn from EVERY group.
 * Returns the max relative error seen (gate on < tol). */
double cce_supra_train_gradcheck(cce_supra_train* t, const int* tokens, int T,
                                 int target, int n_samples);

/* Flip the QAT knobs live. Ternarization happens on the fly from the FP
 * shadows, so "post-hoc baseline" == train FP, then set_qat(1,...) and eval
 * WITHOUT further training; "QAT" == set_qat(1,...) and keep training. */
void cce_supra_train_set_qat(cce_supra_train* t, int qkv, int proj, int mlp,
                             int head, int emb);

#ifdef __cplusplus
}
#endif

#endif /* CCE_SUPRA_TRAIN_H */
