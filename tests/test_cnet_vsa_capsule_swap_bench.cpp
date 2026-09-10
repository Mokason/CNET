#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <vector>

#include "../include/cnet_vsa.h"
#include "../include/cnet_vsa_bsc.h"
#include "../include/cnet_vsa_device.h"
#include "../include/cnet_vsa_capsule_swap.h"

static int checks = 0;
static int failures = 0;

static void check(int ok, const char *msg) {
    checks++;
    printf("  %-62s %s\n", msg, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

int main(void) {
    printf("=================================================================\n");
    printf(" CNET-VSA Task 2: Capsule Hot-Swapper & VRAM Paging Benchmark\n");
    printf("=================================================================\n\n");

    cnet_vsa_device_init();
    printf("[Device Context]\n");
    printf("  Active Backend: %s\n", cnet_vsa_device_is_gpu_active() ? "AMD ROCm GPU (gfx1201)" : "Host CPU");

    const int D = CNET_VSA_DEFAULT_DIM;
    uint64_t rng = 0x123456789ABCDEFULL;

    /* -------------------------------------------------------------
     * Suite 1: Capsule Registration & Bounded Budget Initialization
     * ------------------------------------------------------------- */
    printf("\n[1/3] Registering 8 Specialist Capsules (Total: 128 MB)...\n");
    CnetVsaCapsuleManager mgr;
    /* Hard VRAM budget: 48 MB (can hold at most 3 capsules concurrently) */
    const size_t VRAM_BUDGET = 48 * 1024 * 1024;
    const size_t CAP_WEIGHT_SZ = 16 * 1024 * 1024; // 16 MB each

    cnet_vsa_capsule_mgr_init(&mgr, VRAM_BUDGET);

    const char *names[8] = {
        "c_compiler", "legal_contract", "math_solver", "medical_coder",
        "quantum_sim", "financial_audit", "embedded_arm", "sql_optimizer"
    };

    float cap_centroids[8][CNET_VSA_DEFAULT_DIM];
    std::vector<uint8_t> dummy_weights(CAP_WEIGHT_SZ);

    for (int i = 0; i < 8; ++i) {
        cnet_vsa_random(cap_centroids[i], D, &rng);
        /* Fill weights with identifiable pattern */
        memset(dummy_weights.data(), (uint8_t)(i + 1), CAP_WEIGHT_SZ);
        int cap_id = cnet_vsa_capsule_register(&mgr, names[i], "Domain Specialist",
                                              cap_centroids[i], dummy_weights.data(), CAP_WEIGHT_SZ);
        check(cap_id == i + 1, "Capsule registered successfully");
    }

    printf("  Registered Capsules: %zu (Total weight: %.1f MB)\n",
           mgr.capsule_count, (double)(mgr.capsule_count * CAP_WEIGHT_SZ) / (1024.0 * 1024.0));
    printf("  VRAM Budget Limit:   %.1f MB (Max 3 concurrent capsules)\n",
           (double)mgr.max_vram_bytes / (1024.0 * 1024.0));

    /* Warm-up driver page tables */
    CnetVsaCapsuleEntry *warm_cap = NULL;
    void *warm_dev = NULL;
    double warm_ms = 0.0;
    cnet_vsa_capsule_hot_swap(&mgr, cap_centroids[0], &warm_cap, &warm_dev, &warm_ms);
    cnet_vsa_capsule_evict(&mgr, warm_cap->capsule_id);
    mgr.swap_in_count = 0;
    mgr.evict_count = 0;
    mgr.total_swap_time_ms = 0.0;

    /* -------------------------------------------------------------
     * Suite 2: Dynamic Query-Driven Swapping Stress Test
     * ------------------------------------------------------------- */
    printf("\n[2/3] Running 40 Multi-Domain Queries under Dynamic Paging...\n");
    const int NUM_QUERIES = 40;
    int budget_violations = 0;
    int routing_correct = 0;
    int cache_hits = 0;
    int cache_misses = 0;
    double max_swap_ms = 0.0;

    for (int q = 0; q < NUM_QUERIES; ++q) {
        /* Realistic temporal locality: 2-3 queries per domain burst */
        int target_domain = (q / 2) % 8;

        /* Create query vector with noise around target centroid */
        float query_v[CNET_VSA_DEFAULT_DIM];
        float noise[CNET_VSA_DEFAULT_DIM];
        cnet_vsa_random(noise, D, &rng);
        for (int j = 0; j < D; ++j) {
            query_v[j] = cap_centroids[target_domain][j] + 0.15f * noise[j];
        }
        cnet_vsa_normalize(query_v, D);

        CnetVsaCapsuleEntry *active_cap = NULL;
        void *dev_ptr = NULL;
        double swap_ms = 0.0;

        int rc = cnet_vsa_capsule_hot_swap(&mgr, query_v, &active_cap, &dev_ptr, &swap_ms);

        if (rc == 0 && active_cap != NULL && strcmp(active_cap->name, names[target_domain]) == 0) {
            routing_correct++;
        }

        if (swap_ms == 0.0) {
            cache_hits++;
        } else {
            cache_misses++;
            if (swap_ms > max_swap_ms) max_swap_ms = swap_ms;
        }

        if (mgr.current_vram_bytes > mgr.max_vram_bytes) {
            budget_violations++;
        }
    }

    double avg_swap_ms = mgr.swap_in_count > 0 ? (mgr.total_swap_time_ms / (double)mgr.swap_in_count) : 0.0;
    printf("  Queries Executed:    %d\n", NUM_QUERIES);
    printf("  Routing Accuracy:    %d / %d (%.1f%%)\n", routing_correct, NUM_QUERIES, (double)routing_correct / NUM_QUERIES * 100.0);
    printf("  VRAM Cache Hits:     %d (%.1f%%)\n", cache_hits, (double)cache_hits / NUM_QUERIES * 100.0);
    printf("  VRAM Cache Misses:   %d (paged via DMA)\n", cache_misses);
    printf("  Total Swaps In:      %lu, Evictions: %lu\n", mgr.swap_in_count, mgr.evict_count);
    printf("  Peak VRAM Used:      %.1f MB / %.1f MB Limit\n",
           (double)mgr.peak_vram_bytes / (1024.0 * 1024.0), (double)mgr.max_vram_bytes / (1024.0 * 1024.0));
    printf("  Avg DMA Paging Time: %.3f ms for 16 MB capsule\n", avg_swap_ms);
    printf("  Max DMA Paging Time: %.3f ms for 16 MB capsule\n", max_swap_ms);

    check(routing_correct == NUM_QUERIES, "VSA Router directed 100% of queries to correct specialist capsule");
    check(budget_violations == 0, "Hard VRAM budget was strictly respected on every query (0 violations)");
    check(mgr.evict_count > 0, "LRU eviction successfully reclaimed VRAM when budget was exceeded");
    check(cache_hits > 0, "Temporal locality yielded hot VRAM cache hits (0 latency)");
    check(avg_swap_ms < 5.0, "Average DMA hot-swap completes in sub-5 milliseconds per 16 MB capsule");

    /* -------------------------------------------------------------
     * Suite 3: Manual Eviction & Cleanup
     * ------------------------------------------------------------- */
    printf("\n[3/3] Testing Manual Eviction & VRAM Teardown...\n");
    int evict_rc = cnet_vsa_capsule_evict(&mgr, 1);
    check(evict_rc == 0, "Manual capsule eviction executed cleanly");

    cnet_vsa_capsule_mgr_free(&mgr);
    check(mgr.current_vram_bytes == 0, "VRAM completely freed and zeroed upon manager destruction");

    printf("\n-----------------------------------------------------------------\n");
    if (failures == 0) {
        printf("CNET_VSA_CAPSULE_SWAP_BENCH_PASS: all %d checks passed.\n", checks);
        return 0;
    } else {
        printf("CNET_VSA_CAPSULE_SWAP_BENCH_FAIL: %d of %d checks failed.\n", failures, checks);
        return 1;
    }
}
