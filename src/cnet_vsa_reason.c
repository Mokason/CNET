#include "../include/cnet_vsa_reason.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

int cnet_vsa_resonator_factor3(const float *composite_s,
                               const CnetVsaCodebook *cb_x,
                               const CnetVsaCodebook *cb_y,
                               const CnetVsaCodebook *cb_z,
                               int max_iters,
                               CnetVsaResonatorResult *result) {
    if (!composite_s || !cb_x || !cb_y || !cb_z || !result) return -1;
    memset(result, 0, sizeof(*result));
    int D = cb_x->dim;
    if (cb_y->dim != D || cb_z->dim != D) return -1;

    /* Initialize estimates as bundle superposition over each codebook */
    float est_x[CNET_VSA_DEFAULT_DIM];
    float est_y[CNET_VSA_DEFAULT_DIM];
    float est_z[CNET_VSA_DEFAULT_DIM];

    const float **list_x = malloc(cb_x->count * sizeof(float *));
    const float **list_y = malloc(cb_y->count * sizeof(float *));
    const float **list_z = malloc(cb_z->count * sizeof(float *));

    for (size_t i = 0; i < cb_x->count; ++i) list_x[i] = &cb_x->vectors[i * D];
    for (size_t i = 0; i < cb_y->count; ++i) list_y[i] = &cb_y->vectors[i * D];
    for (size_t i = 0; i < cb_z->count; ++i) list_z[i] = &cb_z->vectors[i * D];

    cnet_vsa_bundle(est_x, list_x, NULL, (int)cb_x->count, D);
    cnet_vsa_bundle(est_y, list_y, NULL, (int)cb_y->count, D);
    cnet_vsa_bundle(est_z, list_z, NULL, (int)cb_z->count, D);

    free(list_x); free(list_y); free(list_z);

    int iters = 0;
    int limit = max_iters > 0 ? max_iters : CNET_VSA_RESONATOR_MAX_ITERS;

    while (iters < limit) {
        iters++;

        /* 1. Estimate X: probe = S * est_y * est_z */
        float probe_yz[CNET_VSA_DEFAULT_DIM];
        float probe_x[CNET_VSA_DEFAULT_DIM];
        cnet_vsa_bind(probe_yz, est_y, est_z, D);
        cnet_vsa_unbind(probe_x, composite_s, probe_yz, D);

        char match_x[CNET_VSA_NAME_MAX] = {0};
        float next_x[CNET_VSA_DEFAULT_DIM];
        float sim_x = 0.0f;
        cnet_vsa_codebook_cleanup(cb_x, probe_x, next_x, match_x, sizeof(match_x), &sim_x);

        /* 2. Estimate Y: probe = S * next_x * est_z */
        float probe_xz[CNET_VSA_DEFAULT_DIM];
        float probe_y[CNET_VSA_DEFAULT_DIM];
        cnet_vsa_bind(probe_xz, next_x, est_z, D);
        cnet_vsa_unbind(probe_y, composite_s, probe_xz, D);

        char match_y[CNET_VSA_NAME_MAX] = {0};
        float next_y[CNET_VSA_DEFAULT_DIM];
        float sim_y = 0.0f;
        cnet_vsa_codebook_cleanup(cb_y, probe_y, next_y, match_y, sizeof(match_y), &sim_y);

        /* 3. Estimate Z: probe = S * next_x * next_y */
        float probe_xy[CNET_VSA_DEFAULT_DIM];
        float probe_z[CNET_VSA_DEFAULT_DIM];
        cnet_vsa_bind(probe_xy, next_x, next_y, D);
        cnet_vsa_unbind(probe_z, composite_s, probe_xy, D);

        char match_z[CNET_VSA_NAME_MAX] = {0};
        float next_z[CNET_VSA_DEFAULT_DIM];
        float sim_z = 0.0f;
        cnet_vsa_codebook_cleanup(cb_z, probe_z, next_z, match_z, sizeof(match_z), &sim_z);

        memcpy(est_x, next_x, D * sizeof(float));
        memcpy(est_y, next_y, D * sizeof(float));
        memcpy(est_z, next_z, D * sizeof(float));

        snprintf(result->name_x, sizeof(result->name_x), "%s", match_x);
        snprintf(result->name_y, sizeof(result->name_y), "%s", match_y);
        snprintf(result->name_z, sizeof(result->name_z), "%s", match_z);
        result->sim_x = sim_x;
        result->sim_y = sim_y;
        result->sim_z = sim_z;
        result->iterations = iters;

        /* Check convergence: all 3 factors clearly recognized */
        if (sim_x > 0.85f && sim_y > 0.85f && sim_z > 0.85f) {
            result->converged = 1;
            return 0;
        }
    }

    result->converged = (result->sim_x > 0.70f && result->sim_y > 0.70f && result->sim_z > 0.70f);
    return result->converged ? 0 : 1;
}

int cnet_vsa_sudoku_init(CnetVsaSudoku *s, const int *initial_grid) {
    if (!s) return -1;
    s->grid_size = 4;
    s->box_rows = 2;
    s->box_cols = 2;
    s->backtracks = 0;
    s->steps_explored = 0;
    if (initial_grid) {
        memcpy(s->cells, initial_grid, 16 * sizeof(int));
    } else {
        memset(s->cells, 0, 16 * sizeof(int));
    }
    return 0;
}

static int sudoku_is_valid(const CnetVsaSudoku *s, int row, int col, int val) {
    for (int c = 0; c < 4; ++c) {
        if (s->cells[row * 4 + c] == val) return 0;
    }
    for (int r = 0; r < 4; ++r) {
        if (s->cells[r * 4 + col] == val) return 0;
    }
    int box_start_row = (row / 2) * 2;
    int box_start_col = (col / 2) * 2;
    for (int r = 0; r < 2; ++r) {
        for (int c = 0; c < 2; ++c) {
            if (s->cells[(box_start_row + r) * 4 + (box_start_col + c)] == val)
                return 0;
        }
    }
    return 1;
}

static int sudoku_solve_recursive(CnetVsaSudoku *s, CnetVsaStm *scratchpad) {
    s->steps_explored++;
    int empty_row = -1, empty_col = -1;
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            if (s->cells[r * 4 + c] == 0) {
                empty_row = r;
                empty_col = c;
                break;
            }
        }
        if (empty_row != -1) break;
    }

    if (empty_row == -1) {
        /* Entire grid filled and valid */
        return 1;
    }

    for (int val = 1; val <= 4; ++val) {
        if (sudoku_is_valid(s, empty_row, empty_col, val)) {
            /* 1. Push STM working state before exploring hypothetical branch */
            if (scratchpad) {
                cnet_vsa_stm_push(scratchpad);
            }

            s->cells[empty_row * 4 + empty_col] = val;

            /* 2. Recurse */
            if (sudoku_solve_recursive(s, scratchpad)) {
                return 1;
            }

            /* 3. Conflict encountered: Backtrack in STM scratchpad and reset cell */
            s->cells[empty_row * 4 + empty_col] = 0;
            s->backtracks++;
            if (scratchpad) {
                cnet_vsa_stm_pop(scratchpad);
            }
        }
    }
    return 0;
}

int cnet_vsa_sudoku_solve(CnetVsaSudoku *s, CnetVsaStm *scratchpad) {
    if (!s) return -1;
    return sudoku_solve_recursive(s, scratchpad) ? 0 : 1;
}
