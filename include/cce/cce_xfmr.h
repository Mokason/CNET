#ifndef CCE_XFMR_H
#define CCE_XFMR_H

/*
 * CNET-native transformer (residual LLM), not a PyTorch clone.
 * =================================================================
 * Linears are capsule-shaped maps (propose-only; this module does NOT
 * import or CERT). Fat GEMMs use cce_amdmath when attached and above
 * the offload floor; attention softmax / RMS stay host.
 *
 * N-gram skip is the CALLER's cycle gate (cce_ngram), not this net.
 * Hashtable / ROUTE / CERT stay CPU. Residual logits are not certificates.
 *
 * Does NOT replace cce_transformer_qat (Supra QAT / STE). This is the
 * GPU-math + capsule-map residual speaker.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CCE_XFMR_OK   0
#define CCE_XFMR_ERR -1
#define CCE_XFMR_VERSION "0.3.0"

typedef struct cce_amdmath cce_amdmath;
typedef struct cce_xfmr cce_xfmr;

typedef struct cce_xfmr_config {
    int n_layer;
    int n_embd;     /* D, divisible by n_head */
    int n_head;
    int n_hidden;   /* MLP width; 0 => 4*D */
    int vocab;
    int block_size; /* max T */
    unsigned seed;
} cce_xfmr_config;

cce_xfmr *cce_xfmr_create(const cce_xfmr_config *cfg);
void      cce_xfmr_free(cce_xfmr *m);
void      cce_xfmr_attach_gpu(cce_xfmr *m, cce_amdmath *gpu); /* NULL = CPU GEMM */
void      cce_xfmr_set_ternary(cce_xfmr *m, int on); /* QAT: enable BEFORE train */
int       cce_xfmr_ternary(const cce_xfmr *m);
const char *cce_xfmr_version(void);

/* Causal LM forward. toks[T] in [0,vocab). Fills logits[T*vocab] (row-major). */
int cce_xfmr_forward(cce_xfmr *m, const int *toks, int T, float *logits);

/* Next-token CE on toks[0..T): predict toks[t] from prefix t (t=1..T-1).
 * SGD on all linears. Returns mean CE (nats). */
double cce_xfmr_train_ce(cce_xfmr *m, const int *toks, int T, float lr);

int cce_xfmr_param_count(const cce_xfmr *m);

/* Pack a linear W[out,in] to BitNet codes+γ (per-output absmean).
 * codes[out*in] in {-1,0,1}, gamma[out]. STE train keeps FP shadows. */
int cce_xfmr_pack_linear(const float *W, int out, int in, int8_t *codes, float *gamma);

int cce_xfmr_head_shape(const cce_xfmr *m, int *vocab, int *d);
int cce_xfmr_pack_head(const cce_xfmr *m, int8_t *codes, float *gamma);

/* Propose-only capsule inbox. Writes dir with PROPOSE.json + packed head.
 * NEVER writes unit.cnb / manifest.cknow. auto_cert=false. Not CERT.
 * dir_out created if needed. path_out gets the proposal directory. */
int cce_xfmr_propose(const cce_xfmr *m, const char *unit, const char *dir_out,
                     char *path_out, size_t path_cap);

#ifdef __cplusplus
}
#endif

#endif /* CCE_XFMR_H */
