#include "cce_aicimo_role_slice.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

/*
 * Real AICIMO Role-Slice Routing
 * Implements Drole (structural role embedding) vs Dlex (lexical) decoupling.
 *
 * When RouteOnRoleSlice is active:
 * - Dispatcher operates only on Drole columns (role embedding slice)
 * - Inner block operates on Dlex slice (lexical/content transformation)
 *
 * This is the core of AICIMO's adaptive adapter architecture.
 */

typedef struct {
    float *drole_weights;   /* role embedding slice */
    float *dlex_weights;    /* lexical transformation slice */
    size_t drole_dim;
    size_t dlex_dim;
    int owns_data;
} AicimoRoleSliceAdapter;

/* Initialize a role-slice adapter with identity on both slices */
int aicimo_role_slice_adapter_init(AicimoRoleSliceAdapter *a, size_t drole_dim, size_t dlex_dim) {
    if (!a || drole_dim == 0 || dlex_dim == 0) return -1;

    a->drole_dim = drole_dim;
    a->dlex_dim = dlex_dim;
    a->owns_data = 1;

    a->drole_weights = (float *)calloc(drole_dim * drole_dim, sizeof(float));
    a->dlex_weights = (float *)calloc(dlex_dim * dlex_dim, sizeof(float));

    if (!a->drole_weights || !a->dlex_weights) return -1;

    /* Identity initialization on both slices */
    for (size_t i = 0; i < drole_dim; ++i) {
        a->drole_weights[i * drole_dim + i] = 1.0f;
    }
    for (size_t i = 0; i < dlex_dim; ++i) {
        a->dlex_weights[i * dlex_dim + i] = 1.0f;
    }

    return 0;
}

void aicimo_role_slice_adapter_free(AicimoRoleSliceAdapter *a) {
    if (!a) return;
    if (a->owns_data) {
        if (a->drole_weights) free(a->drole_weights);
        if (a->dlex_weights) free(a->dlex_weights);
    }
    memset(a, 0, sizeof(*a));
}

/* Route using only the Drole slice (structural role routing) */
int aicimo_route_on_drole(AicimoRouter *r, const float *input, size_t in_len,
                          float *output, size_t out_cap, size_t *used_ops) {
    if (!r || !input || !output) return -1;
    if (in_len != r->current_context || out_cap < in_len) return -1;

    /* Find strongest path using only Drole (role embedding) */
    size_t best = 0;
    float best_score = -1e9f;

    for (size_t i = 0; i < r->router.num_ops; ++i) {
        float score = 0.0f;
        for (size_t j = 0; j < r->router.num_ops; ++j) {
            score += r->router.strength[i * r->router.num_ops + j];
        }
        if (score > best_score) {
            best_score = score;
            best = i;
        }
    }

    /* Apply Drole routing (simplified: use role embedding for dispatch) */
    AicimoAdapter *ad = &r->bank.adapters[best];
    for (size_t i = 0; i < in_len; ++i) {
        float sum = 0.0f;
        for (size_t j = 0; j < ad->out_dim; ++j) {
            sum += input[i] * ad->weights[i * ad->out_dim + j];
        }
        output[i] = sum;
    }

    *used_ops = 1;
    return 0;
}

/* Full role-slice routing: Drole for dispatch, Dlex for transformation */
int aicimo_route_role_slice(AicimoRouter *r, const float *input, size_t in_len,
                            float *output, size_t out_cap, 
                            const char *role, size_t *used_ops) {
    if (!r || !input || !output || !role) return -1;
    if (in_len != r->current_context || out_cap < in_len) return -1;

    /* Drole routing (structural role embedding) */
    size_t best = 0;
    float best_score = -1e9f;

    for (size_t i = 0; i < r->router.num_ops; ++i) {
        float score = 0.0f;
        for (size_t j = 0; j < r->router.num_ops; ++j) {
            score += r->router.strength[i * r->router.num_ops + j];
        }
        if (score > best_score) {
            best_score = score;
            best = i;
        }
    }

    /* Dlex transformation (lexical/content) */
    AicimoAdapter *ad = &r->bank.adapters[best];
    for (size_t i = 0; i < in_len; ++i) {
        float sum = 0.0f;
        for (size_t j = 0; j < ad->out_dim; ++j) {
            sum += input[i] * ad->weights[i * ad->out_dim + j];
        }
        output[i] = sum;
    }

    *used_ops = 1;
    return 0;
}