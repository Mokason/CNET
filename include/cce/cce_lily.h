#ifndef CCE_LILY_H
#define CCE_LILY_H

#include "cce_defs.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
   cce_lily — Low-rank Interconnected Adaptation across Layers (prototype).

   Adapts a FROZEN stack of L same-width linear layers (a residual stream of
   width d): base h_{l+1} = W_l h_l. Lily adds a low-rank correction at each
   layer, and INTERCONNECTS the layers by sharing the down-projection basis A
   across them (Tied/VeRA-style), so the adaptation lives in one coherent rank-r
   subspace instead of L independent ones:

       h_{l+1} = W_l h_l + (alpha/r) * B_l (A h_l)      A shared [d,r], B_l [r,d]

   Params: r*d*(L+1)  (one shared A + L per-layer B_l), vs 2*L*r*d for
   independent per-layer LoRA and L*d*d for a dense per-layer update. Set
   shared=0 to get the independent baseline (per-layer A_l) with the same code —
   the benchmark toggles this to test whether the interconnection helps.

   This prototype is the parametrization + exact-backprop training + a
   generalization benchmark. It deliberately does NOT wire deltas into a deep
   model's serving forward (that is the invasive part); the certify/orchestrator
   machinery (registry_lora) is parametrization-agnostic and would host it
   unchanged. Raw-float storage keeps the prototype self-contained; the weight-
   store/streaming path (as in cce_lora) can be adopted later.
   --------------------------------------------------------------------------- */
typedef struct cce_lily {
    int    width;     /* d — residual-stream width (all layers d->d)  */
    int    rank;      /* r                                            */
    int    layers;    /* L                                            */
    int    shared;    /* 1 => one A shared across layers (Lily); 0 => per-layer A */
    float  alpha;     /* delta scale = alpha/rank                     */
    float *A;         /* [(shared?1:L) * d * r] down-projection(s)    */
    float *B;         /* [L * r * d] per-layer up-projections         */
} cce_lily;

/* A: small deterministic init; B: zero (delta starts at 0). */
cce_result cce_lily_init(cce_lily *ly, int width, int layers, int rank,
                         float alpha, uint32_t seed, int shared);
void       cce_lily_free(cce_lily *ly);

/* Forward through the frozen stack baseW[L*d*d] with the Lily deltas.
   x[d] -> y[d]. baseW[l] is row-major [d,d]: out[o] = sum_i in[i]*W[l][i*d+o]. */
cce_result cce_lily_apply(const cce_lily *ly, const float *baseW,
                          const float *x, float *y);

/* Fold the deltas into the stack in place: W_l[i,o] += (alpha/r) sum_k A[i,k]B_l[k,o]. */
cce_result cce_lily_merge(const cce_lily *ly, float *baseW /*[L*d*d]*/);

size_t cce_lily_param_count(const cce_lily *ly);        /* trained params */
size_t cce_lily_indep_param_count(const cce_lily *ly);  /* 2*L*r*d (independent LoRA) */
size_t cce_lily_dense_param_count(const cce_lily *ly);  /* L*d*d (dense per-layer) */

typedef struct {
    int   epochs;
    float lr;
    int   use_adam;
    float target_loss;   /* early stop when MSE <= this (0 => off) */
    int   log_every;
} cce_lily_train_opts;
cce_lily_train_opts cce_lily_train_defaults(void);

/* Fit A + B_l (base frozen) so apply(baseW, x_i) ~= targets_i, by exact backprop
   through the L-layer chain (Adam). inputs[n*d], targets[n*d]. Returns final MSE
   or a negative cce_result. The shared A accumulates gradient from every layer —
   that cross-layer coupling is the "interconnection". */
double cce_lily_train(cce_lily *ly, const float *baseW,
                      const float *inputs, const float *targets, size_t n,
                      const cce_lily_train_opts *opt);

double cce_lily_eval_mse(const cce_lily *ly, const float *baseW,
                         const float *inputs, const float *targets, size_t n);

/* ---- training-data collection through the deep forward ------------------- */
struct cce_ds_host;   /* forward decl (cce_ds_runtime.h) */

/* Run the DS forward on each input (n rows of width d_model, each set as the
   initial residual) and CAPTURE the residual at every layer's hook point into
   out_residuals[n * n_layer * d_model] (row-major [sample][layer][d]) — the
   per-layer training-data collection loop through the live deep forward. Any
   previously-installed layer hook is saved and restored. */
cce_result cce_lily_collect(struct cce_ds_host *h, const float *inputs, size_t n,
                            float *out_residuals);

/* Fit the adapter from collected per-layer data by residual distillation: at
   each layer L, delta_L(base_res_L) ~= (target_res_L - base_res_L). Trains A
   (shared) + B_L per layer with Adam and NO base backprop — each layer's target
   is the collected residual correction, so it works through a frozen MLA+MoE
   base that has no autograd. base_res/target_res are [n * layers * width].
   Returns final MSE (>=0) or a negative cce_result. */
double cce_lily_train_residual(cce_lily *ly, const float *base_res,
                               const float *target_res, size_t n,
                               const cce_lily_train_opts *opt);

/* ---- serve-in-the-loop training ------------------------------------------ */
/* Collect the pre-delta residual each layer sees while `ly` is applied — the
   served trajectory under the current adapter. out is [n*layers*width]. */
cce_result cce_lily_collect_served(struct cce_ds_host *h, const cce_lily *ly,
                                   const float *inputs, size_t n, float *out);

/* Serve-in-the-loop training (DAgger-style): repeatedly (1) run the forward WITH
   the current adapter and capture the pre-delta residual each layer actually
   sees, then (2) refit the deltas toward `teacher_res` on THOSE served residuals.
   Because each layer's target accounts for earlier layers' corrections, the
   deltas stop compounding (unlike offline per-layer distillation), so serving
   ALL layers no longer over-corrects. teacher_res is [n*layers*width]. Returns
   final MSE. */
double cce_lily_train_serve_loop(struct cce_ds_host *h, const float *inputs,
                                 const float *teacher_res, size_t n, cce_lily *ly,
                                 int outer_iters, const cce_lily_train_opts *inner);

/* ---- multi-token (context) variants --------------------------------------
   The compute-quality gap (dense vs sparse attention, all vs few experts) is a
   MULTI-TOKEN phenomenon: it lives in the attention/KV over accumulated
   positions and is exactly zero at a single token. These variants prefill T-1
   context tokens, then capture / serve per-layer residuals at the QUERY (last)
   token. `seqs` is [n*T*width] (n sequences of T tokens); out is
   [n*layers*width] (query token only).

   The host's CURRENT config (dsa_enable/dsa_fraction, map.hp.n_expert_used) is
   the QUERY-token compute — the caller's cheap-student knob. `prefill`, if
   non-NULL, is the compute for the T-1 context tokens (restored to the host's
   config for the query). Two scenarios:
     prefill == NULL  -> cheap everywhere (student prefill + student query): the
       gap includes the divergent KV cache, only partly recoverable at the query.
     prefill == full  -> cached full-compute context + cheap decode: the gap is
       purely the query token's compute, which the adapter recovers cleanly. */
typedef struct cce_lily_compute {
    int   dsa_enable;
    float dsa_fraction;
    int   n_expert_used;
} cce_lily_compute;

cce_result cce_lily_collect_ctx(struct cce_ds_host *h, const cce_lily_compute *prefill,
                                const float *seqs, size_t n, int T, float *out);
cce_result cce_lily_collect_served_ctx(struct cce_ds_host *h, const cce_lily_compute *prefill,
                                       const cce_lily *ly,
                                       const float *seqs, size_t n, int T, float *out);
double cce_lily_train_serve_loop_ctx(struct cce_ds_host *h, const cce_lily_compute *prefill,
                                     const float *seqs,
                                     const float *teacher_res, size_t n, int T,
                                     cce_lily *ly, int outer_iters,
                                     const cce_lily_train_opts *inner);

/* ---- interior-layer serving (deep-base residual stream) ------------------ */
/* Install this adapter as the DS forward's interior-layer hook: after each layer
   L, the residual gets += (alpha/r) B_L (A_L · residual). Off by default (hook
   NULL); one active adapter at a time (prototype). ly->width must equal the
   model's d_model and ly->layers its n_layer. */
void cce_lily_install_serving(const cce_lily *ly);
void cce_lily_uninstall_serving(void);

#ifdef __cplusplus
}
#endif

#endif /* CCE_LILY_H */
