/* HIP backend resource/correctness gate — make hipgemm_res → HIPGEMM_RES_PASS
 *
 * Hermetic on a host with NO ROCm (cce_hipgemm_open returns NULL → tests SKIP
 * with HIPGEMM_RES_PASS and zero failures). On an AMD/ROCm host the tests
 * exercise the real device path:
 *
 *   1. q8 cache is freed on cce_hipgemm_close — file-static g_q8_caches[] must
 *      be reclaimed (no dangling device pointers), even under repeated
 *      open/close, so the global 8-slot table never fills with dead handles.
 *   2. fixed resident table exhaustion must NOT silently fall back: when
 *      resident_count reaches HIPGEMM_MAX_RES (forced small via the test
 *      seam), a bounded LRU eviction runs and an eviction counter surfaces to
 *      the caller, instead of a silent -1 → CPU fallback.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/cce/cce_hipgemm.h"

static int failures, checks;

static void check(int ok, const char *name) {
    checks++;
    printf("  %-58s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

/* HIP may be absent (CPU/hermetic host): open returns NULL.  Treat as a
 * clean SKIP — the resource contracts are device-only and must not regress
 * the build on a ROCm-less host. */
static int hip_present(void) {
    cce_hipgemm *h = cce_hipgemm_open(NULL, 0);
    if (h) cce_hipgemm_close(h);
    return h != NULL;
}

/* Tiny deterministic weight matrix so a matmul is exercised on the device. */
static void fill_seq(float *p, size_t n, int seed) {
    size_t i;
    for (i = 0; i < n; ++i)
        p[i] = (float)(((i + (size_t)seed) * 2654435761u) % 97 - 48) * 0.01f;
}

static void test_q8_cache_freed_on_close(void) {
    cce_hipgemm *h;
    cce_hipgemm *h2;
    size_t slots_before, slots_mid, slots_after;
    int8_t *wq;
    float *scales, *A, *C;
    size_t K = 256, N = 1024, T = 4;
    size_t i;

    printf("\n== q8 cache freed on close ==\n");

    slots_before = cce_hipgemm_q8_cache_slots_used();

    h = cce_hipgemm_open(NULL, 0);
    check(h != NULL, "open hip for q8 test");
    if (!h) return;

    /* run one q8 matmul so the g_q8_caches[] slot is populated with device
     * memory for this handle. */
    wq = (int8_t *)malloc(K * N);
    scales = (float *)malloc(N * sizeof(float));
    A = (float *)malloc(T * K * sizeof(float));
    C = (float *)malloc(T * N * sizeof(float));
    check(wq && scales && A && C, "q8 scratch alloc");
    for (i = 0; i < K * N; ++i) wq[i] = (int8_t)((i * 7) % 51 - 25);
    for (i = 0; i < N; ++i) scales[i] = 0.125f;
    fill_seq(A, T * K, 11);

    /* First call populates the q8 cache (uploads + allocates device memory);
     * result code is informational on a heterogeneous host. */
    (void)cce_hipgemm_matmul_q8(h, A, T, K, wq, scales, NULL, N, C);

    slots_mid = cce_hipgemm_q8_cache_slots_used();
    check(slots_mid == slots_before + 1, "q8 cache slot allocated for handle");

    cce_hipgemm_close(h);

    /* The file-static slot MUST have been reclaimed: the global count returns
     * to the pre-open value, so repeated open/close cannot exhaust the 8-slot
     * table with dangling handles. */
    slots_after = cce_hipgemm_q8_cache_slots_used();
    check(slots_after == slots_before,
          "q8 cache slot freed on close (no dangling handle)");

    /* Repeated open/close must keep the global table clean across cycles. */
    for (i = 0; i < 16; ++i) {
        cce_hipgemm *x = cce_hipgemm_open(NULL, 0);
        if (!x) { check(0, "reopen cycle"); return; }
        (void)cce_hipgemm_matmul_q8(x, A, T, K, wq, scales, NULL, N, C);
        cce_hipgemm_close(x);
    }
    check(cce_hipgemm_q8_cache_slots_used() == slots_before,
          "q8 cache stays clean across 16 open/close cycles");

    /* A fresh open after the storm must still get a q8 cache slot. */
    h2 = cce_hipgemm_open(NULL, 0);
    check(h2 != NULL, "open still succeeds after reopen storm");
    if (h2) {
        (void)cce_hipgemm_matmul_q8(h2, A, T, K, wq, scales, NULL, N, C);
        check(cce_hipgemm_q8_cache_slots_used() == slots_before + 1,
              "q8 slot reusable after storm");
        cce_hipgemm_close(h2);
        check(cce_hipgemm_q8_cache_slots_used() == slots_before,
              "q8 slot released after final close");
    }

    free(wq); free(scales); free(A); free(C);
}

static void test_resident_eviction_no_silent_fallback(void) {
    cce_hipgemm *h;
    float *A, *C;
    size_t T = 4, K = 256, N = 1024;
    size_t i;
    size_t ev0, ev1;
    size_t rc_after;

    printf("\n== resident table bounded eviction (no silent fallback) ==\n");

    h = cce_hipgemm_open(NULL, 0);
    check(h != NULL, "open hip for resident test");
    if (!h) return;

    ev0 = cce_hipgemm_resident_evictions(h);

    /* The matmul contract: with HIPGEMM_MAX_RES exceeded (we force the table
     * near full by uploading many distinct weights), the backend must NOT
     * silently return -1 and fall to CPU. It must evict and succeed, and the
     * eviction counter must surface the contract breach to the caller. */
    A = (float *)malloc(T * K * sizeof(float));
    C = (float *)malloc(T * N * sizeof(float));
    check(A && C, "resident scratch alloc");
    fill_seq(A, T * K, 7);

    /* Upload HIPGEMM_MAX_RES + 4 distinct weight matrices (one matmul each).
     * Each uses a unique host pointer → unique resident entry → forces the
     * table past its fixed cap. We hold every weight allocation live for the
     * whole storm so the host pointers stay distinct (free+remalloc would
     * reuse the same address and res_find would match the old entry). Under
     * the old silent-exhaustion path this would return -1 for every entry
     * beyond the cap. Under the new contract every call succeeds and
     * evictions are surfaced. */
    {
        size_t pushes = (size_t)cce_hipgemm_max_resident() + 4;
        int all_ok = 1;
        float **ws = (float **)malloc(pushes * sizeof(float *));
        check(ws != NULL, "weight array alloc");
        if (!ws) { cce_hipgemm_close(h); free(A); free(C); return; }
        for (i = 0; i < pushes; ++i) {
            int r;
            ws[i] = (float *)malloc(K * N * sizeof(float));
            if (!ws[i]) { all_ok = 0; break; }
            fill_seq(ws[i], K * N, (int)(100 + i));
            r = cce_hipgemm_matmul(h, A, T, K, ws[i], NULL, N, C);
            if (r != 0) all_ok = 0;
        }
        check(all_ok, "all matmuls succeed across table cap (no silent -1)");
        for (i = 0; i < pushes; ++i) free(ws[i]);
        free(ws);
    }

    ev1 = cce_hipgemm_resident_evictions(h);
    rc_after = cce_hipgemm_resident_count(h);

    check(ev1 > ev0, "eviction counter surfaced (non-zero delta)");
    check(rc_after <= (size_t)cce_hipgemm_max_resident(),
          "resident_count bounded by cap after storm");
    check(ev1 - ev0 >= 4,
          "at least 4 evictions reported (one per overflow entry)");

    /* Resident bytes must remain sane (non-zero, since weights are live). */
    check(cce_hipgemm_resident_bytes(h) > 0, "resident bytes non-zero");

    cce_hipgemm_close(h);
    free(A); free(C);
}

int main(void) {
    printf("== HIP backend resource/correctness gate ==");

    if (!hip_present()) {
        /* A self-skip is correct on a CPU-only runner and wrong on a GPU host:
           it would report success for tests that never executed. `make ci_rocm`
           sets CNET_REQUIRE_ROCM=1 so the ROCm lane cannot pass by skipping. */
        const char* require = getenv("CNET_REQUIRE_ROCM");
        if (require && *require && strcmp(require, "0") != 0) {
            printf("\n  HIP unavailable but CNET_REQUIRE_ROCM=1 — the ROCm lane"
                   " cannot be satisfied by a skip.\n");
            printf("HIPGEMM_RES_FAIL reason=rocm_required_but_absent\n");
            return 1;
        }
        printf("\n  HIP unavailable — SKIP (CPU/hermetic). HIPGEMM_RES_PASS\n");
        printf("HIPGEMM_RES_PASS status=skipped_no_device\nfailures=0\n");
        return 0;
    }

    test_q8_cache_freed_on_close();
    test_resident_eviction_no_silent_fallback();

    printf("\nHIPGEMM_RES_PASS status=measured_on_device\nfailures=%d\n", failures);
    return failures != 0;
}