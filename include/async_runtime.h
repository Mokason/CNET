#ifndef CNET_ASYNC_RUNTIME_H
#define CNET_ASYNC_RUNTIME_H

/* Work-conserving asynchronous execution over equivalent Oracle v2 lanes.
 *
 * Each lane borrows one OracleEntry and is serviced by exactly one worker
 * thread. That ownership boundary is deliberate: GPU queues, KV sessions, and
 * model scratch do not need internal locks and are never used concurrently.
 * Submitted inputs are copied; completed outputs stay pool-owned until wait or
 * try_collect copies them to the caller. Oracle entries must outlive the pool.
 */

#include <stddef.h>
#include <stdint.h>

#include "acquire.h"
#include "cnet_export.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CNET_LANE_ABI_VERSION 1u
#define CNET_LANE_NAME_MAX 64

typedef uint64_t CnetLaneTicket;
typedef struct CnetLanePool CnetLanePool;

typedef enum {
    CNET_LANE_OK = 0,
    CNET_LANE_WAIT_TIMEOUT = 1,
    CNET_LANE_NOT_READY = 2,
    CNET_LANE_INVALID = -1,
    CNET_LANE_FULL = -2,
    CNET_LANE_NOMEM = -3,
    CNET_LANE_CLOSED = -4
} CnetLaneCode;

typedef struct {
    uint32_t abi_version;
    uint32_t struct_size;
    OracleEntry *oracle;             /* borrowed; one entry per lane */
    uint64_t resource_mask;          /* bit i identifies GPU/resource i */
    uint32_t flags;                  /* reserved; must be zero */
    char name[CNET_LANE_NAME_MAX];
} CnetLaneSpec;

typedef struct {
    uint32_t abi_version;
    uint32_t struct_size;
    uint64_t deadline_ns;            /* absolute monotonic ns; 0 = none */
    uint32_t flags;                  /* reserved; must be zero */
} CnetLaneSubmitOptions;

#define CNET_LANE_RESULT_CANCEL_REQUESTED_DURING_RUN UINT32_C(1)

typedef struct {
    uint32_t abi_version;
    uint32_t struct_size;
    CnetLaneTicket ticket;
    CnetOracleStatus status;
    uint32_t lane_index;             /* UINT32_MAX when cancelled before dispatch */
    uint32_t flags;
    uint64_t submitted_ns;
    uint64_t started_ns;
    uint64_t completed_ns;
    uint64_t queue_ns;
    uint64_t execution_ns;
    CnetOracleResult oracle_result;
} CnetLaneResult;

typedef struct {
    uint32_t abi_version;
    uint32_t struct_size;
    uint32_t lane_index;
    uint32_t active;
    uint64_t resource_mask;
    uint64_t calls;
    uint64_t successes;
    uint64_t failures;
    uint64_t cancelled;
    uint64_t deadlines;
    uint64_t busy_ns;
    char name[CNET_LANE_NAME_MAX];
} CnetLaneStats;

/* Current CLOCK_MONOTONIC timestamp, suitable for deadline_ns. */
CNET_API uint64_t cnet_lane_now_ns(void);

/* All lanes must expose the same input/output element totals. queue_capacity
 * bounds running + queued + completed-but-uncollected jobs. */
CNET_API int cnet_lane_pool_open(CnetLanePool **out,
                                 const CnetLaneSpec *lanes,
                                 size_t lane_count,
                                 size_t queue_capacity);

CNET_API int cnet_lane_pool_submit(CnetLanePool *pool,
                                   const double *input,
                                   size_t input_count,
                                   size_t output_count,
                                   const CnetLaneSubmitOptions *options,
                                   CnetLaneTicket *ticket_out);

/* timeout_ms: -1 = indefinitely, 0 = nonblocking, >0 = bounded wait. A
 * successful collection consumes the ticket. output_cap must cover the whole
 * typed output even when the Oracle returned a non-answer. */
CNET_API int cnet_lane_pool_wait(CnetLanePool *pool,
                                 CnetLaneTicket ticket,
                                 int timeout_ms,
                                 double *output,
                                 size_t output_cap,
                                 CnetLaneResult *result_out);
CNET_API int cnet_lane_pool_try_collect(CnetLanePool *pool,
                                        CnetLaneTicket ticket,
                                        double *output,
                                        size_t output_cap,
                                        CnetLaneResult *result_out);

/* Queued cancellation completes immediately. Running callbacks cannot be
 * preempted safely; cancellation is recorded at their next completion boundary. */
CNET_API int cnet_lane_pool_cancel(CnetLanePool *pool,
                                   CnetLaneTicket ticket);

CNET_API int cnet_lane_pool_lane_stats(CnetLanePool *pool,
                                       size_t lane_index,
                                       CnetLaneStats *stats_out);
CNET_API size_t cnet_lane_pool_lane_count(const CnetLanePool *pool);

/* Refuses new work, drains callbacks already running/queued, joins every worker,
 * and frees uncollected job storage. It never releases borrowed Oracle entries. */
CNET_API void cnet_lane_pool_close(CnetLanePool *pool);

#ifdef __cplusplus
}
#endif

#endif /* CNET_ASYNC_RUNTIME_H */
