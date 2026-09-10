#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "../include/cnet_vsa.h"
#include "../include/cnet_vsa_sleep.h"

static int checks = 0;
static int failures = 0;

static void check(int ok, const char *msg) {
    checks++;
    printf("  %-62s %s\n", msg, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

int main(void) {
    printf("=================================================================\n");
    printf(" CNET-VSA Task 3: Sleep Memory Consolidation Benchmark\n");
    printf("=================================================================\n\n");

    const int D = CNET_VSA_DEFAULT_DIM;
    uint64_t rng = 0x9876543210ABCDEFULL;

    CnetVsaSleepConsolidator sc;
    cnet_vsa_sleep_init(&sc, D, 2, 0.65f);

    CnetVsaCodebook ltm;
    cnet_vsa_codebook_init(&ltm, D, 32);

    /* -------------------------------------------------------------
     * Suite 1: Simulating 100 Session Wake Events
     * ------------------------------------------------------------- */
    printf("[1/3] Ingesting 100 Session Events (70 Noise + 30 Verified Invariants)...\n");

    /* Domain basis vectors for persistent knowledge */
    float math_basis[CNET_VSA_DEFAULT_DIM];
    float legal_basis[CNET_VSA_DEFAULT_DIM];
    float code_basis[CNET_VSA_DEFAULT_DIM];
    cnet_vsa_random(math_basis, D, &rng);
    cnet_vsa_random(legal_basis, D, &rng);
    cnet_vsa_random(code_basis, D, &rng);

    /* Ingest 70 transient noise events (unverified, access count 1) */
    for (int i = 0; i < 70; ++i) {
        float noise_v[CNET_VSA_DEFAULT_DIM];
        cnet_vsa_random(noise_v, D, &rng);
        cnet_vsa_sleep_record_event(&sc, "noise", "transient conversational utterance",
                                    noise_v, 1, 0.40f, false);
    }

    /* Ingest 10 persistent Math rules */
    for (int i = 0; i < 10; ++i) {
        float v[CNET_VSA_DEFAULT_DIM], n[CNET_VSA_DEFAULT_DIM];
        cnet_vsa_random(n, D, &rng);
        for (int j = 0; j < D; ++j) v[j] = math_basis[j] + 0.15f * n[j];
        cnet_vsa_normalize(v, D);
        cnet_vsa_sleep_record_event(&sc, "math", "verified arithmetic invariant",
                                    v, 5, 0.99f, true);
    }

    /* Ingest 10 persistent Legal precedents */
    for (int i = 0; i < 10; ++i) {
        float v[CNET_VSA_DEFAULT_DIM], n[CNET_VSA_DEFAULT_DIM];
        cnet_vsa_random(n, D, &rng);
        for (int j = 0; j < D; ++j) v[j] = legal_basis[j] + 0.15f * n[j];
        cnet_vsa_normalize(v, D);
        cnet_vsa_sleep_record_event(&sc, "legal", "verified contractual precedence",
                                    v, 4, 0.98f, true);
    }

    /* Ingest 10 persistent Code contracts */
    for (int i = 0; i < 10; ++i) {
        float v[CNET_VSA_DEFAULT_DIM], n[CNET_VSA_DEFAULT_DIM];
        cnet_vsa_random(n, D, &rng);
        for (int j = 0; j < D; ++j) v[j] = code_basis[j] + 0.15f * n[j];
        cnet_vsa_normalize(v, D);
        cnet_vsa_sleep_record_event(&sc, "code", "verified memory safety contract",
                                    v, 6, 0.99f, true);
    }

    printf("  Total Session Events Recorded: %zu\n", sc.event_count);
    check(sc.event_count == 100, "100 wake session events recorded in buffer");

    /* -------------------------------------------------------------
     * Suite 2: Running Sleep Consolidation Cycle
     * ------------------------------------------------------------- */
    printf("\n[2/3] Executing Sleep Consolidation Cycle...\n");
    size_t pruned = 0, promoted = 0;
    int rc = cnet_vsa_sleep_consolidate(&sc, &ltm, &pruned, &promoted);

    printf("  Transient Noise Pruned:     %zu (Expected 70)\n", pruned);
    printf("  Prototypes Promoted to LTM: %zu (Expected 3)\n", promoted);
    printf("  Remaining Buffer Events:    %zu\n", sc.event_count);

    check(rc == 0, "Sleep consolidation cycle executed without errors");
    check(pruned == 70, "100% of unverified conversational noise successfully pruned");
    check(promoted == 3, "Exactly 3 verified domain clusters promoted to permanent LTM");
    check(sc.event_count == 0, "Wake event buffer compacted and reset to zero");

    /* -------------------------------------------------------------
     * Suite 3: LTM Retrieval of Promoted Knowledge Under Noise
     * ------------------------------------------------------------- */
    printf("\n[3/3] Querying Permanent LTM for Promoted Prototypes...\n");

    /* Probe Math */
    float probe_math[CNET_VSA_DEFAULT_DIM], n_math[CNET_VSA_DEFAULT_DIM];
    cnet_vsa_random(n_math, D, &rng);
    for (int j = 0; j < D; ++j) probe_math[j] = math_basis[j] + 0.20f * n_math[j];
    cnet_vsa_normalize(probe_math, D);

    char match_name[CNET_VSA_NAME_MAX] = {0};
    float match_sim = 0.0f;
    cnet_vsa_codebook_cleanup(&ltm, probe_math, NULL, match_name, sizeof(match_name), &match_sim);
    printf("  Math Probe -> Matched '%s' (similarity: %.4f)\n", match_name, match_sim);
    check(strcmp(match_name, "proto_math") == 0, "LTM query correctly recovers promoted Math prototype");
    check(match_sim > 0.80f, "Math prototype match has strong similarity (> 0.80)");

    /* Probe Legal */
    float probe_legal[CNET_VSA_DEFAULT_DIM], n_legal[CNET_VSA_DEFAULT_DIM];
    cnet_vsa_random(n_legal, D, &rng);
    for (int j = 0; j < D; ++j) probe_legal[j] = legal_basis[j] + 0.20f * n_legal[j];
    cnet_vsa_normalize(probe_legal, D);

    cnet_vsa_codebook_cleanup(&ltm, probe_legal, NULL, match_name, sizeof(match_name), &match_sim);
    printf("  Legal Probe -> Matched '%s' (similarity: %.4f)\n", match_name, match_sim);
    check(strcmp(match_name, "proto_legal") == 0, "LTM query correctly recovers promoted Legal prototype");
    check(match_sim > 0.80f, "Legal prototype match has strong similarity (> 0.80)");

    /* Probe Code */
    float probe_code[CNET_VSA_DEFAULT_DIM], n_code[CNET_VSA_DEFAULT_DIM];
    cnet_vsa_random(n_code, D, &rng);
    for (int j = 0; j < D; ++j) probe_code[j] = code_basis[j] + 0.20f * n_code[j];
    cnet_vsa_normalize(probe_code, D);

    cnet_vsa_codebook_cleanup(&ltm, probe_code, NULL, match_name, sizeof(match_name), &match_sim);
    printf("  Code Probe -> Matched '%s' (similarity: %.4f)\n", match_name, match_sim);
    check(strcmp(match_name, "proto_code") == 0, "LTM query correctly recovers promoted Code prototype");
    check(match_sim > 0.80f, "Code prototype match has strong similarity (> 0.80)");

    cnet_vsa_codebook_free(&ltm);
    cnet_vsa_sleep_free(&sc);

    printf("\n-----------------------------------------------------------------\n");
    if (failures == 0) {
        printf("CNET_VSA_SLEEP_BENCH_PASS: all %d checks passed.\n", checks);
        return 0;
    } else {
        printf("CNET_VSA_SLEEP_BENCH_FAIL: %d of %d checks failed.\n", failures, checks);
        return 1;
    }
}
