#define _POSIX_C_SOURCE 200809L

#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/acquire.h"
#include "../include/async_runtime.h"
#include "../include/cce/cce_clgemm.h"

#define K 1024u
#define N 4096u
#define JOBS 16u

typedef struct {
    cce_clgemm *gpu;
    const float *weights;
    float *input;
    float *output;
    int lane;
} GpuLane;

static int gpu_matmul_oracle(const double *in, size_t in_count,
                             double *out, size_t out_count,
                             CnetOracleResult *result, void *opaque) {
    GpuLane *lane = (GpuLane *)opaque;
    if (!lane || !lane->gpu || in_count != K || out_count != N) return -1;
    for (size_t i = 0; i < K; ++i) lane->input[i] = (float)in[i];
    if (cce_clgemm_matmul(lane->gpu, lane->input, 1, K,
                          lane->weights, NULL, N, lane->output) != 0) return -1;
    for (size_t i = 0; i < N; ++i) out[i] = (double)lane->output[i];
    memset(result, 0, sizeof *result);
    result->abi_version = CNET_ORACLE_ABI_VERSION;
    result->struct_size = (uint32_t)sizeof *result;
    result->status = CNET_ORACLE_ANSWER;
    result->confidence = 1.0;
    result->evidence_digest = UINT64_C(0xd000) + (uint64_t)lane->lane;
    return 0;
}

static Port raw_port(size_t width, const char *tag) {
    Port port;
    memset(&port, 0, sizeof port);
    port.family = PORT_RAW;
    port.field_width = width;
    port.field_count = 1;
    snprintf(port.tag, sizeof port.tag, "%s", tag);
    return port;
}

static CnetOracleIdentity identity(int lane) {
    CnetOracleIdentity id;
    memset(&id, 0, sizeof id);
    id.abi_version = CNET_ORACLE_ABI_VERSION;
    id.struct_size = (uint32_t)sizeof id;
    id.artifact_digest = UINT64_C(0xd54d0001);
    id.contract_digest = UINT64_C(0xd54d0002);
    id.config_digest = UINT64_C(0xd54d1000) + (uint64_t)lane;
    id.toolchain_digest = UINT64_C(0x12010000);
    return id;
}

static float deterministic_float(uint32_t *state) {
    *state = *state * UINT32_C(1664525) + UINT32_C(1013904223);
    return ((float)(*state >> 9) / (float)(UINT32_C(1) << 23)) - 1.0f;
}

static void cpu_reference(const double *input, const float *weights, double *out) {
    for (size_t n = 0; n < N; ++n) {
        float acc = 0.0f;
        for (size_t k = 0; k < K; ++k)
            acc += (float)input[k] * weights[k * N + n];
        out[n] = (double)acc;
    }
}

int main(void) {
    cce_clgemm *probe = NULL;
    GpuLane lane[2];
    OracleRegistry registry;
    CnetLaneSpec specs[2];
    CnetLanePool *pool = NULL;
    CnetLaneSubmitOptions options;
    CnetLaneTicket tickets[JOBS];
    CnetLaneStats stats[2];
    CnetOracleResult oracle_result;
    CnetOracleIdentity ids[2];
    Port input_port = raw_port(K, "gpu_lane_input");
    Port output_port = raw_port(N, "gpu_lane_output");
    float *weights = NULL;
    double *inputs = NULL;
    double *reference = NULL;
    double *output = NULL;
    uint32_t rng = UINT32_C(0x13579bdf);
    uint64_t serial_start, serial_ns, async_start, async_ns;
    char device_name[160] = {0};
    int rc = 1;

    memset(lane, 0, sizeof lane);
    memset(&registry, 0, sizeof registry);
    memset(specs, 0, sizeof specs);

    probe = cce_clgemm_open(NULL, device_name, sizeof device_name);
    if (!probe || cce_clgemm_device_count(probe) != 2) {
        fprintf(stderr, "expected exactly two selected discrete GPUs, got %zu (%s)\n",
                probe ? cce_clgemm_device_count(probe) : 0, device_name);
        goto cleanup;
    }
    printf("selected: %s; discrete_devices=%zu (unified-memory iGPU excluded)\n",
           device_name, cce_clgemm_device_count(probe));
    cce_clgemm_close(probe);
    probe = NULL;

    weights = (float *)malloc((size_t)K * N * sizeof *weights);
    inputs = (double *)malloc((size_t)JOBS * K * sizeof *inputs);
    reference = (double *)malloc((size_t)JOBS * N * sizeof *reference);
    output = (double *)malloc((size_t)N * sizeof *output);
    if (!weights || !inputs || !reference || !output) {
        fprintf(stderr, "allocation failed\n");
        goto cleanup;
    }
    for (size_t i = 0; i < (size_t)K * N; ++i)
        weights[i] = deterministic_float(&rng) * 0.02f;
    for (size_t j = 0; j < JOBS; ++j) {
        for (size_t k = 0; k < K; ++k)
            inputs[j * K + k] = (double)deterministic_float(&rng);
        cpu_reference(inputs + j * K, weights, reference + j * N);
    }

    for (int i = 0; i < 2; ++i) {
        char lane_name[160] = {0};
        lane[i].gpu = cce_clgemm_open_device(NULL, i, lane_name,
                                              sizeof lane_name);
        lane[i].weights = weights;
        lane[i].input = (float *)malloc(K * sizeof *lane[i].input);
        lane[i].output = (float *)malloc(N * sizeof *lane[i].output);
        lane[i].lane = i;
        ids[i] = identity(i);
        if (!lane[i].gpu || !lane[i].input || !lane[i].output ||
            acquire_oracle_register_v2(&registry, i ? "r9700_1" : "r9700_0",
                input_port, output_port, gpu_matmul_oracle, NULL,
                &ids[i], &lane[i]) != 0) {
            fprintf(stderr, "lane %d initialization failed (%s)\n", i, lane_name);
            goto cleanup;
        }
        specs[i].abi_version = CNET_LANE_ABI_VERSION;
        specs[i].struct_size = (uint32_t)sizeof specs[i];
        specs[i].oracle = &registry.entries[i];
        specs[i].resource_mask = UINT64_C(1) << i;
        snprintf(specs[i].name, sizeof specs[i].name, "r9700-gpu%d", i);
        printf("lane %d: %s\n", i, lane_name);
    }

    /* Warm uploads and kernels independently before timing. */
    for (int i = 0; i < 2; ++i) {
        if (cnet_oracle_invoke(&registry.entries[i], inputs, K, output, N,
                               &oracle_result) != CNET_ORACLE_ANSWER) {
            fprintf(stderr, "lane %d warmup failed\n", i);
            goto cleanup;
        }
    }

    serial_start = cnet_lane_now_ns();
    for (size_t j = 0; j < JOBS; ++j) {
        if (cnet_oracle_invoke(&registry.entries[0], inputs + j * K, K,
                               output, N, &oracle_result) !=
            CNET_ORACLE_ANSWER) {
            fprintf(stderr, "serial call %zu failed\n", j);
            goto cleanup;
        }
    }
    serial_ns = cnet_lane_now_ns() - serial_start;

    if (cnet_lane_pool_open(&pool, specs, 2, JOBS) != CNET_LANE_OK) {
        fprintf(stderr, "dual GPU lane pool failed to open\n");
        goto cleanup;
    }
    memset(&options, 0, sizeof options);
    options.abi_version = CNET_LANE_ABI_VERSION;
    options.struct_size = (uint32_t)sizeof options;

    async_start = cnet_lane_now_ns();
    for (size_t j = 0; j < JOBS; ++j) {
        if (cnet_lane_pool_submit(pool, inputs + j * K, K, N, &options,
                                  &tickets[j]) != CNET_LANE_OK) {
            fprintf(stderr, "async submit %zu failed\n", j);
            goto cleanup;
        }
    }
    for (size_t j = 0; j < JOBS; ++j) {
        CnetLaneResult result;
        if (cnet_lane_pool_wait(pool, tickets[j], 30000, output, N, &result) !=
                CNET_LANE_OK || result.status != CNET_ORACLE_ANSWER) {
            fprintf(stderr, "async collect %zu failed\n", j);
            goto cleanup;
        }
        for (size_t n = 0; n < N; ++n) {
            if (output[n] != reference[j * N + n]) {
                fprintf(stderr,
                        "job %zu lane %u mismatch at %zu gpu=%.9g ref=%.9g\n",
                        j, result.lane_index, n, output[n],
                        reference[j * N + n]);
                goto cleanup;
            }
        }
    }
    async_ns = cnet_lane_now_ns() - async_start;

    if (cnet_lane_pool_lane_stats(pool, 0, &stats[0]) != CNET_LANE_OK ||
        cnet_lane_pool_lane_stats(pool, 1, &stats[1]) != CNET_LANE_OK ||
        stats[0].calls == 0 || stats[1].calls == 0 ||
        stats[0].successes + stats[1].successes != JOBS) {
        fprintf(stderr, "both physical lanes did not execute: %" PRIu64 "/%" PRIu64 "\n",
                stats[0].calls, stats[1].calls);
        goto cleanup;
    }

    printf("serial_gpu0: %.3f ms, %.2f jobs/s\n",
           (double)serial_ns / 1.0e6,
           (double)JOBS * 1.0e9 / (double)serial_ns);
    printf("async_2xr9700: %.3f ms, %.2f jobs/s, speedup %.3fx\n",
           (double)async_ns / 1.0e6,
           (double)JOBS * 1.0e9 / (double)async_ns,
           (double)serial_ns / (double)async_ns);
    printf("lane_calls: gpu0=%" PRIu64 " gpu1=%" PRIu64
           " busy_ms=%.3f/%.3f\n",
           stats[0].calls, stats[1].calls,
           (double)stats[0].busy_ns / 1.0e6,
           (double)stats[1].busy_ns / 1.0e6);
    printf("ASYNC_GPU_LANES_PASS\n");
    rc = 0;

cleanup:
    cnet_lane_pool_close(pool);
    for (int i = 0; i < 2; ++i) {
        cce_clgemm_close(lane[i].gpu);
        free(lane[i].input);
        free(lane[i].output);
    }
    cce_clgemm_close(probe);
    free(weights);
    free(inputs);
    free(reference);
    free(output);
    return rc;
}
