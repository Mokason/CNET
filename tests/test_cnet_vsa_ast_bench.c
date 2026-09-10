#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#include "../include/cnet_vsa.h"
#include "../include/cnet_vsa_ast.h"

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
    printf(" CNET-VSA Task 4: Graph-AST Structural Code Reasoning Benchmark\n");
    printf("=================================================================\n\n");

    const int D = CNET_VSA_DEFAULT_DIM;
    CnetVsaAstGraph g;
    cnet_vsa_ast_init(&g, D);

    /* -------------------------------------------------------------
     * Suite 1: Ingesting Micro-Codebase Call Graph & Invariants
     * ------------------------------------------------------------- */
    printf("[1/4] Ingesting Codebase Entities, Call Edges & Contracts...\n");

    cnet_vsa_ast_add_edge(&g, "cnet_vsa_hot_swap", "hipMalloc", CNET_VSA_EDGE_CALLS);
    cnet_vsa_ast_add_edge(&g, "cnet_vsa_hot_swap", "vram_buffer", CNET_VSA_EDGE_ALLOCATES);
    cnet_vsa_ast_add_edge(&g, "cnet_vsa_hot_swap", "vram_bounds", CNET_VSA_EDGE_VERIFIES);

    cnet_vsa_ast_add_edge(&g, "cnet_vsa_evict", "hipFree", CNET_VSA_EDGE_CALLS);
    cnet_vsa_ast_add_edge(&g, "cnet_vsa_evict", "vram_buffer", CNET_VSA_EDGE_FREES);

    cnet_vsa_ast_add_edge(&g, "cnet_vsa_query", "cnet_vsa_index_search", CNET_VSA_EDGE_CALLS);
    cnet_vsa_ast_add_edge(&g, "cnet_vsa_index_search", "metric_triangle_bound", CNET_VSA_EDGE_VERIFIES);

    cnet_vsa_ast_add_edge(&g, "cnet_vsa_unsafe_worker", "scratch_heap", CNET_VSA_EDGE_ALLOCATES);

    printf("  Total Code Nodes: %zu\n", g.node_count);
    printf("  Total Edges:      %zu\n", g.edge_count);
    check(g.node_count >= 8, "All code symbols registered in symbol vocabulary");
    check(g.edge_count == 8, "All 8 relational AST edges registered");

    int comp_rc = cnet_vsa_ast_compile(&g);
    check(comp_rc == 0, "AST call graph compiled into unified superposition vector");

    /* -------------------------------------------------------------
     * Suite 2: Forward Relational Query (Callee Resolution)
     * ------------------------------------------------------------- */
    printf("\n[2/4] Testing Forward Callee Algebraic Unbinding...\n");

    char target[CNET_VSA_NAME_MAX] = {0};
    float sim = 0.0f;

    /* "What does cnet_vsa_evict CALL?" */
    cnet_vsa_ast_query_callee(&g, "cnet_vsa_evict", CNET_VSA_EDGE_CALLS, target, sizeof(target), &sim);
    printf("  Query: 'cnet_vsa_evict -[CALLS]-> ?'  => Found '%s' (similarity: %.4f)\n", target, sim);
    check(strcmp(target, "hipFree") == 0, "Forward unbinding accurately recovers callee 'hipFree'");
    check(sim > 0.25f, "Callee match has positive projection margin");

    /* "What does cnet_vsa_hot_swap ALLOCATE?" */
    cnet_vsa_ast_query_callee(&g, "cnet_vsa_hot_swap", CNET_VSA_EDGE_ALLOCATES, target, sizeof(target), &sim);
    printf("  Query: 'cnet_vsa_hot_swap -[ALLOCATES]-> ?'  => Found '%s' (similarity: %.4f)\n", target, sim);
    check(strcmp(target, "vram_buffer") == 0, "Forward unbinding recovers allocated resource 'vram_buffer'");

    /* -------------------------------------------------------------
     * Suite 3: Backward Relational Query (Caller Resolution)
     * ------------------------------------------------------------- */
    printf("\n[3/4] Testing Backward Caller Algebraic Unbinding...\n");

    /* "Who FREES vram_buffer?" */
    cnet_vsa_ast_query_caller(&g, "vram_buffer", CNET_VSA_EDGE_FREES, target, sizeof(target), &sim);
    printf("  Query: '? -[FREES]-> vram_buffer'  => Found '%s' (similarity: %.4f)\n", target, sim);
    check(strcmp(target, "cnet_vsa_evict") == 0, "Backward unbinding accurately recovers caller 'cnet_vsa_evict'");
    check(sim > 0.25f, "Caller match has positive projection margin");

    /* -------------------------------------------------------------
     * Suite 4: Static Invariant Verification & Throughput
     * ------------------------------------------------------------- */
    printf("\n[4/4] Testing Semantic Invariant Violation Detection...\n");

    int safe_stat = cnet_vsa_ast_check_leak_invariants(&g, "cnet_vsa_hot_swap");
    int leak_stat = cnet_vsa_ast_check_leak_invariants(&g, "cnet_vsa_unsafe_worker");

    printf("  Invariant Status 'cnet_vsa_hot_swap':    %s (Expected: Safe)\n", safe_stat == 0 ? "SAFE" : "VIOLATION");
    printf("  Invariant Status 'cnet_vsa_unsafe_worker': %s (Expected: VIOLATION)\n", leak_stat == 1 ? "VIOLATION" : "SAFE");

    check(safe_stat == 0, "Verified function satisfies safety invariants (0 false violations)");
    check(leak_stat == 1, "Unsafe worker detected with unverified resource allocation violation");

    const int TIMING_RUNS = 10000;
    uint64_t t0 = bench_mono_ns();
    for (int i = 0; i < TIMING_RUNS; ++i) {
        char dummy[CNET_VSA_NAME_MAX];
        float dummy_sim;
        cnet_vsa_ast_query_callee(&g, "cnet_vsa_evict", CNET_VSA_EDGE_CALLS, dummy, sizeof(dummy), &dummy_sim);
    }
    uint64_t t1 = bench_mono_ns();
    double total_ms = (double)(t1 - t0) / 1000000.0;
    double us_per_query = (total_ms * 1000.0) / (double)TIMING_RUNS;
    printf("  %d graph unbinding queries in %.2f ms (%.2f us/query)\n",
           TIMING_RUNS, total_ms, us_per_query);
    check(us_per_query < 10.0, "AST graph unbinding executes in sub-10 microseconds per query");

    cnet_vsa_ast_free(&g);

    printf("\n-----------------------------------------------------------------\n");
    if (failures == 0) {
        printf("CNET_VSA_AST_BENCH_PASS: all %d checks passed.\n", checks);
        return 0;
    } else {
        printf("CNET_VSA_AST_BENCH_FAIL: %d of %d checks failed.\n", failures, checks);
        return 1;
    }
}
