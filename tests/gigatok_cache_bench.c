/* gigatok_cache_bench — isolate the pretoken-cache MEMORY LAYOUT at scale.
 *
 * The cache-line-packed + huge-page layout only pays when the table is large
 * enough to miss L3 and blow the dTLB (gigatoken's regime: ~1.3M unique
 * pretokens). A 50-word synthetic corpus keeps the table in L2, so the encode
 * bench can't show it. This builds a big table and times random hits for three
 * layouts, isolating exactly what the layout buys:
 *
 *   packed + hugepage : 64-byte entry, key+ids inline (one cache line / hit),
 *                       2 MiB-aligned + MADV_HUGEPAGE table
 *   packed + 4K pages  : same entry, plain 4 KiB-page table (isolates the dTLB
 *                       / huge-page effect)
 *   pointer-chase      : entry holds pointers to separately-malloc'd key + ids
 *                       (the OLD design) — 3 dependent loads / hit
 *
 * Usage: gigatok_cache_bench [n_entries]   (default 4,000,000)
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(__linux__)
#include <sys/mman.h>
#endif

static double now_s(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9; }
static uint64_t fnv(const uint8_t *s, int n) { uint64_t h = 1469598103934665603ULL; int k; for (k = 0; k < n; k++) { h ^= s[k]; h *= 1099511628211ULL; } return h ? h : 1; }
static uint64_t fasthash(const uint8_t *s, int n) {
    const uint64_t Mm = 0xC2B2AE3D27D4EB4FULL;
    uint64_t h = 0x9E3779B97F4A7C15ULL ^ ((uint64_t)(unsigned)n * Mm); int k = 0; uint64_t w;
    for (; k + 8 <= n; k += 8) { memcpy(&w, s + k, 8); h = (h ^ w) * Mm; h ^= h >> 29; }
    if (k < n) { w = 0; memcpy(&w, s + k, (size_t)(n - k)); h = (h ^ w) * Mm; h ^= h >> 29; }
    return h ? h : 1;
}
/* deterministic unique 8-byte key + 2 ids for index i (golden-ratio bijection) */
static int makekey(uint64_t i, uint8_t *key) { uint64_t x = (i + 1) * 0x9E3779B97F4A7C15ULL; memcpy(key, &x, 8); return 8; }

enum { KM = 22, IM = 8 };
typedef struct { uint64_t h; uint8_t blen, nids; uint8_t key[KM]; int32_t ids[IM]; } PE; /* 64B */
typedef struct { uint64_t h; uint8_t *key; int kl; int32_t *ids; int nids; } CE;

static PE *palloc(size_t cap, int huge) {
    size_t b = cap * sizeof(PE); PE *e = NULL;
#if defined(__linux__) && defined(MADV_HUGEPAGE)
    if (huge) { if (posix_memalign((void **)&e, 2u * 1024 * 1024, b) != 0) e = NULL;
                if (e) { madvise(e, b, MADV_HUGEPAGE); memset(e, 0, b); return e; } }
#endif
    (void)huge; return (PE *)calloc(cap, sizeof(PE));
}
static void pinsert(PE *t, size_t cap, const uint8_t *k, int kl, const int32_t *ids, int n) {
    uint64_t h = fnv(k, kl); size_t i = h & (cap - 1); int j;
    while (t[i].h) i = (i + 1) & (cap - 1);
    t[i].h = h; t[i].blen = (uint8_t)kl; t[i].nids = (uint8_t)n; memcpy(t[i].key, k, (size_t)kl);
    for (j = 0; j < n; j++) t[i].ids[j] = ids[j];
}
static int plookup(PE *t, size_t cap, const uint8_t *k, int kl, int32_t *out) {
    uint64_t h = fnv(k, kl); size_t i = h & (cap - 1); int j;
    for (;;) { PE *e = &t[i]; if (!e->h) return 0;
        if (e->h == h && e->blen == kl && memcmp(e->key, k, (size_t)kl) == 0) { for (j = 0; j < e->nids; j++) out[j] = e->ids[j]; return e->nids; }
        i = (i + 1) & (cap - 1); }
}
static void cinsert(CE *t, size_t cap, const uint8_t *k, int kl, const int32_t *ids, int n) {
    uint64_t h = fnv(k, kl); size_t i = h & (cap - 1);
    while (t[i].h) i = (i + 1) & (cap - 1);
    t[i].h = h; t[i].key = (uint8_t *)malloc((size_t)kl); memcpy(t[i].key, k, (size_t)kl); t[i].kl = kl;
    t[i].ids = (int32_t *)malloc((size_t)n * 4); memcpy(t[i].ids, ids, (size_t)n * 4); t[i].nids = n;
}
static int clookup(CE *t, size_t cap, const uint8_t *k, int kl, int32_t *out) {
    uint64_t h = fnv(k, kl); size_t i = h & (cap - 1); int j;
    for (;;) { CE *e = &t[i]; if (!e->h) return 0;
        if (e->h == h && e->kl == kl && memcmp(e->key, k, (size_t)kl) == 0) { for (j = 0; j < e->nids; j++) out[j] = e->ids[j]; return e->nids; }
        i = (i + 1) & (cap - 1); }
}

/* ---- linear vs Robin Hood probing (u64-keyed, like the merge table) ---- */
static uint64_t sm64(uint64_t x) { x += 0x9E3779B97F4A7C15ULL; x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL; x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL; return x ^ (x >> 31); }
typedef struct { uint64_t k; uint32_t v; } TE;
static uint32_t thome(uint64_t k, uint32_t mask) { return (uint32_t)((k * 0x9E3779B97F4A7C15ULL >> 32) & mask); }
static void lin_put(TE *t, uint32_t mask, uint64_t k, uint32_t v) { uint32_t i = thome(k, mask); while (t[i].k) i = (i + 1) & mask; t[i].k = k; t[i].v = v; }
static int lin_get(TE *t, uint32_t mask, uint64_t k) { uint32_t i = thome(k, mask); for (;;) { if (!t[i].k) return -1; if (t[i].k == k) return (int)t[i].v; i = (i + 1) & mask; } }
static void rh_put(TE *t, uint32_t mask, uint64_t k, uint32_t v) {
    TE c; c.k = k; c.v = v; uint32_t i = thome(k, mask), d = 0;
    for (;;) { if (!t[i].k) { t[i] = c; return; } uint32_t ed = (i - thome(t[i].k, mask)) & mask; if (ed < d) { TE tmp = t[i]; t[i] = c; c = tmp; d = ed; } i = (i + 1) & mask; d++; }
}
static int rh_get(TE *t, uint32_t mask, uint64_t k) {
    uint32_t i = thome(k, mask), d = 0;
    for (;;) { if (!t[i].k) return -1; if (t[i].k == k) return (int)t[i].v; if (((i - thome(t[i].k, mask)) & mask) < d) return -1; i = (i + 1) & mask; d++; }
}

int main(int argc, char **argv) {
    size_t N = argc > 1 ? strtoull(argv[1], NULL, 10) : 4000000, M = 20000000, cap = 1, i;
    while (cap < N * 4 / 3) cap <<= 1;
    printf("entries=%zu  table_cap=%zu  (packed %.0f MB, chase %.0f MB + %zu key/id mallocs)  lookups=%zu\n",
           N, cap, (double)cap * sizeof(PE) / 1e6, (double)cap * sizeof(CE) / 1e6, N, M);

    PE *ph = palloc(cap, 1), *p4 = palloc(cap, 0);
    CE *ch = (CE *)calloc(cap, sizeof(CE));
    if (!ph || !p4 || !ch) { fprintf(stderr, "alloc failed (try fewer entries)\n"); return 1; }
    for (i = 0; i < N; i++) { uint8_t k[22]; int kl = makekey(i, k); int32_t ids[2]; ids[0] = (int)(i & 0x7fffffff); ids[1] = (int)((i >> 7) & 0x7fffffff); pinsert(ph, cap, k, kl, ids, 2); pinsert(p4, cap, k, kl, ids, 2); cinsert(ch, cap, k, kl, ids, 2); }

    volatile int64_t sink = 0;
    double base = 0;
    printf("random-hit latency (best of 3):\n");
#define BENCH(NAME, LOOK, TAB) do { \
        double best = 1e300; int r; \
        for (r = 0; r < 3; r++) { uint64_t rr = 0x1234567; double t0 = now_s(); int64_t s = 0; size_t m; int32_t out[8]; \
            for (m = 0; m < M; m++) { rr ^= rr << 13; rr ^= rr >> 7; rr ^= rr << 17; uint64_t ix = rr % N; uint8_t k[22]; int kl = makekey(ix, k); int nn = LOOK(TAB, cap, k, kl, out); s += nn ? out[0] : -1; } \
            double dt = now_s() - t0; if (dt < best) best = dt; sink += s; } \
        double ns = best / (double)M * 1e9; if (base == 0) base = ns; \
        printf("  %-20s %6.1f ns/lookup   %5.1f M/s   %.2fx\n", NAME, ns, (double)M / best / 1e6, base / ns); \
    } while (0)
    BENCH("pointer-chase (old)", clookup, ch);
    BENCH("packed, 4K pages", plookup, p4);
    BENCH("packed + hugepage", plookup, ph);
    printf("(sink=%lld)\n", (long long)sink);

    /* ---- short-key hash compute: FNV (per-byte) vs fast (8B multiply-mix) ---- */
    {
        enum { NK = 4096 };                       /* L1-resident so we time compute, not memory */
        uint8_t keys[NK][14]; int klen[NK]; size_t j, reps = M / NK, r; uint64_t acc;
        for (j = 0; j < NK; j++) { uint64_t x = (j + 1) * 0x9E3779B97F4A7C15ULL; klen[j] = 4 + (int)(x % 11); memcpy(keys[j], &x, 8); memcpy(keys[j] + 8, &x, 6); }
        printf("\nshort-key hash compute (4-14 B keys, %zu hashes, best of 3):\n", reps * NK);
        double bf = 1e300, bx = 1e300;
        for (r = 0; r < 3; r++) { double t0 = now_s(); acc = 0; size_t rp; for (rp = 0; rp < reps; rp++) for (j = 0; j < NK; j++) acc += fnv(keys[j], klen[j]); double dt = now_s() - t0; if (dt < bf) bf = dt; sink += (int64_t)acc; }
        for (r = 0; r < 3; r++) { double t0 = now_s(); acc = 0; size_t rp; for (rp = 0; rp < reps; rp++) for (j = 0; j < NK; j++) acc += fasthash(keys[j], klen[j]); double dt = now_s() - t0; if (dt < bx) bx = dt; sink += (int64_t)acc; }
        printf("  FNV-1a (per byte)   %5.2f ns/hash   1.00x\n", bf / (double)(reps * NK) * 1e9);
        printf("  fast (8B mul-mix)   %5.2f ns/hash   %.2fx\n", bx / (double)(reps * NK) * 1e9, bf / bx);
    }

    /* ---- linear vs Robin Hood probing at two load factors ---- */
    {
        size_t Mp = 10000000, j; int li; double loads[3] = { 0.5, 0.9, 0.95 };
        size_t pc = 1u << 22; uint32_t mask = (uint32_t)(pc - 1); /* fixed cap; load set by N */
        printf("\nprobing: linear vs robin-hood (cap=%zu, %zu random hits):\n", pc, Mp);
        for (li = 0; li < 3; li++) {
            double L = loads[li]; size_t Np = (size_t)((double)pc * L);
            TE *tl = (TE *)calloc(pc, sizeof(TE)), *tr = (TE *)calloc(pc, sizeof(TE));
            if (!tl || !tr) { free(tl); free(tr); continue; }
            for (j = 0; j < Np; j++) { uint64_t k = sm64(j + 1); lin_put(tl, mask, k, (uint32_t)j); rh_put(tr, mask, k, (uint32_t)j); }
            double sl = 0, sr = 0; int ml = 0, mr = 0; size_t nn = 0;
            for (j = 0; j < pc; j++) {
                if (tl[j].k) { int d = (int)(((uint32_t)j - thome(tl[j].k, mask)) & mask); sl += d; if (d > ml) ml = d; nn++; }
                if (tr[j].k) { int d = (int)(((uint32_t)j - thome(tr[j].k, mask)) & mask); sr += d; if (d > mr) mr = d; }
            }
            double bl = 1e300, br = 1e300; int r; size_t m; uint64_t rr;
            for (r = 0; r < 3; r++) { rr = 0x9; double t0 = now_s(); int64_t s = 0; for (m = 0; m < Mp; m++) { rr ^= rr << 13; rr ^= rr >> 7; rr ^= rr << 17; s += lin_get(tl, mask, sm64((rr % Np) + 1)); } double dt = now_s() - t0; if (dt < bl) bl = dt; sink += s; }
            for (r = 0; r < 3; r++) { rr = 0x9; double t0 = now_s(); int64_t s = 0; for (m = 0; m < Mp; m++) { rr ^= rr << 13; rr ^= rr >> 7; rr ^= rr << 17; s += rh_get(tr, mask, sm64((rr % Np) + 1)); } double dt = now_s() - t0; if (dt < br) br = dt; sink += s; }
            printf("  load %2.0f%%:  linear avg=%.2f max=%2d %5.1f ns/hit   |   robin-hood avg=%.2f max=%2d %5.1f ns/hit\n",
                   L * 100, sl / (double)nn, ml, bl / (double)Mp * 1e9, sr / (double)nn, mr, br / (double)Mp * 1e9);
            free(tl); free(tr);
        }
    }
    return 0;
}
