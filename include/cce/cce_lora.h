#ifndef CCE_LORA_H
#define CCE_LORA_H

#include "cce_tensor.h"
#include "cce_defs.h"
#include "cce_cascade.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
   cce_lora — a rank-r low-rank adapter for a FROZEN linear map y = xW + b
   (W row-major [in,out], as used by cce_block LINEAR/LINEAR_HEAD).

   The adapter adds a low-rank correction

       y += (alpha/rank) * (x A) B ,   A:[in,rank], B:[rank,out].

   Only A and B are trained; the base W stays frozen. B is zero-initialised so
   the delta is exactly 0 until trained (standard LoRA), which makes attaching
   an untrained adapter a guaranteed no-op. Trained params: rank*(in+out) vs a
   dense output-layer update's in*out — the whole point of the prototype.

   Math is computed here with explicit matvecs (not cce_block_forward, whose
   LINEAR path applies a sigmoid) so the delta is an exact bilinear map. For the
   streaming/weight-store path the same A,B round-trip through a 2-block
   LINEAR_HEAD cascade (cce_lora_to_cascade), so it packs and demand-loads like
   any expert leaf.
   --------------------------------------------------------------------------- */
typedef struct cce_lora {
    cce_tensor A;          /* [in_dim, rank]  — small deterministic init */
    cce_tensor B;          /* [rank, out_dim] — zero-initialised          */
    int   in_dim;
    int   out_dim;
    int   rank;
    float alpha;           /* effective delta scaled by alpha/rank        */
    uint64_t base_digest;  /* identity of the frozen base (0 if unbound)  */
} cce_lora;

/* Allocate A (small deterministic gaussian, seeded) and B (zeros). */
cce_result cce_lora_init(cce_lora* lo, int in_dim, int out_dim, int rank,
                         float alpha, uint32_t seed);
void       cce_lora_free(cce_lora* lo);

/* y_accum[out] += (alpha/rank) * (x A) B. x:[in], y_accum:[out] (ACCUMULATES).
   Pass a y_accum that already holds the base output y = xW + b. */
cce_result cce_lora_apply(const cce_lora* lo, const cce_tensor* x, cce_tensor* y_accum);

/* Fold the delta into a dense weight tensor W:[in,out] in place:
   W[i,o] += (alpha/rank) * sum_k A[i,k] B[k,o]. Zero inference overhead after. */
cce_result cce_lora_merge(const cce_lora* lo, cce_tensor* W_inout);

/* Trainable vs dense-equivalent parameter counts. */
size_t cce_lora_param_count(const cce_lora* lo);
size_t cce_lora_dense_param_count(const cce_lora* lo);

/* ---- training (M2) ------------------------------------------------------ */
typedef struct {
    int      epochs;        /* passes over the data                        */
    float    lr;            /* learning rate                               */
    int      use_adam;      /* 1 => Adam (recommended), 0 => plain SGD     */
    float    weight_decay;  /* L2 on A,B (0 to disable)                    */
    float    target_loss;   /* early-stop when MSE <= this (0 => off)      */
    int      log_every;     /* 0 => silent; else print MSE every k epochs  */
} cce_lora_train_opts;

cce_lora_train_opts cce_lora_train_defaults(void);

/* Fit the adapter so apply(x_i) ~= residual_i, residual = teacher_out - base_out.
   inputs:[n*in_dim], residuals:[n*out_dim], row-major. Returns final MSE
   (>=0) or a negative cce_result on error. Base W is untouched (frozen). */
double cce_lora_train(cce_lora* lo, const float* inputs, const float* residuals,
                      size_t n, const cce_lora_train_opts* opt);

/* Convenience: mean-squared error of apply(x_i) vs residual_i over the set. */
double cce_lora_eval_mse(const cce_lora* lo, const float* inputs,
                         const float* residuals, size_t n);

/* ---- persistence / streaming bridge (M3) -------------------------------- */
/* Direct self-describing binary round-trip (used by the unit test). */
cce_result cce_lora_save(const cce_lora* lo, const char* path);
cce_result cce_lora_load(cce_lora* lo, const char* path);

/* Build a 2-block LINEAR_HEAD cascade [A, B] carrying the delta, so the adapter
   persists and demand-loads through the existing weight-store/expert path.
   The cascade forward (pure linear heads, bias 0) equals (alpha/rank)*(xA)B when
   the alpha/rank scale is folded into B (done here). Round-trips via merge. */
cce_result cce_lora_to_cascade(const cce_lora* lo, cce_cascade** out);
cce_result cce_lora_from_cascade(const cce_cascade* cas, int in_dim, int out_dim,
                                 int rank, cce_lora* out);

/* ---- integration (M4) --------------------------------------------------- */
/* Frozen-head forward with an optional adapter: out = head(in) + delta.
   lo == NULL gives the plain frozen path with zero overhead — so this is a
   drop-in for cce_block_forward at a head call site. The adapter is resolved
   per-skill by the caller (router), so the shared cce_block struct and its
   serialization are untouched. Only LINEAR_HEAD blocks are adapted (pure
   linear pre-activation); a non-head block returns CCE_ERR_UNSUPPORTED when an
   adapter is supplied. */
cce_result cce_lora_head_forward(const cce_block* head, const cce_lora* lo,
                                 const cce_tensor* in, cce_tensor* out);

/* Router-facing training hook (OFF by default — the registry calls this only
   when adapter-teaching is enabled). Fits the adapter on the residual between a
   teacher's corrected outputs and the frozen head's outputs over n
   port-validated pairs. inputs:[n*in], base_out/teacher_out:[n*out]. Returns
   final MSE or a negative cce_result. */
double cce_lora_fit_residual(cce_lora* lo, const float* inputs,
                             const float* base_out, const float* teacher_out,
                             size_t n, const cce_lora_train_opts* opt);

#ifdef __cplusplus
}
#endif

#endif /* CCE_LORA_H */
