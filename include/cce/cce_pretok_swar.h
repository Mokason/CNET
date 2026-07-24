#ifndef CCE_PRETOK_SWAR_H
#define CCE_PRETOK_SWAR_H

/* Gigatoken-style GPT-2 pretokenizer, ported to C as an independent
 * confirmation of the "SWAR replaces the regex" throughput claim.
 *
 * Grammar (GPT-2 / tiktoken r50k-o200k family), ASCII fast path with non-ASCII
 * bytes (>=0x80) treated as letter-continuation (a benchmarking approximation of
 * \p{L}+ that is applied IDENTICALLY by the scalar and SWAR paths, so their
 * outputs are byte-for-byte equal — the parity gate in the bench enforces this):
 *
 *   '(?:[sdmt]|ll|ve|re) | ?\p{L}+ | ?\p{N}+ | ?[^\s\p{L}\p{N}]+ | \s+(?!\S) | \s+
 *
 * The scalar path is the style CNET's cce_gguf_tok.c uses today (byte/codepoint
 * classify + scan); the SWAR path scans letter/digit/other/whitespace runs eight
 * bytes at a time with branchless u64 arithmetic (Bit-Twiddling-Hacks range
 * tests), and the *_dual variant runs two independent cursors to fill pipeline
 * bubbles (the log's step-8 ILP trick). All are single-threaded; the bench adds
 * an OpenMP fan-out for the aggregate GB/s number.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Emit the start offset of each pretoken into `bounds` (up to `cap`), return the
 * total pretoken count. `bounds` may be NULL to count only. Scalar reference. */
size_t cce_pretok_scalar(const uint8_t *s, size_t n, uint32_t *bounds, size_t cap);

/* Same grammar and output as cce_pretok_scalar, SWAR-accelerated run scans. */
size_t cce_pretok_swar(const uint8_t *s, size_t n, uint32_t *bounds, size_t cap);

/* Pure-count hot loops (no boundary stores), matching the log's count()
 * throughput methodology. */
size_t cce_pretok_count_scalar(const uint8_t *s, size_t n);
size_t cce_pretok_count_swar(const uint8_t *s, size_t n);
size_t cce_pretok_count_swar_dual(const uint8_t *s, size_t n);

/* Per-codepoint decode + classify — the style CNET's cce_gguf_tok.c uses today
 * (utf8_cp per char). Same boundaries as the others; the "before" for the
 * integration speedup. */
size_t cce_pretok_count_codepoint(const uint8_t *s, size_t n);

/* AVX-512 (per-arch wide SIMD): 64 bytes/iteration, one compare-mask per byte
 * class. Same boundaries as the scalar/SWAR paths (gated in the bench). Falls
 * back to SWAR when the build has no AVX-512. */
size_t cce_pretok_avx512(const uint8_t *s, size_t n, uint32_t *bounds, size_t cap);
size_t cce_pretok_count_avx512(const uint8_t *s, size_t n);
size_t cce_pretok_count_avx512_dual(const uint8_t *s, size_t n);

/* A guaranteed pretoken boundary at/after `from` (a 0x20 space followed by a
 * non-whitespace byte), or n if none — used to split work for the dual cursor
 * and for the multithreaded fan-out. */
size_t cce_pretok_safe_split(const uint8_t *s, size_t n, size_t from);

#ifdef __cplusplus
}
#endif

#endif /* CCE_PRETOK_SWAR_H */
