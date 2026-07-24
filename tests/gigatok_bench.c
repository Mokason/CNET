/* gigatok_bench — independent confirmation of gigatoken's core throughput claim.
 *
 * Reproduces the pretokenizer half of gigatoken (the lever its optimization log
 * says dominates: regex-replacement via SWAR) in C, and measures it on THIS
 * machine. Reports single-thread MiB/s for scalar vs SWAR vs SWAR+dual-cursor,
 * and a multithreaded aggregate GB/s — the numbers to compare against the log's
 * ~380 MiB/s scalar / ~1 GiB/s SWAR single-thread and its GB/s fan-out.
 *
 * NOT a full port: no BPE merge, no cache, no Parquet/parallel-IO, no per-family
 * SIMD. It confirms the pretokenizer lever only; the end-to-end "1000x vs HF"
 * needs the whole stack and rests on an asymmetric baseline (see analysis).
 *
 * Usage: gigatok_bench [corpus_file] [size_mb]
 *   no file  -> generate `size_mb` (default 128) of pseudo-OpenWebText.
 *
 * Gate: prints GIGATOK_BENCH_PASS iff scalar and SWAR agree byte-for-byte on
 * pretoken boundaries (and counts match across all variants). */

#include "../include/cce/cce_pretok_swar.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _OPENMP
#include <omp.h>
#endif

static uint32_t S = 0x9E3779B9u;
static uint32_t rr(void) { S = S * 1664525u + 1013904223u; return S; }
static uint32_t rn(uint32_t m) { return rr() % m; }

static const char *WORDS[] = {
    "the", "of", "and", "to", "in", "a", "is", "that", "for", "it", "as", "was",
    "with", "be", "by", "on", "not", "he", "this", "are", "or", "his", "from",
    "at", "which", "but", "have", "an", "they", "you", "one", "had", "word",
    "model", "system", "language", "network", "training", "tokenizer", "throughput",
    "performance", "benchmark", "implementation", "optimization", "computer",
    "research", "internet", "document", "processing", "algorithm", "memory"
};
enum { NWORDS = (int)(sizeof(WORDS) / sizeof(WORDS[0])) };

/* Generate ~bytes of pseudo-OpenWebText: words, spaces, punctuation, numbers,
 * newlines, and a sprinkling of 2-byte UTF-8, to give a realistic run-length mix. */
static size_t gen_corpus(uint8_t *buf, size_t cap) {
    size_t p = 0;
    int sincebreak = 0;
    while (p + 32 < cap) {
        if (rn(30) == 0) { buf[p++] = '\n'; if (rn(3) == 0) buf[p++] = '\n'; sincebreak = 0; continue; }
        if (p > 0 && rn(1000) < 990) buf[p++] = ' ';
        if (rn(30) == 0) { /* a number */
            int d = 1 + (int)rn(5), k;
            for (k = 0; k < d && p < cap; k++) buf[p++] = (uint8_t)('0' + rn(10));
        } else {
            const char *w = WORDS[rn(NWORDS)];
            size_t l = strlen(w), k;
            for (k = 0; k < l && p < cap; k++) buf[p++] = (uint8_t)w[k];
            if (rn(60) == 0 && p + 2 < cap) { buf[p++] = 0xC3; buf[p++] = 0xA9; } /* 'é' */
        }
        if (rn(12) == 0 && p < cap) buf[p++] = (uint8_t)(",.;:!?"[rn(6)]);
        if (++sincebreak > 25 && p < cap) { buf[p++] = '.'; sincebreak = 0; }
    }
    return p;
}

static double now_s(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

typedef size_t (*count_fn)(const uint8_t *, size_t);
static double best_mibs(count_fn f, const uint8_t *s, size_t n, int iters, size_t *out_count) {
    double best = 1e300; size_t c = 0; int it;
    for (it = 0; it < iters; it++) {
        double t0 = now_s(); c = f(s, n); double dt = now_s() - t0;
        if (dt < best) best = dt;
    }
    if (out_count) *out_count = c;
    return ((double)n / (1024.0 * 1024.0)) / best;
}

int main(int argc, char **argv) {
    size_t n = 0; uint8_t *buf = NULL;
    int fail = 0;

    if (argc > 1 && argv[1][0] && strcmp(argv[1], "-") != 0) {
        FILE *f = fopen(argv[1], "rb");
        if (!f) { fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }
        fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
        if (sz <= 0) { fprintf(stderr, "empty file\n"); fclose(f); return 1; }
        buf = (uint8_t *)malloc((size_t)sz + 8);
        n = fread(buf, 1, (size_t)sz, f); fclose(f);
        printf("corpus: %s (%.1f MB)\n", argv[1], (double)n / 1e6);
    } else {
        size_t mb = (argc > 2) ? (size_t)strtoul(argv[2], NULL, 10)
                   : (argc > 1) ? (size_t)strtoul(argv[1], NULL, 10) : 128;
        if (mb == 0) mb = 128;
        size_t cap = mb * 1024 * 1024;
        buf = (uint8_t *)malloc(cap + 8);
        if (!buf) { fprintf(stderr, "oom\n"); return 1; }
        n = gen_corpus(buf, cap);
        printf("corpus: generated pseudo-OpenWebText (%.1f MB)\n", (double)n / 1e6);
    }
    memset(buf + n, 0, 8); /* pad for safe tail reads */

    /* ---- correctness gate: scalar vs SWAR boundaries, exact ---- */
    size_t vlim = n < 32u * 1024 * 1024 ? n : 32u * 1024 * 1024;
    size_t cap_b = vlim; /* at most one boundary per byte */
    uint32_t *ba = (uint32_t *)malloc(cap_b * sizeof(uint32_t));
    uint32_t *bb = (uint32_t *)malloc(cap_b * sizeof(uint32_t));
    size_t na = cce_pretok_scalar(buf, vlim, ba, cap_b);
    size_t nb = cce_pretok_swar(buf, vlim, bb, cap_b);
    int parity = (na == nb) && (memcmp(ba, bb, na * sizeof(uint32_t)) == 0);
    printf("parity (first %.0f MB): scalar=%zu swar=%zu boundaries -> %s\n",
           (double)vlim / 1e6, na, nb, parity ? "IDENTICAL" : "MISMATCH");
    if (!parity) {
        size_t k; for (k = 0; k < na && k < nb; k++) if (ba[k] != bb[k]) {
            size_t o = (ba[k] < bb[k] ? ba[k] : bb[k]); size_t z = o > 6 ? o - 6 : 0, j;
            fprintf(stderr, "  first divergence at pretoken %zu: scalar off=%u swar off=%u\n", k, ba[k], bb[k]);
            fprintf(stderr, "  bytes [%zu..%zu): ", z, o + 18);
            for (j = z; j < o + 18 && j < vlim; j++) fprintf(stderr, "%02x%s", buf[j], j + 1 == o ? "|" : " ");
            fprintf(stderr, "\n  ascii: \"%.24s\"\n", (const char *)(buf + z));
            break; }
        fail = 1;
    }
    free(ba); free(bb);

    /* ---- single-thread throughput ---- */
    int iters = n > 200u * 1024 * 1024 ? 3 : 5;
    size_t c_cp, c_sc, c_sw, c_du;
    (void)best_mibs(cce_pretok_count_swar, buf, n, 1, NULL); /* warm caches */
    double cp = best_mibs(cce_pretok_count_codepoint, buf, n, iters, &c_cp);
    double sc = best_mibs(cce_pretok_count_scalar, buf, n, iters, &c_sc);
    double sw = best_mibs(cce_pretok_count_swar, buf, n, iters, &c_sw);
    double du = best_mibs(cce_pretok_count_swar_dual, buf, n, iters, &c_du);
    int counts_ok = (c_cp == c_sc) && (c_sc == c_sw) && (c_sw == c_du);

    printf("\nsingle-thread pretokenization (best of %d):\n", iters);
    printf("  CNET-style (cp decode) : %8.1f MiB/s   (%zu pretokens)\n", cp, c_cp);
    printf("  scalar (byte class)    : %8.1f MiB/s   %.2fx vs CNET-style\n", sc, sc / cp);
    printf("  SWAR                   : %8.1f MiB/s   %.2fx vs CNET-style\n", sw, sw / cp);
    printf("  SWAR + dual-cursor     : %8.1f MiB/s   %.2fx vs CNET-style, %.2fx vs SWAR\n", du, du / cp, du / sw);
    printf("  (log reference: ~380 MiB/s scalar / ~1049 MiB/s SWAR+dual on Apple M-series)\n");
    if (!counts_ok) { fprintf(stderr, "count mismatch cp=%zu sc=%zu sw=%zu du=%zu\n", c_cp, c_sc, c_sw, c_du); fail = 1; }

#ifdef _OPENMP
    /* ---- multithreaded aggregate GB/s ---- */
    int T = omp_get_max_threads();
    size_t *starts = (size_t *)malloc(((size_t)T + 1) * sizeof(size_t));
    int t;
    for (t = 0; t < T; t++) starts[t] = cce_pretok_safe_split(buf, n, (size_t)t * n / (size_t)T);
    starts[T] = n;
    double best = 1e300; int rep;
    size_t total = 0;
    for (rep = 0; rep < 3; rep++) {
        double t0 = now_s(); size_t acc = 0;
        #pragma omp parallel for reduction(+:acc) schedule(static)
        for (t = 0; t < T; t++) acc += cce_pretok_count_swar_dual(buf + starts[t], starts[t + 1] - starts[t]);
        double dt = now_s() - t0; if (dt < best) { best = dt; total = acc; }
    }
    double gbs = ((double)n / 1e9) / best;
    printf("\nmultithreaded (%d threads, SWAR+dual):\n", T);
    printf("  aggregate       : %8.2f GB/s   (%.2f GiB/s), %zu pretokens\n",
           gbs, ((double)n / (1024.0 * 1024.0 * 1024.0)) / best, total);
    printf("  vs single-SWAR  : %.1fx\n", (gbs * 1e9 / (1024.0 * 1024.0)) / sw);
    free(starts);
#else
    printf("\n(built without OpenMP; skipping multithreaded aggregate)\n");
#endif

    free(buf);
    printf("\n%s\n", fail ? "GIGATOK_BENCH_FAIL" : "GIGATOK_BENCH_PASS");
    return fail ? 1 : 0;
}
