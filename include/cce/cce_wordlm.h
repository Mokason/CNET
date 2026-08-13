#ifndef CCE_WORDLM_H
#define CCE_WORDLM_H

/* Scalable word-level language model that breaks the O(V^2) wall.

   The naive one-hot word head is [ (ctx*V) x V ] = O(ctx * V^2) parameters.
   This model factors it so EVERY part is at most O(V*d):

     tied embedding   E  : [V x d]      (shared across all ctx slots; caveat 4)
     hidden bottleneck W1 : [hid x ctx*d]  (the low-rank factor; kills O(V^2))
     class-factored softmax over ~sqrt(V) classes + per-class word heads
                                            (sub-linear output; caveat 2)

   It is trained with an EXACT gradient (full backprop) + Adam -- not the
   approximate DFA credit -- so the embedding/bottleneck actually learns
   (caveat 1). cce_wordlm_gradcheck verifies the analytic gradient numerically.

   Data sparsity (caveat 3) is not a model property -- it needs a real corpus --
   but the tied embedding + factored head use the available data far more
   efficiently than a dense per-(position,word) head. */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cce_wordlm cce_wordlm;

/* V=vocab, d=embedding dim, ctx=context words, hid=hidden width.
   Class count C ~ ceil(sqrt(V)) is chosen internally. */
cce_wordlm* cce_wordlm_create(int V, int d, int ctx, int hid, unsigned seed);
void        cce_wordlm_free(cce_wordlm* m);

/* One exact-gradient Adam step on (ctx_words[ctx] -> target). Returns CE loss. */
double cce_wordlm_train_step(cce_wordlm* m, const int* ctx_words, int target, float lr);

/* Forward-only cross-entropy (negative log-likelihood) for one example. */
double cce_wordlm_nll(cce_wordlm* m, const int* ctx_words, int target);

/* Greedy next word: argmax class, then argmax word within that class
   (both sub-linear, ~sqrt(V)). `penalty` (NULL ok) is an additive per-word bias
   applied to the within-class word logits (use negative values to discourage). */
int cce_wordlm_predict(cce_wordlm* m, const int* ctx_words, const float* penalty);

/* Static parameter count for given dims (linear in V) -- for scaling reports. */
long cce_wordlm_param_count(int V, int d, int ctx, int hid);

/* Finite-difference gradient check on one example; returns max relative error
   between analytic and numeric gradients over a sample of parameters
   (a correct backprop gives a small value, ~1e-2 or less in float32). */
double cce_wordlm_gradcheck(cce_wordlm* m, const int* ctx_words, int target);

int cce_wordlm_vocab(const cce_wordlm* m);
int cce_wordlm_classes(const cce_wordlm* m);

/* BitNet b1.58 BitLinear mode: ternary {-1,0,+1} weights on W1/Wc/Ww with FP
   shadow weights + straight-through estimator. Enable BEFORE training for true
   QAT; enabling it on an FP-trained model gives (collapsing) post-hoc ternary. */
void cce_wordlm_set_ternary(cce_wordlm* m, int on);
int  cce_wordlm_ternary(const cce_wordlm* m);

/* Independent ternary on the input embedding E (FP shadow + STE). Lets you do
   FP / linear-only / embedding-only / linear+embedding comparisons. */
void cce_wordlm_set_ternary_embed(cce_wordlm* m, int on);
int  cce_wordlm_ternary_embed(const cce_wordlm* m);

/* Opt-in order-invariant context projection. For each hidden row and embedding
   coordinate, average the W1 shadow weight and Adam moments across all context
   slots, then copy that value back to every slot. Calling this after each step
   makes the concatenated WordLM behave as an exact bag of token embeddings;
   callers that never invoke it retain the original positional behavior. */
void cce_wordlm_tie_context_slots(cce_wordlm* m);

/* ---- Packed 1.6-bit ternary export / inference (5 trits/byte, base-3) ----
   Freezes the QAT FP shadow weights of W1/Wc/Ww into per-row absmean ternary
   {-1,0,+1} and packs 5 per byte. The packed model is inference-only and holds
   NO FP shadow weights — it reads trits directly. Embeddings/biases stay FP. */
int cce_wordlm_export_trits(const cce_wordlm* m, const char* path);

typedef struct cce_wordlm_packed cce_wordlm_packed;
cce_wordlm_packed* cce_wordlm_packed_load(const char* path);
void   cce_wordlm_packed_free(cce_wordlm_packed* p);
double cce_wordlm_packed_nll(cce_wordlm_packed* p, const int* ctx_words, int target);
int    cce_wordlm_packed_predict(cce_wordlm_packed* p, const int* ctx_words, const float* penalty);
int    cce_wordlm_packed_vocab(const cce_wordlm_packed* p);

#ifdef __cplusplus
}
#endif

#endif /* CCE_WORDLM_H */
