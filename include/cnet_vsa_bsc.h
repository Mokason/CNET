#ifndef CNET_VSA_BSC_H
#define CNET_VSA_BSC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CNET_VSA_BSC_BITS 512
#define CNET_VSA_BSC_WORDS 8  /* 8 x 64 bits = 512 bits = 64 bytes (exact CPU cache line) */
#define CNET_VSA_NAME_MAX 64

/* 64-byte aligned for direct AVX-512 register loads (vmovdqa64) */
typedef struct __attribute__((aligned(64))) {
    uint64_t w[CNET_VSA_BSC_WORDS];
} CnetVsaBsc;

typedef struct {
    size_t count;
    size_t capacity;
    char (*names)[CNET_VSA_NAME_MAX];
    CnetVsaBsc *vectors; /* [capacity] */
} CnetVsaBscCodebook;

/* Generate random 512-bit dense hypervector */
void cnet_vsa_bsc_random(CnetVsaBsc *out, uint64_t *seed);

/* Bitwise Binding: out = a XOR b.
   Self-inverse: unbind(bind(a, b), b) == a (exact bit-identity). */
void cnet_vsa_bsc_bind(CnetVsaBsc *out, const CnetVsaBsc *a, const CnetVsaBsc *b);

/* Bitwise Unbinding: identical to bind (XOR with key) */
void cnet_vsa_bsc_unbind(CnetVsaBsc *out, const CnetVsaBsc *bound, const CnetVsaBsc *key);

/* Cyclic Permutation: rotates 512-bit vector by shift bits */
void cnet_vsa_bsc_permute(CnetVsaBsc *out, const CnetVsaBsc *in, int shift);

/* Bundling (Majority Voting): each bit is 1 if > count/2 inputs have bit 1 */
void cnet_vsa_bsc_bundle(CnetVsaBsc *out, const CnetVsaBsc *const *vectors, int count);

/* Hamming Distance: count of differing bits in [0, 512] */
int  cnet_vsa_bsc_hamming(const CnetVsaBsc *a, const CnetVsaBsc *b);

/* Normalized Bipolar Similarity in [-1.0, 1.0]:
   sim = 1.0 - (2.0 * hamming / 512.0). 
   Identical = 1.0, Orthogonal = 0.0, Inverted = -1.0. */
float cnet_vsa_bsc_similarity(const CnetVsaBsc *a, const CnetVsaBsc *b);

/* BSC Codebook (Item / Clean-up memory) */
int  cnet_vsa_bsc_codebook_init(CnetVsaBscCodebook *cb, size_t capacity);
void cnet_vsa_bsc_codebook_free(CnetVsaBscCodebook *cb);
int  cnet_vsa_bsc_codebook_add(CnetVsaBscCodebook *cb, const char *name, const CnetVsaBsc *vec);
int  cnet_vsa_bsc_codebook_cleanup(const CnetVsaBscCodebook *cb, const CnetVsaBsc *query,
                                   CnetVsaBsc *out_clean, char *out_name, size_t name_cap,
                                   int *out_hamming);

#ifdef __cplusplus
}
#endif

#endif /* CNET_VSA_BSC_H */
