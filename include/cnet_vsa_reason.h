#ifndef CNET_VSA_REASON_H
#define CNET_VSA_REASON_H

#include "cnet_vsa.h"
#include "cnet_vsa_memory.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CNET_VSA_RESONATOR_MAX_ITERS 32

/* --- 1. Resonator Network for Factored Unbinding ---
   Given composite vector S = X * Y * Z, and codebooks for X, Y, Z,
   converges in a few iterations to recover all 3 factors simultaneously
   without combinatorial O(|X| * |Y| * |Z|) search. */

typedef struct {
    char name_x[CNET_VSA_NAME_MAX];
    char name_y[CNET_VSA_NAME_MAX];
    char name_z[CNET_VSA_NAME_MAX];
    float sim_x;
    float sim_y;
    float sim_z;
    int iterations;
    int converged;
} CnetVsaResonatorResult;

int cnet_vsa_resonator_factor3(const float *composite_s,
                               const CnetVsaCodebook *cb_x,
                               const CnetVsaCodebook *cb_y,
                               const CnetVsaCodebook *cb_z,
                               int max_iters,
                               CnetVsaResonatorResult *result);

/* --- 2. Sudoku / Constraint Satisfaction Solver using STM Scratchpad ---
   Solves a 4x4 or 9x9 grid with constraint satisfaction and backtracking
   via the CnetVsaStm working memory stack. */

typedef struct {
    int grid_size; /* e.g. 4 for 4x4 (2x2 boxes), 9 for 9x9 (3x3 boxes) */
    int box_rows;
    int box_cols;
    int cells[16]; /* 4x4 grid cells (0 = empty, 1..4 = value) */
    size_t backtracks;
    size_t steps_explored;
} CnetVsaSudoku;

int cnet_vsa_sudoku_init(CnetVsaSudoku *s, const int *initial_grid);
int cnet_vsa_sudoku_solve(CnetVsaSudoku *s, CnetVsaStm *scratchpad);

#ifdef __cplusplus
}
#endif

#endif /* CNET_VSA_REASON_H */
