#ifndef CNET_LM_H
#define CNET_LM_H

#include "nn.h"
#include "contract/contract.h"
#include "cce/cce_learn.h"
#include "cce/cce_cascade.h"

/* CNET-native LLM-type generative logic.
 * A trainable next-token (or next-concept) step model using BTN + Contract.
 * Supports the 7 priority slices: deeper integration, state carry, real data,
 * EVIDENCE head, planner composition, scaling via consolidate, cross-modal.
 */

/* Default small vocab for bootstrapping (letters + markers + some words).
 * Can be extended for real data / CONCEPTs / speech tokens.
 */
#define CNET_LM_MAX_VOCAB 32
#define CNET_LM_MAX_VOCAB_NAME 16

typedef struct {
    char tokens[CNET_LM_MAX_VOCAB][CNET_LM_MAX_VOCAB_NAME];
    int size;
} CnetLmVocab;

/* The LM step model: a BTN + contract + current vocab.
 * Loaded from weights + contract files (e.g. build/cnet_lm_step_*).
 */
typedef struct {
    BinaryTransformNetwork btn;
    Contract contract;
    CnetLmVocab vocab;
    int input_dim;   /* usually vocab size for prev token */
    int output_dim;  /* vocab or evidence support size */
    int has_hidden_state; /* for priority 2 */
    size_t hidden_dim;    /* size of carried state if any */

    /* Primary: CCE cascade for next-token (A/B: pure CCE path) */
    cce_cascade cce;
    bool has_cce;
    cce_gpu_ctx* gpu; /* optional for 4070 */
} CnetLmModel;

/* Initialize an empty model. */
void cnet_lm_init(CnetLmModel *model);

/* Free resources. */
void cnet_lm_free(CnetLmModel *model);

/* Try to enable GPU for this model (call before train if desired) */
cce_result cnet_lm_enable_gpu(CnetLmModel *model);

/* Train a next-token step from sequence list (strings over vocab or raw).
 * Builds (prev_token_onehot -> next) or extended with hidden if provided.
 * Uses btn_train_dynamic + canonical contract.
 * Returns final loss or negative on error.
 */
double cnet_lm_train(CnetLmModel *model,
                     const char **sequences, size_t nseq,
                     size_t max_epochs, size_t growth_window,
                     double target_loss, double min_improvement,
                     const char *weights_path, const char *contract_path);

/* Load a previously trained model from weights + contract.
 * Rebuilds ports and vocab from contract if present.
 * Returns 0 on success.
 */
int cnet_lm_load(CnetLmModel *model,
                 const char *weights_path, const char *contract_path);

/* Save current model (weights + contract).
 * Returns 0 on success.
 */
int cnet_lm_save(const CnetLmModel *model,
                 const char *weights_path, const char *contract_path);

/* Autoregressive generation.
 * Starts from seed, runs N steps using the step model.
 * For now argmax (deterministic for CHECKs); will add sampling with EVIDENCE.
 * If model has hidden state (priority 2), carries it.
 * out must have space for at least max_steps + 1.
 * Returns length generated.
 */
int cnet_lm_generate(const CnetLmModel *model,
                     const char *seed,
                     char *out, size_t max_steps);

/* Core step: given prev token index (or vector), predict next.
 * For integration in agents / narrative.
 * If use_evidence, future will output distrib (priority 4).
 * hidden_in/out for state carry (priority 2).
 */
int cnet_lm_step(const CnetLmModel *model,
                 int prev_idx, double *prev_vec, size_t vec_dim,
                 double *out_logits, size_t out_dim,
                 double *hidden_in, double *hidden_out, size_t hidden_dim);

/* Add a token to vocab (for real data feeding, priority 3).
 * Returns index or -1 if full.
 */
int cnet_lm_add_vocab_token(CnetLmVocab *vocab, const char *tok);

/* Lookup token index. Returns -1 if not found (falls back to 0). */
int cnet_lm_vocab_index(const CnetLmVocab *vocab, const char *tok);

/* For priority 5: register multiple heads (different models) with planner.
 * Simple registry for now; later use PrimitiveRegistry.
 */
typedef struct {
    CnetLmModel *heads[8];
    const char *names[8];
    int count;
} CnetLmHeadRegistry;

void cnet_lm_head_registry_add(CnetLmHeadRegistry *reg, CnetLmModel *m, const char *name);

/* Simple router by keyword (placeholder for planner routing). */
CnetLmModel *cnet_lm_route_head(const CnetLmHeadRegistry *reg, const char *query);

/* Cross-modal helper stub (priority 7): map speech features to token indices. */
int cnet_lm_speech_to_token(const double *speech_features, size_t nfeat,
                            const CnetLmVocab *vocab);

/* Benchmark helper: run generate N times, report avg time + length + rough quality (exact match rate on heldout). */
void cnet_lm_benchmark(const CnetLmModel *model, const char **heldout, size_t nheld,
                       int num_runs, double *out_avg_time_ms, double *out_avg_len);

/* Train the LM directly from multiple HF datasets (https://huggingface.co/datasets).
 * Fetches limited lines, parses JSONL or text, builds dynamic word vocab for unique generations.
 * Example: fable traces + glue cola sentences + others.
 * This produces better unique text than hardcoded small vocab.
 */
double cnet_lm_train_from_multiple_hf(CnetLmModel *model,
                                      const char **urls, size_t n_urls,
                                      size_t max_lines_total,
                                      size_t max_epochs, size_t growth_window,
                                      double target_loss, double min_improvement,
                                      const char *weights_path, const char *contract_path);

#endif /* CNET_LM_H */
