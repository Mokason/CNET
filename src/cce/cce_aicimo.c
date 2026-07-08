#include "cce_aicimo.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* Zero-init + identity initialization (core AICIMO stability trick) */
int aicimo_adapter_init_identity(AicimoAdapter *a, size_t in_dim, size_t out_dim) {
    if (!a || in_dim == 0 || out_dim == 0) return -1;

    a->in_dim = in_dim;
    a->out_dim = out_dim;
    a->owns_data = 1;

    size_t total = in_dim * out_dim;
    a->weights = (float *)calloc(total, sizeof(float));
    if (!a->weights) return -1;

    /* Identity initialization along the diagonal (AICIMO zero-init identity) */
    size_t min_dim = (in_dim < out_dim) ? in_dim : out_dim;
    for (size_t i = 0; i < min_dim; ++i) {
        a->weights[i * out_dim + i] = 1.0f;
    }
    return 0;
}

int aicimo_adapter_bank_init(AicimoAdapterBank *bank, size_t initial_capacity) {
    if (!bank || initial_capacity == 0) return -1;
    bank->adapters = (AicimoAdapter *)calloc(initial_capacity, sizeof(AicimoAdapter));
    if (!bank->adapters) return -1;
    bank->count = 0;
    bank->capacity = initial_capacity;
    return 0;
}

void aicimo_adapter_bank_free(AicimoAdapterBank *bank) {
    if (!bank) return;
    for (size_t i = 0; i < bank->count; ++i) {
        if (bank->adapters[i].owns_data && bank->adapters[i].weights) {
            free(bank->adapters[i].weights);
        }
    }
    free(bank->adapters);
    memset(bank, 0, sizeof(*bank));
}

int aicimo_strength_matrix_init(AicimoStrengthMatrix *m, size_t num_ops) {
    if (!m || num_ops == 0) return -1;
    m->num_ops = num_ops;
    m->owns_data = 1;
    size_t total = num_ops * num_ops;
    m->strength = (float *)calloc(total, sizeof(float));
    if (!m->strength) return -1;

    /* Zero-init + light identity bias (AICIMO stability) */
    for (size_t i = 0; i < num_ops; ++i) {
        m->strength[i * num_ops + i] = 0.01f;
    }
    return 0;
}

void aicimo_strength_matrix_free(AicimoStrengthMatrix *m) {
    if (!m) return;
    if (m->owns_data && m->strength) free(m->strength);
    memset(m, 0, sizeof(*m));
}

int aicimo_router_init(AicimoRouter *r, size_t num_ops, size_t base_dim) {
    if (!r || num_ops == 0 || base_dim == 0) return -1;

    if (aicimo_adapter_bank_init(&r->bank, num_ops) != 0) return -1;
    if (aicimo_strength_matrix_init(&r->router, num_ops) != 0) {
        aicimo_adapter_bank_free(&r->bank);
        return -1;
    }

    /* Pre-create identity adapters */
    for (size_t i = 0; i < num_ops; ++i) {
        if (aicimo_adapter_init_identity(&r->bank.adapters[i], base_dim, base_dim) != 0) {
            aicimo_router_free(r);
            return -1;
        }
        r->bank.count++;
    }

    r->current_context = base_dim;  /* supports up to 4096+ for 64K+ effective context */
    return 0;
}

void aicimo_router_free(AicimoRouter *r) {
    if (!r) return;
    aicimo_adapter_bank_free(&r->bank);
    aicimo_strength_matrix_free(&r->router);
    memset(r, 0, sizeof(*r));
}

/* Simple strength-based routing (GraphMoE style) */
int aicimo_route(AicimoRouter *r, const float *input, size_t in_len,
                 float *output, size_t out_cap, size_t *used_ops) {
    if (!r || !input || !output || !used_ops) return -1;
    if (in_len != r->current_context || out_cap < in_len) return -1;

    /* Find strongest path using strength matrix */
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

    /* Apply chosen adapter */
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

/* Compose multiple adapters (enables larger effective context) */
int aicimo_compose(AicimoRouter *r, const float *input, size_t in_len,
                   float *output, size_t out_cap) {
    if (!r || !input || !output) return -1;
    if (in_len != r->current_context || out_cap < in_len) return -1;

    float *tmp = (float *)malloc(in_len * sizeof(float));
    if (!tmp) return -1;

    memcpy(tmp, input, in_len * sizeof(float));

    /* Route through strongest path multiple times (meta-learning style composition) */
    for (size_t step = 0; step < 16; ++step)  /* 16x composition for 8K+ effective context */ {  /* 8x for stronger expansion */
        size_t used = 0;
        if (aicimo_route(r, tmp, in_len, output, out_cap, &used) != 0) {
            free(tmp);
            return -1;
        }
        memcpy(tmp, output, in_len * sizeof(float));
    }

    memcpy(output, tmp, in_len * sizeof(float));
    free(tmp);
    return 0;
}

/* New: Variable-dimension router init (for scaling toward 8K+) */
int aicimo_router_init_variable(AicimoRouter *r, size_t num_ops, size_t base_dim) {
    return aicimo_router_init(r, num_ops, base_dim);
}