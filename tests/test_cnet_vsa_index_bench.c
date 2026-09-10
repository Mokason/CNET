#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#include "../include/cnet_vsa.h"
#include "../include/cnet_vsa_index.h"

static int checks = 0;
static int failures = 0;

static void check(int ok, const char *msg) {
    checks++;
    printf("  %-62s %s\n", msg, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static uint64_t bench_mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

int main(void) {
    printf("=================================================================\n");
    printf(" CNET-VSA Priority 2: Sub-Linear O(log N) Metric Index Stress\n");
    printf("=================================================================\n\n");

    const int D = CNET_VSA_DEFAULT_DIM;
    uint64_t rng = 555666777ULL;

    CnetVsaMetricIndex index;
    cnet_vsa_index_init(&index, D, 64);

    /* -------------------------------------------------------------
     * Suite 1: Population Stress (1,000 Structured Concepts)
     * ------------------------------------------------------------- */
    printf("[1/3] Populating 1,000 Concept Vectors across Clusters...\n");
    const int N_DOMAINS = 20;
    const int PER_DOMAIN = 50;
    const int N_CONCEPTS = N_DOMAINS * PER_DOMAIN; /* 1,000 concepts */
    float domain_centroids[20][CNET_VSA_DEFAULT_DIM];
    for (int d = 0; d < N_DOMAINS; ++d) {
        cnet_vsa_random(domain_centroids[d], D, &rng);
    }

    float *concept_vectors = (float *)malloc((size_t)N_CONCEPTS * (size_t)D * sizeof(float));
    char (*concept_names)[CNET_VSA_NAME_MAX] = malloc((size_t)N_CONCEPTS * sizeof(*concept_names));

    for (int i = 0; i < N_CONCEPTS; ++i) {
        int dom = i / PER_DOMAIN;
        snprintf(concept_names[i], sizeof(concept_names[i]), "dom%02d_capsule_%04d", dom, i);
        float noise[CNET_VSA_DEFAULT_DIM];
        cnet_vsa_random(noise, D, &rng);
        for (int j = 0; j < D; ++j) {
            concept_vectors[i * D + j] = domain_centroids[dom][j] + 0.25f * noise[j];
        }
        cnet_vsa_normalize(&concept_vectors[i * D], D);
        cnet_vsa_index_insert(&index, concept_names[i], &concept_vectors[i * D]);
    }

    printf("  Total items inserted: %zu\n", index.total_items);
    printf("  Clusters formed:      %zu (avg %.1f items/cluster)\n",
           index.cluster_count, (double)index.total_items / (double)index.cluster_count);
    check(index.total_items == (size_t)N_CONCEPTS, "All 1,000 concepts successfully inserted into index");
    check(index.cluster_count > 10, "Hierarchical clustering partitioned concepts into distinct centroid spaces");

    /* -------------------------------------------------------------
     * Suite 2: Sub-Linear Pruning & Query Accuracy under Noise
     * ------------------------------------------------------------- */
    printf("\n[2/3] Querying under Noise & Verifying Triangle Inequality Pruning...\n");
    const int QUERY_COUNT = 100;
    int exact_hits = 0;
    size_t total_clusters_visited = 0;
    const float NOISE_SIGMA = 0.25f;

    for (int q = 0; q < QUERY_COUNT; ++q) {
        int target_id = (q * 7) % N_CONCEPTS;
        float noisy_query[CNET_VSA_DEFAULT_DIM];
        float noise_vec[CNET_VSA_DEFAULT_DIM];
        cnet_vsa_random(noise_vec, D, &rng);

        for (int i = 0; i < D; ++i) {
            noisy_query[i] = concept_vectors[target_id * D + i] + NOISE_SIGMA * noise_vec[i];
        }
        cnet_vsa_normalize(noisy_query, D);

        char match_name[CNET_VSA_NAME_MAX] = {0};
        float match_vec[CNET_VSA_DEFAULT_DIM];
        float sim = 0.0f;
        size_t visited = 0;

        cnet_vsa_index_query(&index, noisy_query, match_vec, match_name,
                             sizeof(match_name), &sim, &visited);

        if (strcmp(match_name, concept_names[target_id]) == 0) {
            exact_hits++;
        }
        total_clusters_visited += visited;
    }

    double avg_visited = (double)total_clusters_visited / (double)QUERY_COUNT;
    double prune_percentage = 100.0 * (1.0 - avg_visited / (double)index.cluster_count);
    printf("  Exact retrieval recall: %d/%d (%.1f%%)\n", exact_hits, QUERY_COUNT, (double)exact_hits);
    printf("  Average clusters visited: %.1f of %zu (%.1f%% search space pruned)\n",
           avg_visited, index.cluster_count, prune_percentage);

    check(exact_hits == QUERY_COUNT, "Hierarchical metric search achieves 100% retrieval recall under 25% continuous noise");
    check(prune_percentage > 25.0, "Triangle inequality prunes > 25% of the cluster search space (sub-linear)");

    /* -------------------------------------------------------------
     * Suite 3: Query Throughput & Latency
     * ------------------------------------------------------------- */
    printf("\n[3/3] Measuring Metric Index Query Latency...\n");
    float test_probe[CNET_VSA_DEFAULT_DIM];
    float probe_noise[CNET_VSA_DEFAULT_DIM];
    cnet_vsa_random(probe_noise, D, &rng);
    for (int i = 0; i < D; ++i) {
        test_probe[i] = concept_vectors[42 * D + i] + 0.15f * probe_noise[i];
    }
    cnet_vsa_normalize(test_probe, D);

    const int TIMING_RUNS = 10000;
    uint64_t t0 = bench_mono_ns();
    for (int i = 0; i < TIMING_RUNS; ++i) {
        char dummy_name[CNET_VSA_NAME_MAX];
        float dummy_vec[CNET_VSA_DEFAULT_DIM];
        float dummy_sim;
        size_t dummy_v;
        cnet_vsa_index_query(&index, test_probe, dummy_vec, dummy_name,
                             sizeof(dummy_name), &dummy_sim, &dummy_v);
    }
    uint64_t t1 = bench_mono_ns();
    double total_ms = (double)(t1 - t0) / 1000000.0;
    double us_per_query = (total_ms * 1000.0) / (double)TIMING_RUNS;
    printf("  %d queries over 1,000 concepts in %.2f ms (%.2f us/query)\n",
           TIMING_RUNS, total_ms, us_per_query);
    check(us_per_query < 50.0, "Sub-linear metric query executes in sub-50 microseconds per 1,000 concepts");

    /* Cleanup */
    cnet_vsa_index_free(&index);
    free(concept_vectors);
    free(concept_names);

    printf("\n-----------------------------------------------------------------\n");
    if (failures == 0) {
        printf("CNET_VSA_INDEX_BENCH_PASS: all %d checks passed.\n", checks);
        return 0;
    } else {
        printf("CNET_VSA_INDEX_BENCH_FAIL: %d of %d checks failed.\n", failures, checks);
        return 1;
    }
}
