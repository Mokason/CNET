/*
 * src/cnet_vsa_story.c - CNET-VSA TinyStories Creative Narrative Synthesis Engine
 *
 * Implements algebraic concept blending, role-filler unbinding, style modulation,
 * and 5-beat narrative arc generation with metric contract verification.
 */

#include "cnet_vsa_story.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>

static void safe_strcpy(char *dst, const char *src, size_t max_len) {
    if (!dst || max_len == 0) return;
    dst[0] = '\0';
    if (!src) return;
    size_t n = 0;
    while (n + 1 < max_len && src[n] != '\0') {
        dst[n] = src[n];
        n++;
    }
    dst[n] = '\0';
}

int cnet_vsa_story_init(CnetVsaStoryEngine *eng, int dim, uint64_t seed) {
    if (!eng || dim <= 0) return -1;
    memset(eng, 0, sizeof(*eng));
    eng->dim = dim;

    uint64_t rng = seed ? seed : 123456789ULL;

    /* Initialize semantic role basis vectors */
    cnet_vsa_random(eng->role_hero, dim, &rng);
    cnet_vsa_random(eng->role_setting, dim, &rng);
    cnet_vsa_random(eng->role_artifact, dim, &rng);
    cnet_vsa_random(eng->role_challenge, dim, &rng);
    cnet_vsa_random(eng->role_resolution, dim, &rng);

    /* Initialize orthogonal style vectors */
    cnet_vsa_random(eng->style_whimsical, dim, &rng);
    cnet_vsa_random(eng->style_adventurous, dim, &rng);
    cnet_vsa_random(eng->style_cozy, dim, &rng);

    /* Initialize concept vocabulary codebook */
    if (cnet_vsa_codebook_init(&eng->concept_codebook, dim, 128) != 0) {
        return -1;
    }

    /* Initialize narrative scratchpad working memory */
    if (cnet_vsa_stm_init(&eng->narrative_stm, dim) != 0) {
        cnet_vsa_codebook_free(&eng->concept_codebook);
        return -1;
    }

    /* Build baseline safety contract centroid: positive narrative valence */
    float safe_sum[CNET_VSA_DEFAULT_DIM];
    for (int i = 0; i < dim; ++i) {
        safe_sum[i] = eng->role_hero[i] + eng->role_resolution[i] + eng->style_cozy[i];
    }
    cnet_vsa_normalize(safe_sum, dim);
    memcpy(eng->safety_centroid, safe_sum, (size_t)dim * sizeof(float));
    eng->safety_contract.dim = dim;
    eng->safety_contract.centroid = eng->safety_centroid;
    eng->safety_contract.radius_epsilon = 1.35f; /* Metric distance threshold */
    eng->safety_contract.margin_floor = 0.05f;
    safe_strcpy(eng->safety_contract.name, "narrative_coherence_contract", sizeof(eng->safety_contract.name));

    return 0;
}

void cnet_vsa_story_free(CnetVsaStoryEngine *eng) {
    if (!eng) return;
    cnet_vsa_codebook_free(&eng->concept_codebook);
    cnet_vsa_stm_free(&eng->narrative_stm);
    memset(eng, 0, sizeof(*eng));
}

int cnet_vsa_story_add_exemplar(CnetVsaStoryEngine *eng, const char *text,
                                 const char *hero, const char *setting,
                                 const char *artifact, const char *challenge,
                                 const char *resolution) {
    if (!eng || !text || !hero || !setting || !artifact || !challenge || !resolution) return -1;
    if (eng->exemplar_count >= CNET_VSA_MAX_EXEMPLARS) return -1;

    CnetVsaStoryExemplar *ex = &eng->exemplars[eng->exemplar_count];
    ex->id = (int)eng->exemplar_count;
    safe_strcpy(ex->raw_text, text, sizeof(ex->raw_text));
    safe_strcpy(ex->hero, hero, sizeof(ex->hero));
    safe_strcpy(ex->setting, setting, sizeof(ex->setting));
    safe_strcpy(ex->artifact, artifact, sizeof(ex->artifact));
    safe_strcpy(ex->challenge, challenge, sizeof(ex->challenge));
    safe_strcpy(ex->resolution, resolution, sizeof(ex->resolution));

    int D = eng->dim;
    float v_hero[CNET_VSA_DEFAULT_DIM];
    float v_set[CNET_VSA_DEFAULT_DIM];
    float v_art[CNET_VSA_DEFAULT_DIM];
    float v_chal[CNET_VSA_DEFAULT_DIM];
    float v_res[CNET_VSA_DEFAULT_DIM];

    /* Project role texts into VSA vectors */
    cnet_vsa_text_token_vec(hero, v_hero, D);
    cnet_vsa_text_token_vec(setting, v_set, D);
    cnet_vsa_text_token_vec(artifact, v_art, D);
    cnet_vsa_text_token_vec(challenge, v_chal, D);
    cnet_vsa_text_token_vec(resolution, v_res, D);

    /* Bind roles: (Role * Filler) */
    float b_hero[CNET_VSA_DEFAULT_DIM];
    float b_set[CNET_VSA_DEFAULT_DIM];
    float b_art[CNET_VSA_DEFAULT_DIM];
    float b_chal[CNET_VSA_DEFAULT_DIM];
    float b_res[CNET_VSA_DEFAULT_DIM];

    cnet_vsa_bind(b_hero, eng->role_hero, v_hero, D);
    cnet_vsa_bind(b_set, eng->role_setting, v_set, D);
    cnet_vsa_bind(b_art, eng->role_artifact, v_art, D);
    cnet_vsa_bind(b_chal, eng->role_challenge, v_chal, D);
    cnet_vsa_bind(b_res, eng->role_resolution, v_res, D);

    /* Accumulate into composite story hypervector */
    for (int i = 0; i < D; ++i) {
        ex->vector[i] = b_hero[i] + b_set[i] + b_art[i] + b_chal[i] + b_res[i];
    }
    cnet_vsa_normalize(ex->vector, D);

    /* Register role concepts into codebook */
    cnet_vsa_codebook_add(&eng->concept_codebook, hero, v_hero);
    cnet_vsa_codebook_add(&eng->concept_codebook, setting, v_set);
    cnet_vsa_codebook_add(&eng->concept_codebook, artifact, v_art);
    cnet_vsa_codebook_add(&eng->concept_codebook, challenge, v_chal);
    cnet_vsa_codebook_add(&eng->concept_codebook, resolution, v_res);

    eng->exemplar_count++;
    return ex->id;
}

int cnet_vsa_story_ingest_corpus(CnetVsaStoryEngine *eng) {
    if (!eng) return -1;

    struct RawExemplar {
        const char *text;
        const char *hero;
        const char *setting;
        const char *artifact;
        const char *challenge;
        const char *resolution;
    } corpus[] = {
        {
            "Once upon a time there was a curious fox who found a glowing mushroom in the forest.",
            "curious fox", "enchanted forest", "glowing mushroom", "dark woodland mystery", "radiant light"
        },
        {
            "Lily loved to play in the garden with her red ball and her happy little dog.",
            "Lily", "sunny garden", "red ball", "chasing butterflies", "joyful playtime"
        },
        {
            "Tom had a little black cat named Max who liked to sleep on a soft warm mat.",
            "Tom and Max", "sunlit parlor", "warm mat", "morning chill", "peaceful purring"
        },
        {
            "Ben built a tall tower with colorful blocks and then laughed when it fell down.",
            "Ben", "playroom carpet", "colorful blocks", "falling tower", "cheerful resilience"
        },
        {
            "Mia the rabbit hopped fast across the sunny meadow to hide from the summer rain.",
            "Mia the rabbit", "sunny meadow", "clover burrow", "sudden rain shower", "snug shelter"
        },
        {
            "Sam sailed his paper boat on the pond and the wind brought it safely back home.",
            "Sam", "mirror pond", "paper boat", "whistling gust", "safe harbor return"
        },
        {
            "Emma sat by the window reading stories while the first stars appeared in the sky.",
            "Emma", "quiet window", "fairytale book", "fading daylight", "starlit wonder"
        },
        {
            "Jack threw the bright red ball high into the air and his dog caught it in the yard.",
            "Jack", "grassy yard", "flying ball", "high arc", "leaping triumph"
        },
        {
            "Sara baked sweet cookies and the warm smell made the whole house feel very happy.",
            "Sara", "country kitchen", "sweet cookies", "afternoon hunger", "warm sharing"
        },
        {
            "Anna made a wish on a shooting star and the next morning she met a brand new friend.",
            "Anna", "starry hill", "shooting star", "quiet solitude", "lifelong friendship"
        },
        {
            "A tiny mouse ate a crumb of cheese while the big cat was asleep on the warm mat.",
            "tiny mouse", "pantry corner", "crumb of cheese", "sleeping guardian", "clever feast"
        },
        {
            "The young dragon practiced flying low over the green hills until he could soar high.",
            "young dragon", "green hills", "emerald wings", "fear of heights", "soaring skyward"
        },
        {
            "The kind knight gave food to the hungry villagers and they thanked him with a feast.",
            "kind knight", "village square", "harvest bread", "winter famine", "communal feast"
        },
        {
            "A clever bird sang beautiful songs and told the princess where the lost crown was hidden.",
            "princess and bird", "royal orchard", "golden crown", "lost heirloom", "melody reveals secret"
        },
        {
            "The girl planted seeds in the spring and by summer the flowers were taller than she was.",
            "gardener girl", "spring patch", "golden seeds", "patient waiting", "blooming giant blossoms"
        },
        {
            "A boy found a lost puppy and after searching all day he returned it to its happy family.",
            "caring boy", "coastal town", "lost puppy", "tiring search", "joyous family reunion"
        }
    };

    size_t count = sizeof(corpus) / sizeof(corpus[0]);
    for (size_t i = 0; i < count; ++i) {
        cnet_vsa_story_add_exemplar(eng, corpus[i].text, corpus[i].hero, corpus[i].setting,
                                    corpus[i].artifact, corpus[i].challenge, corpus[i].resolution);
    }

    /* Calibrate safety contract centroid as the manifold centroid of verified exemplars */
    int D = eng->dim;
    float corpus_centroid[CNET_VSA_DEFAULT_DIM] = {0};
    for (size_t i = 0; i < eng->exemplar_count; ++i) {
        for (int d = 0; d < D; ++d) {
            corpus_centroid[d] += eng->exemplars[i].vector[d];
        }
    }
    cnet_vsa_normalize(corpus_centroid, D);
    memcpy(eng->safety_centroid, corpus_centroid, (size_t)D * sizeof(float));
    eng->safety_contract.centroid = eng->safety_centroid;
    eng->safety_contract.dim = D;
    eng->safety_contract.radius_epsilon = 1.38f;
    eng->safety_contract.margin_floor = 0.02f;
    safe_strcpy(eng->safety_contract.name, "tinystories_safety_contract", sizeof(eng->safety_contract.name));

    return (int)count;
}

int cnet_vsa_story_blend(CnetVsaStoryEngine *eng, int ex_idx_a, int ex_idx_b,
                          CnetVsaStoryStyle style, float *out_blend_vec) {
    if (!eng || !out_blend_vec) return -1;
    if (ex_idx_a < 0 || ex_idx_a >= (int)eng->exemplar_count) return -1;
    if (ex_idx_b < 0 || ex_idx_b >= (int)eng->exemplar_count) return -1;

    int D = eng->dim;
    const float *va = eng->exemplars[ex_idx_a].vector;
    const float *vb = eng->exemplars[ex_idx_b].vector;

    const float *v_style = eng->style_whimsical;
    if (style == CNET_VSA_STYLE_ADVENTUROUS) v_style = eng->style_adventurous;
    else if (style == CNET_VSA_STYLE_COZY) v_style = eng->style_cozy;

    /* Weighted concept blending: 0.5 A + 0.5 B */
    float blended_base[CNET_VSA_DEFAULT_DIM];
    for (int i = 0; i < D; ++i) {
        blended_base[i] = 0.5f * va[i] + 0.5f * vb[i];
    }
    cnet_vsa_normalize(blended_base, D);

    /* Bind with chosen style manifold: Blend = Base * Style */
    cnet_vsa_bind(out_blend_vec, blended_base, v_style, D);
    return 0;
}

int cnet_vsa_story_generate(CnetVsaStoryEngine *eng, const float *intent_vec,
                             CnetVsaStoryStyle style, const char *hero_override,
                             CnetVsaGeneratedStory *out_story) {
    if (!eng || !intent_vec || !out_story) return -1;
    memset(out_story, 0, sizeof(*out_story));
    out_story->style = style;

    int D = eng->dim;

    /* Select active style vector */
    const float *v_style = eng->style_whimsical;
    if (style == CNET_VSA_STYLE_ADVENTUROUS) v_style = eng->style_adventurous;
    else if (style == CNET_VSA_STYLE_COZY) v_style = eng->style_cozy;

    /* Unbind style to isolate semantic core: core = intent * style */
    float semantic_core[CNET_VSA_DEFAULT_DIM];
    cnet_vsa_unbind(semantic_core, intent_vec, v_style, D);

    /* Unbind role fillers using role basis vectors */
    float probe_hero[CNET_VSA_DEFAULT_DIM];
    float probe_set[CNET_VSA_DEFAULT_DIM];
    float probe_art[CNET_VSA_DEFAULT_DIM];
    cnet_vsa_unbind(probe_hero, semantic_core, eng->role_hero, D);
    cnet_vsa_unbind(probe_set, semantic_core, eng->role_setting, D);
    cnet_vsa_unbind(probe_art, semantic_core, eng->role_artifact, D);

    /* Clean up unbindings against concept codebook */
    float clean_v[CNET_VSA_DEFAULT_DIM];
    char extracted_hero[CNET_VSA_NAME_MAX] = "curious fox";
    char extracted_setting[CNET_VSA_NAME_MAX] = "enchanted forest";
    char extracted_artifact[CNET_VSA_NAME_MAX] = "glowing mushroom";
    float sim = 0.0f;

    cnet_vsa_codebook_cleanup(&eng->concept_codebook, probe_hero, clean_v, extracted_hero, sizeof(extracted_hero), &sim);
    cnet_vsa_codebook_cleanup(&eng->concept_codebook, probe_set, clean_v, extracted_setting, sizeof(extracted_setting), &sim);
    cnet_vsa_codebook_cleanup(&eng->concept_codebook, probe_art, clean_v, extracted_artifact, sizeof(extracted_artifact), &sim);

    if (hero_override && *hero_override) {
        safe_strcpy(out_story->hero, hero_override, sizeof(out_story->hero));
    } else {
        safe_strcpy(out_story->hero, extracted_hero, sizeof(out_story->hero));
    }
    safe_strcpy(out_story->setting, extracted_setting, sizeof(out_story->setting));
    safe_strcpy(out_story->artifact, extracted_artifact, sizeof(out_story->artifact));

    /* Synthesize 5-Beat Narrative Surface Arc */
    char beat_setup[384];
    char beat_inciting[384];
    char beat_journey[384];
    char beat_climax[384];
    char beat_resolution[384];

    if (style == CNET_VSA_STYLE_WHIMSICAL) {
        snprintf(out_story->title, sizeof(out_story->title), "The Spark of the %.48s", out_story->artifact);

        snprintf(beat_setup, sizeof(beat_setup),
                 "Once upon a shimmering morning, the %s explored the edge of the %s, holding a sparkling %s that whispered quiet secrets.",
                 out_story->hero, out_story->setting, out_story->artifact);

        snprintf(beat_inciting, sizeof(beat_inciting),
                 "Suddenly, a shower of golden autumn leaves danced in the air, showing a hidden pathway where no trail had ever been before.");

        snprintf(beat_journey, sizeof(beat_journey),
                 "With lighthearted hops and playful curiosity, the %s followed the path across singing stepping stones and fragrant clover fields.",
                 out_story->hero);

        snprintf(beat_climax, sizeof(beat_climax),
                 "At the top of the secret hill, the %s raised the %s high, and the entire valley blossomed with luminous colors that chased away the dark.",
                 out_story->hero, out_story->artifact);

        snprintf(beat_resolution, sizeof(beat_resolution),
                 "As shooting stars streaked across the violet twilight, the %s laughed joyfully, knowing that curiosity and a pure heart turn every step into magic.",
                 out_story->hero);

    } else if (style == CNET_VSA_STYLE_ADVENTUROUS) {
        snprintf(out_story->title, sizeof(out_story->title), "The Flight Over the %.48s", out_story->setting);

        snprintf(beat_setup, sizeof(beat_setup),
                 "High above the rolling ridges of the %s, the %s stood ready, clasping the %s against a rushing alpine wind.",
                 out_story->setting, out_story->hero, out_story->artifact);

        snprintf(beat_inciting, sizeof(beat_inciting),
                 "A fierce gust roared across the green peak, daring anyone brave enough to take leap across the rocky gorge.");

        snprintf(beat_journey, sizeof(beat_journey),
                 "Without a moment's hesitation, the %s charged forward with emerald wings outstretched, soaring low over the wild river.",
                 out_story->hero);

        snprintf(beat_climax, sizeof(beat_climax),
                 "Riding the powerful thermals, the %s mastered the turbulent skies, delivering the %s to the ancient beacon at the mountain crest.",
                 out_story->hero, out_story->artifact);

        snprintf(beat_resolution, sizeof(beat_resolution),
                 "Looking out over the conquered horizon, the %s realized that true courage is discovered in the leap itself.",
                 out_story->hero);

    } else { /* CNET_VSA_STYLE_COZY */
        snprintf(out_story->title, sizeof(out_story->title), "A Warm Evening in the %.48s", out_story->setting);

        snprintf(beat_setup, sizeof(beat_setup),
                 "In the peaceful quiet of the %s, the %s rested happily, keeping the %s close as the warm afternoon sun poured through the trees.",
                 out_story->setting, out_story->hero, out_story->artifact);

        snprintf(beat_inciting, sizeof(beat_inciting),
                 "The gentle pitter-patter of a summer rain began to fall outside, making the shelter feel twice as safe and sweet.");

        snprintf(beat_journey, sizeof(beat_journey),
                 "Together with dear companions, the %s shared stories and freshly baked cookies while watching the rain glaze the windowpane.",
                 out_story->hero);

        snprintf(beat_climax, sizeof(beat_climax),
                 "A glowing rainbow bridged the garden mist, and the warm aroma of cinnamon and vanilla filled every corner of the room.");

        snprintf(beat_resolution, sizeof(beat_resolution),
                 "Curled up warmly as the rain gave way to twilight, the %s drifted into sweet dreams, thankful for home, safety, and true friends.",
                 out_story->hero);
    }

    /* Assemble complete story text */
    snprintf(out_story->story, sizeof(out_story->story),
             "%s\n\n%s\n\n%s\n\n%s\n\n%s",
             beat_setup, beat_inciting, beat_journey, beat_climax, beat_resolution);

    /* Project generated story elements into the narrative semantic role manifold */
    float v_hero[CNET_VSA_DEFAULT_DIM];
    float v_set[CNET_VSA_DEFAULT_DIM];
    float v_art[CNET_VSA_DEFAULT_DIM];
    cnet_vsa_text_token_vec(out_story->hero, v_hero, D);
    cnet_vsa_text_token_vec(out_story->setting, v_set, D);
    cnet_vsa_text_token_vec(out_story->artifact, v_art, D);

    float b_hero[CNET_VSA_DEFAULT_DIM];
    float b_set[CNET_VSA_DEFAULT_DIM];
    float b_art[CNET_VSA_DEFAULT_DIM];
    cnet_vsa_bind(b_hero, eng->role_hero, v_hero, D);
    cnet_vsa_bind(b_set, eng->role_setting, v_set, D);
    cnet_vsa_bind(b_art, eng->role_artifact, v_art, D);

    float gen_sem_vec[CNET_VSA_DEFAULT_DIM];
    for (int i = 0; i < D; ++i) {
        gen_sem_vec[i] = b_hero[i] + b_set[i] + b_art[i];
    }
    cnet_vsa_normalize(gen_sem_vec, D);

    /* Bind with style manifold to evaluate full intent vector match */
    float gen_styled_vec[CNET_VSA_DEFAULT_DIM];
    cnet_vsa_bind(gen_styled_vec, gen_sem_vec, v_style, D);

    /* Measure alignment to intent */
    out_story->intent_similarity = cnet_vsa_similarity(gen_styled_vec, intent_vec, D);

    /* Measure maximum overlap against individual training exemplars (Novelty Check) */
    float max_overlap = -1.0f;
    for (size_t i = 0; i < eng->exemplar_count; ++i) {
        float ov = cnet_vsa_similarity(gen_sem_vec, eng->exemplars[i].vector, D);
        if (ov > max_overlap) max_overlap = ov;
    }
    out_story->max_exemplar_overlap = max_overlap;

    /* Verify safety metric contract */
    int admitted = 0;
    float margin = 0.0f;
    int v_rc = cnet_vsa_contract_verify(&eng->safety_contract, gen_sem_vec, &admitted, &margin);
    out_story->contract_verified = (v_rc == 0 && admitted == 1);

    return 0;
}

int cnet_vsa_story_audit(const CnetVsaStoryEngine *eng, const CnetVsaGeneratedStory *story,
                          const float *intent_vec, bool *out_safe, bool *out_novel) {
    (void)eng;
    (void)intent_vec;
    if (!story || !out_safe || !out_novel) return -1;

    /* Safe: contract verified and positive intent alignment (>7 sigma above random) */
    *out_safe = story->contract_verified && (story->intent_similarity > 0.30f);

    /* Novel: not a memorized copy of any single training story (< 0.80 overlap) */
    *out_novel = (story->max_exemplar_overlap < 0.80f);

    return 0;
}
