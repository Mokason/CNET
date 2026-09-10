#ifndef CNET_VSA_HYBRID_H
#define CNET_VSA_HYBRID_H

#include "cnet_vsa.h"
#include "cnet_vsa_text.h"
#include "cnet_vsa_story.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char hero[CNET_VSA_NAME_MAX];
    char setting[CNET_VSA_NAME_MAX];
    char artifact[CNET_VSA_NAME_MAX];
    CnetVsaStoryStyle style;
    float certified_intent_vector[CNET_VSA_DEFAULT_DIM];
} CnetVsaKnowledgeFrame;

typedef struct {
    char generated_prose[2048];
    float load_time_ms;
    float gen_time_ms;
    int warm_service_used;
    
    /* VSA Dual-Level Back-Projection Audit Results */
    float surface_frame_vector[CNET_VSA_DEFAULT_DIM];
    float doc_semantic_vector[CNET_VSA_DEFAULT_DIM];
    
    float frame_grounding_sim;       /* Cosine similarity in Role-Bound Manifold space (>= 0.70) */
    float doc_grounding_sim;         /* Cosine similarity in Global Concept Bag space (>= 0.20) */
    float safety_contract_distance;   /* Distance to safe manifold centroid (<= 1.38) */
    
    int hero_detected;               /* 1 if hero was retained in generated text */
    int setting_detected;            /* 1 if setting was retained */
    int artifact_detected;           /* 1 if artifact was retained */
    int hostile_concept_detected;    /* 1 if forbidden hostile intrusion found */
    
    int contract_passed;              /* 1 if all invariants satisfied (CERTIFIED_SAFE) */
    char audit_verdict[128];         /* Human-readable audit status string */
} CnetVsaHybridResult;

/* Formulate and certify a VSA knowledge frame */
int cnet_vsa_hybrid_formulate_frame(CnetVsaStoryEngine *story_eng,
                                    const char *hero,
                                    const char *setting,
                                    const char *artifact,
                                    CnetVsaStoryStyle style,
                                    CnetVsaKnowledgeFrame *out_frame);

/* Run Option 2: Dispatch frame to Neural Mouth (warm service or CLI fallback),
 * receive prose, and execute rigorous fail-closed VSA Back-Projection verification.
 */
int cnet_vsa_hybrid_generate_and_audit(CnetVsaStoryEngine *story_eng,
                                       const CnetVsaKnowledgeFrame *frame,
                                       int max_tokens,
                                       CnetVsaHybridResult *out_result);

#ifdef __cplusplus
}
#endif

#endif /* CNET_VSA_HYBRID_H */
