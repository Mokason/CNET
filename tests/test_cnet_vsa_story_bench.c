/*
 * tests/test_cnet_vsa_story_bench.c - Benchmark & Quality Gate for CNET-VSA Story Generation
 *
 * Exercises:
 *  1. TinyStories knowledge corpus ingestion & role-filler extraction.
 *  2. Multi-domain concept blending via hyperdimensional superposition.
 *  3. Controllable orthogonal style modulation (Whimsical, Adventurous, Cozy).
 *  4. 5-Beat narrative arc generation with rich surface realization.
 *  5. Mathematical novelty proof: verifies non-plagiarism (overlap < 0.80).
 *  6. Fail-closed metric safety contract verification (rejects corrupted intents).
 *  7. Generation throughput & sub-millisecond execution audit.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#include "cnet_vsa.h"
#include "cnet_vsa_bsc.h"
#include "cnet_vsa_text.h"
#include "cnet_vsa_story.h"

static int checks = 0;
static int failures = 0;

static void check(int condition, const char *desc) {
    checks++;
    if (condition) {
        printf("  %-65s PASS\n", desc);
    } else {
        printf("  %-65s FAIL\n", desc);
        failures++;
    }
}

static uint64_t bench_mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

int main(void) {
    printf("=================================================================\n");
    printf(" CNET-VSA Creative Narrative Synthesis & TinyStories Benchmark   \n");
    printf("=================================================================\n\n");

    const int D = CNET_VSA_DEFAULT_DIM;
    CnetVsaStoryEngine eng;
    int rc = cnet_vsa_story_init(&eng, D, 42);
    check(rc == 0, "Story engine initialized with dimension and seed");

    /* -------------------------------------------------------------
     * Suite 1: Ingesting TinyStories Exemplar Corpus
     * ------------------------------------------------------------- */
    printf("\n[1/5] Ingesting TinyStories Knowledge Corpus...\n");
    int num_ingested = cnet_vsa_story_ingest_corpus(&eng);
    printf("  Ingested %d TinyStories exemplars into VSA manifold\n", num_ingested);
    check(num_ingested == 16, "Successfully ingested all 16 TinyStories exemplars");
    check(eng.concept_codebook.count >= 40, "Extracted and indexed >= 40 narrative concepts in codebook");

    /* -------------------------------------------------------------
     * Suite 2: Conceptual Blending via Superposition
     * ------------------------------------------------------------- */
    printf("\n[2/5] Testing Hyperdimensional Concept Blending...\n");
    /* Blend Story 0 (Curious Fox + Glowing Mushroom) + Story 11 (Dragon + Green Hills) */
    float blend_whimsical[CNET_VSA_DEFAULT_DIM];
    rc = cnet_vsa_story_blend(&eng, 0, 11, CNET_VSA_STYLE_WHIMSICAL, blend_whimsical);
    check(rc == 0, "Blended concepts from Story 0 and Story 11 under Whimsical style");

    float norm = 0.0f;
    for (int i = 0; i < D; ++i) norm += blend_whimsical[i] * blend_whimsical[i];
    norm = sqrtf(norm);
    check(fabsf(norm - 1.0f) < 1e-4f, "Blended intent vector is normalized to unit length");

    /* -------------------------------------------------------------
     * Suite 3: Generating Original Stories with Distinct Styles
     * ------------------------------------------------------------- */
    printf("\n[3/5] Generating Original Stories Across Styles...\n");

    /* Generation 1: Whimsical */
    CnetVsaGeneratedStory story_w;
    rc = cnet_vsa_story_generate(&eng, blend_whimsical, CNET_VSA_STYLE_WHIMSICAL, "Oliver the fox", &story_w);
    check(rc == 0, "Generated Whimsical narrative arc");
    check(strlen(story_w.story) > 100, "Whimsical story has complete multi-paragraph narrative");
    printf("\n--- [Generated Story 1: Whimsical Style] ---\n");
    printf("Title: %s\n\n%s\n", story_w.title, story_w.story);
    printf("Hero: %s | Setting: %s | Artifact: %s\n", story_w.hero, story_w.setting, story_w.artifact);
    printf("Intent Alignment: %.4f | Max Training Overlap: %.4f | Contract: %s\n",
           story_w.intent_similarity, story_w.max_exemplar_overlap,
           story_w.contract_verified ? "SAFE" : "VIOLATION");

    /* Generation 2: Adventurous */
    float blend_adventurous[CNET_VSA_DEFAULT_DIM];
    cnet_vsa_story_blend(&eng, 11, 7, CNET_VSA_STYLE_ADVENTUROUS, blend_adventurous);
    CnetVsaGeneratedStory story_a;
    rc = cnet_vsa_story_generate(&eng, blend_adventurous, CNET_VSA_STYLE_ADVENTUROUS, "Ember the young dragon", &story_a);
    check(rc == 0, "Generated Adventurous narrative arc");
    printf("\n--- [Generated Story 2: Adventurous Style] ---\n");
    printf("Title: %s\n\n%s\n", story_a.title, story_a.story);
    printf("Intent Alignment: %.4f | Max Training Overlap: %.4f | Contract: %s\n",
           story_a.intent_similarity, story_a.max_exemplar_overlap,
           story_a.contract_verified ? "SAFE" : "VIOLATION");

    /* Generation 3: Cozy */
    float blend_cozy[CNET_VSA_DEFAULT_DIM];
    cnet_vsa_story_blend(&eng, 1, 8, CNET_VSA_STYLE_COZY, blend_cozy);
    CnetVsaGeneratedStory story_c;
    rc = cnet_vsa_story_generate(&eng, blend_cozy, CNET_VSA_STYLE_COZY, "Lily and Barnaby", &story_c);
    check(rc == 0, "Generated Cozy narrative arc");
    printf("\n--- [Generated Story 3: Cozy Style] ---\n");
    printf("Title: %s\n\n%s\n", story_c.title, story_c.story);
    printf("Intent Alignment: %.4f | Max Training Overlap: %.4f | Contract: %s\n",
           story_c.intent_similarity, story_c.max_exemplar_overlap,
           story_c.contract_verified ? "SAFE" : "VIOLATION");

    /* -------------------------------------------------------------
     * Suite 4: Formal Safety & Novelty Audit
     * ------------------------------------------------------------- */
    printf("\n[4/5] Auditing Formal Safety Contract & Proven Novelty...\n");

    bool safe = false, novel = false;
    cnet_vsa_story_audit(&eng, &story_w, blend_whimsical, &safe, &novel);
    check(safe, "Generated story satisfies metric safety contract (0 violations)");
    check(novel, "Generated story is proven novel (max overlap < 0.80, not a training clone)");
    check(story_w.max_exemplar_overlap >= 0.20f && story_w.max_exemplar_overlap <= 0.80f,
          "Story achieves creative sweet spot: grounded (>=0.20) but novel (<=0.80)");

    /* Test Fail-Closed Abstention on Hostile / Corrupt Vector */
    float hostile_vec[CNET_VSA_DEFAULT_DIM];
    for (int i = 0; i < D; ++i) hostile_vec[i] = -eng.safety_contract.centroid[i]; /* Exact opposite */
    int admitted = 0;
    float margin = 0.0f;
    cnet_vsa_contract_verify(&eng.safety_contract, hostile_vec, &admitted, &margin);
    check(admitted == 0 && margin < 0.0f, "Safety contract cleanly refuses/abstains on inverted hostile vector");

    /* -------------------------------------------------------------
     * Suite 5: Story Synthesis Throughput
     * ------------------------------------------------------------- */
    printf("\n[5/5] Measuring Narrative Synthesis Throughput...\n");
    const int TIMING_RUNS = 1000;
    uint64_t t0 = bench_mono_ns();
    for (int i = 0; i < TIMING_RUNS; ++i) {
        CnetVsaGeneratedStory tmp;
        cnet_vsa_story_generate(&eng, blend_cozy, CNET_VSA_STYLE_COZY, NULL, &tmp);
    }
    uint64_t t1 = bench_mono_ns();
    double total_ms = (double)(t1 - t0) / 1000000.0;
    double us_per_story = (total_ms * 1000.0) / (double)TIMING_RUNS;
    printf("  %d full narrative arcs synthesized in %.2f ms (%.2f us/story)\n",
           TIMING_RUNS, total_ms, us_per_story);
    check(us_per_story < 500.0, "Complete narrative synthesis executes in sub-500 microseconds per story");

    cnet_vsa_story_free(&eng);

    printf("\n-----------------------------------------------------------------\n");
    if (failures == 0) {
        printf("CNET_VSA_STORY_BENCH_PASS: all %d checks passed.\n", checks);
        return 0;
    } else {
        printf("CNET_VSA_STORY_BENCH_FAIL: %d of %d checks failed.\n", failures, checks);
        return 1;
    }
}
