#ifndef CNET_HYBRID_AI_H
#define CNET_HYBRID_AI_H

/* Hybrid A/B/C layer used by personal_ai for universally stronger serve:
 *   A certified plan  → B soft specialist  → C residual generator
 * Soft never outranks certified. Residual answers are always uncertified.
 * Structure mining promotes stable residual successes into Tier A.
 *
 * Gate: make hybrid_ai → HYBRID_AI_PASS
 * Bench: make hybrid_bench → HYBRID_BENCH_PASS
 */

#include <stddef.h>
#include <stdint.h>

#include "acquire.h"
#include "cnet_export.h"
#include "nn.h"
#include "resource_governor.h"
#include "router.h"
#include "self_improve.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HYBRID_SOFT_MAX 16
#define HYBRID_MED_MAX 8
#define HYBRID_TRACE_MAX 64
#define HYBRID_ADAPTER_DIM 64

typedef enum {
    HYBRID_TIER_A = 0, /* certified */
    HYBRID_TIER_B = 1, /* soft */
    HYBRID_TIER_C = 2  /* residual */
} HybridTier;

typedef enum {
    HYBRID_TRUST_CERTIFIED = 0,
    HYBRID_TRUST_PROVISIONAL = 1,
    HYBRID_TRUST_UNCERTIFIED = 2
} HybridTrust;

/* Soft specialist (Tier B): callback + margin abstain. */
typedef struct {
    char name[64];
    Port input_port;
    Port output_port;
    uint64_t in_key;  /* port shape+tag hash — reject before strcmp */
    uint64_t out_key;
    CnetOracleFn fn;
    void *ctx;
    double min_margin; /* abstain if top1-top2 < this (0 = never) */
    int enabled;
} HybridSoftSlot;

/* Medium frozen module (Tier B capacity): residency accounting only + forward. */
typedef struct {
    char name[64];
    Port input_port;
    Port output_port;
    uint64_t in_key;
    uint64_t out_key;
    CnetOracleFn fn;
    void *ctx;
    uint64_t resident_bytes;
    int enabled;
} HybridMediumSlot;

/* Residual generative / open-ended (Tier C). */
typedef struct {
    CnetOracleFn fn;
    void *ctx;
    int bound;
    char name[64];
} HybridResidual;

/* Personal adapter: additive residual bias (simple LoRA stand-in). */
typedef struct {
    double bias[HYBRID_ADAPTER_DIM];
    size_t dim;
    double scale; /* 0 = off */
    int enabled;
} HybridAdapter;

/* Trace of residual successes for structure mining. */
typedef struct {
    Port input_port;
    Port goal_port;
    uint64_t in_key;
    uint64_t goal_key;
    double *in;
    double *out;
    size_t in_dim;
    size_t out_dim;
    size_t hits;
    uint32_t heat;      /* Colibrì-style heat for mine priority */
    uint32_t last_tick; /* recency for LFRU score */
} HybridTrace;

typedef struct {
    HybridSoftSlot soft[HYBRID_SOFT_MAX];
    size_t soft_count;
    HybridMediumSlot medium[HYBRID_MED_MAX];
    size_t medium_count;
    HybridResidual residual;
    HybridAdapter adapter;
    HybridTrace traces[HYBRID_TRACE_MAX];
    size_t trace_count;
    uint32_t heat_clock; /* global tick for LFRU recency */
    /* Counters */
    size_t tier_a_hits;
    size_t tier_b_hits;
    size_t tier_c_hits;
    size_t soft_abstains;
    size_t distills;
    size_t structure_mines;
    size_t adapter_applies;
    size_t prefer_warm_hits; /* served without residual (A/B) */
    size_t batch_label_rows; /* residual labels produced in batch mine */
    uint64_t medium_resident_bytes;
} HybridAi;

CNET_API void hybrid_ai_init(HybridAi *h);
CNET_API void hybrid_ai_free(HybridAi *h);

CNET_API const char *hybrid_tier_name(HybridTier t);
CNET_API const char *hybrid_trust_name(HybridTrust t);

/* ---- P1 Soft specialists ---- */
CNET_API int hybrid_bind_soft(HybridAi *h, const char *name,
                              Port in, Port out, CnetOracleFn fn, void *ctx,
                              double min_margin);

/* Try soft specialists matching ports. Returns 0 answered, 1 abstain-all, -1 none. */
CNET_API int hybrid_try_soft(HybridAi *h, Port in_port, Port out_port,
                             const double *in, size_t in_len,
                             double *out, size_t out_cap,
                             char *name_out, size_t name_cap);

/* ---- P0 Residual ---- */
CNET_API int hybrid_bind_residual(HybridAi *h, const char *name,
                                  CnetOracleFn fn, void *ctx);
CNET_API int hybrid_try_residual(HybridAi *h, Port in_port, Port out_port,
                                 const double *in, size_t in_len,
                                 double *out, size_t out_cap);

/* ---- P4 Personal adapter ---- */
CNET_API int hybrid_adapter_enable(HybridAi *h, const double *bias, size_t dim,
                                   double scale);
CNET_API void hybrid_adapter_apply(HybridAi *h, double *out, size_t out_dim);

/* ---- P3 Medium modules (residency via governor) ---- */
CNET_API int hybrid_bind_medium(HybridAi *h, CnetResourceGovernor *gov,
                                const char *name, Port in, Port out,
                                CnetOracleFn fn, void *ctx,
                                uint64_t resident_bytes);
CNET_API int hybrid_try_medium(HybridAi *h, Port in_port, Port out_port,
                               const double *in, size_t in_len,
                               double *out, size_t out_cap);

/* ---- P2 Distill multi-step plan into registry ---- */
CNET_API int hybrid_distill_plan(HybridAi *h, PrimitiveRegistry *reg,
                                 const RoutePlan *plan,
                                 CnetResourceGovernor *gov,
                                 BinaryTransformNetwork **chunk_out);

/* ---- P5 Structure mining from residual traces ---- */
CNET_API int hybrid_trace_residual(HybridAi *h, Port in_port, Port out_port,
                                   const double *in, size_t in_dim,
                                   const double *out, size_t out_dim);
/* If a signature has >= min_hits, mine+admit a certified unit. */
CNET_API int hybrid_structure_mine(HybridAi *h, PrimitiveRegistry *reg,
                                   size_t min_hits,
                                   BinaryTransformNetwork **student_out);

/* Hermetic residual: maps one-hot input → rotated one-hot (open-ended stand-in). */
CNET_API int hybrid_hermetic_residual(const double *in, double *out, void *ctx);

/* Hermetic soft: same as residual but can abstain on low margin via ctx. */
typedef struct {
    double min_margin;
    int force_abstain;
} HybridSoftCtx;
CNET_API int hybrid_hermetic_soft(const double *in, double *out, void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* CNET_HYBRID_AI_H */
