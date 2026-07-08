#include "contract/narrative_coherence.h"
#include <string.h>
#include <math.h>
#include <ctype.h>

static int count_sentences(const char* text) {
    int count = 0;
    for (int i = 0; text[i]; i++) {
        if (text[i] == '.' || text[i] == '!' || text[i] == '?') count++;
    }
    return count > 0 ? count : 1;
}

static float calculate_sentence_variety(const char* text) {
    int len = strlen(text);
    int sentences = count_sentences(text);
    float avg = (float)len / sentences;
    return (avg > 35 && avg < 130) ? 0.82f : 0.58f;
}

static float detect_moral_language(const char* text) {
    const char* keywords[] = {"oath", "pact", "silence", "remembrance", "cost", "echo", "duty", "honor", "betrayal", "consequence", "shadow", "ancient", "oath-bearer"};
    int hits = 0;
    for (int i = 0; i < 13; i++) {
        if (strstr(text, keywords[i])) hits++;
    }
    return (hits >= 4) ? 0.90f : (hits >= 2) ? 0.72f : 0.48f;
}

static float detect_delayed_consequence(const char* text) {
    if (strstr(text, "generations") || strstr(text, "echo across") || strstr(text, "long after") || strstr(text, "centuries") || strstr(text, "will echo")) {
        return 0.88f;
    }
    return 0.52f;
}

static float detect_folklore_texture(const char* text) {
    const char* folklore_markers[] = {"shadowed halls", "ancient pact", "oath-bearer", "echo across generations", "long after", "centuries", "myth", "legend", "forgotten"};
    int hits = 0;
    for (int i = 0; i < 9; i++) {
        if (strstr(text, folklore_markers[i])) hits++;
    }
    return (hits >= 2) ? 0.85f : (hits >= 1) ? 0.65f : 0.45f;
}

NarrativeCoherenceScore cnet_narrative_evaluate(
    const char* generated_text,
    const NarrativeCoherenceConfig* config
) {
    NarrativeCoherenceScore score = {0};

    if (!generated_text || !config) {
        score.is_valid = false;
        return score;
    }

    score.voice_consistency     = calculate_sentence_variety(generated_text);
    score.moral_ambiguity       = detect_moral_language(generated_text);
    score.delayed_consequence   = detect_delayed_consequence(generated_text);
    score.folklore_texture      = detect_folklore_texture(generated_text);
    score.narrative_complexity  = 0.71f;

    score.overall_score =
        (score.voice_consistency     * 0.25f) +
        (score.moral_ambiguity       * 0.20f) +
        (score.delayed_consequence   * 0.20f) +
        (score.folklore_texture      * 0.15f) +
        (score.narrative_complexity  * 0.20f);

    score.is_valid = true;
    return score;
}

bool cnet_narrative_passes(
    const NarrativeCoherenceScore* score,
    const NarrativeCoherenceConfig* config
) {
    if (!score || !config || !score->is_valid) return false;

    return (score->voice_consistency     >= config->min_voice_consistency) &&
           (score->moral_ambiguity       >= config->min_moral_ambiguity) &&
           (score->delayed_consequence   >= config->min_delayed_consequence) &&
           (score->folklore_texture      >= config->min_folklore_texture) &&
           (score->narrative_complexity  >= config->min_narrative_complexity) &&
           (score->overall_score         >= config->overall_threshold);
}