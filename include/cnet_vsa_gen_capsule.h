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
#define CNET_VSA_GENCAP_VERSION     1
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
} CnetVsaGenCapsule;

/* Initialize empty generative capsule */
int cnet_vsa_gencap_init(CnetVsaGenCapsule *cap, const char *name, const char *domain, int dim);

/* Ingest text into capsule vocabulary, transitions, and manifold */
int cnet_vsa_gencap_ingest(CnetVsaGenCapsule *cap, const char *text);

/* Encode query text into a topical intent vector for steering and domain gating */
int cnet_vsa_gencap_encode_intent(const char *text, float *out_vec, int dim);

/* Register a structural grammar frame into the capsule */
int cnet_vsa_gencap_add_frame(CnetVsaGenCapsule *cap, const char *frame_name,
                             const float *trigger_intent, int slot_count,
                             const char slot_names[][32]);

/* Seal and certify capsule: computes centroid, coverage radius, and digest */
int cnet_vsa_gencap_seal(CnetVsaGenCapsule *cap);

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
} CnetVsaRegisteredCap;

typedef struct {
    int dim;
    size_t count;
    CnetVsaRegisteredCap capsules[CNET_VSA_REGISTRY_MAX_CAPSULES];
} CnetVsaGenRegistry;

/* Initialize empty capsule registry */
int cnet_vsa_registry_init(CnetVsaGenRegistry *reg, int dim);

/* Register a capsule binary by reading its metadata and centroid header */
int cnet_vsa_registry_add_capsule(CnetVsaGenRegistry *reg, const char *filepath);

/* Auto-discover and register all .gencap files in a directory */
int cnet_vsa_registry_load_dir(CnetVsaGenRegistry *reg, const char *dir_path);

/* Route query vector to the best-matching certified capsule:
 * Returns capsule index (0..count-1) if min_dist <= safe_radius,
 * or -1 if no capsule matches (out-of-domain / abstain). */
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
