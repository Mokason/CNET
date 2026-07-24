/* GGUF GPT-2 / Qwen BPE tokenizer — pure C, no Python.
 * See include/cce/cce_gguf_tok.h.
 */
#include "../../include/cce/cce_gguf_tok.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifndef _WIN32
#include <unistd.h>
#include <sys/types.h>
#ifdef __linux__
#include <sys/mman.h> /* madvise(MADV_HUGEPAGE) for the pretoken cache table */
#endif
#define cce_fseeko fseeko
#else
#define cce_fseeko _fseeki64
#endif

/* llama.cpp token_type values */
enum {
    TOK_NORMAL = 1,
    TOK_UNKNOWN = 2,
    TOK_CONTROL = 3,
    TOK_USER_DEFINED = 4,
    TOK_UNUSED = 5,
    TOK_BYTE = 6
};

enum {
    GGUF_U8 = 0, GGUF_I8 = 1, GGUF_U16 = 2, GGUF_I16 = 3,
    GGUF_U32 = 4, GGUF_I32 = 5, GGUF_F32 = 6, GGUF_BOOL = 7,
    GGUF_STR = 8, GGUF_ARR = 9, GGUF_U64 = 10, GGUF_I64 = 11, GGUF_F64 = 12
};

struct Ptc; /* persistent pretoken cache (defined below) */
static void ptc_free(struct Ptc *c);

/* id-space merge table entry, packed AoS so a pair_get hit touches ONE cache
   line (key + value together) instead of three separate arrays. key 0 = empty. */
typedef struct { uint64_t key; int32_t rank; int32_t merged; } PairE;

struct cce_gguf_tok {
    char **id_to_piece;       /* [vocab] owned UTF-8 pieces (Ġ/Ċ form) */
    unsigned char *tok_type;  /* [vocab] */
    int vocab;
    int eos_id;
    int bos_id;
    int pad_id;
    int im_start_id;
    int im_end_id;

    /* piece → id open-address hash (keys point into id_to_piece) */
    const char **vkey;
    int *vid;
    int vcap;

    /* merge "a\x1fb" → rank */
    char **mkey;
    int *mrank;
    int mcap;
    int n_merges;

    /* specials for encode scan (longest-first) */
    char **special_str;
    int *special_id;
    int n_special;

    /* GPT-2 byte tables */
    char byte_enc[256][4]; /* NUL-terminated UTF-8 of mapped char */
    int byte_enc_len[256];
    int cp_to_byte[1024];  /* unicode codepoint → byte, -1 if none */

    /* id-space BPE (hot path): avoids string hashing/compare in the merge loop.
     * byte_to_id[b] = vocab id of byte_enc[b]; the pair table maps
     * (id_a,id_b) -> (rank, merged id). Built at load; id_bpe is set only when
     * every byte and every merge resolves to a valid id, so the integer path is
     * provably identical to the string path (else the string path is used). */
    int byte_to_id[256];
    PairE *pe;             /* [pcap] packed (key,rank,merged); key 0 = empty */
    int pcap;
    int id_bpe;

    /* Optional persistent pretoken cache: survives across encode_fast calls, so
     * streaming many small documents accumulates hits (a per-call cache would
     * reset each call). Enabled via cce_gguf_tok_cache_enable; NULL = off. */
    struct Ptc *pcache;
};

/* ---- tiny IO ---- */
static int rd_u32(FILE *f, uint32_t *v) { return fread(v, 4, 1, f) == 1; }
static int rd_u64(FILE *f, uint64_t *v) { return fread(v, 8, 1, f) == 1; }

static int rd_string_alloc(FILE *f, char **out) {
    uint64_t n = 0;
    char *s;
    if (!rd_u64(f, &n)) return 0;
    if (n > (1ull << 28)) return 0;
    s = (char *)malloc((size_t)n + 1);
    if (!s) return 0;
    if (n && fread(s, 1, (size_t)n, f) != (size_t)n) { free(s); return 0; }
    s[n] = 0;
    *out = s;
    return 1;
}

static int skip_string(FILE *f) {
    uint64_t n = 0;
    if (!rd_u64(f, &n)) return 0;
    return cce_fseeko(f, (off_t)n, SEEK_CUR) == 0;
}

static size_t elem_size(uint32_t t) {
    switch (t) {
    case GGUF_U8: case GGUF_I8: case GGUF_BOOL: return 1;
    case GGUF_U16: case GGUF_I16: return 2;
    case GGUF_U32: case GGUF_I32: case GGUF_F32: return 4;
    case GGUF_U64: case GGUF_I64: case GGUF_F64: return 8;
    default: return 0;
    }
}

/* ---- hash ---- */
static uint32_t fnv1a(const char *s) {
    uint32_t h = 2166136261u;
    while (*s) { h ^= (unsigned char)*s++; h *= 16777619u; }
    return h;
}

static int next_pow2(int n) {
    int p = 1;
    while (p < n) p <<= 1;
    return p < 16 ? 16 : p;
}

static void vocab_put(cce_gguf_tok *t, const char *piece, int id) {
    uint32_t h;
    int i;
    if (!piece || t->vcap < 1) return;
    h = fnv1a(piece);
    i = (int)(h & (uint32_t)(t->vcap - 1));
    for (;;) {
        if (!t->vkey[i]) { t->vkey[i] = piece; t->vid[i] = id; return; }
        if (strcmp(t->vkey[i], piece) == 0) { t->vid[i] = id; return; }
        i = (i + 1) & (t->vcap - 1);
    }
}

static int vocab_get(const cce_gguf_tok *t, const char *piece) {
    uint32_t h;
    int i;
    if (!piece || t->vcap < 1) return -1;
    h = fnv1a(piece);
    i = (int)(h & (uint32_t)(t->vcap - 1));
    for (;;) {
        if (!t->vkey[i]) return -1;
        if (strcmp(t->vkey[i], piece) == 0) return t->vid[i];
        i = (i + 1) & (t->vcap - 1);
    }
}

static void merges_put(cce_gguf_tok *t, char *key, int rank) {
    uint32_t h;
    int i;
    if (!key || t->mcap < 1) { free(key); return; }
    h = fnv1a(key);
    i = (int)(h & (uint32_t)(t->mcap - 1));
    for (;;) {
        if (!t->mkey[i]) { t->mkey[i] = key; t->mrank[i] = rank; return; }
        if (strcmp(t->mkey[i], key) == 0) { t->mrank[i] = rank; free(key); return; }
        i = (i + 1) & (t->mcap - 1);
    }
}

static int merges_get(const cce_gguf_tok *t, const char *key) {
    uint32_t h;
    int i;
    if (!key || t->mcap < 1) return -1;
    h = fnv1a(key);
    i = (int)(h & (uint32_t)(t->mcap - 1));
    for (;;) {
        if (!t->mkey[i]) return -1;
        if (strcmp(t->mkey[i], key) == 0) return t->mrank[i];
        i = (i + 1) & (t->mcap - 1);
    }
}

/* ---- id-space merge table: (id_a,id_b) -> (rank, merged id) ---- */
static inline uint64_t pair_key(int ida, int idb) {
    return (((uint64_t)(uint32_t)(ida + 1)) << 32) | (uint64_t)(uint32_t)(idb + 1);
}
static void pair_put(cce_gguf_tok *t, int ida, int idb, int rank, int merged) {
    uint64_t key = pair_key(ida, idb);
    int i = (int)((key * 0x9E3779B97F4A7C15ULL >> 32) & (uint32_t)(t->pcap - 1));
    for (;;) {
        PairE *e = &t->pe[i];
        if (!e->key) { e->key = key; e->rank = rank; e->merged = merged; return; }
        if (e->key == key) return; /* merges are rank-ordered; keep the first (lowest) */
        i = (i + 1) & (t->pcap - 1);
    }
}
static inline int pair_get(const cce_gguf_tok *t, int ida, int idb, int *merged) {
    uint64_t key = pair_key(ida, idb);
    int i = (int)((key * 0x9E3779B97F4A7C15ULL >> 32) & (uint32_t)(t->pcap - 1));
    for (;;) {
        const PairE *e = &t->pe[i];          /* key + value share one cache line */
        if (!e->key) return -1;
        if (e->key == key) { *merged = e->merged; return e->rank; }
        i = (i + 1) & (t->pcap - 1);
    }
}

/* ---- GPT-2 bytes_to_unicode ---- */
static void build_byte_tables(cce_gguf_tok *t) {
    int bs[512], cs[512], n = 0, i, b, n2;
    int printable[256];
    memset(printable, 0, sizeof printable);
    for (i = 0; i < 1024; ++i) t->cp_to_byte[i] = -1;
    for (b = 33; b <= 126; ++b) printable[b] = 1;
    for (b = 161; b <= 172; ++b) printable[b] = 1;
    for (b = 174; b <= 255; ++b) printable[b] = 1;
    for (b = 0; b < 256; ++b)
        if (printable[b]) { bs[n] = b; cs[n] = b; n++; }
    n2 = 0;
    for (b = 0; b < 256; ++b)
        if (!printable[b]) { bs[n] = b; cs[n] = 256 + n2; n++; n2++; }
    for (i = 0; i < n; ++i) {
        int cp = cs[i];
        unsigned char *s = (unsigned char *)t->byte_enc[bs[i]];
        if (cp < 0x80) {
            s[0] = (unsigned char)cp;
            s[1] = 0;
            t->byte_enc_len[bs[i]] = 1;
        } else if (cp < 0x800) {
            s[0] = (unsigned char)(0xC0 | (cp >> 6));
            s[1] = (unsigned char)(0x80 | (cp & 0x3F));
            s[2] = 0;
            t->byte_enc_len[bs[i]] = 2;
        } else {
            s[0] = (unsigned char)(0xE0 | (cp >> 12));
            s[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
            s[2] = (unsigned char)(0x80 | (cp & 0x3F));
            s[3] = 0;
            t->byte_enc_len[bs[i]] = 3;
        }
        if (cp >= 0 && cp < 1024) t->cp_to_byte[cp] = bs[i];
    }
}

/* UTF-8 helpers */
static int utf8_cp(const char *s, int *adv) {
    unsigned char c0 = (unsigned char)s[0];
    if (c0 < 0x80) { *adv = 1; return c0; }
    if ((c0 & 0xE0) == 0xC0 && s[1]) {
        *adv = 2;
        return ((c0 & 0x1F) << 6) | ((unsigned char)s[1] & 0x3F);
    }
    if ((c0 & 0xF0) == 0xE0 && s[1] && s[2]) {
        *adv = 3;
        return ((c0 & 0x0F) << 12) | (((unsigned char)s[1] & 0x3F) << 6) |
               ((unsigned char)s[2] & 0x3F);
    }
    if ((c0 & 0xF8) == 0xF0 && s[1] && s[2] && s[3]) {
        *adv = 4;
        return ((c0 & 0x07) << 18) | (((unsigned char)s[1] & 0x3F) << 12) |
               (((unsigned char)s[2] & 0x3F) << 6) | ((unsigned char)s[3] & 0x3F);
    }
    *adv = 1;
    return c0;
}

static int is_letter_cp(int cp) {
    if ((cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z')) return 1;
    if (cp >= 0xC0 && cp < 0x2000 && cp != 0xD7 && cp != 0xF7) return 1; /* loose */
    if (cp >= 0x100) return 1; /* treat non-ASCII as letter-ish for pretok */
    return 0;
}

static int is_digit_cp(int cp) { return cp >= '0' && cp <= '9'; }

/* ---- BPE one pretoken (raw UTF-8 bytes of the pretok span) ----
 * Fixed-array rewrite of the original malloc-per-symbol / strlen-per-pair loop.
 * Symbols are (offset,length) slices of ONE contiguous byte-encoded buffer, so a
 * merge just extends the left slice and drops the right (adjacent slices touch);
 * no malloc, no strlen, no per-symbol copies. Adjacent pair-ranks are cached and
 * only the two neighbours of a merge are recomputed, replacing the original full
 * O(ns^2) rescan of merges_get with O(ns) lookups. The 512-byte merge-key cap is
 * kept identical to the original, so output is byte-for-byte the same. */
static int bpe_mk_key(char *key, const unsigned char *a, int alen,
                      const unsigned char *b, int blen_) {
    if (alen + blen_ + 2 > 512) return -1; /* same cap as the original keybuf */
    memcpy(key, a, (size_t)alen); key[alen] = '\x1f';
    memcpy(key + alen + 1, b, (size_t)blen_); key[alen + 1 + blen_] = 0;
    return alen + 1 + blen_;
}
static void bpe_core_str(const cce_gguf_tok *t, const char *w, int wlen,
                     int *ids, int *cnt, int max_ids,
                     unsigned char *buf, int *soff, int *slen, int *rnk,
                     char *vkey, size_t vcap) {
    int ns = 0, blen = 0, k;
    char mkey[512];
    for (k = 0; k < wlen; ++k) {
        int b = (unsigned char)w[k], bl = t->byte_enc_len[b];
        memcpy(buf + blen, t->byte_enc[b], (size_t)bl);
        soff[ns] = blen; slen[ns] = bl; blen += bl; ns++;
    }
    for (k = 0; k + 1 < ns; ++k) {
        int kl = bpe_mk_key(mkey, buf + soff[k], slen[k], buf + soff[k] + slen[k], slen[k + 1]);
        int r = (kl < 0) ? -1 : merges_get(t, mkey);
        rnk[k] = (r < 0) ? 0x7fffffff : r;
    }
    while (ns > 1) {
        int best = 0x7fffffff, bi = -1;
        for (k = 0; k + 1 < ns; ++k) if (rnk[k] < best) { best = rnk[k]; bi = k; }
        if (bi < 0) break;                          /* no mergeable adjacent pair */
        slen[bi] += slen[bi + 1];                   /* extend left slice over right */
        for (k = bi + 1; k + 1 < ns; ++k) { soff[k] = soff[k + 1]; slen[k] = slen[k + 1]; rnk[k] = rnk[k + 1]; }
        ns--;
        if (bi + 1 < ns) {                          /* recompute pair (bi, bi+1) */
            int kl = bpe_mk_key(mkey, buf + soff[bi], slen[bi], buf + soff[bi] + slen[bi], slen[bi + 1]);
            int r = (kl < 0) ? -1 : merges_get(t, mkey);
            rnk[bi] = (r < 0) ? 0x7fffffff : r;
        }
        if (bi > 0) {                               /* recompute pair (bi-1, bi) */
            int kl = bpe_mk_key(mkey, buf + soff[bi - 1], slen[bi - 1], buf + soff[bi - 1] + slen[bi - 1], slen[bi]);
            int r = (kl < 0) ? -1 : merges_get(t, mkey);
            rnk[bi - 1] = (r < 0) ? 0x7fffffff : r;
        }
    }
    for (k = 0; k < ns; ++k) {
        int id;
        if ((size_t)slen[k] + 1 > vcap) continue;
        memcpy(vkey, buf + soff[k], (size_t)slen[k]); vkey[slen[k]] = 0;
        id = vocab_get(t, vkey);
        if (id >= 0 && *cnt < max_ids) ids[(*cnt)++] = id;
    }
}
/* Integer BPE: symbols are token ids, ranks/merges come from the id-pair table,
   so the merge loop touches no strings — no fnv1a, no strcmp, no key building.
   Used whenever id_bpe is set (all bytes+merges resolve to ids); byte-for-byte
   identical to bpe_core_str. */
static void bpe_core_id(const cce_gguf_tok *t, const char *w, int wlen,
                        int *ids, int *cnt, int max_ids,
                        int *sid, int *rnk, int *mrg) {
    int ns = 0, k;
    for (k = 0; k < wlen; ++k) sid[ns++] = t->byte_to_id[(unsigned char)w[k]];
    for (k = 0; k + 1 < ns; ++k) { int m = -1, r = pair_get(t, sid[k], sid[k + 1], &m); rnk[k] = (r < 0) ? 0x7fffffff : r; mrg[k] = m; }
    while (ns > 1) {
        int best = 0x7fffffff, bi = -1;
        for (k = 0; k + 1 < ns; ++k) if (rnk[k] < best) { best = rnk[k]; bi = k; }
        if (bi < 0) break;
        sid[bi] = mrg[bi];                          /* merged symbol id */
        for (k = bi + 1; k + 1 < ns; ++k) { sid[k] = sid[k + 1]; rnk[k] = rnk[k + 1]; mrg[k] = mrg[k + 1]; }
        ns--;
        if (bi + 1 < ns) { int m = -1, r = pair_get(t, sid[bi], sid[bi + 1], &m); rnk[bi] = (r < 0) ? 0x7fffffff : r; mrg[bi] = m; }
        if (bi > 0) { int m = -1, r = pair_get(t, sid[bi - 1], sid[bi], &m); rnk[bi - 1] = (r < 0) ? 0x7fffffff : r; mrg[bi - 1] = m; }
    }
    for (k = 0; k < ns; ++k) if (*cnt < max_ids) ids[(*cnt)++] = sid[k];
}
static void bpe_word(const cce_gguf_tok *t, const char *w, int wlen,
                     int *ids, int *cnt, int max_ids) {
    if (wlen <= 0 || !w) return;
    if (t->id_bpe) { /* integer path */
        if (wlen <= 256) { int sid[256], rnk[256], mrg[256];
            bpe_core_id(t, w, wlen, ids, cnt, max_ids, sid, rnk, mrg);
        } else {
            int *sid = (int *)malloc((size_t)wlen * sizeof(int));
            int *rnk = (int *)malloc((size_t)wlen * sizeof(int));
            int *mrg = (int *)malloc((size_t)wlen * sizeof(int));
            if (sid && rnk && mrg) bpe_core_id(t, w, wlen, ids, cnt, max_ids, sid, rnk, mrg);
            free(sid); free(rnk); free(mrg);
        }
        return;
    }
    if (wlen <= 256) { /* string path (fallback), pure stack */
        unsigned char buf[256 * 4]; int soff[256], slen[256], rnk[256]; char vkey[256 * 4 + 8];
        bpe_core_str(t, w, wlen, ids, cnt, max_ids, buf, soff, slen, rnk, vkey, sizeof vkey);
    } else { /* rare long span: one bulk allocation, not per-symbol */
        size_t bs = (size_t)wlen * 4 + 8;
        unsigned char *buf = (unsigned char *)malloc(bs);
        int *soff = (int *)malloc((size_t)wlen * sizeof(int));
        int *slen = (int *)malloc((size_t)wlen * sizeof(int));
        int *rnk = (int *)malloc((size_t)wlen * sizeof(int));
        char *vkey = (char *)malloc(bs);
        if (buf && soff && slen && rnk && vkey)
            bpe_core_str(t, w, wlen, ids, cnt, max_ids, buf, soff, slen, rnk, vkey, bs);
        free(buf); free(soff); free(slen); free(rnk); free(vkey);
    }
}

/* Qwen/GPT-2-ish pretok over a non-special span */
static void pretok_encode(const cce_gguf_tok *t, const char *s, int n,
                          int *ids, int *cnt, int max_ids) {
    int i = 0;
    while (i < n && *cnt < max_ids) {
        int start = i, adv = 1;
        int cp = utf8_cp(s + i, &adv);
        /* contractions 's 't 're 've 'm 'll 'd */
        if (cp == '\'') {
            int rem = n - (i + 1);
            const char *r = s + i + 1;
            int take = 0;
            if (rem >= 1 && (r[0] == 's' || r[0] == 't' || r[0] == 'm' || r[0] == 'd'))
                take = 2;
            else if (rem >= 2 &&
                     ((r[0] == 'r' && r[1] == 'e') || (r[0] == 'v' && r[1] == 'e') ||
                      (r[0] == 'l' && r[1] == 'l')))
                take = 3;
            if (take) {
                bpe_word(t, s + start, take, ids, cnt, max_ids);
                i += take;
                continue;
            }
        }
        if (cp == ' ') {
            int j = i + 1;
            if (j < n) {
                int a2, c2 = utf8_cp(s + j, &a2);
                if (is_letter_cp(c2)) {
                    j += a2;
                    while (j < n) {
                        int a3, c3 = utf8_cp(s + j, &a3);
                        if (!is_letter_cp(c3) && c3 != '\'') break;
                        j += a3;
                    }
                } else if (is_digit_cp(c2)) {
                    j += a2;
                    while (j < n) {
                        int a3, c3 = utf8_cp(s + j, &a3);
                        if (!is_digit_cp(c3)) break;
                        j += a3;
                    }
                } else if (c2 != ' ' && c2 != '\n' && c2 != '\r' && c2 != '\t') {
                    j += a2;
                    while (j < n) {
                        int a3, c3 = utf8_cp(s + j, &a3);
                        if (c3 == ' ' || c3 == '\n' || c3 == '\r' || c3 == '\t' ||
                            is_letter_cp(c3) || is_digit_cp(c3))
                            break;
                        j += a3;
                    }
                } else {
                    /* pure whitespace run */
                    while (j < n) {
                        int a3, c3 = utf8_cp(s + j, &a3);
                        if (c3 != ' ' && c3 != '\t') break;
                        j += a3;
                    }
                }
            }
            bpe_word(t, s + start, j - start, ids, cnt, max_ids);
            i = j;
            continue;
        }
        if (is_letter_cp(cp)) {
            int j = i + adv;
            while (j < n) {
                int a3, c3 = utf8_cp(s + j, &a3);
                if (!is_letter_cp(c3) && c3 != '\'') break;
                j += a3;
            }
            bpe_word(t, s + start, j - start, ids, cnt, max_ids);
            i = j;
            continue;
        }
        if (is_digit_cp(cp)) {
            int j = i + adv;
            while (j < n) {
                int a3, c3 = utf8_cp(s + j, &a3);
                if (!is_digit_cp(c3)) break;
                j += a3;
            }
            bpe_word(t, s + start, j - start, ids, cnt, max_ids);
            i = j;
            continue;
        }
        if (cp == '\n' || cp == '\r') {
            /* swallow \r\n as one or keep separate — Qwen maps each byte */
            bpe_word(t, s + start, adv, ids, cnt, max_ids);
            i += adv;
            continue;
        }
        if (cp == '\t' || (cp < 0x20)) {
            bpe_word(t, s + start, adv, ids, cnt, max_ids);
            i += adv;
            continue;
        }
        /* punctuation / other: take run of non-space non-alnum */
        {
            int j = i + adv;
            while (j < n) {
                int a3, c3 = utf8_cp(s + j, &a3);
                if (c3 == ' ' || c3 == '\n' || c3 == '\r' || c3 == '\t' ||
                    is_letter_cp(c3) || is_digit_cp(c3))
                    break;
                j += a3;
            }
            bpe_word(t, s + start, j - start, ids, cnt, max_ids);
            i = j;
        }
    }
}

/* ---- load ---- */
cce_result cce_gguf_tok_load(const char *gguf_path, cce_gguf_tok **out) {
    FILE *f = NULL;
    cce_gguf_tok *t = NULL;
    char magic[4];
    uint32_t ver = 0;
    uint64_t n_tensors = 0, n_kv = 0;
    uint64_t ki;
    char **tokens = NULL;
    int *types = NULL;
    int n_tok = 0;
    char **merges = NULL;
    int n_merges = 0;
    int i;

    if (!gguf_path || !out) return CCE_ERR_INVALID_ARG;
    *out = NULL;
    f = fopen(gguf_path, "rb");
    if (!f) return CCE_ERR_IO;
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "GGUF", 4) != 0) {
        fclose(f);
        return CCE_ERR_UNSUPPORTED;
    }
    if (!rd_u32(f, &ver) || !rd_u64(f, &n_tensors) || !rd_u64(f, &n_kv)) {
        fclose(f);
        return CCE_ERR_IO;
    }

    for (ki = 0; ki < n_kv; ++ki) {
        char *key = NULL;
        uint32_t typ = 0;
        if (!rd_string_alloc(f, &key) || !rd_u32(f, &typ)) {
            free(key);
            goto fail_io;
        }
        if (typ == GGUF_STR) {
            char *val = NULL;
            if (!rd_string_alloc(f, &val)) { free(key); goto fail_io; }
            free(val);
        } else if (typ == GGUF_ARR) {
            uint32_t et = 0;
            uint64_t n = 0;
            if (!rd_u32(f, &et) || !rd_u64(f, &n)) { free(key); goto fail_io; }
            if (key && strcmp(key, "tokenizer.ggml.tokens") == 0 && et == GGUF_STR &&
                n > 0 && n < (1u << 24)) {
                free(tokens);
                tokens = (char **)calloc((size_t)n, sizeof(char *));
                n_tok = (int)n;
                if (!tokens) { free(key); goto fail_oom; }
                for (i = 0; i < n_tok; ++i) {
                    if (!rd_string_alloc(f, &tokens[i])) { free(key); goto fail_io; }
                }
            } else if (key && strcmp(key, "tokenizer.ggml.merges") == 0 &&
                       et == GGUF_STR && n > 0 && n < (1u << 24)) {
                free(merges);
                merges = (char **)calloc((size_t)n, sizeof(char *));
                n_merges = (int)n;
                if (!merges) { free(key); goto fail_oom; }
                for (i = 0; i < n_merges; ++i) {
                    if (!rd_string_alloc(f, &merges[i])) { free(key); goto fail_io; }
                }
            } else if (key && strcmp(key, "tokenizer.ggml.token_type") == 0 &&
                       et == GGUF_I32 && n > 0 && n < (1u << 24)) {
                free(types);
                types = (int *)malloc((size_t)n * sizeof(int));
                if (!types) { free(key); goto fail_oom; }
                for (i = 0; i < (int)n; ++i) {
                    int32_t v = 0;
                    if (fread(&v, 4, 1, f) != 1) { free(key); goto fail_io; }
                    types[i] = (int)v;
                }
            } else if (et == GGUF_STR) {
                for (i = 0; i < (int)n; ++i)
                    if (!skip_string(f)) { free(key); goto fail_io; }
            } else {
                size_t es = elem_size(et);
                if (!es || cce_fseeko(f, (off_t)(n * es), SEEK_CUR) != 0) {
                    free(key);
                    goto fail_io;
                }
            }
        } else {
            size_t es = elem_size(typ);
            if (!es || cce_fseeko(f, (off_t)es, SEEK_CUR) != 0) {
                free(key);
                goto fail_io;
            }
        }
        free(key);
    }
    fclose(f);
    f = NULL;

    if (!tokens || n_tok < 1) {
        /* free partial */
        if (merges) {
            for (i = 0; i < n_merges; ++i) free(merges[i]);
            free(merges);
        }
        free(types);
        return CCE_ERR_NOT_FOUND;
    }

    t = (cce_gguf_tok *)calloc(1, sizeof(*t));
    if (!t) goto fail_oom;
    t->vocab = n_tok;
    t->id_to_piece = tokens;
    tokens = NULL; /* owned by t */
    t->tok_type = (unsigned char *)calloc((size_t)n_tok, 1);
    if (!t->tok_type) goto fail_oom;
    for (i = 0; i < n_tok; ++i)
        t->tok_type[i] = (types && i < n_tok) ? (unsigned char)types[i] : TOK_NORMAL;
    free(types);
    types = NULL;

    t->eos_id = t->bos_id = t->pad_id = t->im_start_id = t->im_end_id = -1;
    t->vcap = next_pow2(n_tok * 2);
    t->vkey = (const char **)calloc((size_t)t->vcap, sizeof(char *));
    t->vid = (int *)calloc((size_t)t->vcap, sizeof(int));
    if (!t->vkey || !t->vid) goto fail_oom;
    for (i = 0; i < n_tok; ++i) {
        if (t->id_to_piece[i]) vocab_put(t, t->id_to_piece[i], i);
    }

    /* specials: type CONTROL or USER_DEFINED, longest-first later */
    {
        int cap = 64, ns = 0;
        t->special_str = (char **)calloc((size_t)cap, sizeof(char *));
        t->special_id = (int *)calloc((size_t)cap, sizeof(int));
        if (!t->special_str || !t->special_id) goto fail_oom;
        for (i = 0; i < n_tok; ++i) {
            unsigned char ty = t->tok_type[i];
            const char *p = t->id_to_piece[i];
            if (!p || (ty != TOK_CONTROL && ty != TOK_USER_DEFINED)) continue;
            if (ns >= cap) {
                int ncap = cap * 2;
                char **ns_ = (char **)realloc(t->special_str, (size_t)ncap * sizeof(char *));
                int *ni = (int *)realloc(t->special_id, (size_t)ncap * sizeof(int));
                if (!ns_ || !ni) { free(ns_); free(ni); goto fail_oom; }
                t->special_str = ns_;
                t->special_id = ni;
                cap = ncap;
            }
            t->special_str[ns] = (char *)p; /* non-owning alias */
            t->special_id[ns] = i;
            ns++;
            if (strcmp(p, "<|im_end|>") == 0) t->im_end_id = i;
            if (strcmp(p, "<|im_start|>") == 0) t->im_start_id = i;
            if (strcmp(p, "<|endoftext|>") == 0) t->pad_id = i;
        }
        t->n_special = ns;
        t->eos_id = t->im_end_id >= 0 ? t->im_end_id : t->pad_id;
        /* longest special first for greedy encode scan */
        for (i = 0; i < ns; ++i) {
            int j;
            for (j = i + 1; j < ns; ++j) {
                if (strlen(t->special_str[j]) > strlen(t->special_str[i])) {
                    char *ts = t->special_str[i];
                    int ti = t->special_id[i];
                    t->special_str[i] = t->special_str[j];
                    t->special_id[i] = t->special_id[j];
                    t->special_str[j] = ts;
                    t->special_id[j] = ti;
                }
            }
        }
    }

    t->n_merges = n_merges;
    t->mcap = next_pow2(n_merges * 2 + 16);
    t->mkey = (char **)calloc((size_t)t->mcap, sizeof(char *));
    t->mrank = (int *)calloc((size_t)t->mcap, sizeof(int));
    if (!t->mkey || !t->mrank) goto fail_oom;
    /* id-space merge table, built alongside the string table (needs vocab, which
       is already populated above). pair_ok drops to 0 if any merge fails to
       resolve all three ids, forcing the safe string path. */
    t->pcap = t->mcap;
    t->pe = (PairE *)calloc((size_t)t->pcap, sizeof(PairE));
    if (!t->pe) goto fail_oom;
    {
        int pair_ok = 1;
        for (i = 0; i < n_merges; ++i) {
            char *m = merges[i];
            char *sp;
            if (!m) continue;
            sp = strchr(m, ' ');
            if (!sp) { free(m); continue; }
            *sp = 0;
            {
                size_t la = strlen(m), lb = strlen(sp + 1);
                char *key = (char *)malloc(la + lb + 2);
                if (!key) { free(m); continue; }
                memcpy(key, m, la);
                key[la] = '\x1f';
                memcpy(key + la + 1, sp + 1, lb);
                key[la + 1 + lb] = 0;
                merges_put(t, key, i);
                { int ida = vocab_get(t, m), idb = vocab_get(t, sp + 1), idab = -1;
                  char *ab = (char *)malloc(la + lb + 1);
                  if (ab) { memcpy(ab, m, la); memcpy(ab + la, sp + 1, lb); ab[la + lb] = 0; idab = vocab_get(t, ab); free(ab); }
                  if (ida >= 0 && idb >= 0 && idab >= 0) pair_put(t, ida, idb, i, idab); else pair_ok = 0; }
            }
            free(m);
        }
        free(merges);
        merges = NULL;

        build_byte_tables(t);
        { int b, bytes_ok = 1;
          for (b = 0; b < 256; ++b) { int id = vocab_get(t, t->byte_enc[b]); t->byte_to_id[b] = id; if (id < 0) bytes_ok = 0; }
          t->id_bpe = (pair_ok && bytes_ok) ? 1 : 0; }
    }
    *out = t;
    return CCE_OK;

fail_oom:
    if (f) fclose(f);
    if (tokens) {
        for (i = 0; i < n_tok; ++i) free(tokens[i]);
        free(tokens);
    }
    if (merges) {
        for (i = 0; i < n_merges; ++i) free(merges[i]);
        free(merges);
    }
    free(types);
    cce_gguf_tok_free(t);
    return CCE_ERR_OOM;
fail_io:
    if (f) fclose(f);
    if (tokens) {
        for (i = 0; i < n_tok; ++i) free(tokens[i]);
        free(tokens);
    }
    if (merges) {
        for (i = 0; i < n_merges; ++i) free(merges[i]);
        free(merges);
    }
    free(types);
    cce_gguf_tok_free(t);
    return CCE_ERR_IO;
}

void cce_gguf_tok_free(cce_gguf_tok *t) {
    int i;
    if (!t) return;
    if (t->id_to_piece) {
        for (i = 0; i < t->vocab; ++i) free(t->id_to_piece[i]);
        free(t->id_to_piece);
    }
    free(t->tok_type);
    free((void *)t->vkey);
    free(t->vid);
    if (t->mkey) {
        for (i = 0; i < t->mcap; ++i) free(t->mkey[i]);
        free(t->mkey);
    }
    free(t->mrank);
    free(t->pe);
    ptc_free(t->pcache);
    free(t->special_str); /* aliases into id_to_piece */
    free(t->special_id);
    free(t);
}

int cce_gguf_tok_vocab_size(const cce_gguf_tok *t) { return t ? t->vocab : 0; }
int cce_gguf_tok_eos_id(const cce_gguf_tok *t) { return t ? t->eos_id : -1; }
int cce_gguf_tok_bos_id(const cce_gguf_tok *t) { return t ? t->bos_id : -1; }

int cce_gguf_tok_is_special(const cce_gguf_tok *t, int id) {
    if (!t || id < 0 || id >= t->vocab) return 0;
    {
        unsigned char ty = t->tok_type[id];
        return ty == TOK_CONTROL || ty == TOK_USER_DEFINED;
    }
}

const char *cce_gguf_tok_piece(const cce_gguf_tok *t, int id) {
    if (!t || id < 0 || id >= t->vocab) return NULL;
    return t->id_to_piece[id];
}

/* ================= fast encode path (gigatoken-style) =====================
 * (1) SWAR fast-skip of ASCII letter/digit/other runs inside the SAME grammar
 *     as pretok_encode (non-ASCII and apostrophes still go through the exact
 *     codepoint logic, so output is identical), and
 * (2) a pretoken cache that memoizes span -> token-ids, skipping bpe_word on the
 *     ~99% of pretokens that repeat. Both are exact: the bench asserts the fast
 *     path's ids equal cce_gguf_tok_encode's byte-for-byte. */

#define GT_ONES 0x0101010101010101ULL
#define GT_HIGH 0x8080808080808080ULL
#define GT_LOW7 0x7F7F7F7F7F7F7F7FULL
static inline uint64_t gt_ld(const char *p) { uint64_t w; memcpy(&w, p, 8); return w; }
static inline uint64_t gt_nz(uint64_t x) { return (((x & GT_LOW7) + GT_LOW7) | x) & GT_HIGH; }
static inline uint64_t gt_eq(uint64_t x, unsigned v) { return (~gt_nz(x ^ (GT_ONES * v))) & GT_HIGH; }
static inline uint64_t gt_lt(uint64_t x, unsigned nn) { return ((GT_ONES * (0x7Fu + nn)) - (x & GT_LOW7)) & GT_HIGH & ~x; }
static inline int gt_first(uint64_t h) { return h ? (__builtin_ctzll(h) >> 3) : 8; }

/* advance over ASCII [A-Za-z] (non-ASCII stops -> codepoint path decides) */
static int skip_alpha(const char *s, int n, int i) {
    while (i + 8 <= n) { uint64_t w = gt_ld(s + i), lo = w | (GT_ONES * 0x20);
        uint64_t a = gt_lt(lo, 0x7B) & (~gt_lt(lo, 0x61)) & GT_HIGH;
        int k = gt_first((~a) & GT_HIGH); i += k; if (k < 8) return i; }
    while (i < n) { unsigned char b = (unsigned char)s[i], l = (unsigned char)(b | 0x20); if (l < 'a' || l > 'z') break; i++; }
    return i;
}
static int skip_digit(const char *s, int n, int i) {
    while (i + 8 <= n) { uint64_t w = gt_ld(s + i);
        uint64_t d = gt_lt(w, 0x3A) & (~gt_lt(w, 0x30)) & GT_HIGH;
        int k = gt_first((~d) & GT_HIGH); i += k; if (k < 8) return i; }
    while (i < n) { unsigned char b = (unsigned char)s[i]; if (b < '0' || b > '9') break; i++; }
    return i;
}
/* advance over ASCII "other" (not alpha/digit/ws, and ASCII); non-ASCII stops */
static int skip_other(const char *s, int n, int i) {
    while (i + 8 <= n) { uint64_t w = gt_ld(s + i), lo = w | (GT_ONES * 0x20);
        uint64_t a = gt_lt(lo, 0x7B) & (~gt_lt(lo, 0x61)) & GT_HIGH;
        uint64_t d = gt_lt(w, 0x3A) & (~gt_lt(w, 0x30)) & GT_HIGH;
        uint64_t ws = gt_eq(w, 0x20) | gt_eq(w, 0x09) | gt_eq(w, 0x0A) | gt_eq(w, 0x0D);
        uint64_t stop = (a | d | ws | (w & GT_HIGH)) & GT_HIGH;
        int k = gt_first(stop); i += k; if (k < 8) return i; }
    while (i < n) { unsigned char b = (unsigned char)s[i], l = (unsigned char)(b | 0x20);
        if (b >= 0x80 || (l >= 'a' && l <= 'z') || (b >= '0' && b <= '9') || b == ' ' || b == '\n' || b == '\r' || b == '\t') break;
        i++; }
    return i;
}

/* ---- pretoken cache: span bytes -> token ids ----------------------------
 * Cache-line-packed (gigatoken pretoken_cache.rs style): each entry is exactly
 * one 64-byte cache line with the span key AND token ids inlined, so a hit
 * touches ONE line with no pointer chasing (the old design followed two heap
 * pointers per hit). The table is 2 MiB-aligned + MADV_HUGEPAGE so a large table
 * stays in the dTLB. Spans that don't fit inline (key > PTC_KEYMAX, or > PTC_IDMAX
 * tokens) are encoded directly and not cached — rare for natural text. */
enum { PTC_KEYMAX = 22, PTC_IDMAX = 8 };
typedef struct {
    uint64_t h;                 /* hash; 0 = empty slot */
    uint8_t  blen;              /* span byte length (<= PTC_KEYMAX) */
    uint8_t  nids;              /* token count (<= PTC_IDMAX) */
    uint8_t  key[PTC_KEYMAX];   /* inline span bytes */
    int32_t  ids[PTC_IDMAX];    /* inline token ids */
} PtcE;
typedef struct Ptc { PtcE *e; int cap; int cnt; } Ptc;
_Static_assert(sizeof(PtcE) == 64, "PtcE must be exactly one cache line");

/* FNV-1a over the span bytes. NB: an 8-byte-at-a-time multiply-mix hash was
   tried and MEASURED SLOWER here (gigatok_cache_bench: 4.3 vs 2.5 ns) — for the
   short 4-14 byte pretokens this table sees, FNV's byte loop pipelines well and
   avoids a variable-length tail memcpy. Keep FNV. */
static uint64_t ptc_hash(const char *s, int n) {
    uint64_t h = 1469598103934665603ULL; int k;
    for (k = 0; k < n; k++) { h ^= (unsigned char)s[k]; h *= 1099511628211ULL; }
    return h ? h : 1;
}
/* Zeroed slot array; 2 MiB-aligned + MADV_HUGEPAGE once it outgrows one huge
   page (madvise BEFORE the zeroing fault so it maps as 2 MiB pages). */
static PtcE *ptc_alloc(int cap) {
    size_t bytes = (size_t)cap * sizeof(PtcE);
    PtcE *e = NULL;
#if defined(__linux__) && defined(MADV_HUGEPAGE)
    if (bytes >= 2u * 1024 * 1024) {
        if (posix_memalign((void **)&e, 2u * 1024 * 1024, bytes) != 0) e = NULL;
        if (e) { madvise(e, bytes, MADV_HUGEPAGE); memset(e, 0, bytes); return e; }
    }
#endif
    return (PtcE *)calloc((size_t)cap, sizeof(PtcE));
}
static Ptc *ptc_new(int cap0) {
    Ptc *c = (Ptc *)calloc(1, sizeof *c); if (!c) return NULL;
    c->cap = cap0; c->e = ptc_alloc(cap0);
    if (!c->e) { free(c); return NULL; } return c;
}
static void ptc_free(Ptc *c) { if (!c) return; free(c->e); free(c); } /* entries are inline */
static void ptc_grow(Ptc *c) {
    int nc = c->cap * 2, k; PtcE *ne = ptc_alloc(nc);
    if (!ne) return;
    for (k = 0; k < c->cap; k++) if (c->e[k].h) {
        uint64_t idx = c->e[k].h & (uint64_t)(nc - 1);
        while (ne[idx].h) idx = (idx + 1) & (uint64_t)(nc - 1);
        ne[idx] = c->e[k];
    }
    free(c->e); c->e = ne; c->cap = nc;
}
/* Look up (or compute+store) the encoding of span [w,wlen); append its ids. */
static void emit_word(const cce_gguf_tok *t, Ptc *cache, const char *w, int wlen,
                      int *ids, int *cnt, int max_ids) {
    uint64_t h, idx; PtcE *e; int tmp[128], tn = 0, k;
    if (!cache || wlen <= 0 || wlen > PTC_KEYMAX) { bpe_word(t, w, wlen, ids, cnt, max_ids); return; }
    if ((cache->cnt + 1) * 4 >= cache->cap * 3) ptc_grow(cache);
    h = ptc_hash(w, wlen);
    idx = h & (uint64_t)(cache->cap - 1);
    for (;;) {
        e = &cache->e[idx];
        if (!e->h) break;                                       /* empty -> miss */
        if (e->h == h && e->blen == wlen && memcmp(e->key, w, (size_t)wlen) == 0) {
            for (k = 0; k < e->nids && *cnt < max_ids; k++) ids[(*cnt)++] = e->ids[k];
            return;                                             /* hit: one cache line */
        }
        idx = (idx + 1) & (uint64_t)(cache->cap - 1);
    }
    bpe_word(t, w, wlen, tmp, &tn, 128);                        /* miss: compute + output */
    for (k = 0; k < tn && *cnt < max_ids; k++) ids[(*cnt)++] = tmp[k];
    if (tn <= PTC_IDMAX) {                                      /* store inline if it fits */
        e->h = h; e->blen = (uint8_t)wlen; e->nids = (uint8_t)tn;
        memcpy(e->key, w, (size_t)wlen);
        for (k = 0; k < tn; k++) e->ids[k] = tmp[k];
        cache->cnt++;
    }
}

/* Same grammar as pretok_encode, with SWAR run-skips (swar) + cache emit. */
static void pretok_encode_fast(const cce_gguf_tok *t, const char *s, int n,
                               int *ids, int *cnt, int max_ids, int swar, Ptc *cache) {
    int i = 0;
    while (i < n && *cnt < max_ids) {
        int start = i, adv = 1;
        int cp = utf8_cp(s + i, &adv);
        if (cp == '\'') {
            int rem = n - (i + 1); const char *r = s + i + 1; int take = 0;
            if (rem >= 1 && (r[0] == 's' || r[0] == 't' || r[0] == 'm' || r[0] == 'd')) take = 2;
            else if (rem >= 2 && ((r[0] == 'r' && r[1] == 'e') || (r[0] == 'v' && r[1] == 'e') || (r[0] == 'l' && r[1] == 'l'))) take = 3;
            if (take) { emit_word(t, cache, s + start, take, ids, cnt, max_ids); i += take; continue; }
        }
        if (cp == ' ') {
            int j = i + 1;
            if (j < n) {
                int a2, c2 = utf8_cp(s + j, &a2);
                if (is_letter_cp(c2)) { j += a2; for (;;) { if (swar) j = skip_alpha(s, n, j); if (j >= n) break; int a3, c3 = utf8_cp(s + j, &a3); if (!is_letter_cp(c3) && c3 != '\'') break; j += a3; } }
                else if (is_digit_cp(c2)) { j += a2; for (;;) { if (swar) j = skip_digit(s, n, j); if (j >= n) break; int a3, c3 = utf8_cp(s + j, &a3); if (!is_digit_cp(c3)) break; j += a3; } }
                else if (c2 != ' ' && c2 != '\n' && c2 != '\r' && c2 != '\t') { j += a2; for (;;) { if (swar) j = skip_other(s, n, j); if (j >= n) break; int a3, c3 = utf8_cp(s + j, &a3); if (c3 == ' ' || c3 == '\n' || c3 == '\r' || c3 == '\t' || is_letter_cp(c3) || is_digit_cp(c3)) break; j += a3; } }
                else { while (j < n) { int a3, c3 = utf8_cp(s + j, &a3); if (c3 != ' ' && c3 != '\t') break; j += a3; } }
            }
            emit_word(t, cache, s + start, j - start, ids, cnt, max_ids); i = j; continue;
        }
        if (is_letter_cp(cp)) { int j = i + adv; for (;;) { if (swar) j = skip_alpha(s, n, j); if (j >= n) break; int a3, c3 = utf8_cp(s + j, &a3); if (!is_letter_cp(c3) && c3 != '\'') break; j += a3; } emit_word(t, cache, s + start, j - start, ids, cnt, max_ids); i = j; continue; }
        if (is_digit_cp(cp)) { int j = i + adv; for (;;) { if (swar) j = skip_digit(s, n, j); if (j >= n) break; int a3, c3 = utf8_cp(s + j, &a3); if (!is_digit_cp(c3)) break; j += a3; } emit_word(t, cache, s + start, j - start, ids, cnt, max_ids); i = j; continue; }
        if (cp == '\n' || cp == '\r') { emit_word(t, cache, s + start, adv, ids, cnt, max_ids); i += adv; continue; }
        if (cp == '\t' || (cp < 0x20)) { emit_word(t, cache, s + start, adv, ids, cnt, max_ids); i += adv; continue; }
        { int j = i + adv; for (;;) { if (swar) j = skip_other(s, n, j); if (j >= n) break; int a3, c3 = utf8_cp(s + j, &a3); if (c3 == ' ' || c3 == '\n' || c3 == '\r' || c3 == '\t' || is_letter_cp(c3) || is_digit_cp(c3)) break; j += a3; } emit_word(t, cache, s + start, j - start, ids, cnt, max_ids); i = j; }
    }
}

/* Allocate the persistent pretoken cache on `t` (idempotent). After this, encode
   with CCE_TOK_FAST_PERSIST to reuse it across calls. */
void cce_gguf_tok_cache_enable(cce_gguf_tok *t) {
    if (t && !t->pcache) t->pcache = ptc_new(1024);
}

int cce_gguf_tok_encode_fast(const cce_gguf_tok *t, const char *text,
                             int *ids, int max_ids, int flags) {
    int n = 0, i = 0, L; Ptc *cache = NULL; int owns = 0;
    int swar = (flags & CCE_TOK_FAST_SWAR) != 0;
    if (!t || !text || !ids || max_ids < 1) return 0;
    /* Persistent cache (on t) survives across calls; a per-call cache does not.
       Reading t->pcache through const t is fine — only *cache is mutated. */
    if ((flags & CCE_TOK_FAST_PERSIST) && t->pcache) cache = t->pcache;
    else if (flags & CCE_TOK_FAST_CACHE) { cache = ptc_new(1024); owns = 1; }
    L = (int)strlen(text);
    while (i < L && n < max_ids) {
        int hit = -1, hlen = 0, k;
        for (k = 0; k < t->n_special; ++k) {
            const char *sp = t->special_str[k];
            int sl = sp ? (int)strlen(sp) : 0;
            if (sl > 0 && i + sl <= L && memcmp(text + i, sp, (size_t)sl) == 0) { hit = t->special_id[k]; hlen = sl; break; }
        }
        if (hit >= 0) { ids[n++] = hit; i += hlen; continue; }
        { int j = i, found = L;
            for (k = 0; k < t->n_special; ++k) {
                const char *sp = t->special_str[k]; int sl = sp ? (int)strlen(sp) : 0;
                if (sl < 1) continue;
                { const char *p = strstr(text + i, sp); if (p) { int at = (int)(p - text); if (at < found) found = at; } }
            }
            j = found;
            pretok_encode_fast(t, text + i, j - i, ids, &n, max_ids, swar, cache);
            i = j;
        }
    }
    if (owns) ptc_free(cache); /* persistent cache is owned by t, not freed here */
    return n;
}

int cce_gguf_tok_encode(const cce_gguf_tok *t, const char *text,
                        int *ids, int max_ids) {
    int n = 0, i = 0, L;
    if (!t || !text || !ids || max_ids < 1) return 0;
    L = (int)strlen(text);
    while (i < L && n < max_ids) {
        int hit = -1, hlen = 0, k;
        for (k = 0; k < t->n_special; ++k) {
            const char *sp = t->special_str[k];
            int sl = sp ? (int)strlen(sp) : 0;
            if (sl > 0 && i + sl <= L && memcmp(text + i, sp, (size_t)sl) == 0) {
                hit = t->special_id[k];
                hlen = sl;
                break;
            }
        }
        if (hit >= 0) {
            ids[n++] = hit;
            i += hlen;
            continue;
        }
        /* span until next special */
        {
            int j = i, found = L;
            for (k = 0; k < t->n_special; ++k) {
                const char *sp = t->special_str[k];
                int sl = sp ? (int)strlen(sp) : 0;
                if (sl < 1) continue;
                {
                    const char *p = strstr(text + i, sp);
                    if (p) {
                        int at = (int)(p - text);
                        if (at < found) found = at;
                    }
                }
            }
            j = found;
            pretok_encode(t, text + i, j - i, ids, &n, max_ids);
            i = j;
        }
    }
    return n;
}

int cce_gguf_tok_decode(const cce_gguf_tok *t, const int *ids, int n_ids,
                        char *out, int max_out, int skip_special) {
    int w = 0, k;
    if (!out || max_out <= 0) return 0;
    out[0] = 0;
    if (!t || !ids || n_ids < 1) return 0;
    for (k = 0; k < n_ids; ++k) {
        int id = ids[k];
        const char *piece;
        int p = 0;
        if (id < 0 || id >= t->vocab) continue;
        if (skip_special && cce_gguf_tok_is_special(t, id)) continue;
        piece = t->id_to_piece[id];
        if (!piece) continue;
        while (piece[p] && w < max_out - 1) {
            int adv = 1, cp = utf8_cp(piece + p, &adv);
            int byte = (cp >= 0 && cp < 1024) ? t->cp_to_byte[cp] : -1;
            if (byte >= 0) {
                out[w++] = (char)byte;
            } else {
                /* passthrough raw utf-8 of piece */
                int q;
                for (q = 0; q < adv && w < max_out - 1; ++q)
                    out[w++] = piece[p + q];
            }
            p += adv;
        }
    }
    out[w] = 0;
    return w;
}

static const char *QWYTHOS_IDENTITY =
    "You are Qwythos, a model created by Empero AI. "
    "Only bring up your identity if the user asks.";

int cce_gguf_tok_chat_template(char *out, int max_out,
                               const char *system, const char *user,
                               int enable_thinking) {
    int n;
    const char *sys = (system && system[0]) ? system : NULL;
    if (!out || max_out < 32 || !user) return -1;
    if (sys) {
        n = snprintf(out, (size_t)max_out,
                     "<|im_start|>system\n%s\n\n%s<|im_end|>\n"
                     "<|im_start|>user\n%s<|im_end|>\n"
                     "<|im_start|>assistant\n",
                     sys, QWYTHOS_IDENTITY, user);
    } else {
        n = snprintf(out, (size_t)max_out,
                     "<|im_start|>system\n%s<|im_end|>\n"
                     "<|im_start|>user\n%s<|im_end|>\n"
                     "<|im_start|>assistant\n",
                     QWYTHOS_IDENTITY, user);
    }
    if (n < 0 || n >= max_out) return -1;
    if (enable_thinking) {
        int n2 = snprintf(out + n, (size_t)(max_out - n), "<think>\n");
        if (n2 < 0 || n + n2 >= max_out) return -1;
        n += n2;
    } else {
        int n2 = snprintf(out + n, (size_t)(max_out - n),
                          "<think>\n\n</think>\n\n");
        if (n2 < 0 || n + n2 >= max_out) return -1;
        n += n2;
    }
    return n;
}

int cce_gguf_tok_encode_chat(const cce_gguf_tok *t,
                             const char *system, const char *user,
                             int enable_thinking,
                             int *ids, int max_ids) {
    char buf[8192];
    int tn;
    if (!t || !ids || max_ids < 1) return 0;
    tn = cce_gguf_tok_chat_template(buf, (int)sizeof buf, system, user,
                                    enable_thinking);
    if (tn < 0) return 0;
    return cce_gguf_tok_encode(t, buf, ids, max_ids);
}
