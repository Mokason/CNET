#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <immintrin.h>

#include "../include/cnet_vsa.h"
#include "../include/cnet_vsa_bsc.h"

static int checks = 0;
static int failures = 0;

static void check(int ok, const char *msg) {
    checks++;
    printf("  %-62s %s\n", msg, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static uint64_t bench_mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* AVX2 SIMD dot product over 512 floats */
static float simd_dot_product_avx2(const float *a, const float *b, int dim) {
    __m256 sum = _mm256_setzero_ps();
    for (int i = 0; i < dim; i += 8) {
        __m256 va = _mm256_loadu_ps(a + i);
        __m256 vb = _mm256_loadu_ps(b + i);
        sum = _mm256_fmadd_ps(va, vb, sum);
    }
    float buffer[8];
    _mm256_storeu_ps(buffer, sum);
    return buffer[0] + buffer[1] + buffer[2] + buffer[3] +
           buffer[4] + buffer[5] + buffer[6] + buffer[7];
}

int main(void) {
    printf("=================================================================\n");
    printf(" CNET-VSA Priority 1: Hardware SIMD & 512-Bit BSC Stress Test\n");
    printf("=================================================================\n\n");

    uint64_t rng = 987654321ULL;
    const int D = CNET_VSA_DEFAULT_DIM;

    /* -------------------------------------------------------------
     * Suite 1: 512-Bit Packed Binary VSA (BSC) Correctness
     * ------------------------------------------------------------- */
    printf("[1/4] Testing 512-bit BSC Math & Exact Reversibility...\n");
    CnetVsaBsc bsc_a, bsc_b, bsc_bound, bsc_rec, bsc_perm;
    cnet_vsa_bsc_random(&bsc_a, &rng);
    cnet_vsa_bsc_random(&bsc_b, &rng);

    /* Quasi-orthogonality in 512-bit Hamming space (expected distance ~ 256) */
    int h_ab = cnet_vsa_bsc_hamming(&bsc_a, &bsc_b);
    check(abs(h_ab - 256) < 40, "Random 512-bit BSC vectors are quasi-orthogonal (Hamming ~ 256 +/- 40)");

    /* Exact bit-level unbinding (a ^ b ^ b == a) */
    cnet_vsa_bsc_bind(&bsc_bound, &bsc_a, &bsc_b);
    cnet_vsa_bsc_unbind(&bsc_rec, &bsc_bound, &bsc_b);
    int h_rec = cnet_vsa_bsc_hamming(&bsc_rec, &bsc_a);
    check(h_rec == 0, "BSC unbinding is 100% bit-exact (Hamming distance == 0)");

    /* Permutation */
    cnet_vsa_bsc_permute(&bsc_perm, &bsc_a, 17);
    int h_perm = cnet_vsa_bsc_hamming(&bsc_perm, &bsc_a);
    check(abs(h_perm - 256) < 40, "Permuted vector is quasi-orthogonal to original");

    CnetVsaBsc bsc_restored;
    cnet_vsa_bsc_permute(&bsc_restored, &bsc_perm, -17);
    int h_restored = cnet_vsa_bsc_hamming(&bsc_restored, &bsc_a);
    check(h_restored == 0, "Inverse permutation restores exact 512-bit pattern");

    /* -------------------------------------------------------------
     * Suite 2: Noise Robustness under Massive Bit Corruption
     * ------------------------------------------------------------- */
    printf("\n[2/4] Stress-Testing Codebook Clean-up under Heavy Bit Flips...\n");
    CnetVsaBscCodebook codebook;
    cnet_vsa_bsc_codebook_init(&codebook, 512);

    CnetVsaBsc targets[10];
    char names[10][CNET_VSA_NAME_MAX];
    for (int i = 0; i < 10; ++i) {
        snprintf(names[i], sizeof(names[i]), "kernel_skill_%d", i);
        cnet_vsa_bsc_random(&targets[i], &rng);
        cnet_vsa_bsc_codebook_add(&codebook, names[i], &targets[i]);
    }

    /* Corrupt 100 bits out of 512 (~20% random bit flips) */
    int recovered_count = 0;
    const int TRIALS = 100;
    for (int t = 0; t < TRIALS; ++t) {
        int target_idx = t % 10;
        CnetVsaBsc corrupted = targets[target_idx];

        /* Flip 100 random bits */
        for (int b = 0; b < 100; ++b) {
            int word = (int)(rand() % 8);
            int bit = (int)(rand() % 64);
            corrupted.w[word] ^= (1ULL << bit);
        }

        char match_name[CNET_VSA_NAME_MAX] = {0};
        CnetVsaBsc clean;
        int dist = 0;
        cnet_vsa_bsc_codebook_cleanup(&codebook, &corrupted, &clean, match_name, sizeof(match_name), &dist);

        if (strcmp(match_name, names[target_idx]) == 0 && dist < 150) {
            recovered_count++;
        }
    }
    check(recovered_count == TRIALS, "BSC clean-up recovers exact target under 20% random bit-flips (100/100, 100%)");

    /* -------------------------------------------------------------
     * Suite 3: 1,000,000 Operations Throughput & Latency Stress Test
     * ------------------------------------------------------------- */
    printf("\n[3/4] 1,000,000 Operations Throughput & Latency Stress Test...\n");
    const int MILLION = 1000000;

    /* 1. BSC Bitwise Binding Throughput (XOR) */
    uint64_t t0 = bench_mono_ns();
    for (int i = 0; i < MILLION; ++i) {
        cnet_vsa_bsc_bind(&bsc_bound, &bsc_a, &bsc_b);
    }
    uint64_t t1 = bench_mono_ns();
    double bsc_bind_ns = (double)(t1 - t0) / (double)MILLION;
    printf("  [BSC 512-bit Bind]    1,000,000 ops in %.2f ms (%.2f ns/op, %.1f M ops/sec)\n",
           (double)(t1 - t0) / 1000000.0, bsc_bind_ns, 1000.0 / bsc_bind_ns);
    check(bsc_bind_ns < 10.0, "BSC 512-bit binding executes in sub-10 nanoseconds (< 10 ns/op)");

    /* 2. BSC Hardware Popcount Hamming Distance */
    volatile int dummy_h = 0;
    t0 = bench_mono_ns();
    for (int i = 0; i < MILLION; ++i) {
        dummy_h += cnet_vsa_bsc_hamming(&bsc_a, &bsc_b);
    }
    t1 = bench_mono_ns();
    double bsc_dist_ns = (double)(t1 - t0) / (double)MILLION;
    printf("  [BSC Hamming Dist]   1,000,000 ops in %.2f ms (%.2f ns/op, %.1f M ops/sec)\n",
           (double)(t1 - t0) / 1000000.0, bsc_dist_ns, 1000.0 / bsc_dist_ns);
    check(bsc_dist_ns < 10.0, "BSC Hamming distance executes in sub-10 nanoseconds (< 10 ns/op)");

    /* 3. Continuous AVX2 SIMD Dot Product (512 floats) */
    float fa[CNET_VSA_DEFAULT_DIM], fb[CNET_VSA_DEFAULT_DIM];
    cnet_vsa_random(fa, D, &rng);
    cnet_vsa_random(fb, D, &rng);

    volatile float dummy_dot = 0.0f;
    t0 = bench_mono_ns();
    for (int i = 0; i < MILLION; ++i) {
        dummy_dot += simd_dot_product_avx2(fa, fb, D);
    }
    t1 = bench_mono_ns();
    double simd_dot_ns = (double)(t1 - t0) / (double)MILLION;
    printf("  [AVX2 Dot Product]   1,000,000 ops in %.2f ms (%.2f ns/op, %.1f M ops/sec)\n",
           (double)(t1 - t0) / 1000000.0, simd_dot_ns, 1000.0 / simd_dot_ns);
    check(simd_dot_ns < 50.0, "AVX2 512-dim continuous dot product runs in sub-50 nanoseconds");

    /* -------------------------------------------------------------
     * Suite 4: Memory Footprint & Cache Alignment
     * ------------------------------------------------------------- */
    printf("\n[4/4] Verifying Memory Footprint & Cache Line Alignment...\n");
    size_t bsc_size = sizeof(CnetVsaBsc);
    size_t cont_size = D * sizeof(float);
    printf("  Continuous float vector size: %zu bytes\n", cont_size);
    printf("  Packed BSC hypervector size:  %zu bytes (exactly %zu%% of float vector)\n",
           bsc_size, (bsc_size * 100) / cont_size);
    check(bsc_size == 64, "BSC vector is exactly 64 bytes (1 CPU L1 cache line)");
    check(((uintptr_t)&bsc_a % 64) == 0, "BSC vector is 64-byte aligned for AVX-512 register loads");

    cnet_vsa_bsc_codebook_free(&codebook);

    printf("\n-----------------------------------------------------------------\n");
    if (failures == 0) {
        printf("CNET_VSA_SIMD_BENCH_PASS: all %d checks passed.\n", checks);
        return 0;
    } else {
        printf("CNET_VSA_SIMD_BENCH_FAIL: %d of %d checks failed.\n", failures, checks);
        return 1;
    }
}
