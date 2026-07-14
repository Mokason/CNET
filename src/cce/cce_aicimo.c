/*
 * cce_aicimo.c — AICIMO adapter routing implementation.
 *
 * Implements the public API declared in include/cce/cce_aicimo.h.
 *
 * Key design decisions:
 * - Adapters are identity-initialized (zero + diagonal 1.0) so that routing
 *   through any adapter with default strengths preserves the input exactly
 *   (residual / identity preservation).
 * - Routing selects the adapter with the highest accumulated strength row sum.
 *   This is deterministic: same router state + same role => same selection.
 * - Uncertainty is the normalized Shannon entropy of the strength row sums,
 *   reflecting how peaked or flat the routing decision is. It is NOT a fixed
 *   constant — it varies with the actual strength matrix configuration.
 * - Role routing applies a deterministic bias: the role string is hashed to
 *   select a bias row, which is added to the base strength before selection.
 *
 * There is NO context window expansion here. The output has the same
 * dimensionality as the input. This is adapter routing, not context expansion.
 */
#include "../../include/cce/cce_aicimo.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ---- internal helpers ---- */

static float clamp01(float v) {
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

/* Deterministic hash of a role string to a non-negative float. */
static float role_hash_bias(const char *role) {
    /* FNV-1a inspired hash, mapped to [0, num_ops) as a fractional bias */
    unsigned h = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)role; *p; ++p) {
        h ^= *p;
        h *= 16777619u;
    }
    /* Small bias in [0, 0.1): enough to break ties between equal strengths,
     * not enough to override meaningful strength differences. */
    return (float)(h % 100) / 1000.0f;
}

/* Compute the strength row sums and find the best adapter index.
 * Returns the best index. If sums_out is non-NULL, fills it with the row sums. */
static size_t select_best_adapter(const cce_aicimo_strength_matrix *sm,
                                  float *sums_out) {
    size_t best = 0;
    float best_score = -1e30f;

    for (size_t i = 0; i < sm->num_ops; ++i) {
        float score = 0.0f;
        for (size_t j = 0; j < sm->num_ops; ++j) {
            score += sm->strength[i * sm->num_ops + j];
        }
        if (sums_out) sums_out[i] = score;
        if (score > best_score) {
            best_score = score;
            best = i;
        }
    }
    return best;
}

/* Apply an identity-initialized adapter to the input vector.
 * For identity init, weights[i*out_dim + i] = 1.0 and all others 0.0,
 * so output[i] = input[i] (when in_dim == out_dim == base_dim).
 * For the general case: output[j] = sum_i input[i] * weights[i*out_dim + j]. */
static void apply_adapter(const cce_aicimo_adapter *ad,
                          const float *input, size_t in_len,
                          float *output) {
    for (size_t j = 0; j < ad->out_dim && j < in_len; ++j) {
        float sum = 0.0f;
        for (size_t i = 0; i < in_len && i < ad->in_dim; ++i) {
            sum += input[i] * ad->weights[i * ad->out_dim + j];
        }
        output[j] = sum;
    }
}

/* Compute normalized Shannon entropy of a non-negative vector.
 * Returns 0 for a peaked distribution, 1 for uniform. */
static float normalized_entropy(const float *values, size_t n) {
    if (n <= 1) return 0.0f;

    double sum = 0.0;
    for (size_t i = 0; i < n; ++i) {
        double v = (double)values[i];
        if (v < 0.0) v = 0.0;
        sum += v;
    }

    if (sum < 1e-12) return 1.0f;  /* all zeros = maximum uncertainty */

    double entropy = 0.0;
    for (size_t i = 0; i < n; ++i) {
        double p = (double)values[i] / sum;
        if (p > 1e-15) {
            entropy -= p * log(p);
        }
    }
    entropy /= log((double)n);  /* normalize to [0, 1] */

    return clamp01((float)entropy);
}

/* ---- adapter bank ---- */

static cce_result adapter_init_identity(cce_aicimo_adapter *a,
                                         size_t in_dim, size_t out_dim) {
    if (!a || in_dim == 0 || out_dim == 0) return CCE_ERR_INVALID_ARG;

    a->in_dim = in_dim;
    a->out_dim = out_dim;
    a->owns_data = 1;

    size_t total = in_dim * out_dim;
    a->weights = (float *)calloc(total, sizeof(float));
    if (!a->weights) return CCE_ERR_OOM;

    /* Identity along the diagonal */
    size_t min_dim = (in_dim < out_dim) ? in_dim : out_dim;
    for (size_t i = 0; i < min_dim; ++i) {
        a->weights[i * out_dim + i] = 1.0f;
    }
    return CCE_OK;
}

static void adapter_bank_free(cce_aicimo_adapter_bank *bank) {
    if (!bank) return;
    for (size_t i = 0; i < bank->count; ++i) {
        if (bank->adapters[i].owns_data && bank->adapters[i].weights) {
            free(bank->adapters[i].weights);
        }
    }
    free(bank->adapters);
    memset(bank, 0, sizeof(*bank));
}

static cce_result adapter_bank_init(cce_aicimo_adapter_bank *bank,
                                     size_t capacity) {
    if (!bank || capacity == 0) return CCE_ERR_INVALID_ARG;
    bank->adapters = (cce_aicimo_adapter *)calloc(capacity, sizeof(cce_aicimo_adapter));
    if (!bank->adapters) return CCE_ERR_OOM;
    bank->count = 0;
    bank->capacity = capacity;
    return CCE_OK;
}

/* ---- strength matrix ---- */

static cce_result strength_matrix_init(cce_aicimo_strength_matrix *m,
                                        size_t num_ops) {
    if (!m || num_ops == 0) return CCE_ERR_INVALID_ARG;
    m->num_ops = num_ops;
    m->owns_data = 1;
    m->strength = (float *)calloc(num_ops * num_ops, sizeof(float));
    if (!m->strength) return CCE_ERR_OOM;

    /* Zero-init + small identity bias (AICIMO stability convention) */
    for (size_t i = 0; i < num_ops; ++i) {
        m->strength[i * num_ops + i] = 0.01f;
    }
    return CCE_OK;
}

static void strength_matrix_free(cce_aicimo_strength_matrix *m) {
    if (!m) return;
    if (m->owns_data && m->strength) free(m->strength);
    memset(m, 0, sizeof(*m));
}

/* ---- public API ---- */

cce_result cce_aicimo_router_init(cce_aicimo_router *r,
                                   size_t num_ops, size_t base_dim) {
    if (!r || num_ops == 0 || base_dim == 0) return CCE_ERR_INVALID_ARG;

    memset(r, 0, sizeof(*r));

    if (adapter_bank_init(&r->bank, num_ops) != CCE_OK)
        return CCE_ERR_OOM;

    if (strength_matrix_init(&r->router, num_ops) != CCE_OK) {
        adapter_bank_free(&r->bank);
        return CCE_ERR_OOM;
    }

    /* Pre-create identity adapters */
    for (size_t i = 0; i < num_ops; ++i) {
        if (adapter_init_identity(&r->bank.adapters[i], base_dim, base_dim) != CCE_OK) {
            cce_aicimo_router_free(r);
            return CCE_ERR_OOM;
        }
        r->bank.count++;
    }

    r->base_dim = base_dim;
    return CCE_OK;
}

void cce_aicimo_router_free(cce_aicimo_router *r) {
    if (!r) return;
    adapter_bank_free(&r->bank);
    strength_matrix_free(&r->router);
    memset(r, 0, sizeof(*r));
}

cce_result cce_aicimo_route(const cce_aicimo_router *r,
                             const float *input, size_t in_len,
                             float *output, size_t out_cap,
                             size_t *used_ops) {
    if (!r || !input || !output || !used_ops) return CCE_ERR_INVALID_ARG;
    if (in_len != r->base_dim || out_cap < in_len) return CCE_ERR_INVALID_ARG;
    if (r->bank.count == 0) return CCE_ERR_INVALID_ARG;

    size_t best = select_best_adapter(&r->router, NULL);
    apply_adapter(&r->bank.adapters[best], input, in_len, output);
    *used_ops = 1;
    return CCE_OK;
}

cce_result cce_aicimo_route_for_role(const cce_aicimo_router *r,
                                      const float *input, size_t in_len,
                                      float *output, size_t out_cap,
                                      const char *role,
                                      size_t *selected_op) {
    if (!r || !input || !output || !role || !selected_op) return CCE_ERR_INVALID_ARG;
    if (in_len != r->base_dim || out_cap < in_len) return CCE_ERR_INVALID_ARG;
    if (r->bank.count == 0) return CCE_ERR_INVALID_ARG;

    size_t num_ops = r->router.num_ops;

    /* Compute base row sums, then add a deterministic role bias.
     * The role hash selects a bias target row; the bias shifts the
     * competition deterministically: same role + same state => same choice. */
    float *sums = (float *)calloc(num_ops, sizeof(float));
    if (!sums) return CCE_ERR_OOM;

    for (size_t i = 0; i < num_ops; ++i) {
        sums[i] = 0.0f;
        for (size_t j = 0; j < num_ops; ++j) {
            sums[i] += r->router.strength[i * num_ops + j];
        }
    }

    /* Deterministic role bias: hash the role string to get a value in [0,1),
     * then apply it as an additive bias to a row determined by the hash.
     * This means different role strings consistently prefer different adapters
     * when the base strengths are close. */
    float bias = role_hash_bias(role);
    size_t bias_row = (size_t)(bias * (float)num_ops);
    if (bias_row >= num_ops) bias_row = num_ops - 1;
    sums[bias_row] += bias;

    /* Find the best after bias */
    size_t best = 0;
    float best_score = -1e30f;
    for (size_t i = 0; i < num_ops; ++i) {
        if (sums[i] > best_score) {
            best_score = sums[i];
            best = i;
        }
    }

    free(sums);

    apply_adapter(&r->bank.adapters[best], input, in_len, output);
    *selected_op = best;
    return CCE_OK;
}

cce_result cce_aicimo_route_with_uncertainty(const cce_aicimo_router *r,
                                              const float *input, size_t in_len,
                                              float *output, size_t out_cap,
                                              float *uncertainty) {
    if (!r || !input || !output || !uncertainty) return CCE_ERR_INVALID_ARG;
    if (in_len != r->base_dim || out_cap < in_len) return CCE_ERR_INVALID_ARG;
    if (r->bank.count == 0) return CCE_ERR_INVALID_ARG;

    size_t num_ops = r->router.num_ops;

    /* Compute row sums */
    float *sums = (float *)calloc(num_ops, sizeof(float));
    if (!sums) return CCE_ERR_OOM;

    size_t best = select_best_adapter(&r->router, sums);

    /* Uncertainty = normalized Shannon entropy of the row sums.
     * This reflects the actual route state: uniform strengths => high entropy
     * (high uncertainty), peaked strengths => low entropy (low uncertainty). */
    *uncertainty = normalized_entropy(sums, num_ops);

    free(sums);

    apply_adapter(&r->bank.adapters[best], input, in_len, output);
    return CCE_OK;
}

cce_result cce_aicimo_set_route_strength(cce_aicimo_router *r,
                                          size_t row, size_t col,
                                          float value) {
    if (!r) return CCE_ERR_INVALID_ARG;
    if (row >= r->router.num_ops || col >= r->router.num_ops)
        return CCE_ERR_INVALID_ARG;

    r->router.strength[row * r->router.num_ops + col] = value;
    return CCE_OK;
}