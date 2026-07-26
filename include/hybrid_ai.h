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
/* Default K: distinct real (in,out) samples retained per port shape.
   Override with CNET_RESIDUAL_RESERVOIR_K (1..1024). */
#define HYBRID_RESERVOIR_K 64
/* Mined units whose certified coverage is tracked for abstention. */
#define HYBRID_COVERAGE_MAX 8

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

/* Trace of residual successes for structure mining.
 *
 * in/out hold the most recent exemplar (kept for callers that read one pair).
 * The reservoir holds up to res_cap DISTINCT real (in,out) pairs actually seen
 * on this port shape, so the miner can train on traffic instead of a synthetic
 * one-hot basis. It is port-family agnostic — raw doubles keyed by port shape —
 * so non-ONEHOT families get real training rows too. Eviction is FIFO, i.e.
 * "the most recent K distinct inputs", which keeps gates deterministic. */
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
    /* Reservoir of real traffic (B2 fix). */
    double *res_in;     /* res_cap * in_dim */
    double *res_out;    /* res_cap * out_dim */
    size_t res_cap;
    size_t res_count;   /* distinct pairs retained */
    size_t res_next;    /* FIFO write cursor once full */
    size_t res_offered; /* total pairs offered (pre-dedup) */
} HybridTrace;

/* Certified coverage of a mined unit: the exact input rows its contract was
 * certified on. A contract certifies over a DOMAIN, so answering outside that
 * domain is an uncertified claim wearing a certified badge. Measured on this
 * substrate, output margin cannot detect it — a mined BTN emits a saturated
 * one-hot and scores margin 1.0 on inputs it has never seen and gets wrong —
 * so coverage has to be membership, not confidence. */
typedef struct {
    Port input_port;
    Port goal_port;
    uint64_t in_key;
    uint64_t goal_key;
    char unit[64];
    double *rows;   /* n_rows * in_dim — the certified input set */
    size_t n_rows;
    size_t in_dim;
    int active;
} HybridCoverage;

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
    size_t reservoir_mines;  /* mines trained on real traffic, not synthetic */
    size_t synthetic_mines;  /* mines that fell back to the one-hot basis */
    HybridCoverage coverage[HYBRID_COVERAGE_MAX];
    size_t coverage_count;
    size_t coverage_abstains; /* Tier-A refusals outside certified coverage */
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

/* Total real (in,out) rows retained across every trace's reservoir. This is
 * the size of the real-traffic training set the miner can draw on. */
CNET_API size_t hybrid_reservoir_rows(const HybridAi *h);

/* Reservoir rows retained for one port shape (0 if no matching trace). */
CNET_API size_t hybrid_reservoir_rows_for(const HybridAi *h, Port in_port,
                                          Port out_port);

/* ---- S6 coverage-gated abstention ---------------------------------------
 * Record the input set a mined unit was certified on. Called by the miner on
 * successful admit; replaces any prior record for the same port shape. */
CNET_API int hybrid_coverage_record(HybridAi *h, Port in_port, Port out_port,
                                    const char *unit, const double *inputs,
                                    size_t n_rows, size_t in_dim);

/* May a certified (Tier A) answer be claimed for this input?
 *   1 = yes — no coverage record for this shape, or the input is inside it
 *   0 = no  — a record exists and the input is outside the certified domain
 * Default-allow keeps units certified over their whole domain (hand-admitted,
 * or mined from a full basis) untouched. Exact membership is only meaningful
 * for discrete port families; PORT_RAW is always allowed (see plan risks). */
CNET_API int hybrid_coverage_admits(const HybridAi *h, Port in_port,
                                    Port out_port, const double *in,
                                    size_t in_len);

/* Rows recorded for a port shape (0 if none). */
CNET_API size_t hybrid_coverage_rows(const HybridAi *h, Port in_port,
                                     Port out_port);

/* S7 durability: coverage lives beside the base as <base>.coverage, because an
 * in-memory-only gate lapses on the one process that matters — the long-running
 * lane. Saved after every successful mine, reloaded on open. Text with %.17g so
 * doubles round-trip bit-exactly (membership is an exact match). Returns 0 on
 * success; load returns 0 when the file is simply absent (nothing mined yet). */
CNET_API int hybrid_coverage_save(const HybridAi *h, const char *path);
CNET_API int hybrid_coverage_load(HybridAi *h, const char *path);

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
