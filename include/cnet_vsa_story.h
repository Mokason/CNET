/*
 * include/cnet_vsa_story.h - CNET-VSA TinyStories Creative Narrative Synthesis Engine
 *
 * Neuro-symbolic story generation grounded in TinyStories knowledge:
 *  1. Ingests TinyStories narrative exemplars into VSA role-filler manifolds.
 *  2. Decomposes stories into algebraic roles: Hero, Setting, Artifact, Challenge, Resolution.
 *  3. Enables conceptual blending: combining distant story concepts via superposition.
 *  4. Orthogonal Style Modulation: Whimsical, Adventurous, and Cozy tone vectors.
 *  5. 5-Beat Narrative Arc Generation with fail-closed contract verification.
 *  6. Metric novelty audit: mathematically proves non-plagiarism (similarity < 0.75 to training)
 *     while maintaining high semantic alignment (similarity > 0.70 to blended intent).
 */

#ifndef CNET_VSA_STORY_H
#define CNET_VSA_STORY_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "cnet_vsa.h"
#include "cnet_vsa_bsc.h"
#include "cnet_vsa_text.h"
#include "cnet_vsa_memory.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CNET_VSA_MAX_EXEMPLARS 32
#define CNET_VSA_STORY_TITLE_MAX 128
#define CNET_VSA_STORY_TEXT_MAX 4096
#define CNET_VSA_STORY_SLOT_MAX 64

typedef enum {
    CNET_VSA_STYLE_WHIMSICAL = 0,   /* Sparkling, wondrous, magical vocabulary */
    CNET_VSA_STYLE_ADVENTUROUS = 1, /* Bold, daring, dynamic, soaring vocabulary */
    CNET_VSA_STYLE_COZY = 2         /* Warm, gentle, peaceful, comforting vocabulary */
} CnetVsaStoryStyle;

typedef enum {
    CNET_VSA_BEAT_SETUP = 0,        /* Introduce protagonist in setting with companion/artifact */
    CNET_VSA_BEAT_INCITING = 1,     /* Encounter surprise, discovery, or gentle mystery */
    CNET_VSA_BEAT_JOURNEY = 2,      /* Cooperative action, exploration, or perseverance */
    CNET_VSA_BEAT_CLIMAX = 3,       /* Moment of breakthrough, realization, or sharing */
    CNET_VSA_BEAT_RESOLUTION = 4    /* Warm closure with lasting friendship/satisfaction */
} CnetVsaStoryBeat;

typedef struct {
    int id;
    char raw_text[CNET_VSA_STORY_TEXT_MAX];
    char hero[CNET_VSA_STORY_SLOT_MAX];
    char setting[CNET_VSA_STORY_SLOT_MAX];
    char artifact[CNET_VSA_STORY_SLOT_MAX];
    char challenge[CNET_VSA_STORY_SLOT_MAX];
    char resolution[CNET_VSA_STORY_SLOT_MAX];
    float vector[CNET_VSA_DEFAULT_DIM];
} CnetVsaStoryExemplar;

typedef struct {
    int dim;
    size_t exemplar_count;
    CnetVsaStoryExemplar exemplars[CNET_VSA_MAX_EXEMPLARS];
    
    /* Semantic Role Basis Vectors */
    float role_hero[CNET_VSA_DEFAULT_DIM];
    float role_setting[CNET_VSA_DEFAULT_DIM];
    float role_artifact[CNET_VSA_DEFAULT_DIM];
    float role_challenge[CNET_VSA_DEFAULT_DIM];
    float role_resolution[CNET_VSA_DEFAULT_DIM];
    
    /* Orthogonal Style Manifolds */
    float style_whimsical[CNET_VSA_DEFAULT_DIM];
    float style_adventurous[CNET_VSA_DEFAULT_DIM];
    float style_cozy[CNET_VSA_DEFAULT_DIM];
    
    /* Codebook of concepts */
    CnetVsaCodebook concept_codebook;
    
    /* Working memory scratchpad for narrative progression */
    CnetVsaStm narrative_stm;
    
    /* Formal Metric Safety Contract */
    float safety_centroid[CNET_VSA_DEFAULT_DIM];
    CnetVsaMetricContract safety_contract;
} CnetVsaStoryEngine;

typedef struct {
    char title[CNET_VSA_STORY_TITLE_MAX];
    char story[CNET_VSA_STORY_TEXT_MAX];
    char hero[CNET_VSA_STORY_SLOT_MAX];
    char setting[CNET_VSA_STORY_SLOT_MAX];
    char artifact[CNET_VSA_STORY_SLOT_MAX];
    CnetVsaStoryStyle style;
    float intent_similarity;    /* Alignment to blended goal vector (> 0.65) */
    float max_exemplar_overlap; /* Maximum similarity to any single training story (< 0.80) */
    bool contract_verified;     /* Satisfies safety invariants and coherence */
} CnetVsaGeneratedStory;

/* Initialize Story Engine with dimension and seed */
int  cnet_vsa_story_init(CnetVsaStoryEngine *eng, int dim, uint64_t seed);
void cnet_vsa_story_free(CnetVsaStoryEngine *eng);

/* Ingest TinyStories standard corpus */
int  cnet_vsa_story_ingest_corpus(CnetVsaStoryEngine *eng);

/* Ingest an individual exemplar with parsed roles */
int  cnet_vsa_story_add_exemplar(CnetVsaStoryEngine *eng, const char *text,
                                 const char *hero, const char *setting,
                                 const char *artifact, const char *challenge,
                                 const char *resolution);

/* Conceptual Blending: creates an algebraic superposition vector combining roles from 2 or more exemplars */
int  cnet_vsa_story_blend(CnetVsaStoryEngine *eng, int ex_idx_a, int ex_idx_b,
                          CnetVsaStoryStyle style, float *out_blend_vec);

/* Generate a complete original story grounded in blended knowledge and style */
int  cnet_vsa_story_generate(CnetVsaStoryEngine *eng, const float *intent_vec,
                             CnetVsaStoryStyle style, const char *hero_override,
                             CnetVsaGeneratedStory *out_story);

/* Verify narrative contract and novelty */
int  cnet_vsa_story_audit(const CnetVsaStoryEngine *eng, const CnetVsaGeneratedStory *story,
                          const float *intent_vec, bool *out_safe, bool *out_novel);

#ifdef __cplusplus
}
#endif

#endif /* CNET_VSA_STORY_H */
