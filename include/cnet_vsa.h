#ifndef CNET_VSA_H
#define CNET_VSA_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CNET_VSA_DEFAULT_DIM 512
#define CNET_VSA_NAME_MAX 64

/* Vector Symbolic Architecture (VSA) / Hyperdimensional Computing Core */

typedef struct {
    int dim;
    size_t count;
    size_t capacity;
    char (*names)[CNET_VSA_NAME_MAX];
    float *vectors; /* [capacity * dim] */
} CnetVsaCodebook;

typedef struct {
    char name[CNET_VSA_NAME_MAX];
    int dim;
    float *centroid;       /* [dim] certified prototype */
    float radius_epsilon;  /* maximum acceptable metric distance (e.g. 0.30) */
    float margin_floor;    /* minimum margin to boundary (e.g. 0.05) */
} CnetVsaMetricContract;

/* Initialize a vector with pseudorandom Gaussian values and normalize to unit norm. */
void cnet_vsa_random(float *out, int dim, uint64_t *seed);

/* Unit normalize in place: ||v||_2 = 1.0. Returns L2 norm before normalization. */
float cnet_vsa_normalize(float *v, int dim);

/* Cosine similarity (dot product of normalized vectors). Range [-1.0, 1.0]. */
float cnet_vsa_similarity(const float *a, const float *b, int dim);

/* Metric distance: Euclidean distance between unit vectors: sqrt(2 * (1 - sim)). */
float cnet_vsa_distance(const float *a, const float *b, int dim);

/* Bundling (superposition / set union): out = normalize(sum_i weights[i] * vectors[i]).
   If weights is NULL, treats all weights as 1.0. */
void cnet_vsa_bundle(float *out, const float **vectors, const float *weights,
                     int count, int dim);

/* Binding (role-filler association): element-wise product with unit normalization.
   Commutative, associative, and quasi-invertible (unbinding is binding with the same key). */
void cnet_vsa_bind(float *out, const float *a, const float *b, int dim);

/* Unbinding: for element-wise normalized vectors, unbinding recovers the partner:
   unbind(bind(a, b), b) ~ a. */
void cnet_vsa_unbind(float *out, const float *bound, const float *key, int dim);

/* Permutation (temporal sequence shift): cyclic coordinate rotation by shift positions.
   Invertible via shift = -shift. */
void cnet_vsa_permute(float *out, const float *in, int shift, int dim);

/* Codebook (Item Memory / Clean-up memory) */
int cnet_vsa_codebook_init(CnetVsaCodebook *cb, int dim, size_t capacity);
void cnet_vsa_codebook_free(CnetVsaCodebook *cb);
int cnet_vsa_codebook_add(CnetVsaCodebook *cb, const char *name, const float *vec);
int cnet_vsa_codebook_find(const CnetVsaCodebook *cb, const char *name, float *vec_out);

/* Clean-up associative search: finds the closest vector in codebook to noisy_vec.
   Writes best match into out_clean, best match name into out_name, and cosine similarity to out_sim. */
int cnet_vsa_codebook_cleanup(const CnetVsaCodebook *cb, const float *noisy_vec,
                              float *out_clean, char *out_name, size_t name_cap,
                              float *out_sim);

/* Metric Contract: verify that an input vector satisfies the certified epsilon-ball contract.
   Returns 0 if admitted (within certified radius), 1 if rejected / abstained (fail closed).
   out_margin records the distance margin: radius_epsilon - distance. */
int cnet_vsa_contract_verify(const CnetVsaMetricContract *contract,
                             const float *input_vec, int *admitted,
                             float *out_margin);

#ifdef __cplusplus
}
#endif

#endif /* CNET_VSA_H */
