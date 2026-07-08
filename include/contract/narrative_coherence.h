#ifndef NARRATIVE_COHERENCE_H
#define NARRATIVE_COHERENCE_H

#include <stdint.h>
#include <stdbool.h>

/*
 * NarrativeCoherenceContract
 *
 * RPG Storytelling framing:
 *   Place: Heavily post-trained creative models under compression.
 *   Dilemma: Current CNET contracts optimize for structure but lack mechanisms
 *            to protect narrative texture, moral ambiguity, and folklore depth.
 *   Social Consequence: Compressed models lose creative value.
 *
 * Random Variables framing:
 *   Systematic: Voice, moral ambiguity, delayed consequence — measurable.
 *   Irreducible: Some flattening from original post-training (arXiv:2605.27878).
 */

typedef struct {
    float voice_consistency;
    float moral_ambiguity;
    float delayed_consequence;
    float folklore_texture;
    float narrative_complexity;
    float overall_score;
    bool  is_valid;
} NarrativeCoherenceScore;

typedef struct {
    float min_voice_consistency;
    float min_moral_ambiguity;
    float min_delayed_consequence;
    float min_folklore_texture;
    float min_narrative_complexity;
    float overall_threshold;
} NarrativeCoherenceConfig;

NarrativeCoherenceScore cnet_narrative_evaluate(
    const char* generated_text,
    const NarrativeCoherenceConfig* config
);

bool cnet_narrative_passes(
    const NarrativeCoherenceScore* score,
    const NarrativeCoherenceConfig* config
);

/* Default config for Mythos-style models */
static const NarrativeCoherenceConfig NARRATIVE_DEFAULT_CONFIG = {
    .min_voice_consistency     = 0.65f,
    .min_moral_ambiguity       = 0.60f,
    .min_delayed_consequence   = 0.55f,
    .min_folklore_texture      = 0.50f,
    .min_narrative_complexity  = 0.60f,
    .overall_threshold         = 0.62f
};

#endif /* NARRATIVE_COHERENCE_H */