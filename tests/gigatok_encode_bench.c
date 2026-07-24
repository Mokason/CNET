/* gigatok_encode_bench — end-to-end BPE encode throughput on a real GGUF vocab.
 *
 * Loads a tokenizer from a GGUF, encodes a corpus with cce_gguf_tok's current
 * path vs the gigatoken-style fast path (SWAR pretok skip and/or pretoken cache),
 * verifies the fast path's token ids are IDENTICAL, and reports tokens/s + MB/s.
 *
 * The point: decompose where the end-to-end win actually comes from. gigatoken's
 * own log says the pretoken cache dominates (bpe_word is the bottleneck, not
 * pretokenization) — this measures that on our code.
 *
 * Usage: gigatok_encode_bench <model.gguf> [corpus_file] [size_mb]
 */

#include "../include/cce/cce_gguf_tok.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint32_t S = 0x1234567u;
static uint32_t rr(void) { S = S * 1664525u + 1013904223u; return S; }
static uint32_t rn(uint32_t m) { return rr() % m; }
static const char *WORDS[] = {
    "the", "of", "and", "to", "in", "a", "is", "that", "for", "it", "as", "was",
    "with", "be", "by", "on", "not", "he", "this", "are", "or", "from", "at",
    "which", "but", "have", "an", "they", "you", "one", "had", "word", "model",
    "system", "language", "network", "training", "tokenizer", "throughput",
    "performance", "benchmark", "implementation", "optimization", "research",
    "document", "processing", "algorithm", "don't", "can't", "we're"
};
enum { NW = (int)(sizeof(WORDS) / sizeof(WORDS[0])) };
static size_t gen_corpus(char *b, size_t cap) {
    size_t p = 0; int sb = 0;
    while (p + 32 < cap) {
        if (rn(30) == 0) { b[p++] = '\n'; sb = 0; continue; }
        if (p > 0) b[p++] = ' ';
        if (rn(30) == 0) { int d = 1 + (int)rn(4), k; for (k = 0; k < d; k++) b[p++] = (char)('0' + rn(10)); }
        else { const char *w = WORDS[rn(NW)]; size_t l = strlen(w), k; for (k = 0; k < l; k++) b[p++] = w[k];
               if (rn(50) == 0) { b[p++] = (char)0xC3; b[p++] = (char)0xA9; } }
        if (rn(12) == 0) b[p++] = ".,;:!?"[rn(6)];
        if (++sb > 22) { b[p++] = '.'; sb = 0; }
    }
    b[p] = 0; return p;
}
static double now_s(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9; }

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <model.gguf> [corpus_file] [size_mb]\n", argv[0]); return 2; }
    cce_gguf_tok *t = NULL;
    if (cce_gguf_tok_load(argv[1], &t) != CCE_OK || !t) { fprintf(stderr, "failed to load tokenizer from %s\n", argv[1]); return 1; }
    printf("tokenizer: %s  vocab=%d\n", argv[1], cce_gguf_tok_vocab_size(t));

    char *corpus = NULL; size_t n = 0;
    int arg2_is_size = (argc > 2) && argv[2][0] && (strspn(argv[2], "0123456789") == strlen(argv[2]));
    if (argc > 2 && argv[2][0] && strcmp(argv[2], "-") != 0 && !arg2_is_size) {
        FILE *f = fopen(argv[2], "rb"); if (!f) { fprintf(stderr, "cannot open %s\n", argv[2]); return 1; }
        fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
        size_t cap = (size_t)sz; if (argc > 3) { size_t mb = (size_t)strtoul(argv[3], NULL, 10) * 1024 * 1024; if (mb && mb < cap) cap = mb; }
        corpus = (char *)malloc(cap + 1); n = fread(corpus, 1, cap, f); corpus[n] = 0; fclose(f);
        printf("corpus: %s (%.1f MB)\n", argv[2], (double)n / 1e6);
    } else {
        size_t mb = (argc > 2) ? (size_t)strtoul(argv[2], NULL, 10) : 4; if (!mb) mb = 4;
        size_t cap = mb * 1024 * 1024; corpus = (char *)malloc(cap + 1); n = gen_corpus(corpus, cap);
        printf("corpus: generated pseudo-OpenWebText (%.1f MB)\n", (double)n / 1e6);
    }

    int max_ids = (int)n + 16;
    int *base = (int *)malloc((size_t)max_ids * sizeof(int));
    int *fast = (int *)malloc((size_t)max_ids * sizeof(int));

    /* baseline once for reference ids + count */
    int nb = cce_gguf_tok_encode(t, corpus, base, max_ids);
    double bytes_mb = (double)n / 1e6;
    { uint64_t hsh = 1469598103934665603ULL; int k; for (k = 0; k < nb; k++) { hsh ^= (uint64_t)(unsigned)base[k]; hsh *= 1099511628211ULL; }
      printf("baseline produced %d tokens (%.3f bytes/token)  idhash=%016llx\n\n", nb, (double)n / (nb ? nb : 1), (unsigned long long)hsh); }

    struct { const char *name; int flags; int is_base; } cfg[] = {
        { "baseline (encode)",        0,                                   1 },
        { "SWAR pretok",              CCE_TOK_FAST_SWAR,                    0 },
        { "pretoken cache",           CCE_TOK_FAST_CACHE,                   0 },
        { "SWAR + cache",             CCE_TOK_FAST_SWAR | CCE_TOK_FAST_CACHE, 0 },
    };
    int fail = 0, iters = n > 8u * 1024 * 1024 ? 2 : 3, ci;
    double base_mbs = 0;
    printf("end-to-end encode (best of %d):\n", iters);
    for (ci = 0; ci < 4; ci++) {
        double best = 1e300; int it, nf = 0;
        for (it = 0; it < iters; it++) {
            double t0 = now_s();
            nf = cfg[ci].is_base ? cce_gguf_tok_encode(t, corpus, fast, max_ids)
                                 : cce_gguf_tok_encode_fast(t, corpus, fast, max_ids, cfg[ci].flags);
            double dt = now_s() - t0; if (dt < best) best = dt;
        }
        double mbs = bytes_mb / best, mtok = (double)nf / 1e6 / best;
        int ok = (nf == nb) && (memcmp(fast, base, (size_t)nb * sizeof(int)) == 0);
        if (ci == 0) base_mbs = mbs;
        printf("  %-22s %7.1f MB/s  %6.1f Mtok/s  %5.2fx  %s\n",
               cfg[ci].name, mbs, mtok, mbs / base_mbs, cfg[ci].is_base ? "" : (ok ? "ids OK" : "ids MISMATCH"));
        if (!cfg[ci].is_base && !ok) {
            fail = 1;
            int k; for (k = 0; k < nb && k < nf; k++) if (base[k] != fast[k]) { fprintf(stderr, "    first diff at token %d: base=%d fast=%d\n", k, base[k], fast[k]); break; }
            if (nf != nb) fprintf(stderr, "    count base=%d fast=%d\n", nb, nf);
        }
    }

    /* ---- streaming: many small documents (where the persistent cache pays) ----
     * A per-call cache resets every document; the persistent cache accumulates
     * across the whole stream. All three modes use the SAME doc splits, so they
     * must produce byte-for-byte identical token streams. */
    {
        size_t target = 256, p = 0, capd = n / 64 + 16, nd = 0, d;
        size_t *doff = (size_t *)malloc(capd * sizeof(size_t));
        int *dlen = (int *)malloc(capd * sizeof(int));
        while (p < n && nd < capd) {
            size_t e = p + target;
            if (e >= n) e = n; else { while (e < n && corpus[e] != ' ' && corpus[e] != '\n') e++; }
            doff[nd] = p; dlen[nd] = (int)(e - p); nd++; p = e;
        }
        int *sref = (int *)malloc((size_t)max_ids * sizeof(int)); int nref = 0;
        for (d = 0; d < nd; d++) { char sv = corpus[doff[d] + dlen[d]]; corpus[doff[d] + dlen[d]] = 0;
            nref += cce_gguf_tok_encode(t, corpus + doff[d], sref + nref, max_ids - nref);
            corpus[doff[d] + dlen[d]] = sv; }

        printf("\nstreaming %zu docs (~%zu B each), best of %d:\n", nd, target, iters);
        struct { const char *name; int flags; int persist; } sm[] = {
            { "baseline (no cache)", 0, 0 },
            { "per-call cache",      CCE_TOK_FAST_CACHE,   0 },
            { "persistent cache",    CCE_TOK_FAST_PERSIST, 1 },
        };
        double sbase = 0; int m;
        for (m = 0; m < 3; m++) {
            double best = 1e300; int it, nn = 0;
            if (sm[m].persist) cce_gguf_tok_cache_enable(t);
            for (it = 0; it < iters; it++) {
                double t0 = now_s(); nn = 0;
                for (d = 0; d < nd; d++) { char sv = corpus[doff[d] + dlen[d]]; corpus[doff[d] + dlen[d]] = 0;
                    nn += sm[m].flags ? cce_gguf_tok_encode_fast(t, corpus + doff[d], fast + nn, max_ids - nn, sm[m].flags)
                                      : cce_gguf_tok_encode(t, corpus + doff[d], fast + nn, max_ids - nn);
                    corpus[doff[d] + dlen[d]] = sv; }
                double dt = now_s() - t0; if (dt < best) best = dt;
            }
            double mbs = bytes_mb / best; if (m == 0) sbase = mbs;
            int ok = (nn == nref) && (memcmp(fast, sref, (size_t)nref * sizeof(int)) == 0);
            printf("  %-20s %7.1f MB/s  %5.2fx  %s\n", sm[m].name, mbs, mbs / sbase, sm[m].persist || sm[m].flags ? (ok ? "ids OK" : "ids MISMATCH") : "");
            if ((sm[m].flags || sm[m].persist) && !ok) fail = 1;
        }
        free(doff); free(dlen); free(sref);
    }

    free(base); free(fast); free(corpus); cce_gguf_tok_free(t);
    printf("\n%s\n", fail ? "GIGATOK_ENCODE_FAIL" : "GIGATOK_ENCODE_PASS");
    return fail ? 1 : 0;
}
