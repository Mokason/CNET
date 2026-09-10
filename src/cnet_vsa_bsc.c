#include "../include/cnet_vsa_bsc.h"

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

void cnet_vsa_bsc_random(CnetVsaBsc *out, uint64_t *seed) {
    if (!out) return;
    for (int i = 0; i < CNET_VSA_BSC_WORDS; ++i) {
        out->w[i] = xorshift64(seed);
    }
}

void cnet_vsa_bsc_bind(CnetVsaBsc *out, const CnetVsaBsc *a, const CnetVsaBsc *b) {
    if (!out || !a || !b) return;
    for (int i = 0; i < CNET_VSA_BSC_WORDS; ++i) {
        out->w[i] = a->w[i] ^ b->w[i];
    }
}

void cnet_vsa_bsc_unbind(CnetVsaBsc *out, const CnetVsaBsc *bound, const CnetVsaBsc *key) {
    cnet_vsa_bsc_bind(out, bound, key);
}

int cnet_vsa_bsc_hamming(const CnetVsaBsc *a, const CnetVsaBsc *b) {
    if (!a || !b) return CNET_VSA_BSC_BITS;
    int dist = 0;
    for (int i = 0; i < CNET_VSA_BSC_WORDS; ++i) {
        dist += __builtin_popcountll(a->w[i] ^ b->w[i]);
    }
    return dist;
}

float cnet_vsa_bsc_similarity(const CnetVsaBsc *a, const CnetVsaBsc *b) {
    int h = cnet_vsa_bsc_hamming(a, b);
    return 1.0f - (2.0f * (float)h / (float)CNET_VSA_BSC_BITS);
}

void cnet_vsa_bsc_permute(CnetVsaBsc *out, const CnetVsaBsc *in, int shift) {
    if (!out || !in) return;
    int s = shift % CNET_VSA_BSC_BITS;
    if (s < 0) s += CNET_VSA_BSC_BITS;
    if (s == 0) {
        memcpy(out, in, sizeof(*out));
        return;
    }

    int word_shift = s / 64;
    int bit_shift = s % 64;

    memset(out, 0, sizeof(*out));

    if (bit_shift == 0) {
        for (int i = 0; i < CNET_VSA_BSC_WORDS; ++i) {
            out->w[(i + word_shift) % CNET_VSA_BSC_WORDS] = in->w[i];
        }
    } else {
        int r_shift = 64 - bit_shift;
        for (int i = 0; i < CNET_VSA_BSC_WORDS; ++i) {
            int target_low = (i + word_shift) % CNET_VSA_BSC_WORDS;
            int target_high = (i + word_shift + 1) % CNET_VSA_BSC_WORDS;
            out->w[target_low] |= (in->w[i] << bit_shift);
            out->w[target_high] |= (in->w[i] >> r_shift);
        }
    }
}

void cnet_vsa_bsc_bundle(CnetVsaBsc *out, const CnetVsaBsc *const *vectors, int count) {
    if (!out || !vectors || count <= 0) return;
    memset(out, 0, sizeof(*out));
    int threshold = count / 2;

    for (int word = 0; word < CNET_VSA_BSC_WORDS; ++word) {
        uint64_t w_out = 0;
        for (int bit = 0; bit < 64; ++bit) {
            uint64_t mask = 1ULL << bit;
            int votes = 0;
            for (int k = 0; k < count; ++k) {
                if (vectors[k] && (vectors[k]->w[word] & mask)) {
                    votes++;
                }
            }
            if (votes > threshold) {
                w_out |= mask;
            }
        }
        out->w[word] = w_out;
    }
}

int cnet_vsa_bsc_codebook_init(CnetVsaBscCodebook *cb, size_t capacity) {
    if (!cb || capacity == 0) return -1;
    cb->count = 0;
    cb->capacity = capacity;
    cb->names = (char (*)[CNET_VSA_NAME_MAX])calloc(capacity, sizeof(*cb->names));
    cb->vectors = (CnetVsaBsc *)aligned_alloc(64, capacity * sizeof(CnetVsaBsc));
    if (!cb->names || !cb->vectors) {
        cnet_vsa_bsc_codebook_free(cb);
        return -1;
    }
    return 0;
}

void cnet_vsa_bsc_codebook_free(CnetVsaBscCodebook *cb) {
    if (!cb) return;
    free(cb->names);
    free(cb->vectors);
    memset(cb, 0, sizeof(*cb));
}

int cnet_vsa_bsc_codebook_add(CnetVsaBscCodebook *cb, const char *name, const CnetVsaBsc *vec) {
    if (!cb || !name || !vec || cb->count >= cb->capacity) return -1;
    strncpy(cb->names[cb->count], name, CNET_VSA_NAME_MAX - 1);
    cb->names[cb->count][CNET_VSA_NAME_MAX - 1] = '\0';
    cb->vectors[cb->count] = *vec;
    cb->count++;
    return 0;
}

int cnet_vsa_bsc_codebook_cleanup(const CnetVsaBscCodebook *cb, const CnetVsaBsc *query,
                                   CnetVsaBsc *out_clean, char *out_name, size_t name_cap,
                                   int *out_hamming) {
    if (!cb || !query || cb->count == 0) return -1;
    int min_dist = CNET_VSA_BSC_BITS + 1;
    size_t best_idx = 0;

    for (size_t i = 0; i < cb->count; ++i) {
        int dist = cnet_vsa_bsc_hamming(query, &cb->vectors[i]);
        if (dist < min_dist) {
            min_dist = dist;
            best_idx = i;
        }
    }

    if (out_clean) {
        *out_clean = cb->vectors[best_idx];
    }
    if (out_name && name_cap > 0) {
        strncpy(out_name, cb->names[best_idx], name_cap - 1);
        out_name[name_cap - 1] = '\0';
    }
    if (out_hamming) {
        *out_hamming = min_dist;
    }
    return 0;
}
