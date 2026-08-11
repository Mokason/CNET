/* STM/LTM bridge — HOT never blocks on COLD.
 *
 * STM  = HOT/WARM working set (stream index pins + recent)
 * LTM  = COLD rehydrate queue + optional CERT local hit
 * Async worker core interacts with HOT producer without stalling decode.
 *
 * Pure C. Law: COLD/KV recall ≠ CERT; only explicit cert_fn may admit skill.
 */
#ifndef CCE_STM_LTM_BRIDGE_H
#define CCE_STM_LTM_BRIDGE_H

#include "cce_defs.h"
#include "cce_sparse_kv.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct cce_kv_pager; /* optional */

#define CCE_STM_LTM_QMAX 64
#define CCE_STM_LTM_PIN_MAX 64
#define CCE_STM_LTM_CERT 128

typedef enum {
    CCE_RECALL_NONE = 0,
    CCE_RECALL_STM_HIT = 1,   /* already in stream index / pin */
    CCE_RECALL_LTM_QUEUED = 2,
    CCE_RECALL_LTM_READY = 3,
    CCE_RECALL_CERT_HIT = 4,
    CCE_RECALL_MISS = 5,
    CCE_RECALL_BUSY = 6       /* queue full; HOT still ok */
} cce_recall_status;

typedef struct cce_stm_ltm_job {
    int active;
    int done;
    int pos;           /* cold position or page anchor */
    int page_hint;
    char query[CCE_STM_LTM_CERT];
    cce_recall_status status;
    int rehydrate_rc;  /* from pager if any */
    char cert_skill[64];
    int cert_hit;
    uint64_t enqueue_ns;
    uint64_t done_ns;
    double simulated_cold_ms; /* bench hook when no pager */
} cce_stm_ltm_job;

/* Optional CERT probe: return 1 and fill skill_out if LOCAL hit. */
typedef int (*cce_stm_ltm_cert_fn)(const char *query, char *skill_out,
                                   size_t skill_cap, void *ud);

typedef struct cce_stm_ltm_bridge {
    cce_kv_stream_index stm; /* HOT attention budget index */
    struct cce_kv_pager *pager; /* optional COLD backend */
    cce_stm_ltm_cert_fn cert_fn;
    void *cert_ud;

    int pins[CCE_STM_LTM_PIN_MAX];
    int n_pins;

    cce_stm_ltm_job q[CCE_STM_LTM_QMAX];
    int q_head, q_tail, q_count;

    /* async */
    int async;
    int stop;
    int worker_live;
    void *thread_handle; /* pthread_t stored opaquely size-wise in .c */

    /* stats */
    uint64_t n_stm_hits;
    uint64_t n_ltm_queued;
    uint64_t n_ltm_done;
    uint64_t n_cert_hits;
    uint64_t n_hot_steps;
    uint64_t n_queue_full;
    double sum_ltm_wait_ms;
    double simulated_cold_ms_default;
} cce_stm_ltm_bridge;

typedef struct cce_stm_ltm_opts {
    cce_specialist_kv_budget budget;
    int legal_max;
    int k_slot;
    int v_slot;
    int async; /* 1 = worker thread */
    double simulated_cold_ms; /* used if pager NULL */
    struct cce_kv_pager *pager;
    cce_stm_ltm_cert_fn cert_fn;
    void *cert_ud;
} cce_stm_ltm_opts;

void cce_stm_ltm_opts_default(cce_stm_ltm_opts *o, int legal_max);

cce_result cce_stm_ltm_open(cce_stm_ltm_bridge *B, const cce_stm_ltm_opts *o);
void cce_stm_ltm_close(cce_stm_ltm_bridge *B);

/* --- STM (HOT path — never blocks on COLD I/O) ------------------------- */

/* Streaming decode step: append token pos into STM index. scores optional. */
cce_result cce_stm_ltm_hot_append(cce_stm_ltm_bridge *B, int pos,
                                  const float *scores, int score_len);

/* Pin positions that must stay in STM working set (short-term). */
cce_result cce_stm_ltm_pin(cce_stm_ltm_bridge *B, int pos);
cce_result cce_stm_ltm_unpin(cce_stm_ltm_bridge *B, int pos);

/* Active STM indices (stream index ∪ pins), sorted unique into out. */
cce_result cce_stm_ltm_stm_active(const cce_stm_ltm_bridge *B, int *out,
                                  int out_cap, int *out_n);

/* --- LTM (COLD path — async) ------------------------------------------- */

/* Non-blocking: if pos already STM → STM_HIT; else queue COLD recall.
 * Returns immediately. HOT decode must not wait on this. */
cce_recall_status cce_stm_ltm_request_recall(cce_stm_ltm_bridge *B, int pos,
                                             const char *query_opt);

/* Optional CERT-only fast path (sync, local skill — still not COLD I/O). */
cce_recall_status cce_stm_ltm_cert_probe(cce_stm_ltm_bridge *B,
                                         const char *query, char *skill_out,
                                         size_t skill_cap);

/* Poll completed LTM jobs into out[0..*n). Non-blocking. */
int cce_stm_ltm_poll(cce_stm_ltm_bridge *B, cce_stm_ltm_job *out, int cap,
                     int *n);

/* Drain async (test/bench). */
void cce_stm_ltm_sync(cce_stm_ltm_bridge *B);

/* Stats one-liner. */
int cce_stm_ltm_format(const cce_stm_ltm_bridge *B, char *buf, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* CCE_STM_LTM_BRIDGE_H */
