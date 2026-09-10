#ifndef CNET_VSA_TEXT_H
#define CNET_VSA_TEXT_H

#include <stddef.h>
#include <stdint.h>
#include "cnet_vsa.h"
#include "cnet_vsa_bsc.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CNET_VSA_MAX_TOKENS 1024
#define CNET_VSA_TOKEN_LEN 64

typedef struct {
    char token[CNET_VSA_TOKEN_LEN];
    int position;
} CnetVsaToken;

typedef struct {
    size_t count;
    CnetVsaToken tokens[CNET_VSA_MAX_TOKENS];
} CnetVsaTokenList;

/* Deterministic token to hypervector projection (Continuous and BSC) */
void cnet_vsa_text_token_vec(const char *token, float *out_vec, int dim);
void cnet_vsa_text_token_bsc(const char *token, CnetVsaBsc *out_bsc);

/* Tokenize raw UTF-8 string into word/punctuation tokens */
int  cnet_vsa_text_tokenize(const char *text, CnetVsaTokenList *out_list);

/* Positional sequence encoding for continuous vectors:
   V_seq = normalize( sum_{i=0}^{N-1} \Pi^i( E(token_i) ) ) */
int  cnet_vsa_text_encode_continuous(const CnetVsaTokenList *tokens, float *out_seq_vec, int dim);

/* Positional sequence encoding for packed 512-bit BSC:
   V_seq = majority_vote( \Pi^i( E_bsc(token_i) ) ) */
int  cnet_vsa_text_encode_bsc(const CnetVsaTokenList *tokens, CnetVsaBsc *out_seq_bsc);

/* Extract token at position pos from sequence vector by inverse permutation:
   probe = \Pi^{-pos}(V_seq).
   Matches probe against vocabulary codebook to recover token text and similarity. */
int  cnet_vsa_text_decode_at_pos(const float *seq_vec, int pos, const CnetVsaCodebook *vocab,
                                char *out_token, size_t max_len, float *out_sim);

/* Locate position of a query token in sequence vector:
   Tests correlation with \Pi^k(E(token)) for k in 0..max_pos-1.
   Returns best position and similarity. */
int  cnet_vsa_text_find_token_pos(const float *seq_vec, const char *token, int max_pos,
                                  int *out_pos, float *out_sim, int dim);

/* Compute order-sensitive sequence similarity:
   Distinguishes anagram sentences (e.g. "dog bit cat" vs "cat bit dog") */
float cnet_vsa_text_sequence_similarity(const char *text_a, const char *text_b, int dim);

/* Check if token is an ultra-common grammatical stopword */
int  cnet_vsa_text_is_stopword(const char *token);

/* Order-invariant topical semantic encoding (stopword-filtered superposition):
   V_topical = normalize( sum_{w \in tokens, !is_stopword(w)} E(w) ) */
int  cnet_vsa_text_encode_topical(const CnetVsaTokenList *tokens, float *out_topical_vec, int dim);

/* Topical similarity between two texts (stopword-filtered order-invariant) */
float cnet_vsa_text_topical_similarity(const char *text_a, const char *text_b, int dim);

#ifdef __cplusplus
}
#endif

#endif /* CNET_VSA_TEXT_H */
