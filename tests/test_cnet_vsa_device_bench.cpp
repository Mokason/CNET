#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <vector>

#include "../include/cnet_vsa.h"
#include "../include/cnet_vsa_bsc.h"
#include "../include/cnet_vsa_gpu.h"
#include "../include/cnet_vsa_device.h"

static int checks = 0;
static int failures = 0;

static void check(int ok, const char *msg) {
    checks++;
    printf("  %-62s %s\n", msg, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

int main(void) {
    printf("=================================================================\n");
    printf(" CNET-VSA Device Configuration & Unified Dispatch Benchmark\n");
    printf("=================================================================\n\n");

    /* Ensure clean environment */
    unsetenv("CNET_VSA_DEVICE");
    unsetenv("CNET_VSA_GPU_ID");

    /* -------------------------------------------------------------
     * Test 1: Default Device Selection (GPU by default)
     * ------------------------------------------------------------- */
    printf("[1/4] Verifying Default Hardware Selection...\n");
    cnet_vsa_device_init();
    const CnetVsaDeviceConfig *cfg = cnet_vsa_device_get_config();

    printf("  Default Backend:  %s\n", cfg->backend == CNET_VSA_BACKEND_GPU ? "GPU (AMD ROCm)" : "CPU");
    printf("  Device Name:      %s\n", cfg->device_name);
    printf("  Compute Units:    %d\n", cfg->compute_units);
    printf("  Clock Rate:       %d MHz\n", cfg->clock_mhz);

    check(cfg->backend == CNET_VSA_BACKEND_GPU, "GPU is selected as default execution backend");
    check(cnet_vsa_device_is_gpu_active() == 1, "GPU is verified active by default");

    /* -------------------------------------------------------------
     * Test 2: Runtime Optional Backend Switching (GPU <-> CPU)
     * ------------------------------------------------------------- */
    printf("\n[2/4] Testing Dynamic Runtime Backend Switching...\n");
    int set_cpu_rc = cnet_vsa_device_set_backend(CNET_VSA_BACKEND_CPU);
    check(set_cpu_rc == 0, "Switched backend to CPU at runtime");
    check(cnet_vsa_device_is_gpu_active() == 0, "Device subsystem confirms CPU active");

    int set_gpu_rc = cnet_vsa_device_set_backend(CNET_VSA_BACKEND_GPU);
    check(set_gpu_rc == 0, "Switched backend back to GPU at runtime");
    check(cnet_vsa_device_is_gpu_active() == 1, "Device subsystem confirms GPU active again");

    /* -------------------------------------------------------------
     * Test 3: Output Parity & Verification across Backends
     * ------------------------------------------------------------- */
    printf("\n[3/4] Testing Unified Dispatch & Verification (CPU vs GPU)...\n");
    const size_t N = 100000;
    std::vector<CnetVsaBsc> a(N), b(N), out_gpu(N), out_cpu(N);
    uint64_t rng = 0xABCDEF1234567890ULL;
    for (size_t i = 0; i < N; ++i) {
        cnet_vsa_bsc_random(&a[i], &rng);
        cnet_vsa_bsc_random(&b[i], &rng);
    }

    /* Dispatch on GPU */
    cnet_vsa_device_set_backend(CNET_VSA_BACKEND_GPU);
    double gpu_ms = 0.0;
    cnet_vsa_dispatch_batch_bind(a.data(), b.data(), out_gpu.data(), N, &gpu_ms);

    /* Dispatch on CPU */
    cnet_vsa_device_set_backend(CNET_VSA_BACKEND_CPU);
    double cpu_ms = 0.0;
    cnet_vsa_dispatch_batch_bind(a.data(), b.data(), out_cpu.data(), N, &cpu_ms);

    /* Verify 100% bitwise parity */
    int parity_ok = (memcmp(out_gpu.data(), out_cpu.data(), N * sizeof(CnetVsaBsc)) == 0);
    printf("  Batch Binding (%zu vectors):\n", N);
    printf("    GPU Dispatch Time: %.3f ms (%.3f ns/op)\n", gpu_ms, (gpu_ms * 1e6) / (double)N);
    printf("    CPU Dispatch Time: %.3f ms (%.3f ns/op)\n", cpu_ms, (cpu_ms * 1e6) / (double)N);
    check(parity_ok == 1, "GPU and CPU dispatches produce 100% bit-identical results");

    /* Codebook clean-up parity */
    size_t target_idx = 42424;
    CnetVsaBsc query = a[target_idx];
    /* Corrupt 10 bits */
    for (int k = 0; k < 10; ++k) {
        query.w[k % 8] ^= (1ULL << (k * 5));
    }

    size_t gpu_best_idx = 0, cpu_best_idx = 0;
    int gpu_dist = 0, cpu_dist = 0;
    double gpu_search_ms = 0.0, cpu_search_ms = 0.0;

    cnet_vsa_device_set_backend(CNET_VSA_BACKEND_GPU);
    cnet_vsa_dispatch_codebook_search(&query, a.data(), N, &gpu_best_idx, &gpu_dist, &gpu_search_ms);

    cnet_vsa_device_set_backend(CNET_VSA_BACKEND_CPU);
    cnet_vsa_dispatch_codebook_search(&query, a.data(), N, &cpu_best_idx, &cpu_dist, &cpu_search_ms);

    printf("  Codebook Search (%zu vectors):\n", N);
    printf("    GPU Search: Index %zu (dist: %d) in %.3f ms\n", gpu_best_idx, gpu_dist, gpu_search_ms);
    printf("    CPU Search: Index %zu (dist: %d) in %.3f ms\n", cpu_best_idx, cpu_dist, cpu_search_ms);
    check(gpu_best_idx == target_idx && cpu_best_idx == target_idx, "Both backends locate exact needle index");
    check(gpu_dist == cpu_dist, "Both backends report identical minimum Hamming distance");

    /* -------------------------------------------------------------
     * Test 4: Environment Variable Configuration
     * ------------------------------------------------------------- */
    printf("\n[4/4] Testing Environment Variable Overrides...\n");
    setenv("CNET_VSA_DEVICE", "cpu", 1);
    cnet_vsa_device_init();
    check(cnet_vsa_device_is_gpu_active() == 0, "CNET_VSA_DEVICE=cpu forces CPU backend");

    setenv("CNET_VSA_DEVICE", "gpu", 1);
    cnet_vsa_device_init();
    check(cnet_vsa_device_is_gpu_active() == 1, "CNET_VSA_DEVICE=gpu enables GPU backend");

    unsetenv("CNET_VSA_DEVICE");

    printf("\n-----------------------------------------------------------------\n");
    if (failures == 0) {
        printf("CNET_VSA_DEVICE_BENCH_PASS: all %d checks passed.\n", checks);
        return 0;
    } else {
        printf("CNET_VSA_DEVICE_BENCH_FAIL: %d of %d checks failed.\n", failures, checks);
        return 1;
    }
}
