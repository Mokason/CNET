#ifndef CCE_DSA_H
#define CCE_DSA_H

/*
 * DeepSeek Sparse Attention (DSA) — local single-user full kernel.
 *
 * DeepSeek-V3.2-Exp:
 *   lightning indexer (multi-head ReLU scores) → top-k select →
 *   attend only on support.
 *
 * We solve the prior limits:
 *   1. Real lightning indexer (multi-head ReLU), not raw q·k alone.
 *      Optional external weights; default synthetic multi-head split of
 *      the attention head (no separate trained tensors required).
 *   2. Selection uses index scores; softmax weights use TRUE q·k on the
 *      support (DeepSeek-style: index decides WHO, affinity decides HOW).
 *   3. Quality-safe defaults: sleep dust only; floor optional (speed profile).
 *   4. MLA-lite int8 KV helpers so non-selected rows stay compressed.
 *
 * Datacenter EP / dual-batch disagg is intentionally out of scope.
 */

#include "cce_defs.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Indexer modes for cce_dsa_lightning_index */
typedef enum {
    CCE_DSA_INDEX_QK = 0,           /* plain q·k (baseline) */
    CCE_DSA_INDEX_RELU_MH = 1,      /* DeepSeek-style multi-head ReLU */
    CCE_DSA_INDEX_HYBRID = 2        /* 0.5*qk_scaled + 0.5*relu_mh (default) */
} cce_dsa_index_mode;

typedef struct cce_dsa_config {
    int   top_k;          /* hard support cap; 0 → derive from fraction */
    float fraction;       /* of visible tokens when top_k==0; clamp (0,1] */
    float sleep_eps;      /* post-softmax mass < eps → exact 0 (keep ≥1) */
    float floor_quantum;  /* round positive weights DOWN; 0=off (quality default) */
    int   keep_anchors;   /* 1: force sink + recent rows into support */
    int   enabled;
    int   index_mode;     /* cce_dsa_index_mode */
    int   index_heads;    /* multi-head ReLU groups; 0 → auto (≤8, |hd) */
    float hybrid_alpha;   /* weight on ReLU path in HYBRID; rest on qk (0.5) */
} cce_dsa_config;

/* Quality-first single-user defaults (sleep only; no aggressive floor). */
void cce_dsa_config_default(cce_dsa_config* cfg);
/* Speed profile: enable floor grid for more zeros / skips. */
void cce_dsa_config_speed(cce_dsa_config* cfg);

int cce_dsa_resolve_k(int n, const cce_dsa_config* cfg);

/*
 * Lightning indexer — DeepSeek multi-head ReLU prototype:
 *   I_s = Σ_j w_j * ReLU( q_j · k_s,j )
 * head is split into index_heads groups of size head_dim/heads.
 * w_j: optional head weights (NULL → uniform 1).
 * qk_scale: usually 1/sqrt(d) for the qk term; ReLU path uses raw group dots.
 * scores_out[0..n): index scores (negatives allowed).
 *
 * k_rows: either packed [n * head_dim] contiguous, or caller fills via
 * cce_dsa_lightning_index_ptr (array of pointers).
 */
cce_result cce_dsa_lightning_index(const float* q, int head_dim,
                                   const float* k_packed, int n,
                                   const cce_dsa_config* cfg,
                                   float qk_scale,
                                   const float* head_weights,
                                   float* scores_out);

/* Same as above but K rows are separate pointers (cache layout). */
cce_result cce_dsa_lightning_index_ptr(const float* q, int head_dim,
                                       const float* const* k_ptrs, int n,
                                       const cce_dsa_config* cfg,
                                       float qk_scale,
                                       const float* head_weights,
                                       float* scores_out);

/*
 * Select on index_scores, softmax weights from attn_scores (true q·k).
 * If attn_scores is NULL, index_scores are used for both (legacy).
 */
cce_result cce_dsa_select_and_weights(const float* index_scores, int n,
                                      const cce_dsa_config* cfg,
                                      int* out_idx, float* out_w,
                                      int out_cap, int* out_n);

cce_result cce_dsa_select_dual(const float* index_scores,
                               const float* attn_scores, int n,
                               const cce_dsa_config* cfg,
                               int* out_idx, float* out_w,
                               int out_cap, int* out_n);

int cce_round_down_keep_one(float* w, int n, float quantum, float sleep_eps,
                            int fallback_idx);

cce_result cce_dsa_weights_scatter(const float* index_scores, int n,
                                   const cce_dsa_config* cfg,
                                   float* out_probs,
                                   int* out_idx, int* out_n, int out_cap);

/* ---- MLA-lite: int8 compressed KV for single-user long context ----
 * Store full f32 once; optionally also maintain int8+scale. DSA only
 * dequants selected rows → mem bandwidth win. Identity path uses f32. */

typedef struct cce_mla_kv_row {
    int8_t*  q8;     /* [dim] */
    float    scale;  /* dequant: f = q8 * scale */
    int      dim;
} cce_mla_kv_row;

/* Quantize one row: max-abs scale, round to int8. */
void cce_mla_kv_quantize(const float* src, int dim, int8_t* q8, float* scale);

/* Dequant one row into dst[dim]. */
void cce_mla_kv_dequant(const int8_t* q8, float scale, int dim, float* dst);

/* Dot product q (f32) · dequant(k_q8) without full materialize when possible. */
float cce_mla_kv_dot_q8(const float* q, const int8_t* k_q8, float k_scale,
                        int dim);

#ifdef __cplusplus
}
#endif

#endif /* CCE_DSA_H */
