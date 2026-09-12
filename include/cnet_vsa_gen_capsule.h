#ifndef CNET_VSA_GEN_CAPSULE_H
#define CNET_VSA_GEN_CAPSULE_H

#include "cnet_vsa.h"
#include "cnet_vsa_bsc.h"
#include "cnet_vsa_text.h"
#include "cnet_vsa_ngram.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CNET_VSA_GENCAP_MAGIC       0x47434150 /* 'GCAP' */
#define CNET_VSA_GENCAP_VERSION     3   /* current: receipt + wide binary topical block */
#define CNET_VSA_GENCAP_VERSION_V2  2   /* receipt only; routes in the 512-d float space */
#define CNET_VSA_GENCAP_VERSION_V1  1   /* legacy: fixed 0.90 radius, no receipt */
#define CNET_VSA_GENCAP_NAME_MAX    64
#define CNET_VSA_GENCAP_MAX_FRAMES  16
#define CNET_VSA_GENCAP_SLOTS_MAX   8

typedef struct {
    char name[32];
    float role_vector[CNET_VSA_DEFAULT_DIM];  /* TPR Role binding vector */
    char exemplar_words[4][32];               /* Frequent fillers */
    int exemplar_count;
} CnetVsaGrammarSlot;

typedef struct {
    char frame_name[32];
    float trigger_intent[CNET_VSA_DEFAULT_DIM];
    CnetVsaGrammarSlot slots[CNET_VSA_GENCAP_SLOTS_MAX];
    int slot_count;
} CnetVsaGrammarFrame;

/* Calibration receipt: how the capsule's safe_radius was chosen.
 * Written by cnet_vsa_gencap_calibrate, covered by the digest, stored at the
 * end of the capsule so version-1 files remain loadable as a prefix. */
typedef struct {
    uint32_t calibrated;        /* 1 if safe_radius came from held-out evidence */
    uint32_t in_domain_count;   /* leave-one-out corpus sentences + explicit probes measured */
    uint32_t negative_count;    /* cross-domain negatives measured */
    uint32_t reserved0;         /* encoder id: CNET_VSA_ENCODER_BAG (0) or CNET_VSA_ENCODER_HD (1) */
    float target_in_accept;     /* requested fraction of in-domain probes inside radius */
    float target_neg_reject;    /* requested fraction of negatives outside radius */
    float in_accept_rate;       /* measured at the chosen radius */
    float neg_reject_rate;      /* measured at the chosen radius */
    float radius_in;            /* tightest radius meeting the in-domain target */
    float radius_neg;           /* loosest radius meeting the negative target */
    float separation;           /* radius_neg - radius_in; < 0 means not separable */
    float in_dist_mean;
    float in_dist_std;
    float neg_dist_mean;
    float neg_dist_std;
    float reserved1;
} CnetVsaGencapCalibration;

/* Version 3: wide int8 topical centroid for routing and scope, with its own
 * calibration receipt. 2048 x int8 = 2 KB, the same footprint as the float-512
 * centroid, with the noise floor of a 2048-d space and the magnitudes that
 * separate sibling topics (a sign-bit block was measured and rejected: 2 points
 * worse top-1 and a third smaller runner-up margin). Stored after the receipt
 * so v1/v2 files stay loadable as prefixes. */
typedef struct {
    uint32_t present;            /* 1 when q8[] is valid */
    uint32_t width;              /* CNET_VSA_TOPICAL_DIM at seal time */
    uint32_t calibrated;         /* radius chosen from held-out evidence in this space */
    uint32_t kind;               /* CNET_VSA_TOPICAL_KIND_Q8 */
    float    safe_radius;        /* max accepted (1 - cosine) in this space */
    float    in_accept_rate;
    float    neg_reject_rate;
    float    radius_in;
    float    radius_neg;
    float    separation;
    float    in_dist_mean;
    float    in_dist_std;
    float    neg_dist_mean;
    float    neg_dist_std;
    float    q8_norm;            /* L2 norm of q8[], precomputed for routing */
    float    reserved1;
    int8_t   q8[CNET_VSA_TOPICAL_DIM];
} CnetVsaTopicalBlock;

typedef struct {
    /* Header & Identity */
    uint32_t magic;
    uint32_t version;
    char name[CNET_VSA_GENCAP_NAME_MAX];
    char domain[CNET_VSA_GENCAP_NAME_MAX];
    uint64_t created_tick;
    uint64_t digest;                          /* FNV-1a checksum of codebook & weights */
    int certified;                            /* 1 if sealed and verified, 0 otherwise */

    /* Metric Manifold & Coverage Gate */
    float centroid[CNET_VSA_DEFAULT_DIM];     /* Mean semantic centroid */
    float safe_radius;                        /* Maximum allowable cosine distance for valid domain */

    /* Embedded Positional N-Gram Cognitive Core */
    CnetVsaNgramEngine ngram;

    /* Structural Grammar Frames */
    CnetVsaGrammarFrame frames[CNET_VSA_GENCAP_MAX_FRAMES];
    size_t frame_count;

    /* Version 2: calibration receipt */
    CnetVsaGencapCalibration calib;

    /* Version 3: wide int8 topical block (last persisted member) */
    CnetVsaTopicalBlock topical;

    /* Build-time only, never written: raw sum of wide sentence vectors so that
     * calibration can measure each sentence leave-one-out in the wide space,
     * and the number of sentences in it. A loaded capsule has neither, so it
     * can be routed and generated from but never resealed (rebuild from the
     * retained corpus instead). */
    float wide_sum[CNET_VSA_TOPICAL_DIM];
    uint32_t wide_count;
    uint64_t wide_hash;   /* order-free multiset hash of the ingested sentences */
} CnetVsaGenCapsule;

/* Persisted byte lengths per format version (each is a prefix of the next). */
#define CNET_VSA_GENCAP_V1_SIZE   offsetof(CnetVsaGenCapsule, calib)
#define CNET_VSA_GENCAP_V2_SIZE   offsetof(CnetVsaGenCapsule, topical)
#define CNET_VSA_GENCAP_V3_SIZE   offsetof(CnetVsaGenCapsule, wide_sum)
#define CNET_VSA_GENCAP_FILE_SIZE CNET_VSA_GENCAP_V3_SIZE

/* Calibration / seal status codes */
#define CNET_VSA_GENCAP_NOT_SEPARABLE          (-3)
#define CNET_VSA_GENCAP_INSUFFICIENT_EVIDENCE  (-6)
#define CNET_VSA_GENCAP_MIN_IN_DOMAIN  8
#define CNET_VSA_GENCAP_MIN_NEGATIVES  8

/* Initialize empty generative capsule */
int cnet_vsa_gencap_init(CnetVsaGenCapsule *cap, const char *name, const char *domain, int dim);

/* Ingest text into capsule vocabulary, transitions, and manifold. Refused
 * (-1) once the capsule is certified or once calibration has been attempted:
 * a receipt describes exactly the sentences that were in the centroid. */
int cnet_vsa_gencap_ingest(CnetVsaGenCapsule *cap, const char *text);

/* Encode query text into a topical intent vector for steering and domain gating.
 * Always uses CNET_VSA_ENCODER_BAG so existing v1 centroids stay comparable. */
int cnet_vsa_gencap_encode_intent(const char *text, float *out_vec, int dim);

/* Same, with an explicit encoder id (BAG or HD). */
int cnet_vsa_gencap_encode_intent_ex(const char *text, float *out_vec, int dim, uint32_t encoder_id);

/* Encoder used for this capsule's centroid. 0 (BAG) on legacy files. */
uint32_t cnet_vsa_gencap_encoder_id(const CnetVsaGenCapsule *cap);

/* Set encoder before ingest. Refused on sealed capsules or after any ingest. */
int cnet_vsa_gencap_set_encoder(CnetVsaGenCapsule *cap, uint32_t encoder_id);

/* Register a structural grammar frame into the capsule */
int cnet_vsa_gencap_add_frame(CnetVsaGenCapsule *cap, const char *frame_name,
                             const float *trigger_intent, int slot_count,
                             const char slot_names[][32]);

/* Calibrate safe_radius from held-out evidence. Call after ingest, before seal.
 *   in_sentences  : the sentences already ingested (measured leave-one-out
 *                   against the centroid, so each is held out of its own test)
 *   probes        : optional extra in-domain queries never ingested
 *   negatives     : sentences from other domains
 * Chooses the radius midway between the tightest radius that admits
 * target_in_accept of the in-domain set and the loosest radius that rejects
 * target_neg_reject of the negatives. Fills cap->calib with the receipt.
 * Returns 0 on success, CNET_VSA_GENCAP_NOT_SEPARABLE when no radius meets
 * both targets (receipt still filled, calibrated=0),
 * CNET_VSA_GENCAP_INSUFFICIENT_EVIDENCE below the minimum counts, -1 on bad args. */
int cnet_vsa_gencap_calibrate(CnetVsaGenCapsule *cap,
                              const char *const *in_sentences, size_t in_count,
                              const char *const *probes, size_t probe_count,
                              const char *const *negatives, size_t neg_count,
                              float target_in_accept, float target_neg_reject);

/* Same as cnet_vsa_gencap_calibrate but also returns the measured float-space
 * distances (sorted ascending) so callers can evaluate other targets offline.
 * Buffers may be NULL; *out_in_n / *out_neg_n receive the counts measured. */
int cnet_vsa_gencap_calibrate_ex(CnetVsaGenCapsule *cap,
                                 const char *const *in_sentences, size_t in_count,
                                 const char *const *probes, size_t probe_count,
                                 const char *const *negatives, size_t neg_count,
                                 float target_in_accept, float target_neg_reject,
                                 float *out_d_in, size_t in_cap, size_t *out_in_n,
                                 float *out_d_neg, size_t neg_cap, size_t *out_neg_n);

/* Measured, sorted distances of one space, filled by calibrate_dual. */
typedef struct {
    float *d_in;  size_t in_cap;  size_t in_n;
    float *d_neg; size_t neg_cap; size_t neg_n;
} CnetVsaCalibDistances;

/* Calibrates BOTH spaces. The wide binary space decides success (it is what a
 * v3 registry routes on); the float-512 radius is recorded too, on the
 * fail-closed side when float is not separable, so mixed registries keep a
 * usable float gate. Either distances pointer may be NULL. */
int cnet_vsa_gencap_calibrate_dual(CnetVsaGenCapsule *cap,
                                   const char *const *in_sentences, size_t in_count,
                                   const char *const *probes, size_t probe_count,
                                   const char *const *negatives, size_t neg_count,
                                   float target_in_accept, float target_neg_reject,
                                   CnetVsaCalibDistances *float_space,
                                   CnetVsaCalibDistances *binary_space);

/* Wide int8 query vector (signed word counts) for a text under an encoder. */
int cnet_vsa_gencap_encode_intent_q8(const char *text, int8_t *out_q8, uint32_t encoder_id);

/* Wide-space scope gate (1 = in domain, 0 = refuse). Requires topical.present. */
int cnet_vsa_gencap_verify_scope_q8(const CnetVsaGenCapsule *cap, const int8_t *query_q8,
                                    float *out_dist);

/* Seal and certify capsule: normalizes centroid, fixes radius, builds the wide
 * block from the build-time accumulator, computes digest. Refuses an already
 * certified (loaded) capsule (-1), an empty one (-2), one whose calibration
 * was attempted and found not separable (CNET_VSA_GENCAP_NOT_SEPARABLE), and
 * one with no wide evidence (-7). An uncalibrated capsule seals with the
 * legacy 0.90 radius and calib.calibrated == 0. Writes version 3. */
int cnet_vsa_gencap_seal(CnetVsaGenCapsule *cap);

/* Test-only: seal as a version-1 capsule (legacy digest, no receipt). */
int cnet_vsa_gencap_seal_legacy_v1(CnetVsaGenCapsule *cap);
/* Test-only: seal as a version-2 capsule (receipt, no topical block). */
int cnet_vsa_gencap_seal_legacy_v2(CnetVsaGenCapsule *cap);

/* Export capsule to portable certified binary file (.gencap) */
int cnet_vsa_gencap_save(const CnetVsaGenCapsule *cap, const char *filepath);

/* Import and verify capsule from binary file (fails closed if tampered or corrupt) */
int cnet_vsa_gencap_load(CnetVsaGenCapsule *cap, const char *filepath);

/* Coverage gate: checks if query is within certified domain radius (1=valid, 0=OOD refuse) */
int cnet_vsa_gencap_verify_scope(const CnetVsaGenCapsule *cap, const float *query_vec, float *out_dist);

/* Autonomous generation from capsule:
 * Unbinds sequence word-by-word steered by intent vector, respecting grammar frames.
 * Operates purely algebraically in C/HIP with ZERO external LLM/neural dependencies. */
int cnet_vsa_gencap_generate(const CnetVsaGenCapsule *cap,
                             const char *seed_word,
                             const float *intent_vec,
                             float steer_weight,
                             int max_tokens,
                             char *out_text,
                             size_t out_text_size,
                             int *out_tokens);

/* Same, with the scope gate taken in the wide int8 space when intent_q8 is
 * given and the capsule carries a topical block; intent_vec then only steers.
 * Falls back to the float gate otherwise. */
int cnet_vsa_gencap_generate_ex(const CnetVsaGenCapsule *cap,
                                const char *seed_word,
                                const float *intent_vec,
                                const int8_t *intent_q8,
                                float steer_weight,
                                int max_tokens,
                                char *out_text,
                                size_t out_text_size,
                                int *out_tokens);

/* -------------------------------------------------------------
 * Multi-Capsule Registry & Intent Router
 * ------------------------------------------------------------- */
#define CNET_VSA_REGISTRY_MAX_CAPSULES 4096

typedef struct {
    uint32_t magic;
    uint32_t version;
    char name[CNET_VSA_GENCAP_NAME_MAX];
    char domain[CNET_VSA_GENCAP_NAME_MAX];
    uint64_t created_tick;
    uint64_t digest;
    int certified;
    float centroid[CNET_VSA_DEFAULT_DIM];
    float safe_radius;
} CnetVsaCapsuleHeader;

typedef struct {
    CnetVsaCapsuleHeader header;
    char filepath[256];
    uint32_t encoder_id;      /* in-memory; on disk this is calib.reserved0 */
    uint32_t has_topical;     /* 1 when topical[] / topical_radius are valid (v3) */
    float    topical_radius;  /* calibrated wide-space radius */
    float    topical_norm;    /* L2 norm of topical[] */
    int8_t   topical[CNET_VSA_TOPICAL_DIM];
} CnetVsaRegisteredCap;

typedef struct {
    int dim;
    size_t count;
    /* Margin gate parameters (in-memory only). A winner must exceed the mean
     * similarity of the other capsules by z_min = sqrt(2 ln N) + z_margin
     * standard deviations, N being the number of other certified capsules.
     * sqrt(2 ln N) is the expected maximum of N null draws, so the gate scales
     * with registry size instead of relying on a fixed radius. Below
     * min_null_count other capsules the margin gate is skipped. */
    float z_margin;
    size_t min_null_count;
    /* Ambiguity gate: refuse when the winner beats the runner-up by less than
     * k standard deviations of the null; 0 disables. Sibling topics are the
     * dominant wrong-accept, and no per-capsule radius can see them. The wide
     * space accepts far more than float, so it needs the gate (measured); the
     * legacy float space keeps its prior behaviour unless set explicitly. */
    float ambiguity_k_wide;
    float ambiguity_k_float;
    /* 1 (default): the term-dependence gate runs on wide-space prompt routing */
    int term_gate;
    /* 1 forces the float-512 space even when every entry has a topical block
     * (A/B measurement only). */
    int force_float;
    CnetVsaRegisteredCap capsules[CNET_VSA_REGISTRY_MAX_CAPSULES];
} CnetVsaGenRegistry;

#define CNET_VSA_ROUTE_Z_MARGIN_DEFAULT   1.0f
#define CNET_VSA_ROUTE_MIN_NULL_DEFAULT   8
/* Held-out A/B on 101 v3 capsules, 202 queries, score = correct - 2 * wrong:
 * wide k = 0 -> -36, k = 1 -> -2, k = 2 -> +19 (wrong 26% -> 14% -> 5%);
 * float k = 0 -> -6, k = 1 -> +10. On the legacy 824-capsule float registry
 * k >= 1 also refuses keyword queries whose runner-up merely shares one word
 * (wavefront -> wavefront_sensing), so float stays off by default.
 * result/cnet_vsa_v3_topical_block_20260911.md. Policy knobs, not floors. */
#define CNET_VSA_ROUTE_AMBIGUITY_K_WIDE_DEFAULT  2.0f
#define CNET_VSA_ROUTE_AMBIGUITY_K_FLOAT_DEFAULT 0.0f

/* Route decision status */
#define CNET_VSA_ROUTE_ACCEPT          0
#define CNET_VSA_ROUTE_REFUSE_RADIUS   1
#define CNET_VSA_ROUTE_REFUSE_MARGIN   2
#define CNET_VSA_ROUTE_REFUSE_AMBIGUOUS 3
#define CNET_VSA_ROUTE_REFUSE_TERM      4   /* the accept depended on a single content word */
#define CNET_VSA_ROUTE_NONE           -1

#define CNET_VSA_ROUTE_SPACE_FLOAT   0
#define CNET_VSA_ROUTE_SPACE_WIDE    1   /* int8-2048 topical blocks */
#define CNET_VSA_ROUTE_SPACE_BINARY  CNET_VSA_ROUTE_SPACE_WIDE

typedef struct {
    int   space;         /* CNET_VSA_ROUTE_SPACE_* the decision was taken in */
    int   status;        /* CNET_VSA_ROUTE_* */
    int   best_idx;      /* closest certified capsule, or -1 */
    float best_sim;
    float best_dist;     /* 1 - best_sim */
    float radius;        /* winner's safe_radius */
    int   second_idx;    /* runner-up, or -1 */
    float second_sim;
    float gap;           /* best_sim - second_sim */
    float null_mean;     /* mean similarity over the other certified capsules */
    float null_std;
    float z;             /* (best_sim - null_mean) / null_std */
    float z_min;         /* required z */
    int   radius_ok;
    int   margin_ok;     /* 1 if z >= z_min or margin gate skipped */
    int   margin_checked;/* 0 when fewer than min_null_count others */
    int   ambiguous;     /* 1 if gap < null_std (two capsules nearly tied) */
    int   ambiguity_ok;  /* 1 if gap >= ambiguity_k * null_std or the gate is off */
    /* term-dependence gate (wide space, prompt routing): an accept must survive
     * the removal of any one content word, and a prompt with fewer than two
     * content words is never accepted. A single word such as "dress" can sit
     * inside a capsule's radius by itself (stem collision with "dressing"). */
    int   term_checked;  /* 1 when the gate ran */
    int   term_ok;       /* 1 if every leave-one-word-out query stays inside the radius */
    int   term_words;    /* content words in the prompt */
    float term_worst_dist; /* largest leave-one-word-out distance to the winner */
    char  term_word[CNET_VSA_TOKEN_LEN]; /* the word whose removal broke the accept ("" if none) */
} CnetVsaRouteResult;

/* Initialize empty capsule registry */
int cnet_vsa_registry_init(CnetVsaGenRegistry *reg, int dim);

/* Register a capsule file. The whole persisted capsule is read and its digest
 * verified first (same rules as cnet_vsa_gencap_load); a corrupt file is
 * refused and never influences routing statistics. Costs one full file read
 * per capsule: load a registry once per process, not per query. */
int cnet_vsa_registry_add_capsule(CnetVsaGenRegistry *reg, const char *filepath);

/* Auto-discover and register all .gencap files in a directory */
int cnet_vsa_registry_load_dir(CnetVsaGenRegistry *reg, const char *dir_path);

/* Register a capsule from an already-read header (filepath may be NULL; such
 * an entry can be routed to but not dispatched). No encoder id or topical
 * block: the entry routes as BAG in the float space. */
int cnet_vsa_registry_add_header(CnetVsaGenRegistry *reg, const CnetVsaCapsuleHeader *header,
                                 const char *filepath);

/* Register a sealed in-memory capsule with its encoder id and topical block. */
int cnet_vsa_registry_add_capsule_mem(CnetVsaGenRegistry *reg, const CnetVsaGenCapsule *cap,
                                      const char *filepath);

/* 1 when every certified entry carries a topical block (and force_float is
 * off), i.e. route_query decides in the wide int8 space; 0 means float-512. */
int cnet_vsa_registry_binary_space(const CnetVsaGenRegistry *reg);

/* Route with a full explanation. Returns the winning index or -1; out is
 * always filled when non-NULL. Acceptance requires BOTH:
 *   radius gate : best_dist <= winner.safe_radius (per-capsule, calibrated)
 *   margin gate : z >= z_min against the other capsules' similarity spread */
int cnet_vsa_registry_route_ex(const CnetVsaGenRegistry *reg, const float *query_vec,
                               CnetVsaRouteResult *out);

/* Encode the prompt with every encoder present in the registry and score each
 * capsule against the matching query vector. Use this for mixed bag/HD
 * registries; route_ex assumes one query vector for every centroid. */
int cnet_vsa_registry_route_query(const CnetVsaGenRegistry *reg, const char *prompt,
                                  CnetVsaRouteResult *out);

/* Route query vector to the best-matching certified capsule:
 * Returns capsule index (0..count-1) if both gates pass,
 * or -1 if no capsule matches (out-of-domain / abstain).
 * out_best_idx/out_best_dist report the closest capsule even on refusal. */
int cnet_vsa_registry_route(const CnetVsaGenRegistry *reg, const float *query_vec,
                            int *out_best_idx, float *out_best_dist);

/* End-to-end autonomous dispatch:
 * Routes query to the winning capsule and synthesizes response.
 * Fails closed if out-of-domain across all capsules. */
int cnet_vsa_registry_dispatch(const CnetVsaGenRegistry *reg,
                               const char *prompt,
                               char *out_text,
                               size_t out_text_size,
                               char *out_capsule_name,
                               float *out_dist);

#ifdef __cplusplus
}
#endif

#endif /* CNET_VSA_GEN_CAPSULE_H */
