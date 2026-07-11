#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "../include/acquire.h"
#include "../include/async_runtime.h"

static int failures;
#define CHECK(cond, msg) do { \
    if (cond) printf("  ok: %s\n", (msg)); \
    else { printf("  FAIL: %s\n", (msg)); failures++; } \
} while (0)

typedef struct {
    int lane;
    long delay_ns;
} Fixture;

static int delayed_oracle(const double *in, size_t in_count,
                          double *out, size_t out_count,
                          CnetOracleResult *result, void *opaque) {
    Fixture *fixture = (Fixture *)opaque;
    struct timespec delay;
    if (!fixture || in_count != 1 || out_count != 1) return -1;
    delay.tv_sec = fixture->delay_ns / 1000000000L;
    delay.tv_nsec = fixture->delay_ns % 1000000000L;
    nanosleep(&delay, NULL);
    memset(result, 0, sizeof *result);
    result->abi_version = CNET_ORACLE_ABI_VERSION;
    result->struct_size = (uint32_t)sizeof *result;
    result->status = CNET_ORACLE_ANSWER;
    result->confidence = 1.0;
    result->evidence_digest = UINT64_C(0xc000) + (uint64_t)fixture->lane;
    out[0] = in[0] + 1.0;
    return 0;
}

static Port raw_scalar(const char *tag) {
    Port port;
    memset(&port, 0, sizeof port);
    port.family = PORT_RAW;
    port.field_width = 1;
    port.field_count = 1;
    snprintf(port.tag, sizeof port.tag, "%s", tag);
    return port;
}

static CnetOracleIdentity identity(uint64_t artifact) {
    CnetOracleIdentity id;
    memset(&id, 0, sizeof id);
    id.abi_version = CNET_ORACLE_ABI_VERSION;
    id.struct_size = (uint32_t)sizeof id;
    id.artifact_digest = artifact;
    id.contract_digest = UINT64_C(0x2000);
    id.config_digest = UINT64_C(0x3000);
    id.toolchain_digest = UINT64_C(0x4000);
    return id;
}

static int register_lanes(OracleRegistry *registry, Fixture fixtures[2],
                          CnetLaneSpec specs[2], long delay_ns) {
    Port in = raw_scalar("async_in");
    Port out = raw_scalar("async_out");
    CnetOracleIdentity ids[2] = {identity(0x1001), identity(0x1002)};
    memset(registry, 0, sizeof *registry);
    memset(specs, 0, 2 * sizeof specs[0]);
    for (int i = 0; i < 2; ++i) {
        fixtures[i].lane = i;
        fixtures[i].delay_ns = delay_ns;
        if (acquire_oracle_register_v2(registry, i ? "lane1" : "lane0",
                                       in, out, delayed_oracle, NULL,
                                       &ids[i], &fixtures[i]) != 0) return -1;
        specs[i].abi_version = CNET_LANE_ABI_VERSION;
        specs[i].struct_size = (uint32_t)sizeof specs[i];
        specs[i].oracle = &registry->entries[i];
        specs[i].resource_mask = UINT64_C(1) << i;
        snprintf(specs[i].name, sizeof specs[i].name, "gpu%d", i);
    }
    return 0;
}

static void test_two_lanes_are_work_conserving(void) {
    OracleRegistry registry;
    Fixture fixtures[2];
    CnetLaneSpec specs[2];
    CnetLanePool *pool = NULL;
    CnetLaneTicket tickets[8];
    double expected[8];
    CnetLaneSubmitOptions options;
    CnetLaneStats stats0, stats1;
    uint64_t started, elapsed;

    CHECK(register_lanes(&registry, fixtures, specs, 30000000L) == 0,
          "two independent Oracle lanes register");
    CHECK(cnet_lane_pool_open(&pool, specs, 2, 16) == CNET_LANE_OK,
          "work-conserving lane pool opens");
    memset(&options, 0, sizeof options);
    options.abi_version = CNET_LANE_ABI_VERSION;
    options.struct_size = (uint32_t)sizeof options;

    started = cnet_lane_now_ns();
    for (int i = 0; i < 8; ++i) {
        double input[1] = {(double)i};
        expected[i] = input[0] + 1.0;
        CHECK(cnet_lane_pool_submit(pool, input, 1, 1, &options,
                                    &tickets[i]) == CNET_LANE_OK,
              "job submits without borrowing caller input");
        input[0] = -999.0;
    }
    for (int i = 0; i < 8; ++i) {
        double output[1] = {0};
        CnetLaneResult result;
        CHECK(cnet_lane_pool_wait(pool, tickets[i], 2000, output, 1,
                                  &result) == CNET_LANE_OK &&
              result.status == CNET_ORACLE_ANSWER &&
              output[0] == expected[i] &&
              result.ticket == tickets[i] &&
              result.lane_index < 2 &&
              result.queue_ns > 0 && result.execution_ns > 0,
              "ordered collection returns exact output and timing evidence");
    }
    elapsed = cnet_lane_now_ns() - started;
    CHECK(elapsed < UINT64_C(200000000),
          "two lanes beat the 240 ms serial lower bound");
    CHECK(cnet_lane_pool_lane_stats(pool, 0, &stats0) == CNET_LANE_OK &&
          cnet_lane_pool_lane_stats(pool, 1, &stats1) == CNET_LANE_OK &&
          stats0.calls > 0 && stats1.calls > 0 &&
          stats0.resource_mask == 1 && stats1.resource_mask == 2 &&
          stats0.successes + stats1.successes == 8,
          "both resource lanes execute useful work");
    cnet_lane_pool_close(pool);
}

static void test_backpressure_cancel_deadline_and_wait_timeout(void) {
    OracleRegistry registry;
    Fixture fixtures[2];
    CnetLaneSpec specs[2];
    CnetLanePool *pool = NULL;
    CnetLaneSubmitOptions options;
    CnetLaneTicket first, second, expired;
    CnetLaneResult result;
    double input[1] = {7.0}, output[1] = {0};

    CHECK(register_lanes(&registry, fixtures, specs, 100000000L) == 0,
          "control lanes register");
    CHECK(cnet_lane_pool_open(&pool, specs, 1, 2) == CNET_LANE_OK,
          "single-lane bounded pool opens");
    memset(&options, 0, sizeof options);
    options.abi_version = CNET_LANE_ABI_VERSION;
    options.struct_size = (uint32_t)sizeof options;

    CHECK(cnet_lane_pool_submit(pool, input, 1, 1, &options, &first) ==
          CNET_LANE_OK, "first bounded job submits");
    CHECK(cnet_lane_pool_submit(pool, input, 1, 1, &options, &second) ==
          CNET_LANE_OK, "second bounded job queues");
    CHECK(cnet_lane_pool_submit(pool, input, 1, 1, &options, &expired) ==
          CNET_LANE_FULL, "capacity applies to running plus queued jobs");
    CHECK(cnet_lane_pool_cancel(pool, second) == CNET_LANE_OK,
          "queued job accepts cancellation");
    CHECK(cnet_lane_pool_wait(pool, second, 2000, output, 1, &result) ==
          CNET_LANE_OK && result.status == CNET_ORACLE_CANCELLED,
          "queued cancellation is explicit evidence");
    CHECK(cnet_lane_pool_wait(pool, first, 1, output, 1, &result) ==
          CNET_LANE_WAIT_TIMEOUT,
          "wait timeout does not consume a running job");
    CHECK(cnet_lane_pool_wait(pool, first, 2000, output, 1, &result) ==
          CNET_LANE_OK && result.status == CNET_ORACLE_ANSWER,
          "timed-out waiter can collect later");

    options.deadline_ns = cnet_lane_now_ns() - 1;
    CHECK(cnet_lane_pool_submit(pool, input, 1, 1, &options, &expired) ==
          CNET_LANE_OK,
          "expired work records a ticket instead of disappearing");
    CHECK(cnet_lane_pool_wait(pool, expired, 2000, output, 1, &result) ==
          CNET_LANE_OK && result.status == CNET_ORACLE_DEADLINE_EXCEEDED,
          "expired queued work never enters the backend");
    CHECK(cnet_lane_pool_wait(pool, UINT64_C(0xdeadbeef), 1, output, 1,
                              &result) == CNET_LANE_INVALID,
          "unknown ticket is refused");
    cnet_lane_pool_close(pool);
}

int main(void) {
    printf("== CNET asynchronous Oracle lane runtime ==\n");
    test_two_lanes_are_work_conserving();
    test_backpressure_cancel_deadline_and_wait_timeout();
    printf("ASYNC_RUNTIME_%s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
