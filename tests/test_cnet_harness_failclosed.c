/*
 * test_cnet_harness_failclosed.c — the "no fake backend" fail-closed test.
 *
 * Links cnet_harness_core.c with the plugin's own weak backend default and
 * NO strong overrides. Proves that:
 *
 *   - Opening a session with a valid config and a readable dummy model
 *     path still returns CNET_HARNESS_ERR_BACKEND, because
 *     harness_backend_open's fail-closed weak default refuses to fabricate
 *     a success without a real backend.
 *   - session_out remains NULL on that failure path (no leaked session,
 *     no partially-constructed state).
 *   - cnet_harness_generation_free(NULL) and cnet_harness_close(NULL) are
 *     safe.
 *
 * The main contract test link line intentionally does the opposite: it
 * provides strong fake backend hooks. Splitting the two tests keeps the
 * production weak default honest.
 */
#include "../include/cnet_harness.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int pass_count = 0;
static int fail_count = 0;

#define CHECK(cond, msg) do {                                       \
    if (cond) { pass_count++; }                                     \
    else { fail_count++; fprintf(stderr, "FAIL: %s\n", msg); }      \
} while (0)

int main(void) {
    printf("=== CNET harness fail-closed test ===\n");

    char temp_path[512];
    snprintf(temp_path, sizeof(temp_path),
             "/tmp/cnet_harness_failclosed_%d.gguf", (int)getpid());
    FILE *f = fopen(temp_path, "wb");
    if (!f) {
        fprintf(stderr, "cannot create temp file %s: %s\n",
                temp_path, strerror(errno));
        return 2;
    }
    fwrite("stub", 1u, 4u, f);
    fclose(f);

    CnetHarnessConfig c;
    memset(&c, 0, sizeof(c));
    c.abi_version = CNET_HARNESS_ABI_VERSION;
    c.struct_size = (uint32_t)sizeof(c);
    c.model_id = "dummy";
    c.model_path = temp_path;
    c.resource_mask = (uint64_t)CNET_HARNESS_RESOURCE_CPU;
    c.budget_bytes = 1024ull * 1024ull;
    c.main_gpu = -1;
    c.n_ctx = 512;
    c.n_batch = 128;
    c.n_threads = 2;
    c.aicimo_num_ops = 4;
    c.aicimo_base_dim = 32;

    CnetHarnessSession *s = (CnetHarnessSession *)0xdeadbeef;
    int rc = cnet_harness_open(&c, &s);
    CHECK(rc == CNET_HARNESS_ERR_BACKEND,
          "failclosed: readable dummy still fails ERR_BACKEND without a backend");
    CHECK(s == NULL,
          "failclosed: session_out stays NULL when backend refuses to open");

    /* NULL-teardown safety */
    cnet_harness_generation_free(NULL);
    CHECK(cnet_harness_close(NULL) == CNET_HARNESS_OK,
          "failclosed: close(NULL) is OK");

    unlink(temp_path);

    printf("\nResults: %d passed, %d failed\n", pass_count, fail_count);
    if (fail_count > 0) {
        printf("CNET_HARNESS_FAILCLOSED_TEST_FAIL\n");
        return 1;
    }
    printf("CNET_HARNESS_FAILCLOSED_TEST_PASS\n");
    return 0;
}
