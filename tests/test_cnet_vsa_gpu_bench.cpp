#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <chrono>
#include <vector>

#include "../include/cnet_vsa.h"
#include "../include/cnet_vsa_bsc.h"
#include "../include/cnet_vsa_gpu.h"

static int checks = 0;
static int failures = 0;

static void check(int ok, const char *msg) {
    checks++;
    printf("  %-62s %s\n", msg, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

int main(void) {
    printf("=================================================================\n");
    printf(" CNET-VSA GPU Acceleration Benchmark (AMD ROCm / RDNA 4)\n");
    printf("=================================================================\n\n");

    /* -------------------------------------------------------------
     * GPU Hardware Initialization
     * ------------------------------------------------------------- */
    CnetVsaGpuDeviceInfo dev_info;
    /* Default to device 1 (idle RX 9070 / R9700) */
    int init_rc = cnet_vsa_gpu_init(1, &dev_info);
    if (init_rc != 0) {
        printf("Failed to initialize GPU device 1, trying device 0...\n");
        init_rc = cnet_vsa_gpu_init(0, &dev_info);
    }
    if (init_rc != 0) {
        printf("ERROR: No compatible AMD ROCm GPU found!\n");
        return 1;
    }

    printf("[Hardware Target]\n");
    printf("  Device ID:      %d\n", dev_info.device_id);
    printf("  Device Name:    %s\n", dev_info.device_name);
    printf("  Compute Units:  %d CUs\n", dev_info.compute_units);
    printf("  Clock Rate:     %d MHz\n", dev_info.clock_mhz);
    printf("  Total VRAM:     %.2f GB\n\n", (double)dev_info.total_vram_bytes / (1024.0 * 1024.0 * 1024.0));

    check(init_rc == 0, "AMD ROCm GPU initialized successfully");
    check(dev_info.compute_units >= 16, "Sufficient GPU Compute Units detected");

    uint64_t rng = 0xFEEDC0FFEE123456ULL;

    /* -------------------------------------------------------------
     * Suite 1: Batch 512-bit BSC XOR Binding on GPU
     * ------------------------------------------------------------- */
    printf("\n[1/4] Batch 512-bit BSC Binding (N = 1,000,000 Hypervectors)...\n");
    const size_t BIND_COUNT = 1000000;
    std::vector<CnetVsaBsc> h_a(BIND_COUNT);
    std::vector<CnetVsaBsc> h_b(BIND_COUNT);
    std::vector<CnetVsaBsc> h_out_gpu(BIND_COUNT);

    for (size_t i = 0; i < BIND_COUNT; ++i) {
        cnet_vsa_bsc_random(&h_a[i], &rng);
        cnet_vsa_bsc_random(&h_b[i], &rng);
    }

    double gpu_bind_ms = 0.0;
    int bind_rc = cnet_vsa_gpu_bsc_batch_bind(h_a.data(), h_b.data(), h_out_gpu.data(), BIND_COUNT, &gpu_bind_ms);
    check(bind_rc == 0, "GPU batch binding executed without errors");

    /* Verify bitwise correctness against CPU ground truth */
    int bind_correct = 1;
    for (size_t i = 0; i < 1000; ++i) {
        CnetVsaBsc cpu_expected;
        cnet_vsa_bsc_bind(&cpu_expected, &h_a[i], &h_b[i]);
        if (memcmp(&cpu_expected, &h_out_gpu[i], sizeof(CnetVsaBsc)) != 0) {
            bind_correct = 0;
            break;
        }
    }
    check(bind_correct == 1, "GPU 512-bit binding results bit-identical to CPU ground truth");

    double gpu_bind_ns_per_op = (gpu_bind_ms * 1e6) / (double)BIND_COUNT;
    double gpu_bind_gops = ((double)BIND_COUNT / (gpu_bind_ms * 1e-3)) / 1e9;
    printf("  GPU Kernel Time:  %.3f ms for %zu vectors\n", gpu_bind_ms, BIND_COUNT);
    printf("  GPU Latency:      %.3f ns/op\n", gpu_bind_ns_per_op);
    printf("  GPU Throughput:   %.2f Billion ops/sec\n", gpu_bind_gops);
    check(gpu_bind_ns_per_op < 1.0, "GPU achieves sub-nanosecond per-vector binding throughput");

    /* -------------------------------------------------------------
     * Suite 2: 1,000,000-Hypervector Codebook Search (Hamming Distance)
     * ------------------------------------------------------------- */
    printf("\n[2/4] Massive 1,000,000-Hypervector Nearest-Neighbor Clean-up...\n");
    const size_t SEARCH_COUNT = 1000000;
    std::vector<CnetVsaBsc> h_codebook(SEARCH_COUNT);
    for (size_t i = 0; i < SEARCH_COUNT; ++i) {
        cnet_vsa_bsc_random(&h_codebook[i], &rng);
    }

    /* Select target at index 777,777 and corrupt 15% of bits (77 bit flips) */
    size_t target_idx = 777777;
    CnetVsaBsc query = h_codebook[target_idx];
    for (int k = 0; k < 77; ++k) {
        int word_idx = (int)(rng % 8); rng = rng * 6364136223846793005ULL + 1;
        int bit_idx = (int)(rng % 64);  rng = rng * 6364136223846793005ULL + 1;
        query.w[word_idx] ^= (1ULL << bit_idx);
    }

    size_t best_idx = 0;
    int best_dist = 999999;
    double search_ms = 0.0;
    int search_rc = cnet_vsa_gpu_bsc_codebook_search(&query, h_codebook.data(), SEARCH_COUNT,
                                                     &best_idx, &best_dist, &search_ms);
    check(search_rc == 0, "GPU codebook search executed cleanly");
    check(best_idx == target_idx, "GPU successfully pinpoints exact target index out of 1,000,000 vectors");
    check(best_dist <= 77, "Minimum Hamming distance verified against injected corruption");

    printf("  Codebook Size:    %zu vectors (%.2f MB)\n", SEARCH_COUNT, (double)(SEARCH_COUNT * 64) / (1024.0 * 1024.0));
    printf("  Target Found:     Index %zu (Distance: %d bits)\n", best_idx, best_dist);
    printf("  GPU Search Time:  %.3f ms (%.3f ns/vector searched)\n", search_ms, (search_ms * 1e6) / (double)SEARCH_COUNT);
    check(search_ms < 1.0, "GPU searches 1,000,000 512-bit vectors in sub-1 millisecond (< 1 ms)");

    /* -------------------------------------------------------------
     * Suite 3: Continuous 512-Dim Float Dot Product (100,000 Vectors)
     * ------------------------------------------------------------- */
    printf("\n[3/4] Continuous 512-Dim Float Matrix-Vector Search (100,000 Vectors)...\n");
    const size_t FLOAT_COUNT = 100000;
    std::vector<float> h_matrix(FLOAT_COUNT * 512);
    std::vector<float> h_qvec(512);
    for (int j = 0; j < 512; ++j) {
        h_qvec[j] = (float)((rng % 2000) - 1000) / 1000.0f;
        rng = rng * 6364136223846793005ULL + 1;
    }
    cnet_vsa_normalize(h_qvec.data(), 512);

    for (size_t i = 0; i < FLOAT_COUNT; ++i) {
        for (int j = 0; j < 512; ++j) {
            h_matrix[i * 512 + j] = (float)((rng % 2000) - 1000) / 1000.0f;
            rng = rng * 6364136223846793005ULL + 1;
        }
        cnet_vsa_normalize(&h_matrix[i * 512], 512);
    }

    /* Plant near-identical match at index 42,000 with 90% correlation */
    size_t float_target = 42000;
    float noise_v[512];
    uint64_t target_rng = 0x1122334455667788ULL;
    cnet_vsa_random(noise_v, 512, &target_rng);
    for (int j = 0; j < 512; ++j) {
        h_matrix[float_target * 512 + j] = 0.90f * h_qvec[j] + 0.10f * noise_v[j];
    }
    cnet_vsa_normalize(&h_matrix[float_target * 512], 512);

    size_t best_float_idx = 0;
    float best_float_score = -2.0f;
    double float_ms = 0.0;
    int fdot_rc = cnet_vsa_gpu_float_batch_dot(h_qvec.data(), h_matrix.data(), FLOAT_COUNT,
                                               NULL, &best_float_idx, &best_float_score, &float_ms);
    check(fdot_rc == 0, "GPU float batch dot product executed cleanly");
    check(best_float_idx == float_target, "GPU recovers planted continuous target in 100,000 vectors");
    printf("  Continuous Matrix: %zu x 512 (%.2f MB)\n", FLOAT_COUNT, (double)(FLOAT_COUNT * 512 * 4) / (1024.0 * 1024.0));
    printf("  Top Match:         Index %zu (Cosine Similarity: %.4f)\n", best_float_idx, best_float_score);
    printf("  GPU Compute Time:  %.3f ms (%.3f ns/vector)\n", float_ms, (float_ms * 1e6) / (double)FLOAT_COUNT);
    check(float_ms < 2.0, "GPU sweeps 100,000 continuous 512-dim vectors in sub-2 milliseconds");

    /* -------------------------------------------------------------
     * Suite 4: Parallel Batch Resonator Factorization on GPU
     * ------------------------------------------------------------- */
    printf("\n[4/4] Parallel Batch Resonator Factorization (1,000 Triples Concurrently)...\n");
    const size_t RESONATOR_BATCH = 1000;
    int recovered_count = 0;
    float avg_iters = 0.0f;
    double resonator_ms = 0.0;

    int res_rc = cnet_vsa_gpu_batch_resonator(RESONATOR_BATCH, &recovered_count, &avg_iters, &resonator_ms);
    check(res_rc == 0, "GPU batch resonator executed cleanly");
    double acc = (double)recovered_count / (double)RESONATOR_BATCH * 100.0;
    printf("  Batch Size:       %zu composite triples\n", RESONATOR_BATCH);
    printf("  Accurate Solves:  %d / %zu (%.1f%%)\n", recovered_count, RESONATOR_BATCH, acc);
    printf("  Avg Iterations:   %.2f iters\n", avg_iters);
    printf("  GPU Batch Time:   %.3f ms (%.2f us per complete triple factorized)\n",
           resonator_ms, (resonator_ms * 1e3) / (double)RESONATOR_BATCH);
    check(acc >= 95.0, "GPU achieves >= 95% factorization accuracy on composite triples");
    check(resonator_ms < 10.0, "GPU factorizes 1,000 composite triples in sub-10 milliseconds");

    printf("\n-----------------------------------------------------------------\n");
    if (failures == 0) {
        printf("CNET_VSA_GPU_BENCH_PASS: all %d checks passed.\n", checks);
        return 0;
    } else {
        printf("CNET_VSA_GPU_BENCH_FAIL: %d of %d checks failed.\n", failures, checks);
        return 1;
    }
}
