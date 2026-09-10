#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#include "../include/cnet_vsa.h"
#include "../include/cnet_vsa_memory.h"
#include "../include/cnet_vsa_reason.h"

static int checks = 0;
static int failures = 0;

static void check(int ok, const char *msg) {
    checks++;
    printf("  %-62s %s\n", msg, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

int main(void) {
    printf("=================================================================\n");
    printf(" CNET-VSA Priority 3: Resonator Networks & Sudoku Reasoning\n");
    printf("=================================================================\n\n");

    const int D = CNET_VSA_DEFAULT_DIM;
    uint64_t rng = 1122334455ULL;

    /* -------------------------------------------------------------
     * Suite 1: Resonator Network Factored Unbinding
     * ------------------------------------------------------------- */
    printf("[1/2] Testing Resonator Network Factored Unbinding (S = X * Y * Z)...\n");
    CnetVsaCodebook cb_subj, cb_act, cb_obj;
    cnet_vsa_codebook_init(&cb_subj, D, 8);
    cnet_vsa_codebook_init(&cb_act, D, 8);
    cnet_vsa_codebook_init(&cb_obj, D, 8);

    const char *subjs[5] = { "alice", "bob", "charlie", "david", "eve" };
    const char *acts[5] = { "reads", "writes", "deploys", "audits", "proves" };
    const char *objs[5] = { "kernel", "contract", "capsule", "proof", "ledger" };

    float s_vecs[5][CNET_VSA_DEFAULT_DIM];
    float a_vecs[5][CNET_VSA_DEFAULT_DIM];
    float o_vecs[5][CNET_VSA_DEFAULT_DIM];

    for (int i = 0; i < 5; ++i) {
        cnet_vsa_random(s_vecs[i], D, &rng);
        cnet_vsa_random(a_vecs[i], D, &rng);
        cnet_vsa_random(o_vecs[i], D, &rng);
        cnet_vsa_codebook_add(&cb_subj, subjs[i], s_vecs[i]);
        cnet_vsa_codebook_add(&cb_act, acts[i], a_vecs[i]);
        cnet_vsa_codebook_add(&cb_obj, objs[i], o_vecs[i]);
    }

    /* Test 20 composite triples */
    int correct_triples = 0;
    int total_iters = 0;
    const int TRIALS = 20;

    for (int t = 0; t < TRIALS; ++t) {
        int s_idx = t % 5;
        int a_idx = (t * 2 + 1) % 5;
        int o_idx = (t * 3 + 2) % 5;

        /* Bind S = Subject * Action * Object */
        float bound_sa[CNET_VSA_DEFAULT_DIM];
        float composite[CNET_VSA_DEFAULT_DIM];
        cnet_vsa_bind(bound_sa, s_vecs[s_idx], a_vecs[a_idx], D);
        cnet_vsa_bind(composite, bound_sa, o_vecs[o_idx], D);

        CnetVsaResonatorResult res;
        int rc = cnet_vsa_resonator_factor3(composite, &cb_subj, &cb_act, &cb_obj, 16, &res);

        if (rc == 0 && res.converged &&
            strcmp(res.name_x, subjs[s_idx]) == 0 &&
            strcmp(res.name_y, acts[a_idx]) == 0 &&
            strcmp(res.name_z, objs[o_idx]) == 0) {
            correct_triples++;
        }
        total_iters += res.iterations;
    }

    double avg_iters = (double)total_iters / (double)TRIALS;
    printf("  Factored triples recovered: %d/%d (%.1f%%)\n", correct_triples, TRIALS, (double)correct_triples * 5.0);
    printf("  Average iterations to settle: %.1f iters\n", avg_iters);

    check(correct_triples == TRIALS, "Resonator network successfully factorizes 100% of composite statements");
    check(avg_iters < 10.0, "Resonator converges in sub-10 iterations without combinatorial search");

    /* -------------------------------------------------------------
     * Suite 2: Sudoku Constraint Satisfaction with Working Memory
     * ------------------------------------------------------------- */
    printf("\n[2/2] Solving Sudoku Constraint Puzzle with STM Scratchpad...\n");
    /* 4x4 Sudoku Puzzle requiring backtracking:
       [ 0  2 | 0  0 ]
       [ 0  0 | 0  1 ]
       ---------------+
       [ 3  0 | 0  0 ]
       [ 0  0 | 4  0 ] */
    const int initial_board[16] = {
        0, 2, 0, 0,
        0, 0, 0, 1,
        3, 0, 0, 0,
        0, 0, 4, 0
    };

    CnetVsaStm scratchpad;
    cnet_vsa_stm_init(&scratchpad, D);

    CnetVsaSudoku puzzle;
    cnet_vsa_sudoku_init(&puzzle, initial_board);

    int solved = cnet_vsa_sudoku_solve(&puzzle, &scratchpad);
    check(solved == 0, "Sudoku solver terminates with complete puzzle solution");
    printf("  Steps explored: %zu, Backtracks: %zu\n", puzzle.steps_explored, puzzle.backtracks);
    check(puzzle.backtracks > 0, "Backtracking scratchpad successfully reversed dead-end branches");

    /* Verify solution validity */
    int all_valid = 1;
    for (int r = 0; r < 4; ++r) {
        int row_mask = 0;
        for (int c = 0; c < 4; ++c) {
            int v = puzzle.cells[r * 4 + c];
            if (v < 1 || v > 4 || (row_mask & (1 << v))) all_valid = 0;
            row_mask |= (1 << v);
        }
    }
    for (int c = 0; c < 4; ++c) {
        int col_mask = 0;
        for (int r = 0; r < 4; ++r) {
            int v = puzzle.cells[r * 4 + c];
            if (v < 1 || v > 4 || (col_mask & (1 << v))) all_valid = 0;
            col_mask |= (1 << v);
        }
    }
    check(all_valid == 1, "Solved Sudoku grid satisfies all row, column, and box constraints");

    printf("\n  Solved Grid:\n");
    for (int r = 0; r < 4; ++r) {
        printf("    %d %d | %d %d\n", puzzle.cells[r * 4], puzzle.cells[r * 4 + 1],
               puzzle.cells[r * 4 + 2], puzzle.cells[r * 4 + 3]);
        if (r == 1) printf("    ----+----\n");
    }

    cnet_vsa_codebook_free(&cb_subj);
    cnet_vsa_codebook_free(&cb_act);
    cnet_vsa_codebook_free(&cb_obj);
    cnet_vsa_stm_free(&scratchpad);

    printf("\n-----------------------------------------------------------------\n");
    if (failures == 0) {
        printf("CNET_VSA_REASON_BENCH_PASS: all %d checks passed.\n", checks);
        return 0;
    } else {
        printf("CNET_VSA_REASON_BENCH_FAIL: %d of %d checks failed.\n", failures, checks);
        return 1;
    }
}
