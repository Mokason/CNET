#ifndef CCE_TRANSFORMER_QAT_H
#define CCE_TRANSFORMER_QAT_H

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

typedef struct cce_transformer_qat cce_transformer_qat;

/* ---- modern-block selectors ------------------------------------------
 * Every legacy value is 0, so a memset-zeroed config reproduces exactly the
 * GPT-2-shaped model this trainer has always built. That is the
 * compatibility contract; tests/test_qat_block.c section 4 enforces it with
 * a bit-identity anchor. */
typedef enum { QAT_NORM_LN = 0, QAT_NORM_RMS = 1 } qat_norm_kind;
typedef enum { QAT_POS_LEARNED = 0, QAT_POS_ROPE = 1 } qat_pos_kind;
typedef enum { QAT_MLP_GELU = 0, QAT_MLP_SWIGLU = 1 } qat_mlp_kind;
/* Which dimension pairs rotate together. half-split pairs (i, i+hd/2) and is
 * the HF Llama/Qwen convention; interleaved pairs (0,1),(2,3),... They are
 * NOT interchangeable — a wrong choice trains fine and matches no reference. */
typedef enum { QAT_ROPE_HALF = 0, QAT_ROPE_INTERLEAVED = 1 } qat_rope_pairing;

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
    /* --- modern-block selectors; all-zero = legacy GPT-2 shape --- */
    int norm_kind;      /* qat_norm_kind */
    int pos_kind;       /* qat_pos_kind */
    int mlp_kind;       /* qat_mlp_kind */
    int rope_pairing;   /* qat_rope_pairing */
    int n_kv_head;      /* 0 => n_head (MHA); else must divide n_head */
    int no_bias;        /* 1 => no qkv/proj/up/down bias. Named negatively so
                           zero-init keeps the legacy meaning (biases present). */
    float rope_theta;   /* required > 0 when pos_kind == QAT_POS_ROPE */
    float norm_eps;     /* 0 => 1e-5 default */
} cce_transformer_qat_config;

/* Create with random (seeded) weights — enough for the hermetic gate. */
cce_transformer_qat* cce_transformer_qat_create(const cce_transformer_qat_config* cfg);
void cce_transformer_qat_free(cce_transformer_qat* t);

/* Load REAL pretrained Supra weights into an already-created trainer.
 *
 * decomposed_model is a `cce_supra_decomposed*` (from cce_supra_a2a_load /
 * cce_supra_load_decomposed) — declared void* here so this header does not have
 * to pull in cce_safetensors.h. Every real weight/bias is DIRECT-copied into the
 * matching trainer P.w matrix (same [in][out] row-major orientation, no
 * transpose); pos_emb (frozen FP) is copied too. The trainer MUST have been
 * created with a cfg matching the real model dims:
 *   n_layer=m->n_layer, n_embd=m->n_embd, n_head=m->n_head, vocab=m->vocab_size,
 *   block_size=m->block_size, mlp_hidden = up-projection out dim.
 * Returns CCE_ERR_INVALID_ARG on any dim/element-count mismatch. */
cce_result cce_transformer_qat_load_decomposed(cce_transformer_qat* t, void* decomposed_model);

/* Forward only: logits[vocab] at the LAST position (ternary where QAT). */
cce_result cce_transformer_qat_logits(cce_transformer_qat* t, const int* tokens, int T,
                                  float* logits /*[vocab]*/);

/* One training step on one sequence: forward (ternary where QAT), loss at the
 * last position, full backward, Adam update on every trainable group.
 * teacher_probs != NULL -> soft-KD cross-entropy vs that distribution
 * (the T=1 recipe the head milestones proved); NULL -> hard CE on target.
 * Returns the loss (double). */
double cce_transformer_qat_step(cce_transformer_qat* t, const int* tokens, int T,
                            const float* teacher_probs /*[vocab] or NULL*/,
                            int target, float lr);

/* Analytic-vs-numerical gradient check (FP mode; run with all qat_* = 0).
 * Central differences over n_samples params drawn from EVERY group.
 * Returns the max relative error seen (gate on < tol). */
double cce_transformer_qat_gradcheck(cce_transformer_qat* t, const int* tokens, int T,
                                 int target, int n_samples);

/* Flip the QAT knobs live. Ternarization happens on the fly from the FP
 * shadows, so "post-hoc baseline" == train FP, then set_qat(1,...) and eval
 * WITHOUT further training; "QAT" == set_qat(1,...) and keep training. */
/* Introspection for the registration audit: how many parameter groups the
 * gradcheck will visit, and how many scalar parameters they hold. A group
 * that is allocated but unregistered is silently ungradchecked, so the gate
 * asserts this count against the expected allocation. */
int cce_transformer_qat_group_count(const cce_transformer_qat* t);
int cce_transformer_qat_param_count(const cce_transformer_qat* t);
/* Groups the gradcheck actually visits (excludes frozen-by-design groups). */
int cce_transformer_qat_trainable_count(const cce_transformer_qat* t);

/* Test hook: rotate one head-dim vector in place at position pos. Exposed so
 * the RoPE pairing convention can be gated algebraically — gradcheck cannot
 * catch a wrong pairing, because any consistent rotation differentiates
 * correctly. */
void cce_transformer_qat_rope_test(float* v, int hd, int pos, float theta,
                                   int pairing);

/* Test hook: L2 norm of layer-0 fused-QKV weight gradient after one backward.
 * Exposed so GQA's KV accumulation can be checked directly — a
 * last-writer-wins bug there can cancel inside the directional gradcheck
 * projection and pass a green gate. */
double cce_transformer_qat_kgrad_norm(cce_transformer_qat* t, const int* tokens,
                                      int T, int target);

void cce_transformer_qat_set_qat(cce_transformer_qat* t, int qkv, int proj, int mlp,
                             int head, int emb);

#ifdef __cplusplus
}
#endif

#endif /* CCE_TRANSFORMER_QAT_H */
