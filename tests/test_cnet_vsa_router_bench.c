#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <assert.h>
#include "cnet_vsa.h"
#include "cnet_vsa_bsc.h"
#include "cnet_vsa_text.h"
#include "cnet_vsa_ngram.h"
#include "cnet_vsa_gen_capsule.h"

static double get_time_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000000.0 + (double)ts.tv_nsec / 1000.0;
}

int main(void) {
    printf("=================================================================\n");
    printf(" CNET-VSA Autonomous Multi-Capsule Router & Registry Benchmark   \n");
    printf("=================================================================\n\n");

    /* [1/4] Registry Initialization & Discovery */
    printf("[1/4] Scanning & Indexing Certified Capsules from 'bin/'...\n");
    CnetVsaGenRegistry reg;
    assert(cnet_vsa_registry_init(&reg, CNET_VSA_DEFAULT_DIM) == 0);

    int count = cnet_vsa_registry_load_dir(&reg, "bin");
    printf("  Discovered and indexed %d certified capsules into memory table\n", count);
    assert(count >= 10);

    for (size_t i = 0; i < reg.count; ++i) {
        assert(reg.capsules[i].header.certified == 1);
        assert(reg.capsules[i].header.safe_radius > 0.0f);
    }
    printf("  All %zu registered capsules verified authentic (100%% certified) PASS\n", reg.count);

    /* [2/4] Measuring Intent Routing Latency & Precision */
    printf("\n[2/4] Testing Sub-Microsecond Multi-Capsule Intent Routing...\n");
    
    struct {
        const char *query;
        const char *expected_cap;
    } route_tests[] = {
        {"wavefront lds shared memory coalescing hip execution", "rocm_gpu_compute"},
        {"Virtual File System inodes dentries and epoll in Linux kernel", "linux_kernel_internals"},
        {"llvm compiler ssa dead code elimination loop vectorization lto", "compiler_optimization"},
        {"elliptic curve cryptography aes gcm sha3 diffie hellman", "cryptography_foundations"},
        {"microcontroller interrupt service routine dma memory bus", "hardware_embedded"}
    };

    double t0 = get_time_us();
    int num_tests = sizeof(route_tests) / sizeof(route_tests[0]);

    for (int i = 0; i < num_tests; ++i) {
        float q_vec[CNET_VSA_DEFAULT_DIM];
        assert(cnet_vsa_gencap_encode_intent(route_tests[i].query, q_vec, reg.dim) == 0);

        int best_idx = -1;
        float best_dist = 1.0f;
        int winner = cnet_vsa_registry_route(&reg, q_vec, &best_idx, &best_dist);

        assert(winner >= 0);
        assert(strcmp(reg.capsules[winner].header.name, route_tests[i].expected_cap) == 0);
        printf("  Query: \"%-45s...\" -> Routed: %-22s (dist=%.4f) PASS\n",
               route_tests[i].query, reg.capsules[winner].header.name, best_dist);
    }

    double total_us = get_time_us() - t0;
    printf("  Routed %d queries in %.2f us (%.2f us/route decision) PASS\n",
           num_tests, total_us, total_us / num_tests);

    /* [3/4] Testing Fail-Closed Multi-Capsule Abstention */
    printf("\n[3/4] Testing Global Multi-Capsule Out-of-Domain Abstention...\n");

    const char *ood_queries[] = {
        "baking chocolate strawberry cake with vanilla frosting in kitchen",
        "astrology horoscope zodiac signs crystal energy reading"
    };

    for (int i = 0; i < 2; ++i) {
        float q_vec[CNET_VSA_DEFAULT_DIM];
        assert(cnet_vsa_gencap_encode_intent(ood_queries[i], q_vec, reg.dim) == 0);

        int best_idx = -1;
        float best_dist = 1.0f;
        int winner = cnet_vsa_registry_route(&reg, q_vec, &best_idx, &best_dist);

        assert(winner == -1); /* Must refuse all capsules */
        assert(best_dist > 0.900f);
        printf("  OOD Query: \"%-45s...\" -> Refused (closest='%s', dist=%.4f > 0.900) PASS\n",
               ood_queries[i], reg.capsules[best_idx].header.name, best_dist);
    }

    /* [4/4] End-to-End Autonomous Dispatch */
    printf("\n[4/4] Testing End-to-End Autonomous Dispatch (Zero LLM)...\n");

    char out_buf[1024];
    char cap_name[64];
    float dist = 0.0f;

    /* In-Domain Dispatch */
    int rc1 = cnet_vsa_registry_dispatch(&reg, "wavefront lds shared memory coalescing hip execution", out_buf, sizeof(out_buf), cap_name, &dist);
    assert(rc1 == 0);
    assert(strcmp(cap_name, "rocm_gpu_compute") == 0);
    assert(strlen(out_buf) > 10);
    printf("  Dispatched to '%s': \"%s\" (dist=%.4f) PASS\n", cap_name, out_buf, dist);

    /* Out-of-Domain Dispatch */
    int rc2 = cnet_vsa_registry_dispatch(&reg, "tropical coral reef scuba diving fish", out_buf, sizeof(out_buf), cap_name, &dist);
    assert(rc2 == -3); /* Abstain */
    assert(strstr(out_buf, "ABSTAIN") != NULL);
    printf("  OOD Dispatch Refusal: \"%s\" PASS\n", out_buf);

    printf("\n=================================================================\n");
    printf(" CNET_VSA_ROUTER_BENCH_PASS: All 4 validation gates passed cleanly\n");
    printf("=================================================================\n");
    return 0;
}
