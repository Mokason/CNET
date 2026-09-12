#include "cnet_vsa_ngram.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

static void normalize_token(const char *in, char *out, size_t max_len) {
    size_t j = 0;
    for (size_t i = 0; in[i] && j + 1 < max_len; ++i) {
        if (isalnum((unsigned char)in[i])) {
            out[j++] = (char)tolower((unsigned char)in[i]);
        }
    }
    out[j] = '\0';
}

static int get_or_add_vocab(CnetVsaNgramEngine *eng, const char *clean_word) {
    if (!clean_word || !*clean_word) return -1;
    for (size_t i = 0; i < eng->vocab_count; ++i) {
        if (strcmp(eng->vocab[i].word, clean_word) == 0) {
            eng->vocab[i].frequency++;
            return (int)i;
        }
    }
    if (eng->vocab_count >= CNET_VSA_NGRAM_MAX_VOCAB) return -1;

    int idx = (int)eng->vocab_count++;
    snprintf(eng->vocab[idx].word, sizeof(eng->vocab[idx].word), "%s", clean_word);
    eng->vocab[idx].frequency = 1;

    /* Use deterministic text projector for word vector */
    cnet_vsa_text_token_vec(clean_word, eng->vocab[idx].vector, eng->dim);
    cnet_vsa_normalize(eng->vocab[idx].vector, eng->dim);
    return idx;
}

int cnet_vsa_ngram_init(CnetVsaNgramEngine *eng, int dim, uint32_t seed) {
    if (!eng) return -1;
    memset(eng, 0, sizeof(*eng));
    eng->dim = (dim > 0 && dim <= CNET_VSA_DEFAULT_DIM) ? dim : CNET_VSA_DEFAULT_DIM;
    eng->seed = seed ? seed : 1337u;
    return 0;
}

int cnet_vsa_ngram_ingest_sentence(CnetVsaNgramEngine *eng, const char *sentence) {
    if (!eng || !sentence) return -1;

    /* Tokenize into words */
    char copy[2048];
    snprintf(copy, sizeof(copy), "%s", sentence);

    int token_ids[256];
    int token_count = 0;

    char *saveptr = NULL;
    char *token = strtok_r(copy, " \t\r\n", &saveptr);
    while (token && token_count < 256) {
        char clean[CNET_VSA_NGRAM_WORD_LEN];
        normalize_token(token, clean, sizeof(clean));
        if (clean[0]) {
            int tid = get_or_add_vocab(eng, clean);
            if (tid >= 0) {
                token_ids[token_count++] = tid;
            }
        }
        token = strtok_r(NULL, " \t\r\n", &saveptr);
    }

    if (token_count < 2) return 0;

    int D = eng->dim;
    float pi1_vec[CNET_VSA_DEFAULT_DIM];
    float pi2_vec[CNET_VSA_DEFAULT_DIM];
    float context_vec[CNET_VSA_DEFAULT_DIM];

    for (int t = 1; t < token_count; ++t) {
        int w_prev = token_ids[t - 1];
        int w_curr = token_ids[t];

        /* Bigram Context: Pi^1(w_{t-1}) */
        cnet_vsa_permute(pi1_vec, eng->vocab[w_prev].vector, 1, D);
        memcpy(context_vec, pi1_vec, (size_t)D * sizeof(float));

        /* If trigram available: Context = 0.65 * (Pi^2(w_{t-2}) * Pi^1(w_{t-1})) + 0.35 * Pi^1(w_{t-1}) */
        if (t >= 2) {
            int w_prev2 = token_ids[t - 2];
            cnet_vsa_permute(pi2_vec, eng->vocab[w_prev2].vector, 2, D);
            float trigram_ctx[CNET_VSA_DEFAULT_DIM];
            cnet_vsa_bind(trigram_ctx, pi2_vec, pi1_vec, D);
            for (int d = 0; d < D; ++d) {
                context_vec[d] = 0.65f * trigram_ctx[d] + 0.35f * pi1_vec[d];
            }
            cnet_vsa_normalize(context_vec, D);
        }

        /* Record in transition associative memory */
        if (eng->transition_count < CNET_VSA_NGRAM_MAX_TRANS) {
            size_t tr_idx = eng->transition_count++;
            memcpy(eng->transitions[tr_idx].context_key, context_vec, (size_t)D * sizeof(float));
            eng->transitions[tr_idx].next_token_id = w_curr;
            eng->transitions[tr_idx].weight = 1.0f;
        }

        /* Superimpose into global transition matrix: M += Context * V_next */
        float bound_pair[CNET_VSA_DEFAULT_DIM];
        cnet_vsa_bind(bound_pair, context_vec, eng->vocab[w_curr].vector, D);
        for (int d = 0; d < D; ++d) {
            eng->global_transition_matrix[d] += bound_pair[d];
        }
    }

    cnet_vsa_normalize(eng->global_transition_matrix, D);
    return token_count;
}

int cnet_vsa_ngram_ingest_corpus(CnetVsaNgramEngine *eng) {
    if (!eng) return -1;
    const char *tiny_stories[] = {
        "Once upon a time there was a curious fox who found a glowing mushroom in the enchanted forest.",
        "Lily loved to play in the garden with her red ball and her happy little dog.",
        "Tom had a little black cat named Max who liked to sleep on a soft warm mat.",
        "Ben built a tall tower with colorful blocks and then laughed when it fell down.",
        "Mia the rabbit hopped fast across the sunny meadow to hide from the summer rain.",
        "Sam sailed his paper boat on the mirror pond and the wind brought it safely back home.",
        "Emma sat by the window reading stories while the first stars appeared in the sky.",
        "Jack threw the bright red ball high into the air and his dog caught it in the yard.",
        "Sara baked sweet cookies and the warm smell made the whole house feel very happy.",
        "Anna made a wish on a shooting star and the next morning she met a brand new friend.",
        "A tiny mouse ate a crumb of cheese while the big cat was asleep on the warm mat.",
        "The young dragon practiced flying low over the green hills until he could soar high.",
        "The kind knight gave food to the hungry villagers and they thanked him with a great feast.",
        "A clever bird sang beautiful songs and told the princess where the lost golden crown was hidden.",
        "The girl planted seeds in the spring and by summer the flowers were taller than she was.",
        "A caring boy found a lost puppy and after searching all day he returned it to its happy family."
    };

    size_t count = sizeof(tiny_stories) / sizeof(tiny_stories[0]);
    for (size_t i = 0; i < count; ++i) {
        cnet_vsa_ngram_ingest_sentence(eng, tiny_stories[i]);
    }
    return (int)count;
}

int cnet_vsa_ngram_lookup(const CnetVsaNgramEngine *eng, const char *word) {
    if (!eng || !word) return -1;
    char clean[CNET_VSA_NGRAM_WORD_LEN];
    normalize_token(word, clean, sizeof(clean));
    if (!clean[0]) return -1;
    for (size_t i = 0; i < eng->vocab_count; ++i) if (strcmp(eng->vocab[i].word, clean) == 0) return (int)i;
    return -1;
}

void cnet_vsa_ngram_context_key(const CnetVsaNgramEngine *eng, int prev2, int prev1, float *out_key) {
    const int D = eng->dim;
    float pi1_vec[CNET_VSA_DEFAULT_DIM];
    cnet_vsa_permute(pi1_vec, eng->vocab[prev1].vector, 1, D);
    if (prev2 < 0) { memcpy(out_key, pi1_vec, (size_t)D * sizeof(float)); return; }
    float pi2_vec[CNET_VSA_DEFAULT_DIM], tri[CNET_VSA_DEFAULT_DIM];
    cnet_vsa_permute(pi2_vec, eng->vocab[prev2].vector, 2, D);
    cnet_vsa_bind(tri, pi2_vec, pi1_vec, D);
    for (int d = 0; d < D; ++d) out_key[d] = 0.65f * tri[d] + 0.35f * pi1_vec[d];
    cnet_vsa_normalize(out_key, D);
}

int cnet_vsa_ngram_generate(CnetVsaNgramEngine *eng,
                            const char *seed_word,
                            const float *target_intent_vec,
                            float intent_steer_weight,
                            float repetition_penalty,
                            int max_tokens,
                            char *out_text,
                            size_t out_text_size,
                            int *out_tokens_generated) {
    return cnet_vsa_ngram_generate_ex(eng, seed_word, target_intent_vec, intent_steer_weight, repetition_penalty,
                                      max_tokens, 3u /* table | bundle */, NULL, NULL, out_text, out_text_size, out_tokens_generated);
}

int cnet_vsa_ngram_generate_ex(CnetVsaNgramEngine *eng,
                               const char *seed_word,
                               const float *target_intent_vec,
                               float intent_steer_weight,
                               float repetition_penalty,
                               int max_tokens,
                               unsigned mem,
                               void (*delta_read)(const void *ctx, const float *key, float *out_v),
                               const void *delta_ctx,
                               char *out_text,
                               size_t out_text_size,
                               int *out_tokens_generated) {
    if (!eng || !out_text || out_text_size < 16) return -1;
    if (!(mem & 4u) || !delta_read) mem &= ~4u;
    if (mem == 0) return -1;
    if (eng->vocab_count == 0 || eng->transition_count == 0) return -1;

    char clean_seed[CNET_VSA_NGRAM_WORD_LEN];
    normalize_token(seed_word ? seed_word : "once", clean_seed, sizeof(clean_seed));

    int current_tid = -1;
    for (size_t i = 0; i < eng->vocab_count; ++i) {
        if (strcmp(eng->vocab[i].word, clean_seed) == 0) {
            current_tid = (int)i;
            break;
        }
    }
    if (current_tid < 0) current_tid = 0;

    int D = eng->dim;
    int history[CNET_VSA_NGRAM_MAX_OUTPUT_TOK];
    int hist_count = 0;
    history[hist_count++] = current_tid;

    out_text[0] = '\0';
    snprintf(out_text, out_text_size, "%s", eng->vocab[current_tid].word);

    int total_gen = 1;
    float scores[CNET_VSA_NGRAM_MAX_VOCAB];

    while (total_gen < max_tokens && total_gen < CNET_VSA_NGRAM_MAX_OUTPUT_TOK) {
        /* Construct Context Query Vector Q */
        int w_curr = history[hist_count - 1];
        float pi1_vec[CNET_VSA_DEFAULT_DIM];
        cnet_vsa_permute(pi1_vec, eng->vocab[w_curr].vector, 1, D);

        float query_ctx[CNET_VSA_DEFAULT_DIM];
        if (hist_count >= 2) {
            int w_prev = history[hist_count - 2];
            float pi2_vec[CNET_VSA_DEFAULT_DIM];
            cnet_vsa_permute(pi2_vec, eng->vocab[w_prev].vector, 2, D);
            float trigram_ctx[CNET_VSA_DEFAULT_DIM];
            cnet_vsa_bind(trigram_ctx, pi2_vec, pi1_vec, D);
            for (int d = 0; d < D; ++d) {
                query_ctx[d] = 0.65f * trigram_ctx[d] + 0.35f * pi1_vec[d];
            }
            cnet_vsa_normalize(query_ctx, D);
        } else {
            memcpy(query_ctx, pi1_vec, (size_t)D * sizeof(float));
        }

        /* 1. Scan transition memory for matching contexts */
        memset(scores, 0, sizeof(float) * eng->vocab_count);
        if (mem & 1u) for (size_t tr = 0; tr < eng->transition_count; ++tr) {
            float match = cnet_vsa_similarity(query_ctx, eng->transitions[tr].context_key, D);
            if (match > 0.20f) {
                int next_id = eng->transitions[tr].next_token_id;
                scores[next_id] += match * 1.5f;
            }
        }
        /* 1b. Delta-rule memory read: v_hat = S q, scored like the table */
        if (mem & 4u) {
            float vhat[CNET_VSA_DEFAULT_DIM];
            delta_read(delta_ctx, query_ctx, vhat);
            for (size_t v = 0; v < eng->vocab_count; ++v) {
                float c = cnet_vsa_similarity(vhat, eng->vocab[v].vector, D);
                if (c > 0.0f) scores[v] += c * 1.5f;
            }
        }
        /* 2. Unbind from global transition matrix: U = M * Q */
        float unbind_vec[CNET_VSA_DEFAULT_DIM];
        cnet_vsa_unbind(unbind_vec, eng->global_transition_matrix, query_ctx, D);
        if (!(mem & 2u)) memset(unbind_vec, 0, sizeof(float) * (size_t)D);
        for (size_t v = 0; v < eng->vocab_count; ++v) {
            float dot_u = cnet_vsa_similarity(unbind_vec, eng->vocab[v].vector, D);
            if (dot_u > 0.0f) {
                scores[v] += dot_u * 0.5f;
            }
        }

        /* 3. Apply semantic intent steering: score += intent_steer_weight * cos(V_word, I_target) */
        if (target_intent_vec && intent_steer_weight > 0.001f) {
            for (size_t v = 0; v < eng->vocab_count; ++v) {
                float intent_match = cnet_vsa_similarity(eng->vocab[v].vector, target_intent_vec, D);
                scores[v] += intent_steer_weight * intent_match;
            }
        }

        /* 4. Apply repetition penalty for recently used words */
        int lookback = hist_count > 10 ? 10 : hist_count;
        for (int b = 0; b < lookback; ++b) {
            int past_id = history[hist_count - 1 - b];
            float penalty = repetition_penalty * (1.0f - (float)b * 0.08f);
            scores[past_id] -= penalty;
        }

        /* 4b. Bigram cycle blocking: prevent repeating identical transitions (W_prev -> W_cand) */
        if (hist_count >= 2) {
            int prev_w = history[hist_count - 1];
            for (int h = 1; h < hist_count - 1; ++h) {
                if (history[h - 1] == prev_w) {
                    int cand_repeat = history[h];
                    scores[cand_repeat] -= 3.5f; /* Strongly penalize repeating the same bigram */
                }
            }
        }

        /* 5. Select best candidate word (Argmax) */
        int best_id = -1;
        float best_score = -9999.0f;
        for (size_t v = 0; v < eng->vocab_count; ++v) {
            if (scores[v] > best_score) {
                best_score = scores[v];
                best_id = (int)v;
            }
        }

        if (best_id < 0 || best_score <= -50.0f) {
            break; /* No viable transition */
        }

        /* Append to output text */
        size_t current_len = strlen(out_text);
        if (current_len + strlen(eng->vocab[best_id].word) + 2 >= out_text_size) {
            break;
        }

        strcat(out_text, " ");
        strcat(out_text, eng->vocab[best_id].word);

        history[hist_count++] = best_id;
        total_gen++;

        /* Terminate if reasonable story length reached and hit a sentence boundary */
        if (total_gen >= 15 && (strcmp(eng->vocab[best_id].word, "home") == 0 ||
                                strcmp(eng->vocab[best_id].word, "day") == 0 ||
                                strcmp(eng->vocab[best_id].word, "forest") == 0 ||
                                strcmp(eng->vocab[best_id].word, "sky") == 0 ||
                                strcmp(eng->vocab[best_id].word, "feast") == 0 ||
                                strcmp(eng->vocab[best_id].word, "family") == 0)) {
            strcat(out_text, ".");
            break;
        }
    }

    if (out_tokens_generated) *out_tokens_generated = total_gen;
    return 0;
}
