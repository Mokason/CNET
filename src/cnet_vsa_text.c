#include "../include/cnet_vsa_text.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static uint64_t fnv1a64(const char *str) {
    uint64_t hash = 14695981039346656037ULL;
    while (*str) {
        hash ^= (uint8_t)(*str++);
        hash *= 1099511628211ULL;
    }
    return hash;
}

static uint64_t xorshift64(uint64_t *state) {
    uint64_t x = *state;
    if (x == 0) x = 88172645463325252ULL;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *state = x;
    return x;
}

void cnet_vsa_text_token_vec(const char *token, float *out_vec, int dim) {
    if (!token || !out_vec || dim <= 0) return;
    uint64_t seed = fnv1a64(token);
    float inv_sqrt = 1.0f / sqrtf((float)dim);
    for (int i = 0; i < dim; ++i) {
        out_vec[i] = (xorshift64(&seed) & 1) ? inv_sqrt : -inv_sqrt;
    }
}

void cnet_vsa_text_token_bsc(const char *token, CnetVsaBsc *out_bsc) {
    if (!token || !out_bsc) return;
    uint64_t seed = fnv1a64(token);
    for (int i = 0; i < CNET_VSA_BSC_WORDS; ++i) {
        out_bsc->w[i] = xorshift64(&seed);
    }
}

int cnet_vsa_text_tokenize(const char *text, CnetVsaTokenList *out_list) {
    if (!text || !out_list) return -1;
    memset(out_list, 0, sizeof(*out_list));

    const char *p = text;
    int pos = 0;

    while (*p && out_list->count < CNET_VSA_MAX_TOKENS) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;

        char buf[CNET_VSA_TOKEN_LEN];
        size_t len = 0;

        if (isalnum((unsigned char)*p) || *p == '_') {
            while (*p && (isalnum((unsigned char)*p) || *p == '_') && len < sizeof(buf) - 1) {
                buf[len++] = (char)tolower((unsigned char)*p++);
            }
        } else {
            buf[len++] = *p++;
        }
        buf[len] = '\0';

        CnetVsaToken *t = &out_list->tokens[out_list->count++];
        snprintf(t->token, sizeof(t->token), "%s", buf);
        t->position = pos++;
    }

    return (int)out_list->count;
}

int cnet_vsa_text_encode_continuous(const CnetVsaTokenList *tokens, float *out_seq_vec, int dim) {
    if (!tokens || !out_seq_vec || dim <= 0 || tokens->count == 0) return -1;

    double accum[CNET_VSA_DEFAULT_DIM] = {0};
    double *p_accum = accum;
    if (dim > CNET_VSA_DEFAULT_DIM) {
        p_accum = (double*)calloc((size_t)dim, sizeof(double));
    }

    for (size_t i = 0; i < tokens->count; ++i) {
        float base_vec[CNET_VSA_DEFAULT_DIM];
        float perm_vec[CNET_VSA_DEFAULT_DIM];

        cnet_vsa_text_token_vec(tokens->tokens[i].token, base_vec, dim);
        /* Circular roll by position index */
        cnet_vsa_permute(perm_vec, base_vec, (int)i, dim);

        for (int j = 0; j < dim; ++j) {
            p_accum[j] += (double)perm_vec[j];
        }
    }

    for (int j = 0; j < dim; ++j) {
        out_seq_vec[j] = (float)p_accum[j];
    }
    cnet_vsa_normalize(out_seq_vec, dim);

    if (p_accum != accum) free(p_accum);
    return 0;
}

int cnet_vsa_text_encode_bsc(const CnetVsaTokenList *tokens, CnetVsaBsc *out_seq_bsc) {
    if (!tokens || !out_seq_bsc || tokens->count == 0) return -1;

    CnetVsaBsc perm_list[CNET_VSA_MAX_TOKENS];
    for (size_t i = 0; i < tokens->count; ++i) {
        CnetVsaBsc base_bsc;
        cnet_vsa_text_token_bsc(tokens->tokens[i].token, &base_bsc);
        /* Shift each position by (i * 7 + 1) bits for quasi-orthogonal dispersion */
        cnet_vsa_bsc_permute(&perm_list[i], &base_bsc, (int)(i * 7 + 1));
    }

    const CnetVsaBsc *ptrs[CNET_VSA_MAX_TOKENS];
    for (size_t i = 0; i < tokens->count; ++i) {
        ptrs[i] = &perm_list[i];
    }
    cnet_vsa_bsc_bundle(out_seq_bsc, ptrs, (int)tokens->count);
    return 0;
}

int cnet_vsa_text_decode_at_pos(const float *seq_vec, int pos, const CnetVsaCodebook *vocab,
                                char *out_token, size_t max_len, float *out_sim) {
    if (!seq_vec || !vocab || !out_token || max_len == 0) return -1;

    int D = vocab->dim;
    float probe[CNET_VSA_DEFAULT_DIM];
    /* Inverse permutation roll: -pos */
    cnet_vsa_permute(probe, seq_vec, -pos, D);

    return cnet_vsa_codebook_cleanup(vocab, probe, NULL, out_token, max_len, out_sim);
}

int cnet_vsa_text_find_token_pos(const float *seq_vec, const char *token, int max_pos,
                                  int *out_pos, float *out_sim, int dim) {
    if (!seq_vec || !token || max_pos <= 0 || dim <= 0) return -1;

    float base_vec[CNET_VSA_DEFAULT_DIM];
    cnet_vsa_text_token_vec(token, base_vec, dim);

    float best_sim = -2.0f;
    int best_pos = -1;

    for (int p = 0; p < max_pos; ++p) {
        float perm_vec[CNET_VSA_DEFAULT_DIM];
        cnet_vsa_permute(perm_vec, base_vec, p, dim);
        float sim = cnet_vsa_similarity(perm_vec, seq_vec, dim);
        if (sim > best_sim) {
            best_sim = sim;
            best_pos = p;
        }
    }

    if (out_pos) *out_pos = best_pos;
    if (out_sim) *out_sim = best_sim;
    return 0;
}

float cnet_vsa_text_sequence_similarity(const char *text_a, const char *text_b, int dim) {
    if (!text_a || !text_b || dim <= 0) return 0.0f;

    CnetVsaTokenList list_a, list_b;
    if (cnet_vsa_text_tokenize(text_a, &list_a) <= 0) return 0.0f;
    if (cnet_vsa_text_tokenize(text_b, &list_b) <= 0) return 0.0f;

    float vec_a[CNET_VSA_DEFAULT_DIM];
    float vec_b[CNET_VSA_DEFAULT_DIM];

    cnet_vsa_text_encode_continuous(&list_a, vec_a, dim);
    cnet_vsa_text_encode_continuous(&list_b, vec_b, dim);

    return cnet_vsa_similarity(vec_a, vec_b, dim);
}

int cnet_vsa_text_is_stopword(const char *token) {
    if (!token || !*token) return 1;

    /* Punctuation and non-alphanumeric tokens are purely syntactic delimiters, not topical content */
    int has_alnum = 0;
    for (const char *p = token; *p; ++p) {
        if (isalnum((unsigned char)*p)) {
            has_alnum = 1;
            break;
        }
    }
    if (!has_alnum) return 1;

    static const char * const stopwords[] = {
        "the", "a", "an", "is", "was", "are", "were", "to", "in", "on",
        "of", "and", "or", "for", "with", "from", "at", "by", "this",
        "that", "it", "its", "as", "be", "than", "there", "all", "so",
        "if", "into", "up", "out", "he", "she", "they", "we", "i", "you",
        "how", "what", "which", "where", "when", "why", "who", "do", "does",
        "did", "can", "could", "would", "should", "have", "has", "had",
        "will", "may", "might", "differ", "between",
        NULL
    };
    for (int i = 0; stopwords[i]; ++i) {
        if (strcmp(token, stopwords[i]) == 0) return 1;
    }
    return 0;
}

int cnet_vsa_text_encode_topical(const CnetVsaTokenList *tokens, float *out_topical_vec, int dim) {
    if (!tokens || !out_topical_vec || dim <= 0 || tokens->count == 0) return -1;

    double accum[CNET_VSA_DEFAULT_DIM] = {0};
    double *p_accum = accum;
    if (dim > CNET_VSA_DEFAULT_DIM) {
        p_accum = (double*)calloc((size_t)dim, sizeof(double));
        if (!p_accum) return -2;
    }

    int content_count = 0;
    for (size_t i = 0; i < tokens->count; ++i) {
        if (cnet_vsa_text_is_stopword(tokens->tokens[i].token)) continue;

        float base_vec[CNET_VSA_DEFAULT_DIM];
        cnet_vsa_text_token_vec(tokens->tokens[i].token, base_vec, dim);

        for (int j = 0; j < dim; ++j) {
            p_accum[j] += (double)base_vec[j];
        }
        content_count++;
    }

    /* Fallback: if all tokens were stopwords, encode all tokens */
    if (content_count == 0) {
        for (size_t i = 0; i < tokens->count; ++i) {
            float base_vec[CNET_VSA_DEFAULT_DIM];
            cnet_vsa_text_token_vec(tokens->tokens[i].token, base_vec, dim);

            for (int j = 0; j < dim; ++j) {
                p_accum[j] += (double)base_vec[j];
            }
        }
    }

    for (int j = 0; j < dim; ++j) {
        out_topical_vec[j] = (float)p_accum[j];
    }
    cnet_vsa_normalize(out_topical_vec, dim);

    if (p_accum != accum) free(p_accum);
    return 0;
}

float cnet_vsa_text_topical_similarity(const char *text_a, const char *text_b, int dim) {
    if (!text_a || !text_b || dim <= 0) return 0.0f;

    CnetVsaTokenList list_a, list_b;
    if (cnet_vsa_text_tokenize(text_a, &list_a) <= 0) return 0.0f;
    if (cnet_vsa_text_tokenize(text_b, &list_b) <= 0) return 0.0f;

    float vec_a[CNET_VSA_DEFAULT_DIM];
    float vec_b[CNET_VSA_DEFAULT_DIM];

    if (cnet_vsa_text_encode_topical(&list_a, vec_a, dim) != 0) return 0.0f;
    if (cnet_vsa_text_encode_topical(&list_b, vec_b, dim) != 0) return 0.0f;

    return cnet_vsa_similarity(vec_a, vec_b, dim);
}

