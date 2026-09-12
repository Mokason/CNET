#ifndef CNET_VSA_DELTA_H
#define CNET_VSA_DELTA_H

/* Delta-rule associative memory for a capsule's generative transitions.
 *
 * The n-gram engine stores every (context key, next word) pair explicitly and
 * bundles them into one Hebbian binding; recall from the bundle carries the
 * crosstalk of every other stored pair. The delta rule (Widrow-Hoff, the
 * update inside DeltaNet: S <- S (I - beta k k^T) + beta v k^T, i.e.
 * S += beta (v - S k) k^T) erases the old value at a key before writing, so a
 * d x d matrix recalls up to d linearly independent keys exactly and gives the
 * least-squares answer beyond that. Read is one matvec, v_hat = S k.
 *
 * The memory is DERIVED, never stored: a v4 capsule carries its sentences, so
 * S is rebuilt from the certified passages with the same key rule the engine
 * used at ingest (permute-and-bind trigram context). Nothing in the capsule
 * format changes and the explicit table stays as the reference. */

#include <stddef.h>
#include <stdint.h>
#include "cnet_vsa_gen_capsule.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int      dim;
    float   *S;          /* dim x dim, row-major: v_hat[i] = sum_j S[i*dim + j] k[j] */
    uint32_t pairs;      /* (key, value) pairs written per pass */
    uint32_t passes;
    float    beta;
    double   build_ms;
} CnetVsaDeltaMemory;

int  cnet_vsa_delta_init(CnetVsaDeltaMemory *m, int dim);
void cnet_vsa_delta_free(CnetVsaDeltaMemory *m);
/* one delta-rule write with a unit-norm key: S += beta * (v - S k) k^T */
void cnet_vsa_delta_write(CnetVsaDeltaMemory *m, const float *k, const float *v, float beta);
void cnet_vsa_delta_read(const CnetVsaDeltaMemory *m, const float *k, float *out_v);

/* Rebuild the memory from a sealed capsule's passages with the engine's own
 * key rule; `passes` sweeps over all pairs with rate `beta` (0 < beta <= 1).
 * Returns the number of pairs, <0 on error (no passages, no vocabulary). */
int cnet_vsa_delta_build_from_capsule(CnetVsaDeltaMemory *m, const CnetVsaGenCapsule *cap,
                                      int passes, float beta);

/* Next-word recall of the three memories on a set of sentences (each position
 * t >= 1 whose words are all in the capsule vocabulary is one trial): does the
 * memory's read rank the true next word first / in the top 5 among the vocab?
 * table = the engine's transition scan, bundle = its global binding unbound. */
typedef struct {
    int positions;
    int delta_top1, delta_top5;
    int table_top1, table_top5;
    int bundle_top1, bundle_top5;
    double delta_read_us, table_read_us, bundle_read_us;   /* mean per position */
} CnetVsaDeltaRecall;
int cnet_vsa_delta_recall(const CnetVsaDeltaMemory *m, const CnetVsaGenCapsule *cap,
                          const char *const *sentences, size_t n, CnetVsaDeltaRecall *out);

/* Generation memory sources (bit set) for cnet_vsa_ngram_generate_ex. */
#define CNET_VSA_GEN_MEM_TABLE   1
#define CNET_VSA_GEN_MEM_BUNDLE  2
#define CNET_VSA_GEN_MEM_DELTA   4
#define CNET_VSA_GEN_MEM_DEFAULT (CNET_VSA_GEN_MEM_TABLE | CNET_VSA_GEN_MEM_BUNDLE)

#ifdef __cplusplus
}
#endif
#endif /* CNET_VSA_DELTA_H */
