/* CUDA backend smoke gate — make cudagemm_res → CUDAGEMM_RES_PASS
 *
 * Hermetic when CUDA is absent (cce_cudagemm_open returns NULL → SKIP with
 * CUDAGEMM_RES_PASS). On an NVIDIA host, exercises a tiny FP32 matmul so the
 * dlopen + cuBLAS path is not a paper tiger.
 *
 * Set CNET_REQUIRE_CUDA=1 to fail the skip (CI / Windows CUDA laptop).
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/cce/cce_cudagemm.h"

static int failures, checks;

static void check(int ok, const char *name) {
    checks++;
    printf("  %-58s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static int cuda_present(void) {
    cce_cudagemm *h = cce_cudagemm_open(NULL, 0);
    if (h) cce_cudagemm_close(h);
    return h != NULL;
}

static void fill_seq(float *p, size_t n, int seed) {
    size_t i;
    for (i = 0; i < n; ++i)
        p[i] = (float)(((i + (size_t)seed) * 2654435761u) % 97 - 48) * 0.01f;
}

static void test_open_close(void) {
    char name[128] = {0};
    cce_cudagemm *h;
    printf("\n== open / close ==\n");
    h = cce_cudagemm_open(name, sizeof name);
    check(h != NULL, "open CUDA");
    if (!h) return;
    check(cce_cudagemm_device_count(h) >= 1, "at least one discrete device");
    printf("  device: %s\n", name[0] ? name : "(unnamed)");
    cce_cudagemm_close(h);
    check(1, "close without crash");
}

static void test_fp32_matmul(void) {
    cce_cudagemm *h;
    const size_t T = 4, K = 64, N = 128;
    float *A, *W, *C, *ref;
    size_t t, k, n;
    int ok = 1;
    double max_abs = 0.0;
    static char min_flops_env[] = "CNET_GPU_MIN_FLOPS=1";

    printf("\n== fp32 matmul ==\n");
    /* min_flops is latched at open(); set before open. */
    putenv(min_flops_env);
    h = cce_cudagemm_open(NULL, 0);
    check(h != NULL, "open for matmul");
    if (!h) return;

    A = (float *)malloc(T * K * sizeof(float));
    W = (float *)malloc(K * N * sizeof(float));
    C = (float *)malloc(T * N * sizeof(float));
    ref = (float *)malloc(T * N * sizeof(float));
    check(A && W && C && ref, "scratch alloc");
    if (!A || !W || !C || !ref) {
        free(A); free(W); free(C); free(ref);
        cce_cudagemm_close(h);
        return;
    }
    fill_seq(A, T * K, 3);
    fill_seq(W, K * N, 7);
    memset(C, 0, T * N * sizeof(float));

    check(cce_cudagemm_matmul(h, A, T, K, W, NULL, N, C) == 0,
          "cce_cudagemm_matmul returns 0");

    for (t = 0; t < T; ++t) {
        for (n = 0; n < N; ++n) {
            double acc = 0.0;
            for (k = 0; k < K; ++k)
                acc += (double)A[t * K + k] * (double)W[k * N + n];
            ref[t * N + n] = (float)acc;
            {
                double d = fabs(acc - (double)C[t * N + n]);
                if (d > max_abs) max_abs = d;
                if (d > 1e-3) ok = 0;
            }
        }
    }
    printf("  max_abs_err=%.6g\n", max_abs);
    check(ok, "decision-close to CPU reference (1e-3)");

    free(A); free(W); free(C); free(ref);
    cce_cudagemm_close(h);
}

int main(void) {
    const char *req = getenv("CNET_REQUIRE_CUDA");
    printf("cudagemm_res\n");
    if (!cuda_present()) {
        if (req && req[0] == '1') {
            fprintf(stderr, "CNET_REQUIRE_CUDA=1 but no CUDA device/runtime\n");
            return 1;
        }
        printf("CUDAGEMM_RES_SKIP (no CUDA)\n");
        printf("CUDAGEMM_RES_PASS\n");
        return 0;
    }
    test_open_close();
    test_fp32_matmul();
    printf("\n%d checks, %d failures\n", checks, failures);
    if (failures) return 1;
    printf("CUDAGEMM_RES_PASS\n");
    return 0;
}
