#include "../include/cnet_vsa.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t xorshift64(uint64_t *state) {
    uint64_t x = *state;
    if (x == 0) x = 88172645463325252ULL;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *state = x;
    return x;
}

void cnet_vsa_random(float *out, int dim, uint64_t *seed) {
    if (!out || dim <= 0) return;
    float inv_sqrt = 1.0f / (float)sqrt((double)dim);
    for (int i = 0; i < dim; ++i) {
        out[i] = (xorshift64(seed) & 1) ? inv_sqrt : -inv_sqrt;
    }
}

float cnet_vsa_normalize(float *v, int dim) {
    if (!v || dim <= 0) return 0.0f;
    double sum = 0.0;
    for (int i = 0; i < dim; ++i) {
        sum += (double)v[i] * (double)v[i];
    }
    float norm = (float)sqrt(sum);
    if (norm > 1e-12f) {
        float inv = 1.0f / norm;
        for (int i = 0; i < dim; ++i) {
            v[i] *= inv;
        }
    } else {
        memset(v, 0, (size_t)dim * sizeof(float));
    }
    return norm;
}

float cnet_vsa_similarity(const float *a, const float *b, int dim) {
    if (!a || !b || dim <= 0) return 0.0f;
    double dot = 0.0;
    for (int i = 0; i < dim; ++i) {
        dot += (double)a[i] * (double)b[i];
    }
    return (float)dot;
}

float cnet_vsa_distance(const float *a, const float *b, int dim) {
    if (!a || !b || dim <= 0) return 2.0f;
    double sum = 0.0;
    for (int i = 0; i < dim; ++i) {
        double diff = (double)a[i] - (double)b[i];
        sum += diff * diff;
    }
    return (float)sqrt(sum);
}

void cnet_vsa_bundle(float *out, const float **vectors, const float *weights,
                     int count, int dim) {
    if (!out || !vectors || count <= 0 || dim <= 0) return;
    double accum[CNET_VSA_DEFAULT_DIM];
    double *p_accum = accum;
    if (dim > CNET_VSA_DEFAULT_DIM) {
        p_accum = (double *)calloc((size_t)dim, sizeof(double));
    } else {
        memset(accum, 0, (size_t)dim * sizeof(double));
    }

    for (int c = 0; c < count; ++c) {
        if (!vectors[c]) continue;
        double w = weights ? (double)weights[c] : 1.0;
        for (int i = 0; i < dim; ++i) {
            p_accum[i] += w * (double)vectors[c][i];
        }
    }

    for (int i = 0; i < dim; ++i) {
        out[i] = (float)p_accum[i];
    }
    cnet_vsa_normalize(out, dim);

    if (p_accum != accum) {
        free(p_accum);
    }
}

void cnet_vsa_bind(float *out, const float *a, const float *b, int dim) {
    if (!out || !a || !b || dim <= 0) return;
    for (int i = 0; i < dim; ++i) {
        out[i] = a[i] * b[i];
    }
    cnet_vsa_normalize(out, dim);
}

void cnet_vsa_unbind(float *out, const float *bound, const float *key, int dim) {
    if (!out || !bound || !key || dim <= 0) return;
    /* In normalized bipolar VSA, key * key ~ 1. Unbinding is identical to binding with key. */
    for (int i = 0; i < dim; ++i) {
        out[i] = bound[i] * key[i];
    }
    cnet_vsa_normalize(out, dim);
}

void cnet_vsa_permute(float *out, const float *in, int shift, int dim) {
    if (!out || !in || dim <= 0) return;
    int s = shift % dim;
    if (s < 0) s += dim;
    for (int i = 0; i < dim; ++i) {
        int target = (i + s) % dim;
        out[target] = in[i];
    }
}

int cnet_vsa_codebook_init(CnetVsaCodebook *cb, int dim, size_t capacity) {
    if (!cb || dim <= 0 || capacity == 0) return -1;
    cb->dim = dim;
    cb->count = 0;
    cb->capacity = capacity;
    cb->names = (char (*)[CNET_VSA_NAME_MAX])calloc(capacity, sizeof(*cb->names));
    cb->vectors = (float *)calloc(capacity * (size_t)dim, sizeof(float));
    if (!cb->names || !cb->vectors) {
        cnet_vsa_codebook_free(cb);
        return -1;
    }
    return 0;
}

void cnet_vsa_codebook_free(CnetVsaCodebook *cb) {
    if (!cb) return;
    free(cb->names);
    free(cb->vectors);
    memset(cb, 0, sizeof(*cb));
}

int cnet_vsa_codebook_add(CnetVsaCodebook *cb, const char *name, const float *vec) {
    if (!cb || !name || !vec || cb->count >= cb->capacity) return -1;
    strncpy(cb->names[cb->count], name, CNET_VSA_NAME_MAX - 1);
    cb->names[cb->count][CNET_VSA_NAME_MAX - 1] = '\0';
    memcpy(&cb->vectors[cb->count * (size_t)cb->dim], vec, (size_t)cb->dim * sizeof(float));
    cb->count++;
    return 0;
}

int cnet_vsa_codebook_find(const CnetVsaCodebook *cb, const char *name, float *vec_out) {
    if (!cb || !name || !vec_out) return -1;
    for (size_t i = 0; i < cb->count; ++i) {
        if (strncmp(cb->names[i], name, CNET_VSA_NAME_MAX) == 0) {
            memcpy(vec_out, &cb->vectors[i * (size_t)cb->dim], (size_t)cb->dim * sizeof(float));
            return 0;
        }
    }
    return -1;
}

int cnet_vsa_codebook_cleanup(const CnetVsaCodebook *cb, const float *noisy_vec,
                              float *out_clean, char *out_name, size_t name_cap,
                              float *out_sim) {
    if (!cb || !noisy_vec || cb->count == 0) return -1;
    float best_sim = -2.0f;
    size_t best_idx = 0;

    for (size_t i = 0; i < cb->count; ++i) {
        float sim = cnet_vsa_similarity(noisy_vec, &cb->vectors[i * (size_t)cb->dim], cb->dim);
        if (sim > best_sim) {
            best_sim = sim;
            best_idx = i;
        }
    }

    if (out_clean) {
        memcpy(out_clean, &cb->vectors[best_idx * (size_t)cb->dim], (size_t)cb->dim * sizeof(float));
    }
    if (out_name && name_cap > 0) {
        strncpy(out_name, cb->names[best_idx], name_cap - 1);
        out_name[name_cap - 1] = '\0';
    }
    if (out_sim) {
        *out_sim = best_sim;
    }
    return 0;
}

int cnet_vsa_contract_verify(const CnetVsaMetricContract *contract,
                             const float *input_vec, int *admitted,
                             float *out_margin) {
    if (!contract || !input_vec || !admitted) return -1;
    *admitted = 0;
    float dist = cnet_vsa_distance(contract->centroid, input_vec, contract->dim);
    float margin = contract->radius_epsilon - dist;
    if (out_margin) {
        *out_margin = margin;
    }

    /* Calibrated Fail-Closed Abstention */
    if (dist <= contract->radius_epsilon && margin >= contract->margin_floor) {
        *admitted = 1;
        return 0;
    }
    return 1;
}
