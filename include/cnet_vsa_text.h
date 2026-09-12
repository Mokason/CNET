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

/* Encoder ids stored in capsule calib.reserved0. 0 remains bag-of-word-hashes
 * so version-1 and uncalibrated version-2 files keep their centroids.
 * The DEFAULT is a policy chosen by `make cnet_vsa_encoder_sweep_bench`:
 * a candidate becomes default only when it is not worse than BAG on the
 * registry-wide separability sweep. */
#define CNET_VSA_ENCODER_BAG      0u  /* stopword-filtered bag of whole-word hashes */
#define CNET_VSA_ENCODER_HD       1u  /* char-trigram words + 0.6 bound bigrams (experimental) */
#define CNET_VSA_ENCODER_STEM     2u  /* stopword-filtered bag of stemmed-word hashes */
#define CNET_VSA_ENCODER_STEM_BI  3u  /* stemmed bag 0.8 + bound stemmed bigrams 0.2 */
#define CNET_VSA_ENCODER_CGRAM_C  4u  /* char-trigram words minus frequent trigrams, unigrams only */
#define CNET_VSA_ENCODER_LEX      5u  /* stem keys; wide space from the active learned lexicon (cnet_vsa_lexicon.h) */
#define CNET_VSA_ENCODER_COUNT    6u
#define CNET_VSA_ENCODER_DEFAULT  CNET_VSA_ENCODER_STEM  /* sweep 2026-09-11: 61.8% vs bag 51.1% at 0.80/0.90, +4% encode cost */

/* ---- wide int8 topical space (capsule format v3) ---------------------------
 * The 512-d float space keeps a cosine noise floor of ~1/sqrt(512) = 0.044 per
 * unrelated pair; at 1000 capsules the largest random match sits near the
 * in-domain signal. A 2048-d centroid quantised to int8 keeps the magnitudes
 * that tell sibling topics apart (measured: lossless against float at every
 * width, +3.5 points top-1 over float-512, and 20 points more separable) in
 * the same 2 KB the float-512 centroid costs. Word signatures here are
 * independent of the 512-d vectors (same FNV-1a seed, whole 64-bit xorshift
 * outputs). All encoders share one wide encoding: stopword-filtered unigrams,
 * stemmed for STEM/STEM_BI, plain for BAG/HD/CGRAM_C. */
#define CNET_VSA_TOPICAL_DIM    2048
#define CNET_VSA_TOPICAL_WORDS  (CNET_VSA_TOPICAL_DIM / 64)
#define CNET_VSA_TOPICAL_KIND_Q8 1u

/* Wide bipolar word signature: CNET_VSA_TOPICAL_WORDS words of sign bits. */
void cnet_vsa_text_token_bits(const char *token, uint64_t *out_words);

/* Wide float encoding (+-1 word bits summed, L2-normalised). out_wide holds
 * CNET_VSA_TOPICAL_DIM floats. Build-time path (leave-one-out calibration). */
int  cnet_vsa_text_encode_topical_wide(const CnetVsaTokenList *tokens, float *out_wide,
                                       uint32_t encoder_id);

/* Query path: signed word counts per dimension, saturated to int8. No float pass. */
int  cnet_vsa_text_encode_topical_q8(const CnetVsaTokenList *tokens, int8_t *out_q8,
                                     uint32_t encoder_id);

/* Quantise a wide float vector to int8 by its max-abs (cosine is scale-free). */
void cnet_vsa_text_wide_to_q8(const float *wide, int8_t *out_q8);

/* Cosine of two int8 vectors (exact int32 dot and squared-norm sums). */
float cnet_vsa_text_q8_similarity(const int8_t *a, const int8_t *b);

/* Same, with the norms precomputed (routing hot path): dot / (norm_a * norm_b). */
float cnet_vsa_text_q8_similarity_n(const int8_t *a, float norm_a, const int8_t *b, float norm_b);

/* L2 norm of an int8 vector. */
float cnet_vsa_text_q8_norm(const int8_t *v);

/* 1 if id names an implemented encoder */
int cnet_vsa_encoder_valid(uint32_t encoder_id);
/* Short stable name ("bag", "hd", "stem", "stem_bi", "cgram_c"); NULL if invalid */
const char *cnet_vsa_encoder_name(uint32_t encoder_id);
/* Parse a name (case-insensitive) or "default"; returns 0 and sets *out, else -1 */
int cnet_vsa_encoder_parse(const char *name, uint32_t *out);

/* Conservative English suffix stripping for hashing only (plural, -ing, -ed,
 * -ly, final y->i). In-place on a lowercase alnum token; never grows it. */
void cnet_vsa_text_stem(char *token);

/* Word vector = bundled character-trigram hypervectors (morphology, not a
 * whole-word hash). Short tokens fall back to cnet_vsa_text_token_vec. */
void cnet_vsa_text_token_vec_chargram(const char *token, float *out_vec, int dim);

/* Topical encoding selected by encoder_id.
 * BAG: stopword-filtered bag of whole-word hashes (legacy).
 * HD:  char-trigram word vectors, bundled unigrams plus bind(permute(w_i,1), w_{i+1})
 *      bigrams. Collocations occupy a different region than shared unigrams. */
int  cnet_vsa_text_encode_topical_ex(const CnetVsaTokenList *tokens, float *out_topical_vec,
                                     int dim, uint32_t encoder_id);

/* Topical similarity between two texts (stopword-filtered order-invariant) */
float cnet_vsa_text_topical_similarity(const char *text_a, const char *text_b, int dim);

#ifdef __cplusplus
}
#endif

#endif /* CNET_VSA_TEXT_H */
