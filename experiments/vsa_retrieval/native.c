/* Experimental CPU adapter. Uses CNET's canonical encoder and q8 operations.
 * No capsule format, certification policy, or production router is changed. */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "cnet_vsa_text.h"
#include "cnet_vsa_lexicon.h"
#include "cnet_vsa_gen_capsule.h"

#define DIM CNET_VSA_TOPICAL_DIM
static CnetVsaLexicon lex;
static int loaded;

int vr_dim(void) { return DIM; }

int vr_open(const char *path, uint64_t expected) {
    if (loaded) return -1;
    if (cnet_vsa_lexicon_load(&lex, path) != 0) return -2;
    if (lex.hdr.digest != expected) {
        cnet_vsa_lexicon_free(&lex);
        return -3;
    }
    cnet_vsa_lexicon_set_active(&lex);
    loaded = 1;
    return 0;
}

int vr_encode(const char *text, int intent, float *out) {
    if (!loaded || !text || !out) return -1;
    if (intent) {
        int8_t q[DIM];
        if (cnet_vsa_gencap_encode_intent_q8(text, q, CNET_VSA_ENCODER_LEX) != 0) return -2;
        for (int i = 0; i < DIM; ++i) out[i] = q[i];
        return 0;
    }
    CnetVsaTokenList tokens;
    if (cnet_vsa_text_tokenize(text, &tokens) <= 0) return -2;
    return cnet_vsa_text_encode_topical_wide(&tokens, out, CNET_VSA_ENCODER_LEX);
}

int vr_terms(const char *text, uint64_t *out, int capacity) {
    CnetVsaTokenList tokens;
    if (!text || !out || capacity < 0) return -1;
    if (cnet_vsa_text_tokenize(text, &tokens) < 0) return -2;
    int n = 0;
    for (size_t i = 0; i < tokens.count; ++i) {
        if (cnet_vsa_text_is_stopword(tokens.tokens[i].token)) continue;
        if (n == capacity) return -3;
        out[n++] = cnet_vsa_lexicon_word_key(tokens.tokens[i].token);
    }
    return n;
}

void vr_quantize(const float *v, int n, int8_t *out) {
    for (int i = 0; i < n; ++i)
        cnet_vsa_text_wide_to_q8(v + (size_t)i * DIM, out + (size_t)i * DIM);
}

void vr_dense(const int8_t *queries, int nq, const int8_t *p, const float *norms,
              const int32_t *owner, int np, int nc, float *out) {
    for (int q = 0; q < nq; ++q) {
        float *scores = out + (size_t)q * nc;
        for (int c = 0; c < nc; ++c) scores[c] = -2.0f;
        const int8_t *v = queries + (size_t)q * DIM;
        float norm = cnet_vsa_text_q8_norm(v);
        for (int i = 0; i < np; ++i) {
            float s = cnet_vsa_text_q8_similarity_n(v, norm, p + (size_t)i * DIM, norms[i]);
            if (s > scores[owner[i]]) scores[owner[i]] = s;
        }
    }
}

/* Inverted postings already contain BM25 weights, calculated offline. */
void vr_sparse(const uint32_t *terms, int nt, const uint32_t *offset,
               const int32_t *owner, const float *weight, int nc, float *out) {
    memset(out, 0, (size_t)nc * sizeof(float));
    for (int i = 0; i < nt; ++i)
        for (uint32_t p = offset[terms[i]]; p < offset[terms[i] + 1]; ++p)
            out[owner[p]] += weight[p];
}

/* Match query evidence within one source passage; preserve query term order.
 * This is a ranking feature, not an answer verifier or a negation parser. */
void vr_passage(const uint64_t *query, int nq, const int32_t *candidates, int nk,
                const uint64_t *terms, const uint32_t *doc_offset,
                const uint32_t *cap_offset, int nc, float *out) {
    memset(out, 0, (size_t)nc * sizeof(float));
    if (nq == 0) return;
    for (int c = 0; c < nk; ++c) {
        int ci = candidates[c];
        for (uint32_t doc = cap_offset[ci]; doc < cap_offset[ci + 1]; ++doc) {
            int hits = 0, pairs = 0;
            uint32_t begin = doc_offset[doc], end = doc_offset[doc + 1];
            for (int i = 0; i < nq; ++i) {
                int match = 0, pair = 0;
                for (uint32_t j = begin; j < end; ++j) {
                    if (terms[j] != query[i]) continue;
                    match = 1;
                    if (i + 1 < nq && j + 1 < end && terms[j + 1] == query[i + 1]) pair = 1;
                }
                hits += match;
                pairs += pair;
            }
            float score = 0.75f * hits / nq + (nq > 1 ? 0.25f * pairs / (nq - 1) : 0.25f * hits);
            if (score > out[ci]) out[ci] = score;
        }
    }
}
