#ifndef CCE_AICIMO_H
#define CCE_AICIMO_H

#include <stddef.h>
#include <stdint.h>

/*
 * AICIMO — Adaptive Adapter Architecture ported to CNET (pure C)
 *
 * Core ideas translated from the meta-learning design:
 * - Adapter bank (B_op) with zero-init identity at creation
 * - GraphMoE-style strength matrices (zero-init, learned routing)
 * - Operation routing for dynamic composition of small units
 * - Stable meta-learning dynamics (no collapse on identity init)
 *
 * Goal: Allow CNET units to be treated as small adapters that can be
 * routed and composed at runtime to achieve effective 8K–16K+ context
 * without making every individual unit gigantic.
 */

typedef struct {
    float *weights;      /* [in_dim][out_dim] */
    size_t in_dim;
    size_t out_dim;
    int owns_data;
} AicimoAdapter;

typedef struct {
    AicimoAdapter *adapters;
    size_t count;
    size_t capacity;
} AicimoAdapterBank;

typedef struct {
    float *strength;     /* [num_ops][num_ops] — GraphMoE style */
    size_t num_ops;
    int owns_data;
} AicimoStrengthMatrix;

typedef struct {
    AicimoAdapterBank bank;
    AicimoStrengthMatrix router;
    size_t current_context;   /* effective context this router can handle */
} AicimoRouter;

/* Initialization with identity (zero-init + identity diagonal) */
int aicimo_adapter_init_identity(AicimoAdapter *a, size_t in_dim, size_t out_dim);
int aicimo_adapter_bank_init(AicimoAdapterBank *bank, size_t initial_capacity);
void aicimo_adapter_bank_free(AicimoAdapterBank *bank);

int aicimo_strength_matrix_init(AicimoStrengthMatrix *m, size_t num_ops);
void aicimo_strength_matrix_free(AicimoStrengthMatrix *m);

/* Routing */
int aicimo_router_init(AicimoRouter *r, size_t num_ops, size_t base_dim);
int aicimo_router_init_variable(AicimoRouter *r, size_t num_ops, size_t base_dim);
void aicimo_router_free(AicimoRouter *r);

int aicimo_route(AicimoRouter *r, const float *input, size_t in_len,
                 float *output, size_t out_cap, size_t *used_ops);

/* Composition: apply a sequence of adapters */
int aicimo_compose(AicimoRouter *r, const float *input, size_t in_len,
                   float *output, size_t out_cap);

#endif /* CCE_AICIMO_H */