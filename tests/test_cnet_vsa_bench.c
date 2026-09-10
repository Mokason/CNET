#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#include "../include/cnet_vsa.h"
#include "../include/cnet_vsa_memory.h"
#include "../include/cnet_vsa_bus.h"

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

/* Dummy specialist for bus chaining */
static int specialist_delta_fn(const float *in, float *delta_out, int dim, void *ctx) {
    float scale = ctx ? *(float *)ctx : 0.1f;
    for (int i = 0; i < dim; ++i) {
        delta_out[i] = in[(i + 1) % dim] * scale;
    }
    return 0;
}

int main(void) {
    printf("=================================================================\n");
    printf(" CNET-VSA: Vector Symbolic Architecture & 3-Way Memory Benchmark\n");
    printf("=================================================================\n\n");

    const int D = CNET_VSA_DEFAULT_DIM;
    uint64_t rng = 123456789ULL;

    /* -------------------------------------------------------------
     * Suite 1: Mathematical Foundations of High-Dimensional VSA
     * ------------------------------------------------------------- */
    printf("[1/5] Testing Core VSA Operations (Dim=%d)...\n", D);
    float a[CNET_VSA_DEFAULT_DIM], b[CNET_VSA_DEFAULT_DIM], c[CNET_VSA_DEFAULT_DIM];
    float bound[CNET_VSA_DEFAULT_DIM], recovered[CNET_VSA_DEFAULT_DIM];
    float bundled[CNET_VSA_DEFAULT_DIM], permuted[CNET_VSA_DEFAULT_DIM], restored[CNET_VSA_DEFAULT_DIM];

    cnet_vsa_random(a, D, &rng);
    cnet_vsa_random(b, D, &rng);
    cnet_vsa_random(c, D, &rng);

    /* Quasi-orthogonality in high dimensions */
    float sim_ab = cnet_vsa_similarity(a, b, D);
    float sim_ac = cnet_vsa_similarity(a, c, D);
    check(fabsf(sim_ab) < 0.15f && fabsf(sim_ac) < 0.15f, "random vectors are quasi-orthogonal (|dot| < 0.15)");

    /* Binding and Unbinding (Role-Filler recovery) */
    cnet_vsa_bind(bound, a, b, D);
    cnet_vsa_unbind(recovered, bound, b, D);
    float sim_recovered = cnet_vsa_similarity(recovered, a, D);
    float sim_noise = cnet_vsa_similarity(recovered, c, D);
    check(sim_recovered > 0.85f, "unbinding recovers original vector (sim > 0.85)");
    check(fabsf(sim_noise) < 0.15f, "recovered vector has near-zero dot product with unrelated vector");

    /* Bundling (Superposition / Set Membership) */
    const float *bundle_list[3] = { a, b, c };
    cnet_vsa_bundle(bundled, bundle_list, NULL, 3, D);
    float sim_ba = cnet_vsa_similarity(bundled, a, D);
    float sim_bb = cnet_vsa_similarity(bundled, b, D);
    float sim_bc = cnet_vsa_similarity(bundled, c, D);
    check(sim_ba > 0.40f && sim_bb > 0.40f && sim_bc > 0.40f,
          "bundled vector contains all 3 components in superposition (sim > 0.40)");

    /* Permutation (Temporal Sequence Shift) */
    cnet_vsa_permute(permuted, a, 1, D);
    float sim_perm_self = cnet_vsa_similarity(permuted, a, D);
    cnet_vsa_permute(restored, permuted, -1, D);
    float sim_restored = cnet_vsa_similarity(restored, a, D);
    check(fabsf(sim_perm_self) < 0.20f, "permuted vector is quasi-orthogonal to original (|dot| < 0.20)");
    check(sim_restored > 0.999f, "inverse permutation exactly restores original vector (sim ~ 1.0)");

    /* -------------------------------------------------------------
     * Suite 2: Noise Tolerance & Generalization vs Discrete LUT
     * ------------------------------------------------------------- */
    printf("\n[2/5] Comparative Quality: Continuous VSA vs Discrete LUT under Noise...\n");
    CnetVsaCodebook codebook;
    cnet_vsa_codebook_init(&codebook, D, 32);
    cnet_vsa_codebook_add(&codebook, "concept_alpha", a);
    cnet_vsa_codebook_add(&codebook, "concept_beta", b);
    cnet_vsa_codebook_add(&codebook, "concept_gamma", c);

    /* Simulate discrete LUT (exact bitwise/hash lookup) */
    int lut_hits = 0;
    int vsa_clean_hits = 0;
    const int TRIALS = 50;
    const float NOISE_SIGMA = 0.35f;

    for (int t = 0; t < TRIALS; ++t) {
        float noisy[CNET_VSA_DEFAULT_DIM];
        float noise_vec[CNET_VSA_DEFAULT_DIM];
        cnet_vsa_random(noise_vec, D, &rng);
        for (int i = 0; i < D; ++i) {
            noisy[i] = a[i] + NOISE_SIGMA * noise_vec[i];
        }
        cnet_vsa_normalize(noisy, D);

        /* Discrete LUT fails on any continuous perturbation */
        if (memcmp(noisy, a, D * sizeof(float)) == 0) {
            lut_hits++;
        }

        /* Continuous VSA clean-up memory associative search */
        char clean_name[CNET_VSA_NAME_MAX] = {0};
        float clean_vec[CNET_VSA_DEFAULT_DIM];
        float clean_sim = 0.0f;
        cnet_vsa_codebook_cleanup(&codebook, noisy, clean_vec, clean_name, sizeof(clean_name), &clean_sim);
        if (strcmp(clean_name, "concept_alpha") == 0 && clean_sim > 0.70f) {
            vsa_clean_hits++;
        }
    }

    check(lut_hits == 0, "Discrete LUT fails completely on perturbed input (0/50 hits, 0.0%)");
    check(vsa_clean_hits == TRIALS, "CNET-VSA recovers correct concept under continuous noise (50/50 hits, 100.0%)");

    /* Metric Contract Epsilon-Ball Verification */
    CnetVsaMetricContract contract;
    strncpy(contract.name, "alpha_contract", sizeof(contract.name) - 1);
    contract.dim = D;
    contract.centroid = a;
    contract.radius_epsilon = 0.50f; /* max allowable distance */
    contract.margin_floor = 0.05f;

    int admitted = 0;
    float margin = 0.0f;
    float in_bound_noisy[CNET_VSA_DEFAULT_DIM];
    for (int i = 0; i < D; ++i) in_bound_noisy[i] = a[i] + 0.1f * b[i];
    cnet_vsa_normalize(in_bound_noisy, D);

    cnet_vsa_contract_verify(&contract, in_bound_noisy, &admitted, &margin);
    check(admitted == 1 && margin > 0.05f, "Metric contract admits in-distribution continuous input with positive margin");

    float out_of_dist[CNET_VSA_DEFAULT_DIM];
    cnet_vsa_random(out_of_dist, D, &rng);
    cnet_vsa_contract_verify(&contract, out_of_dist, &admitted, &margin);
    check(admitted == 0, "Metric contract fails closed on out-of-distribution input (calibrated abstention)");

    /* -------------------------------------------------------------
     * Suite 3: 3-Way Memory Reasoning (STM Scratchpad, Graph, LTM)
     * ------------------------------------------------------------- */
    printf("\n[3/5] Testing 3-Way Memory & Search/Backtracking...\n");
    CnetVsaMemory3Way mem3;
    cnet_vsa_memory_init(&mem3, D, 64, 64);

    /* Test STM scratchpad push/pop/backtrack (Sudoku/Puzzle solving loop) */
    cnet_vsa_stm_step(&mem3.stm, a, 0.5f, 0.5f);
    check(cnet_vsa_stm_push(&mem3.stm) == 0, "STM scratchpad pushes initial state");

    /* Branch to a hypothetical bad candidate */
    cnet_vsa_stm_step(&mem3.stm, b, 0.2f, 0.8f);
    float sim_before_pop = cnet_vsa_similarity(mem3.stm.state, a, D);
    check(sim_before_pop < 0.60f, "Hypothetical branch modifies active working state");

    /* Backtrack */
    check(cnet_vsa_stm_pop(&mem3.stm) == 0, "STM scratchpad pops / backtracks to prior state");
    float sim_after_pop = cnet_vsa_similarity(mem3.stm.state, a, D);
    check(sim_after_pop > 0.99f, "Backtracking restores exact pre-branch state (~1.0)");

    /* Test Session Context Document Graph */
    cnet_vsa_graph_add_node(&mem3.graph, -1, "clause_1", "The warranty is valid for 24 months from purchase.", a);
    cnet_vsa_graph_add_node(&mem3.graph, 1, "clause_2", "Water damage voids all warranty coverage immediately.", b);
    cnet_vsa_graph_add_node(&mem3.graph, 1, "clause_3", "Arbitration must take place in Geneva under Swiss law.", c);

    const CnetVsaGraphNode *results[2] = {NULL};
    float scores[2] = {0};
    size_t found = 0;
    cnet_vsa_graph_query(&mem3.graph, b, 1, results, scores, &found);
    check(found == 1 && results[0] != NULL, "Document graph successfully retrieves node by semantic vector probe");
    check(strcmp(results[0]->label, "clause_2") == 0, "Retrieved node matches exact water-damage warranty clause");

    /* Sleep Consolidation: Session Graph -> Permanent LTM */
    size_t promoted = 0;
    cnet_vsa_memory_consolidate(&mem3, 0.60f, &promoted);
    check(promoted >= 1, "Sleep consolidation successfully promotes persistent session clusters to LTM");

    /* -------------------------------------------------------------
     * Suite 4: Universal Residual Bus & Specialist Dispatch
     * ------------------------------------------------------------- */
    printf("\n[4/5] Testing Universal Residual Bus & Specialist Chaining...\n");
    CnetVsaBus bus;
    cnet_vsa_bus_init(&bus, D);

    float delta_scale = 0.25f;
    cnet_vsa_bus_register(&bus, "math_increment", a, 0.55f, 0.05f, specialist_delta_fn, &delta_scale);
    cnet_vsa_bus_register(&bus, "logic_policy", b, 0.55f, 0.05f, specialist_delta_fn, &delta_scale);

    /* In-distribution routing */
    float bus_in[CNET_VSA_DEFAULT_DIM];
    memcpy(bus_in, a, D * sizeof(float));
    char dispatched[CNET_VSA_NAME_MAX] = {0};
    float route_margin = 0.0f;
    CnetVsaStatus st = cnet_vsa_bus_route_step(&bus, bus_in, dispatched, sizeof(dispatched), &route_margin);
    check(st == CNET_VSA_STATUS_SUCCESS, "Bus routes in-distribution query to registered specialist");
    check(strcmp(dispatched, "math_increment") == 0, "Router accurately selects math_increment specialist");

    /* Out-of-distribution calibrated abstention */
    float bus_ood[CNET_VSA_DEFAULT_DIM];
    cnet_vsa_random(bus_ood, D, &rng);
    st = cnet_vsa_bus_route_step(&bus, bus_ood, dispatched, sizeof(dispatched), &route_margin);
    check(st == CNET_VSA_STATUS_ABSTAIN, "Bus cleanly abstains on out-of-distribution input (fail-closed)");

    /* Multi-hop chaining */
    const char *chain[2] = { "math_increment", "math_increment" };
    size_t hops = 0;
    memcpy(bus_in, a, D * sizeof(float));
    st = cnet_vsa_bus_execute_chain(&bus, chain, 2, bus_in, &hops);
    check(st == CNET_VSA_STATUS_SUCCESS && hops == 2, "Bus executes multi-hop specialist chain with contract verified at every hop");

    /* -------------------------------------------------------------
     * Suite 5: Latency and Throughput
     * ------------------------------------------------------------- */
    printf("\n[5/5] Measuring Execution Latency...\n");
    const int OPS = 10000;
    uint64_t t0 = bench_mono_ns();
    for (int i = 0; i < OPS; ++i) {
        cnet_vsa_bind(bound, a, b, D);
        cnet_vsa_unbind(recovered, bound, b, D);
    }
    uint64_t t1 = bench_mono_ns();
    double total_us = (double)(t1 - t0) / 1000.0;
    double per_op_us = total_us / (double)OPS;
    printf("  %d bind+unbind operations executed in %.2f ms (%.3f us/op)\n", OPS, total_us / 1000.0, per_op_us);
    check(per_op_us < 50.0, "Execution latency is ultra-fast (< 50 microseconds per full bind/unbind)");

    /* Teardown */
    cnet_vsa_codebook_free(&codebook);
    cnet_vsa_memory_free(&mem3);
    cnet_vsa_bus_free(&bus);

    printf("\n-----------------------------------------------------------------\n");
    if (failures == 0) {
        printf("CNET_VSA_BENCH_PASS: all %d checks passed.\n", checks);
        return 0;
    } else {
        printf("CNET_VSA_BENCH_FAIL: %d of %d checks failed.\n", failures, checks);
        return 1;
    }
}
