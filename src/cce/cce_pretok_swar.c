/* Gigatoken-style GPT-2 pretokenizer (see header). Scalar reference + SWAR +
 * dual-cursor, sharing one grammar; the bench gates scalar==SWAR byte-for-byte. */

#include "../../include/cce/cce_pretok_swar.h"
#include <string.h>

/* ---- scalar byte classification ------------------------------------------ */
static inline int is_letter(uint8_t b) {
    uint8_t lo = (uint8_t)(b | 0x20);
    return (lo >= 'a' && lo <= 'z') || b >= 0x80; /* non-ASCII => letter run */
}
static inline int is_digit(uint8_t b) { return b >= '0' && b <= '9'; }
static inline int is_ws(uint8_t b) { return b == 0x20 || b == 0x09 || b == 0x0A || b == 0x0D; }

static inline size_t sc_letter(const uint8_t *s, size_t n, size_t i) { while (i < n && is_letter(s[i])) { i++; } return i; }
static inline size_t sc_digit(const uint8_t *s, size_t n, size_t i) { while (i < n && is_digit(s[i])) { i++; } return i; }
static inline size_t sc_ws(const uint8_t *s, size_t n, size_t i) { while (i < n && is_ws(s[i])) { i++; } return i; }
static inline size_t sc_other(const uint8_t *s, size_t n, size_t i) {
    while (i < n && !is_letter(s[i]) && !is_digit(s[i]) && !is_ws(s[i])) { i++; }
    return i;
}

/* ---- SWAR run scans (8 bytes / iteration) -------------------------------- */
#define ONES 0x0101010101010101ULL
#define HIGH 0x8080808080808080ULL
#define LOW7 0x7F7F7F7F7F7F7F7FULL
/* Borrow-SAFE per-byte predicates (the classic hasless/haszero are only valid as
 * whole-word "any byte" tests; their per-lane high bits are corrupted by
 * cross-lane borrow). High-bit-guard method: each lane stays in [0,0x7F] so no
 * subtraction underflows out of its lane. */
static inline uint64_t nz7(uint64_t x) { return (((x & LOW7) + LOW7) | x) & HIGH; }        /* HIGH where byte != 0 */
static inline uint64_t eq7(uint64_t x, unsigned v) { return (~nz7(x ^ (ONES * v))) & HIGH; } /* HIGH where byte == v */
static inline uint64_t lt7(uint64_t x, unsigned n) { /* HIGH where ascii byte < n (n in 1..128) */
    return ((ONES * (0x7Fu + n)) - (x & LOW7)) & HIGH & ~x;
}
static inline uint64_t ld8(const uint8_t *p) { uint64_t w; memcpy(&w, p, 8); return w; }
static inline int first_hit(uint64_t hits) { return hits ? (__builtin_ctzll(hits) >> 3) : 8; }

static inline uint64_t m_letter(uint64_t w) {
    uint64_t lo = w | (ONES * 0x20);
    uint64_t alpha = lt7(lo, 0x7B) & (~lt7(lo, 0x61)) & HIGH; /* 0x61<=lo<0x7B (ascii a-z) */
    return alpha | (w & HIGH);                                /* ascii letter | non-ascii */
}
static inline uint64_t m_digit(uint64_t w) { return lt7(w, 0x3A) & (~lt7(w, 0x30)) & HIGH; }
static inline uint64_t m_ws(uint64_t w) {
    return eq7(w, 0x20) | eq7(w, 0x09) | eq7(w, 0x0A) | eq7(w, 0x0D);
}

static inline size_t sw_letter(const uint8_t *s, size_t n, size_t i) {
    while (i + 8 <= n) { int k = first_hit((~m_letter(ld8(s + i))) & HIGH); i += (size_t)k; if (k < 8) return i; }
    while (i < n && is_letter(s[i])) { i++; }
    return i;
}
static inline size_t sw_digit(const uint8_t *s, size_t n, size_t i) {
    while (i + 8 <= n) { int k = first_hit((~m_digit(ld8(s + i))) & HIGH); i += (size_t)k; if (k < 8) return i; }
    while (i < n && is_digit(s[i])) { i++; }
    return i;
}
static inline size_t sw_ws(const uint8_t *s, size_t n, size_t i) {
    while (i + 8 <= n) { int k = first_hit((~m_ws(ld8(s + i))) & HIGH); i += (size_t)k; if (k < 8) return i; }
    while (i < n && is_ws(s[i])) { i++; }
    return i;
}
static inline size_t sw_other(const uint8_t *s, size_t n, size_t i) {
    while (i + 8 <= n) {
        uint64_t w = ld8(s + i);
        uint64_t stop = (m_letter(w) | m_digit(w) | m_ws(w)) & HIGH; /* stop at letter/digit/ws */
        int k = first_hit(stop); i += (size_t)k; if (k < 8) return i;
    }
    while (i < n && !is_letter(s[i]) && !is_digit(s[i]) && !is_ws(s[i])) { i++; }
    return i;
}

/* ---- AVX-512 run scans (64 bytes / iteration) ----------------------------
   The per-arch "fast path": one aligned-width compare per byte-class yields a
   64-bit mask directly, and tzcnt on the first non-matching bit gives the run
   end. Falls through to the SWAR scan for the <64-byte tail (and the whole
   scan when not compiled with AVX-512). Same classification as is_letter/etc.,
   so boundaries are identical (the bench gates avx512 == scalar). */
#ifdef __AVX512BW__
#include <immintrin.h>
static inline size_t av_letter(const uint8_t *s, size_t n, size_t i) {
    while (i + 64 <= n) {
        __m512i v = _mm512_loadu_si512((const void *)(s + i));
        __m512i lo = _mm512_or_si512(v, _mm512_set1_epi8(0x20));
        __mmask64 alpha = _mm512_cmpge_epu8_mask(lo, _mm512_set1_epi8('a')) & _mm512_cmple_epu8_mask(lo, _mm512_set1_epi8('z'));
        __mmask64 nonl = ~(alpha | _mm512_cmpge_epu8_mask(v, _mm512_set1_epi8((char)0x80)));
        if (nonl) return i + (size_t)__builtin_ctzll((unsigned long long)nonl);
        i += 64;
    }
    return sw_letter(s, n, i);
}
static inline size_t av_digit(const uint8_t *s, size_t n, size_t i) {
    while (i + 64 <= n) {
        __m512i v = _mm512_loadu_si512((const void *)(s + i));
        __mmask64 nond = ~(_mm512_cmpge_epu8_mask(v, _mm512_set1_epi8('0')) & _mm512_cmple_epu8_mask(v, _mm512_set1_epi8('9')));
        if (nond) return i + (size_t)__builtin_ctzll((unsigned long long)nond);
        i += 64;
    }
    return sw_digit(s, n, i);
}
static inline __mmask64 av_ws_mask(__m512i v) {
    return _mm512_cmpeq_epi8_mask(v, _mm512_set1_epi8(0x20)) | _mm512_cmpeq_epi8_mask(v, _mm512_set1_epi8(0x09))
         | _mm512_cmpeq_epi8_mask(v, _mm512_set1_epi8(0x0A)) | _mm512_cmpeq_epi8_mask(v, _mm512_set1_epi8(0x0D));
}
static inline size_t av_ws(const uint8_t *s, size_t n, size_t i) {
    while (i + 64 <= n) {
        __m512i v = _mm512_loadu_si512((const void *)(s + i));
        __mmask64 nonw = ~av_ws_mask(v);
        if (nonw) return i + (size_t)__builtin_ctzll((unsigned long long)nonw);
        i += 64;
    }
    return sw_ws(s, n, i);
}
static inline size_t av_other(const uint8_t *s, size_t n, size_t i) {
    while (i + 64 <= n) {
        __m512i v = _mm512_loadu_si512((const void *)(s + i));
        __m512i lo = _mm512_or_si512(v, _mm512_set1_epi8(0x20));
        __mmask64 alpha = _mm512_cmpge_epu8_mask(lo, _mm512_set1_epi8('a')) & _mm512_cmple_epu8_mask(lo, _mm512_set1_epi8('z'));
        __mmask64 dig = _mm512_cmpge_epu8_mask(v, _mm512_set1_epi8('0')) & _mm512_cmple_epu8_mask(v, _mm512_set1_epi8('9'));
        __mmask64 stop = alpha | dig | av_ws_mask(v) | _mm512_cmpge_epu8_mask(v, _mm512_set1_epi8((char)0x80));
        if (stop) return i + (size_t)__builtin_ctzll((unsigned long long)stop);
        i += 64;
    }
    return sw_other(s, n, i);
}
#endif /* __AVX512BW__ */

/* ---- shared grammar: next pretoken boundary ------------------------------ */
/* SL/SD/SW/SO are the letter/digit/ws/other scan helpers (scalar or SWAR). The
 * two instantiations are identical apart from those, so their output matches. */
#define DEFINE_NEXT_BOUNDARY(NAME, SL, SD, SW, SO)                                          \
static inline size_t NAME(const uint8_t *s, size_t n, size_t i) {                            \
    uint8_t b = s[i];                                                                        \
    if (b == '\'') {                                                                         \
        if (i + 1 < n) { uint8_t c = s[i + 1];                                               \
            if (c == 's' || c == 't' || c == 'm' || c == 'd') return i + 2;                  \
            if (i + 2 < n) { uint8_t d = s[i + 2];                                           \
                if ((c == 'l' && d == 'l') || (c == 'v' && d == 'e') || (c == 'r' && d == 'e')) return i + 3; } } \
        return SO(s, n, i + 1); /* lone apostrophe => other run */                           \
    }                                                                                        \
    if (b == 0x20) {                                                                         \
        if (i + 1 >= n) return i + 1;                                                        \
        uint8_t c = s[i + 1];                                                                \
        if (is_letter(c)) return SL(s, n, i + 1);                                            \
        if (is_digit(c)) return SD(s, n, i + 1);                                             \
        if (!is_ws(c)) return SO(s, n, i + 1);                                               \
        /* space followed by more whitespace: fall through to ws run at i */                 \
    }                                                                                        \
    if (is_letter(b)) return SL(s, n, i);                                                    \
    if (is_digit(b)) return SD(s, n, i);                                                     \
    if (is_ws(b)) {                                                                          \
        size_t j = SW(s, n, i);                                                              \
        if (j < n && j - 1 > i && s[j - 1] == 0x20) return j - 1; /* trailing space => next token */ \
        return j;                                                                            \
    }                                                                                        \
    return SO(s, n, i);                                                                      \
}

DEFINE_NEXT_BOUNDARY(nb_scalar, sc_letter, sc_digit, sc_ws, sc_other)
DEFINE_NEXT_BOUNDARY(nb_swar, sw_letter, sw_digit, sw_ws, sw_other)
#ifdef __AVX512BW__
DEFINE_NEXT_BOUNDARY(nb_avx, av_letter, av_digit, av_ws, av_other)
#endif

/* Per-codepoint scans (decode each UTF-8 char), mirroring cce_gguf_tok's style. */
static inline int cp_len(uint8_t b) {
    if (b < 0x80) return 1;
    if ((b & 0xE0) == 0xC0) return 2;
    if ((b & 0xF0) == 0xE0) return 3;
    if ((b & 0xF8) == 0xF0) return 4;
    return 1;
}
static inline size_t cp_letter(const uint8_t *s, size_t n, size_t i) { while (i < n && is_letter(s[i])) { i += (size_t)cp_len(s[i]); } return i; }
static inline size_t cp_digit(const uint8_t *s, size_t n, size_t i) { while (i < n && is_digit(s[i])) { i++; } return i; }
static inline size_t cp_ws(const uint8_t *s, size_t n, size_t i) { while (i < n && is_ws(s[i])) { i++; } return i; }
static inline size_t cp_other(const uint8_t *s, size_t n, size_t i) {
    while (i < n && !is_letter(s[i]) && !is_digit(s[i]) && !is_ws(s[i])) { i += (size_t)cp_len(s[i]); }
    return i;
}
DEFINE_NEXT_BOUNDARY(nb_cp, cp_letter, cp_digit, cp_ws, cp_other)

size_t cce_pretok_count_codepoint(const uint8_t *s, size_t n) {
    size_t i = 0, c = 0; while (i < n) { c++; i = nb_cp(s, n, i); } return c;
}

/* ---- public: emit boundaries + count ------------------------------------- */
size_t cce_pretok_scalar(const uint8_t *s, size_t n, uint32_t *bounds, size_t cap) {
    size_t i = 0, c = 0;
    while (i < n) { if (bounds && c < cap) bounds[c] = (uint32_t)i; c++; i = nb_scalar(s, n, i); }
    return c;
}
size_t cce_pretok_swar(const uint8_t *s, size_t n, uint32_t *bounds, size_t cap) {
    size_t i = 0, c = 0;
    while (i < n) { if (bounds && c < cap) bounds[c] = (uint32_t)i; c++; i = nb_swar(s, n, i); }
    return c;
}

/* ---- public: pure-count hot loops ---------------------------------------- */
size_t cce_pretok_count_scalar(const uint8_t *s, size_t n) {
    size_t i = 0, c = 0; while (i < n) { c++; i = nb_scalar(s, n, i); } return c;
}
size_t cce_pretok_count_swar(const uint8_t *s, size_t n) {
    size_t i = 0, c = 0; while (i < n) { c++; i = nb_swar(s, n, i); } return c;
}

size_t cce_pretok_safe_split(const uint8_t *s, size_t n, size_t from) {
    size_t i = from;
    while (i + 1 < n) { if (s[i] == 0x20 && !is_ws(s[i + 1])) return i; i++; }
    return n;
}

/* Two independent cursors over [0,m) and [m,n); the OoO engine overlaps the two
 * serial dependency chains (the log's step-8, ~+25%). m is a true boundary, so
 * cursor 1 lands exactly on it. */
size_t cce_pretok_count_swar_dual(const uint8_t *s, size_t n) {
    if (n < 4096) return cce_pretok_count_swar(s, n);
    size_t m = cce_pretok_safe_split(s, n, n / 2);
    if (m >= n) return cce_pretok_count_swar(s, n);
    size_t p1 = 0, p2 = m, c = 0;
    while (p1 < m && p2 < n) { p1 = nb_swar(s, m, p1); p2 = nb_swar(s, n, p2); c += 2; }
    while (p1 < m) { p1 = nb_swar(s, m, p1); c++; }
    while (p2 < n) { p2 = nb_swar(s, n, p2); c++; }
    return c;
}

/* ---- AVX-512 public (falls back to SWAR when not compiled with AVX-512) ---- */
size_t cce_pretok_avx512(const uint8_t *s, size_t n, uint32_t *bounds, size_t cap) {
#ifdef __AVX512BW__
    size_t i = 0, c = 0;
    while (i < n) { if (bounds && c < cap) bounds[c] = (uint32_t)i; c++; i = nb_avx(s, n, i); }
    return c;
#else
    return cce_pretok_swar(s, n, bounds, cap);
#endif
}
size_t cce_pretok_count_avx512(const uint8_t *s, size_t n) {
#ifdef __AVX512BW__
    size_t i = 0, c = 0; while (i < n) { c++; i = nb_avx(s, n, i); } return c;
#else
    return cce_pretok_count_swar(s, n);
#endif
}
size_t cce_pretok_count_avx512_dual(const uint8_t *s, size_t n) {
#ifdef __AVX512BW__
    if (n < 4096) return cce_pretok_count_avx512(s, n);
    size_t m = cce_pretok_safe_split(s, n, n / 2);
    if (m >= n) return cce_pretok_count_avx512(s, n);
    size_t p1 = 0, p2 = m, c = 0;
    while (p1 < m && p2 < n) { p1 = nb_avx(s, m, p1); p2 = nb_avx(s, n, p2); c += 2; }
    while (p1 < m) { p1 = nb_avx(s, m, p1); c++; }
    while (p2 < n) { p2 = nb_avx(s, n, p2); c++; }
    return c;
#else
    return cce_pretok_count_swar_dual(s, n);
#endif
}
