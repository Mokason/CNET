#ifndef CNET_VSA_NGRAM_H
#define CNET_VSA_NGRAM_H

#include "cnet_vsa.h"
#include "cnet_vsa_text.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CNET_VSA_NGRAM_MAX_VOCAB      512
#define CNET_VSA_NGRAM_MAX_TRANS      2048
#define CNET_VSA_NGRAM_WORD_LEN       32
#define CNET_VSA_NGRAM_MAX_OUTPUT_TOK 128

typedef struct {
    char word[CNET_VSA_NGRAM_WORD_LEN];
    float vector[CNET_VSA_DEFAULT_DIM];
    int frequency;
} CnetVsaVocabEntry;

typedef struct {
    float context_key[CNET_VSA_DEFAULT_DIM];  /* Context: Pi^2(w_{t-1}) * Pi^1(w_t) */
    int next_token_id;                        /* Index in vocab */
    float weight;
} CnetVsaTransition;

typedef struct {
    int dim;
    uint32_t seed;
    
    /* Vocabulary Codebook */
    CnetVsaVocabEntry vocab[CNET_VSA_NGRAM_MAX_VOCAB];
    size_t vocab_count;
    
    /* Transition Associative Memory */
    CnetVsaTransition transitions[CNET_VSA_NGRAM_MAX_TRANS];
    size_t transition_count;
    
    /* Bundled Global Transition Vector (for pure algebraic unbinding) */
    float global_transition_matrix[CNET_VSA_DEFAULT_DIM];
} CnetVsaNgramEngine;

/* Initialize the pure VSA word-by-word generator */
int cnet_vsa_ngram_init(CnetVsaNgramEngine *eng, int dim, uint32_t seed);

/* Ingest raw text sentence into vocabulary and transition memory */
int cnet_vsa_ngram_ingest_sentence(CnetVsaNgramEngine *eng, const char *sentence);

/* Ingest the raw TinyStories corpus (16 raw text sentences, zero templates) */
int cnet_vsa_ngram_ingest_corpus(CnetVsaNgramEngine *eng);

/* Pure VSA Word-by-Word Autonomous Generation:
 * Given a seed word and target semantic intent vector, generate next tokens
 * one-by-one purely via hyperdimensional unbinding and cleanup.
 * NO TEMPLATES. NO PRE-AUTHORED STRINGS.
 */
int cnet_vsa_ngram_generate(CnetVsaNgramEngine *eng,
                            const char *seed_word,
                            const float *target_intent_vec,
                            float intent_steer_weight,
                            float repetition_penalty,
                            int max_tokens,
                            char *out_text,
                            size_t out_text_size,
                            int *out_tokens_generated);

#ifdef __cplusplus
}
#endif

#endif /* CNET_VSA_NGRAM_H */
