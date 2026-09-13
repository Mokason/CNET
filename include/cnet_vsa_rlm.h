#ifndef CNET_VSA_RLM_H
#define CNET_VSA_RLM_H

/* Recurrent language model, trained from scratch in C on the retained corpora.
 * One trainer, three interchangeable token mixers, each a matrix-state
 * recurrence per head (state S: dh x dh), read o = S q:
 *
 *   deltanet  S <- alpha S (I - beta k k^T) + beta v k^T      Gated DeltaNet: scalar decay alpha, scalar
 *                                                             write rate beta, unit key k (delta rule); short
 *                                                             causal conv on the mixer input
 *   rwkv7     S <- S (diag(w) - kappa (a o kappa)^T) + v k^T   RWKV-7 core: vector decay w, vector in-context
 *                                                             rate a on a unit removal key kappa; token shift
 *   ssd       S <- alpha S + (Delta x) k^T,  o += D o x        state-space (Mamba-2 SSD form): scalar decay
 *                                                             exp(-Delta A) per head, selective conv on the input
 *
 * Each layer: x -> RMSNorm -> mixer -> gated output projection -> residual -> RMSNorm -> MLP -> residual.
 * Tied embedding/output. Truncated BPTT over chunks with the state carried across chunks. Explicit
 * forward/backward (no autograd), Adam, finite-difference gradient-checked (make cnet_vsa_rlm_bench builds the
 * module in double precision for that check). Word-level vocabulary built from the training text.
 *
 * Mamba-3: the paper's discretisation is not in this tree; the SSD mixer is the state-space family entry and
 * the place to add it (see cnet_vsa_rlm_mixer_ssd in src/cnet_vsa_rlm.c). */

#include <stddef.h>
#include <stdint.h>

#ifndef RLM_REAL
#define RLM_REAL float
#endif
typedef RLM_REAL rlm_real;

#ifdef __cplusplus
extern "C" {
#endif

#define CNET_VSA_RLM_MIXER_DELTANET 0
#define CNET_VSA_RLM_MIXER_RWKV7    1
#define CNET_VSA_RLM_MIXER_SSD      2
#define CNET_VSA_RLM_CONV_K         4

typedef struct {
    int vocab, d_model, n_head, n_layer, d_ff, mixer, chunk;
    unsigned seed;
    float weight_decay;
} cnet_vsa_rlm_cfg;

typedef struct cnet_vsa_rlm cnet_vsa_rlm;

cnet_vsa_rlm  *cnet_vsa_rlm_create(const cnet_vsa_rlm_cfg *cfg);
void       cnet_vsa_rlm_free(cnet_vsa_rlm *m);
size_t     cnet_vsa_rlm_param_count(const cnet_vsa_rlm *m);
const cnet_vsa_rlm_cfg *cnet_vsa_rlm_config(const cnet_vsa_rlm *m);
void       cnet_vsa_rlm_reset_state(cnet_vsa_rlm *m);

/* One chunk: tokens[0..n] (n+1 ids; token t+1 predicted from the prefix). Returns the mean cross-entropy
 * over the n positions; with do_backward accumulates parameter gradients (truncated at the chunk start;
 * the recurrent state is carried forward for the next chunk). n <= cfg->chunk. */
double cnet_vsa_rlm_chunk(cnet_vsa_rlm *m, const int *tokens, int n, int do_backward);
/* Same, with a position mask: only positions t with mask[t] != 0 contribute to the loss (mean over those). */
double cnet_vsa_rlm_chunk_masked(cnet_vsa_rlm *m, const int *tokens, int n, const unsigned char *mask, int do_backward);
void   cnet_vsa_rlm_zero_grad(cnet_vsa_rlm *m);
void   cnet_vsa_rlm_adam(cnet_vsa_rlm *m, float lr, int step);
/* Next-token distribution after feeding tokens[0..n) (state advances); logits has vocab entries. */
void   cnet_vsa_rlm_predict(cnet_vsa_rlm *m, const int *tokens, int n, rlm_real *logits);
/* Directional finite-difference check of the analytic gradient on one chunk: returns the relative error
 * |(L(+eps) - L(-eps))/2eps - g.dir| / (|g.dir| + 1e-12). */
double cnet_vsa_rlm_grad_check(cnet_vsa_rlm *m, const int *tokens, int n, double eps);
/* Global-norm gradient clip; returns the norm before clipping. */
double cnet_vsa_rlm_grad_clip(cnet_vsa_rlm *m, double max_norm);
/* Data-parallel workers: a worker shares the master's parameters (by pointer, never freed by the worker)
 * and owns its state, caches and gradient buffer; reduce adds a worker's gradient into the master's. */
cnet_vsa_rlm *cnet_vsa_rlm_clone_shared(cnet_vsa_rlm *master);
void      cnet_vsa_rlm_reduce_grad(cnet_vsa_rlm *master, cnet_vsa_rlm *worker, double scale);
int    cnet_vsa_rlm_save(const cnet_vsa_rlm *m, const char *path);
cnet_vsa_rlm *cnet_vsa_rlm_load(const char *path);

/* ---- word-level tokenizer and vocabulary shared by the trainer and the scorer (src/cnet_vsa_rlm_score.c) ----
 * lowercase words [a-z0-9'] (bytes >= 128 kept), every other non-space byte a one-character token;
 * vocabulary file: one token per line, id = line number, <unk> = 0, <eos> = 1. */
#define CNET_VSA_RLM_TOKEN_MAX 64
typedef void (*cnet_vsa_rlm_token_cb)(const char *tok, void *ctx);
void cnet_vsa_rlm_tokenize(const char *text, cnet_vsa_rlm_token_cb cb, void *ctx);
typedef struct cnet_vsa_rlm_vocab cnet_vsa_rlm_vocab;
cnet_vsa_rlm_vocab *cnet_vsa_rlm_vocab_load(const char *path);
cnet_vsa_rlm_vocab *cnet_vsa_rlm_vocab_from_words(char **words, int n);   /* copies; words[0] must be <unk>, words[1] <eos> */
int         cnet_vsa_rlm_vocab_save(const cnet_vsa_rlm_vocab *v, const char *path);
void        cnet_vsa_rlm_vocab_free(cnet_vsa_rlm_vocab *v);
int         cnet_vsa_rlm_vocab_size(const cnet_vsa_rlm_vocab *v);
int         cnet_vsa_rlm_vocab_id(const cnet_vsa_rlm_vocab *v, const char *word);   /* 0 (<unk>) when absent */
const char *cnet_vsa_rlm_vocab_word(const cnet_vsa_rlm_vocab *v, int id);
/* text -> ids (returns the count, at most cap; unk_count may be NULL) */
int         cnet_vsa_rlm_vocab_encode(const cnet_vsa_rlm_vocab *v, const char *text, int *ids, int cap, int *unk_count);

/* ---- scorer: a trained model plus its vocabulary, scoring text by mean per-token negative log-likelihood ----
 * nll(prefix, text): the state starts from <eos>, reads the prefix (may be NULL), then the loss is taken over the
 * text's tokens and its closing <eos>. Matches the registry's scorer hook signature (cnet_vsa_registry_set_scorer). */
typedef struct cnet_vsa_rlm_scorer cnet_vsa_rlm_scorer;
cnet_vsa_rlm_scorer *cnet_vsa_rlm_scorer_load(const char *model_path, const char *vocab_path);
/* convenience: <dir>/model_best.rlm else <dir>/model.rlm, with <dir>/vocab.txt */
cnet_vsa_rlm_scorer *cnet_vsa_rlm_scorer_load_dir(const char *dir);
void   cnet_vsa_rlm_scorer_free(cnet_vsa_rlm_scorer *s);
double cnet_vsa_rlm_scorer_nll(void *scorer, const char *prefix, const char *text, int *ntok);
const cnet_vsa_rlm *cnet_vsa_rlm_scorer_model(const cnet_vsa_rlm_scorer *s);

#ifdef __cplusplus
}
#endif
#endif /* CNET_VSA_RLM_H */
