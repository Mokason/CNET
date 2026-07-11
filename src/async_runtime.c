#define _POSIX_C_SOURCE 200809L

#include "../include/async_runtime.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef enum {
    JOB_FREE = 0,
    JOB_QUEUED,
    JOB_RUNNING,
    JOB_DONE
} JobState;

typedef struct {
    JobState state;
    CnetLaneTicket ticket;
    double *input;
    double *output;
    size_t input_count;
    size_t output_count;
    uint64_t deadline_ns;
    int cancel_requested;
    CnetLaneResult result;
} LaneJob;

typedef struct {
    struct CnetLanePool *pool;
    size_t lane_index;
} WorkerArg;

typedef struct {
    CnetLaneSpec spec;
    CnetLaneStats stats;
} LaneState;

struct CnetLanePool {
    pthread_mutex_t mutex;
    pthread_cond_t work_ready;
    pthread_cond_t job_done;
    int shutting_down;

    LaneState *lanes;
    pthread_t *threads;
    WorkerArg *worker_args;
    size_t lane_count;
    size_t threads_started;

    LaneJob *jobs;
    size_t job_capacity;
    size_t outstanding;
    size_t *queue;
    size_t queue_head;
    size_t queue_tail;
    size_t queue_count;
    CnetLaneTicket next_ticket;

    size_t input_count;
    size_t output_count;
};

uint64_t cnet_lane_now_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

static size_t port_total(Port port) {
    if (port.field_width == 0 || port.field_count == 0 ||
        port.field_width > SIZE_MAX / port.field_count) return 0;
    return port.field_width * port.field_count;
}

static int same_port(Port a, Port b) {
    return a.family == b.family &&
           a.field_width == b.field_width &&
           a.field_count == b.field_count &&
           strncmp(a.tag, b.tag, PORT_TAG_MAX) == 0;
}

static void queue_push(CnetLanePool *pool, size_t slot) {
    pool->queue[pool->queue_tail] = slot;
    pool->queue_tail = (pool->queue_tail + 1) % pool->job_capacity;
    pool->queue_count++;
}

static size_t queue_pop(CnetLanePool *pool) {
    size_t slot = pool->queue[pool->queue_head];
    pool->queue_head = (pool->queue_head + 1) % pool->job_capacity;
    pool->queue_count--;
    return slot;
}

static int queue_remove(CnetLanePool *pool, size_t wanted) {
    size_t count = pool->queue_count;
    int found = 0;
    for (size_t i = 0; i < count; ++i) {
        size_t slot = queue_pop(pool);
        if (!found && slot == wanted) found = 1;
        else queue_push(pool, slot);
    }
    return found;
}

static LaneJob *find_job(CnetLanePool *pool, CnetLaneTicket ticket,
                         size_t *slot_out) {
    if (!pool || ticket == 0) return NULL;
    for (size_t i = 0; i < pool->job_capacity; ++i) {
        if (pool->jobs[i].state != JOB_FREE &&
            pool->jobs[i].ticket == ticket) {
            if (slot_out) *slot_out = i;
            return &pool->jobs[i];
        }
    }
    return NULL;
}

static void result_init(CnetLaneResult *result, CnetLaneTicket ticket,
                        uint64_t submitted_ns) {
    memset(result, 0, sizeof *result);
    result->abi_version = CNET_LANE_ABI_VERSION;
    result->struct_size = (uint32_t)sizeof *result;
    result->ticket = ticket;
    result->lane_index = UINT32_MAX;
    result->submitted_ns = submitted_ns;
}

static void oracle_result_status(CnetOracleResult *result,
                                 CnetOracleStatus status,
                                 const OracleEntry *entry) {
    memset(result, 0, sizeof *result);
    result->abi_version = CNET_ORACLE_ABI_VERSION;
    result->struct_size = (uint32_t)sizeof *result;
    result->status = status;
    if (entry) result->oracle_identity_digest = entry->behavior_digest;
}

static void complete_without_backend(CnetLanePool *pool, LaneJob *job,
                                     CnetOracleStatus status,
                                     uint32_t lane_index) {
    uint64_t now = cnet_lane_now_ns();
    job->result.status = status;
    job->result.lane_index = lane_index;
    job->result.completed_ns = now;
    job->result.queue_ns = now >= job->result.submitted_ns
                           ? now - job->result.submitted_ns : 0;
    oracle_result_status(&job->result.oracle_result, status,
        lane_index == UINT32_MAX ? NULL :
        pool->lanes[lane_index].spec.oracle);
    job->state = JOB_DONE;
    pthread_cond_broadcast(&pool->job_done);
}

static void *lane_worker(void *opaque) {
    WorkerArg *arg = (WorkerArg *)opaque;
    CnetLanePool *pool = arg->pool;
    uint32_t lane_index = (uint32_t)arg->lane_index;
    LaneState *lane = &pool->lanes[arg->lane_index];

    for (;;) {
        LaneJob *job;
        size_t slot;
        uint64_t started, completed;
        CnetOracleResult oracle_result;
        CnetOracleStatus status;

        pthread_mutex_lock(&pool->mutex);
        while (pool->queue_count == 0 && !pool->shutting_down)
            pthread_cond_wait(&pool->work_ready, &pool->mutex);
        if (pool->queue_count == 0 && pool->shutting_down) {
            pthread_mutex_unlock(&pool->mutex);
            break;
        }
        slot = queue_pop(pool);
        job = &pool->jobs[slot];
        if (job->state != JOB_QUEUED) {
            pthread_mutex_unlock(&pool->mutex);
            continue;
        }
        if (job->cancel_requested) {
            lane->stats.cancelled++;
            complete_without_backend(pool, job, CNET_ORACLE_CANCELLED,
                                     lane_index);
            pthread_mutex_unlock(&pool->mutex);
            continue;
        }
        started = cnet_lane_now_ns();
        if (job->deadline_ns != 0 && started >= job->deadline_ns) {
            lane->stats.deadlines++;
            complete_without_backend(pool, job,
                                     CNET_ORACLE_DEADLINE_EXCEEDED,
                                     lane_index);
            pthread_mutex_unlock(&pool->mutex);
            continue;
        }
        job->state = JOB_RUNNING;
        job->result.lane_index = lane_index;
        job->result.started_ns = started;
        job->result.queue_ns = started >= job->result.submitted_ns
                               ? started - job->result.submitted_ns : 0;
        lane->stats.calls++;
        lane->stats.active = 1;
        pthread_mutex_unlock(&pool->mutex);

        status = cnet_oracle_invoke(lane->spec.oracle,
                                    job->input, job->input_count,
                                    job->output, job->output_count,
                                    &oracle_result);
        completed = cnet_lane_now_ns();

        pthread_mutex_lock(&pool->mutex);
        lane->stats.active = 0;
        lane->stats.busy_ns += completed >= started ? completed - started : 0;
        if (job->cancel_requested) {
            status = CNET_ORACLE_CANCELLED;
            oracle_result.status = status;
            job->result.flags |= CNET_LANE_RESULT_CANCEL_REQUESTED_DURING_RUN;
            lane->stats.cancelled++;
        } else if (job->deadline_ns != 0 && completed > job->deadline_ns) {
            status = CNET_ORACLE_DEADLINE_EXCEEDED;
            oracle_result.status = status;
            lane->stats.deadlines++;
        }
        if (status == CNET_ORACLE_ANSWER) lane->stats.successes++;
        else lane->stats.failures++;
        job->result.status = status;
        job->result.completed_ns = completed;
        job->result.execution_ns = completed >= started ? completed - started : 0;
        job->result.oracle_result = oracle_result;
        job->state = JOB_DONE;
        pthread_cond_broadcast(&pool->job_done);
        pthread_mutex_unlock(&pool->mutex);
    }
    return NULL;
}

static void pool_destroy_storage(CnetLanePool *pool) {
    if (!pool) return;
    if (pool->jobs) {
        for (size_t i = 0; i < pool->job_capacity; ++i) {
            free(pool->jobs[i].input);
            free(pool->jobs[i].output);
        }
    }
    free(pool->queue);
    free(pool->jobs);
    free(pool->worker_args);
    free(pool->threads);
    free(pool->lanes);
    pthread_cond_destroy(&pool->job_done);
    pthread_cond_destroy(&pool->work_ready);
    pthread_mutex_destroy(&pool->mutex);
    free(pool);
}

int cnet_lane_pool_open(CnetLanePool **out,
                        const CnetLaneSpec *lanes,
                        size_t lane_count,
                        size_t queue_capacity) {
    CnetLanePool *pool;
    size_t input_count, output_count;
    if (!out || !lanes || lane_count == 0 || lane_count > UINT32_MAX ||
        queue_capacity == 0) return CNET_LANE_INVALID;
    *out = NULL;
    input_count = port_total(lanes[0].oracle ?
                             lanes[0].oracle->input_port : (Port){0});
    output_count = port_total(lanes[0].oracle ?
                              lanes[0].oracle->output_port : (Port){0});
    if (input_count == 0 || output_count == 0) return CNET_LANE_INVALID;
    for (size_t i = 0; i < lane_count; ++i) {
        if (lanes[i].abi_version != CNET_LANE_ABI_VERSION ||
            lanes[i].struct_size < sizeof lanes[i] || lanes[i].flags != 0 ||
            !lanes[i].oracle ||
            (!lanes[i].oracle->fn && !lanes[i].oracle->fn_v2) ||
            !same_port(lanes[i].oracle->input_port,
                       lanes[0].oracle->input_port) ||
            !same_port(lanes[i].oracle->output_port,
                       lanes[0].oracle->output_port)) return CNET_LANE_INVALID;
        for (size_t j = 0; j < i; ++j)
            if (lanes[i].oracle == lanes[j].oracle) return CNET_LANE_INVALID;
    }

    pool = (CnetLanePool *)calloc(1, sizeof *pool);
    if (!pool) return CNET_LANE_NOMEM;
    if (pthread_mutex_init(&pool->mutex, NULL) != 0 ||
        pthread_cond_init(&pool->work_ready, NULL) != 0 ||
        pthread_cond_init(&pool->job_done, NULL) != 0) {
        free(pool);
        return CNET_LANE_NOMEM;
    }
    pool->lanes = (LaneState *)calloc(lane_count, sizeof *pool->lanes);
    pool->threads = (pthread_t *)calloc(lane_count, sizeof *pool->threads);
    pool->worker_args = (WorkerArg *)calloc(lane_count,
                                            sizeof *pool->worker_args);
    pool->jobs = (LaneJob *)calloc(queue_capacity, sizeof *pool->jobs);
    pool->queue = (size_t *)calloc(queue_capacity, sizeof *pool->queue);
    if (!pool->lanes || !pool->threads || !pool->worker_args ||
        !pool->jobs || !pool->queue) {
        pool_destroy_storage(pool);
        return CNET_LANE_NOMEM;
    }
    pool->lane_count = lane_count;
    pool->job_capacity = queue_capacity;
    pool->input_count = input_count;
    pool->output_count = output_count;
    pool->next_ticket = 1;

    for (size_t i = 0; i < lane_count; ++i) {
        pool->lanes[i].spec = lanes[i];
        pool->lanes[i].stats.abi_version = CNET_LANE_ABI_VERSION;
        pool->lanes[i].stats.struct_size =
            (uint32_t)sizeof pool->lanes[i].stats;
        pool->lanes[i].stats.lane_index = (uint32_t)i;
        pool->lanes[i].stats.resource_mask = lanes[i].resource_mask;
        snprintf(pool->lanes[i].stats.name,
                 sizeof pool->lanes[i].stats.name, "%s", lanes[i].name);
        pool->worker_args[i].pool = pool;
        pool->worker_args[i].lane_index = i;
        if (pthread_create(&pool->threads[i], NULL, lane_worker,
                           &pool->worker_args[i]) != 0) {
            pthread_mutex_lock(&pool->mutex);
            pool->shutting_down = 1;
            pthread_cond_broadcast(&pool->work_ready);
            pthread_mutex_unlock(&pool->mutex);
            for (size_t j = 0; j < pool->threads_started; ++j)
                pthread_join(pool->threads[j], NULL);
            pool_destroy_storage(pool);
            return CNET_LANE_NOMEM;
        }
        pool->threads_started++;
    }
    *out = pool;
    return CNET_LANE_OK;
}

int cnet_lane_pool_submit(CnetLanePool *pool,
                          const double *input,
                          size_t input_count,
                          size_t output_count,
                          const CnetLaneSubmitOptions *options,
                          CnetLaneTicket *ticket_out) {
    double *input_copy, *output_store;
    CnetLaneSubmitOptions defaults;
    size_t slot = SIZE_MAX;
    uint64_t submitted;
    if (!pool || !input || !ticket_out ||
        input_count != pool->input_count || output_count != pool->output_count)
        return CNET_LANE_INVALID;
    memset(&defaults, 0, sizeof defaults);
    defaults.abi_version = CNET_LANE_ABI_VERSION;
    defaults.struct_size = (uint32_t)sizeof defaults;
    if (!options) options = &defaults;
    if (options->abi_version != CNET_LANE_ABI_VERSION ||
        options->struct_size < sizeof *options || options->flags != 0)
        return CNET_LANE_INVALID;

    input_copy = (double *)malloc(input_count * sizeof *input_copy);
    output_store = (double *)calloc(output_count, sizeof *output_store);
    if (!input_copy || !output_store) {
        free(input_copy);
        free(output_store);
        return CNET_LANE_NOMEM;
    }
    memcpy(input_copy, input, input_count * sizeof *input_copy);
    submitted = cnet_lane_now_ns();

    pthread_mutex_lock(&pool->mutex);
    if (pool->shutting_down) {
        pthread_mutex_unlock(&pool->mutex);
        free(input_copy);
        free(output_store);
        return CNET_LANE_CLOSED;
    }
    if (pool->outstanding >= pool->job_capacity) {
        pthread_mutex_unlock(&pool->mutex);
        free(input_copy);
        free(output_store);
        return CNET_LANE_FULL;
    }
    for (size_t i = 0; i < pool->job_capacity; ++i)
        if (pool->jobs[i].state == JOB_FREE) { slot = i; break; }
    if (slot == SIZE_MAX) {
        pthread_mutex_unlock(&pool->mutex);
        free(input_copy);
        free(output_store);
        return CNET_LANE_FULL;
    }

    LaneJob *job = &pool->jobs[slot];
    memset(job, 0, sizeof *job);
    job->state = JOB_QUEUED;
    job->ticket = pool->next_ticket++;
    if (job->ticket == 0) job->ticket = pool->next_ticket++;
    job->input = input_copy;
    job->output = output_store;
    job->input_count = input_count;
    job->output_count = output_count;
    job->deadline_ns = options->deadline_ns;
    result_init(&job->result, job->ticket, submitted);
    queue_push(pool, slot);
    pool->outstanding++;
    *ticket_out = job->ticket;
    pthread_cond_signal(&pool->work_ready);
    pthread_mutex_unlock(&pool->mutex);
    return CNET_LANE_OK;
}

static void realtime_deadline_after_ms(struct timespec *deadline,
                                       int timeout_ms) {
    clock_gettime(CLOCK_REALTIME, deadline);
    deadline->tv_sec += timeout_ms / 1000;
    deadline->tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (deadline->tv_nsec >= 1000000000L) {
        deadline->tv_sec++;
        deadline->tv_nsec -= 1000000000L;
    }
}

int cnet_lane_pool_wait(CnetLanePool *pool,
                        CnetLaneTicket ticket,
                        int timeout_ms,
                        double *output,
                        size_t output_cap,
                        CnetLaneResult *result_out) {
    LaneJob *job;
    struct timespec deadline;
    int wait_rc = 0;
    if (!pool || !output || !result_out || timeout_ms < -1)
        return CNET_LANE_INVALID;
    if (timeout_ms > 0) realtime_deadline_after_ms(&deadline, timeout_ms);

    pthread_mutex_lock(&pool->mutex);
    job = find_job(pool, ticket, NULL);
    if (!job || output_cap < job->output_count) {
        pthread_mutex_unlock(&pool->mutex);
        return CNET_LANE_INVALID;
    }
    while (job->state != JOB_DONE) {
        if (timeout_ms == 0) {
            pthread_mutex_unlock(&pool->mutex);
            return CNET_LANE_NOT_READY;
        }
        if (timeout_ms < 0)
            wait_rc = pthread_cond_wait(&pool->job_done, &pool->mutex);
        else
            wait_rc = pthread_cond_timedwait(&pool->job_done, &pool->mutex,
                                             &deadline);
        if (wait_rc == ETIMEDOUT) {
            pthread_mutex_unlock(&pool->mutex);
            return CNET_LANE_WAIT_TIMEOUT;
        }
        if (wait_rc != 0) {
            pthread_mutex_unlock(&pool->mutex);
            return CNET_LANE_INVALID;
        }
        job = find_job(pool, ticket, NULL);
        if (!job) {
            pthread_mutex_unlock(&pool->mutex);
            return CNET_LANE_INVALID;
        }
    }

    memcpy(output, job->output, job->output_count * sizeof *output);
    *result_out = job->result;
    free(job->input);
    free(job->output);
    memset(job, 0, sizeof *job);
    if (pool->outstanding > 0) pool->outstanding--;
    pthread_mutex_unlock(&pool->mutex);
    return CNET_LANE_OK;
}

int cnet_lane_pool_try_collect(CnetLanePool *pool,
                               CnetLaneTicket ticket,
                               double *output,
                               size_t output_cap,
                               CnetLaneResult *result_out) {
    return cnet_lane_pool_wait(pool, ticket, 0, output, output_cap, result_out);
}

int cnet_lane_pool_cancel(CnetLanePool *pool, CnetLaneTicket ticket) {
    LaneJob *job;
    size_t slot;
    if (!pool) return CNET_LANE_INVALID;
    pthread_mutex_lock(&pool->mutex);
    job = find_job(pool, ticket, &slot);
    if (!job) {
        pthread_mutex_unlock(&pool->mutex);
        return CNET_LANE_INVALID;
    }
    if (job->state == JOB_QUEUED) {
        (void)queue_remove(pool, slot);
        job->cancel_requested = 1;
        complete_without_backend(pool, job, CNET_ORACLE_CANCELLED,
                                 UINT32_MAX);
    } else if (job->state == JOB_RUNNING) {
        job->cancel_requested = 1;
    }
    pthread_mutex_unlock(&pool->mutex);
    return CNET_LANE_OK;
}

int cnet_lane_pool_lane_stats(CnetLanePool *pool,
                              size_t lane_index,
                              CnetLaneStats *stats_out) {
    if (!pool || !stats_out || lane_index >= pool->lane_count)
        return CNET_LANE_INVALID;
    pthread_mutex_lock(&pool->mutex);
    *stats_out = pool->lanes[lane_index].stats;
    pthread_mutex_unlock(&pool->mutex);
    return CNET_LANE_OK;
}

size_t cnet_lane_pool_lane_count(const CnetLanePool *pool) {
    return pool ? pool->lane_count : 0;
}

void cnet_lane_pool_close(CnetLanePool *pool) {
    if (!pool) return;
    pthread_mutex_lock(&pool->mutex);
    pool->shutting_down = 1;
    pthread_cond_broadcast(&pool->work_ready);
    pthread_mutex_unlock(&pool->mutex);
    for (size_t i = 0; i < pool->threads_started; ++i)
        pthread_join(pool->threads[i], NULL);
    pool_destroy_storage(pool);
}
