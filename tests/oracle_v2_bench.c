#define _POSIX_C_SOURCE 200809L

#include <inttypes.h>
#include "../include/cnet_platform.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../include/acquire.h"

#if defined(__GNUC__) || defined(__clang__)
#define NOINLINE __attribute__((noinline))
#else
#define NOINLINE
#endif

#define BENCH_ROUNDS 7
#define BENCH_ITERATIONS UINT64_C(20000000)

static volatile double bench_sink;

typedef struct {
    uint64_t evidence_digest;
} BenchContext;

static NOINLINE int invert_v1(const double *in, double *out, void *ctx) {
    (void)ctx;
    out[0] = in[0] > 0.5 ? 0.0 : 1.0;
    return 0;
}

static NOINLINE int invert_v2(const double *in, size_t in_count,
                              double *out, size_t out_count,
                              CnetOracleResult *result, void *ctx) {
    const BenchContext *bench = (const BenchContext *)ctx;
    if (in_count != 1 || out_count != 1) return -1;
    memset(result, 0, sizeof *result);
    result->abi_version = CNET_ORACLE_ABI_VERSION;
    result->struct_size = (uint32_t)sizeof *result;
    result->status = CNET_ORACLE_ANSWER;
    result->confidence = 0.9375;
    result->evidence_digest = bench->evidence_digest;
    out[0] = in[0] > 0.5 ? 0.0 : 1.0;
    return 0;
}

static NOINLINE CnetOracleValidity validate_inverse(
    const double *in, size_t in_count,
    const double *out, size_t out_count,
    const CnetOracleResult *result, void *ctx) {
    const BenchContext *bench = (const BenchContext *)ctx;
    if (in_count != 1 || out_count != 1 ||
        result->evidence_digest != bench->evidence_digest) {
        return CNET_ORACLE_INVALID;
    }
    return out[0] == (in[0] > 0.5 ? 0.0 : 1.0)
           ? CNET_ORACLE_VALID : CNET_ORACLE_INVALID;
}

static Port bit_port(const char *tag) {
    Port port;
    memset(&port, 0, sizeof port);
    port.family = PORT_BINARY_MSB;
    port.field_width = 1;
    port.field_count = 1;
    snprintf(port.tag, sizeof port.tag, "%s", tag);
    return port;
}

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

static double run_direct(uint64_t iterations) {
    double in[1] = {0.0}, out[1] = {0.0};
    uint64_t start = now_ns();
    for (uint64_t i = 0; i < iterations; ++i) {
        in[0] = (double)(i & 1u);
        (void)invert_v1(in, out, NULL);
        bench_sink += out[0];
    }
    return (double)(now_ns() - start) / (double)iterations;
}

static double run_governed(OracleEntry *entry, uint64_t iterations) {
    double in[1] = {0.0}, out[1] = {0.0};
    CnetOracleResult result;
    uint64_t start = now_ns();
    for (uint64_t i = 0; i < iterations; ++i) {
        in[0] = (double)(i & 1u);
        if (cnet_oracle_invoke(entry, in, 1, out, 1, &result) !=
            CNET_ORACLE_ANSWER) {
            fprintf(stderr, "governed invocation refused at iteration %" PRIu64 "\n", i);
            exit(2);
        }
        bench_sink += out[0] + (double)(result.evidence_digest & 1u);
    }
    return (double)(now_ns() - start) / (double)iterations;
}

static int compare_double(const void *a, const void *b) {
    double da = *(const double *)a;
    double db = *(const double *)b;
    return (da > db) - (da < db);
}

static double median(double values[BENCH_ROUNDS]) {
    qsort(values, BENCH_ROUNDS, sizeof values[0], compare_double);
    return values[BENCH_ROUNDS / 2];
}

static void print_result(const char *name, double ns, double direct_ns) {
    printf("%-28s %9.3f ns/call  %12.0f calls/s  %6.2fx direct  %+8.3f ns\n",
           name, ns, 1.0e9 / ns, ns / direct_ns, ns - direct_ns);
}

int main(void) {
    OracleRegistry registry;
    CnetOracleIdentity identity;
    BenchContext context = {UINT64_C(0xfeedbeefcafebabe)};
    double direct[BENCH_ROUNDS], governed_v1[BENCH_ROUNDS];
    double governed_v2[BENCH_ROUNDS], verified_v2[BENCH_ROUNDS];
    Port input_port = bit_port("bench_bit");
    Port output_port = bit_port("bench_not_bit");
    OracleEntry *v1, *v2, *verified;

    memset(&registry, 0, sizeof registry);
    memset(&identity, 0, sizeof identity);
    identity.abi_version = CNET_ORACLE_ABI_VERSION;
    identity.struct_size = (uint32_t)sizeof identity;
    identity.artifact_digest = UINT64_C(0x1111222233334444);
    identity.contract_digest = UINT64_C(0x5555666677778888);
    identity.config_digest = UINT64_C(0x0102030405060708);
    identity.retrieval_snapshot_digest = UINT64_C(0x1020304050607080);
    identity.toolchain_digest = UINT64_C(0xa1a2a3a4a5a6a7a8);

    if (acquire_oracle_register(&registry, "bench_v1", input_port, output_port,
                                invert_v1, NULL) != 0 ||
        acquire_oracle_register_v2(&registry, "bench_v2", input_port, output_port,
                                   invert_v2, NULL, &identity, &context) != 0 ||
        acquire_oracle_register_v2(&registry, "bench_verified", input_port, output_port,
                                   invert_v2, validate_inverse, &identity, &context) != 0) {
        fprintf(stderr, "benchmark Oracle registration failed\n");
        return 1;
    }
    v1 = &registry.entries[0];
    v2 = &registry.entries[1];
    verified = &registry.entries[2];

    /* Warm all code and data paths before measuring. */
    (void)run_direct(100000);
    (void)run_governed(v1, 100000);
    (void)run_governed(v2, 100000);
    (void)run_governed(verified, 100000);

    for (int round = 0; round < BENCH_ROUNDS; ++round) {
        direct[round] = run_direct(BENCH_ITERATIONS);
        governed_v1[round] = run_governed(v1, BENCH_ITERATIONS);
        governed_v2[round] = run_governed(v2, BENCH_ITERATIONS);
        verified_v2[round] = run_governed(verified, BENCH_ITERATIONS);
    }

    double direct_ns = median(direct);
    double governed_v1_ns = median(governed_v1);
    double governed_v2_ns = median(governed_v2);
    double verified_v2_ns = median(verified_v2);

    printf("Oracle v2 governed invocation benchmark\n");
    printf("rounds=%d iterations/round=%" PRIu64 " statistic=median clock=MONOTONIC_RAW\n",
           BENCH_ROUNDS, BENCH_ITERATIONS);
    print_result("direct v1 callback", direct_ns, direct_ns);
    print_result("governed v1 compatibility", governed_v1_ns, direct_ns);
    print_result("governed v2 evidence", governed_v2_ns, direct_ns);
    print_result("governed v2 + semantic", verified_v2_ns, direct_ns);
    printf("v2 semantic-validator delta: %.3f ns/call\n",
           verified_v2_ns - governed_v2_ns);
    printf("sink=%.0f calls=%zu/%zu/%zu rejects=%zu/%zu/%zu\n",
           bench_sink, v1->calls, v2->calls, verified->calls,
           v1->rejects, v2->rejects, verified->rejects);
    return 0;
}
