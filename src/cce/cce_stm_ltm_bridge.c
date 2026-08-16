/* STM/LTM bridge implementation — C only. */
#include "../../include/cce/cce_stm_ltm_bridge.h"
#include "../../include/cce/cce_kv_page.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    pthread_t thr;
    pthread_mutex_t mu;
    pthread_cond_t cv;
} stm_ltm_thr;

static uint64_t mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void scopy(char *d, size_t cap, const char *s) {
    size_t i;
    if (!d || !cap) return;
    if (!s) {
        d[0] = 0;
        return;
    }
    for (i = 0; s[i] && i + 1 < cap; i++) d[i] = s[i];
    d[i] = 0;
}

static stm_ltm_thr *thr_of(cce_stm_ltm_bridge *B) {
    return (stm_ltm_thr *)B->thread_handle;
}

static void sleep_ms(double ms) {
    struct timespec ts;
    if (ms <= 0) return;
    ts.tv_sec = (time_t)(ms / 1000.0);
    ts.tv_nsec = (long)((ms - ts.tv_sec * 1000.0) * 1000000.0);
    if (ts.tv_nsec < 0) ts.tv_nsec = 0;
    nanosleep(&ts, NULL);
}

static int pin_has(const cce_stm_ltm_bridge *B, int pos) {
    int i;
    for (i = 0; i < B->n_pins; i++)
        if (B->pins[i] == pos) return 1;
    return 0;
}

static void process_job(cce_stm_ltm_bridge *B, cce_stm_ltm_job *j) {
    j->rehydrate_rc = 0;
    j->cert_hit = 0;
    j->cert_skill[0] = 0;

    /* CERT probe first (cheap, sync-style on worker — still off HOT) */
    if (j->query[0] && B->cert_fn) {
        if (B->cert_fn(j->query, j->cert_skill, sizeof j->cert_skill, B->cert_ud)) {
            j->cert_hit = 1;
            j->status = CCE_RECALL_CERT_HIT;
            B->n_cert_hits++;
            j->done = 1;
            j->done_ns = mono_ns();
            return;
        }
    }

    if (B->pager) {
        j->rehydrate_rc = cce_kv_pager_rehydrate_pos(B->pager, j->pos);
        j->status = (j->rehydrate_rc == 0) ? CCE_RECALL_LTM_READY : CCE_RECALL_MISS;
    } else {
        /* Simulated COLD latency for bench / no-pager mode */
        double ms = j->simulated_cold_ms > 0 ? j->simulated_cold_ms
                                             : B->simulated_cold_ms_default;
        sleep_ms(ms);
        j->rehydrate_rc = 0;
        j->status = CCE_RECALL_LTM_READY;
    }
    B->n_ltm_done++;
    j->done = 1;
    j->done_ns = mono_ns();
}

static void *worker_main(void *arg) {
    cce_stm_ltm_bridge *B = (cce_stm_ltm_bridge *)arg;
    stm_ltm_thr *T = thr_of(B);
    for (;;) {
        cce_stm_ltm_job *job = NULL;
        int i;
        pthread_mutex_lock(&T->mu);
        for (;;) {
            if (B->stop) {
                /* finish any incomplete jobs then exit */
                job = NULL;
                for (i = 0; i < CCE_STM_LTM_QMAX; i++) {
                    if (B->q[i].active && !B->q[i].done) {
                        job = &B->q[i];
                        break;
                    }
                }
                if (!job) {
                    pthread_mutex_unlock(&T->mu);
                    return NULL;
                }
                break;
            }
            job = NULL;
            for (i = 0; i < CCE_STM_LTM_QMAX; i++) {
                if (B->q[i].active && !B->q[i].done) {
                    job = &B->q[i];
                    break;
                }
            }
            if (job) break;
            pthread_cond_wait(&T->cv, &T->mu);
        }
        pthread_mutex_unlock(&T->mu);
        if (job) process_job(B, job);
    }
    return NULL;
}

void cce_stm_ltm_opts_default(cce_stm_ltm_opts *o, int legal_max) {
    if (!o) return;
    memset(o, 0, sizeof *o);
    cce_specialist_kv_budget_default(&o->budget, legal_max > 0 ? legal_max : 4096);
    o->legal_max = legal_max > 0 ? legal_max : 4096;
    o->k_slot = 128;
    o->v_slot = 128;
    o->async = 1;
    o->simulated_cold_ms = 0.05; /* 50us default sim */
    o->pager = NULL;
    o->cert_fn = NULL;
    o->cert_ud = NULL;
}

cce_result cce_stm_ltm_open(cce_stm_ltm_bridge *B, const cce_stm_ltm_opts *o) {
    cce_stm_ltm_opts def;
    stm_ltm_thr *T;
    if (!B) return CCE_ERR_INVALID_ARG;
    if (!o) {
        cce_stm_ltm_opts_default(&def, 4096);
        o = &def;
    }
    memset(B, 0, sizeof *B);
    if (cce_kv_stream_index_init(&B->stm, &o->budget, o->legal_max) != CCE_OK)
        return CCE_ERR_INVALID_ARG;
    cce_kv_stream_index_set_slot_sizes(&B->stm, o->k_slot, o->v_slot);
    B->pager = o->pager;
    B->cert_fn = o->cert_fn;
    B->cert_ud = o->cert_ud;
    B->async = o->async ? 1 : 0;
    B->simulated_cold_ms_default = o->simulated_cold_ms;

    T = (stm_ltm_thr *)calloc(1, sizeof *T);
    if (!T) return CCE_ERR_OOM;
    B->thread_handle = T;
    if (pthread_mutex_init(&T->mu, NULL) != 0) {
        free(T);
        B->thread_handle = NULL;
        return CCE_ERR_IO;
    }
    if (pthread_cond_init(&T->cv, NULL) != 0) {
        pthread_mutex_destroy(&T->mu);
        free(T);
        B->thread_handle = NULL;
        return CCE_ERR_IO;
    }
    if (B->async) {
        if (pthread_create(&T->thr, NULL, worker_main, B) != 0) {
            B->async = 0; /* fall back sync on queue drain */
        } else {
            B->worker_live = 1;
        }
    }
    return CCE_OK;
}

void cce_stm_ltm_close(cce_stm_ltm_bridge *B) {
    stm_ltm_thr *T;
    if (!B) return;
    T = thr_of(B);
    if (T) {
        pthread_mutex_lock(&T->mu);
        B->stop = 1;
        pthread_cond_broadcast(&T->cv);
        pthread_mutex_unlock(&T->mu);
        if (B->worker_live) {
            pthread_join(T->thr, NULL);
            B->worker_live = 0;
        }
        pthread_cond_destroy(&T->cv);
        pthread_mutex_destroy(&T->mu);
        free(T);
        B->thread_handle = NULL;
    }
    memset(B, 0, sizeof *B);
}

cce_result cce_stm_ltm_hot_append(cce_stm_ltm_bridge *B, int pos,
                                  const float *scores, int score_len) {
    cce_result rc;
    if (!B) return CCE_ERR_INVALID_ARG;
    rc = cce_kv_stream_index_on_append(&B->stm, pos, scores, score_len);
    if (rc == CCE_OK) B->n_hot_steps++;
    return rc;
}

cce_result cce_stm_ltm_pin(cce_stm_ltm_bridge *B, int pos) {
    if (!B || pos < 0) return CCE_ERR_INVALID_ARG;
    if (pin_has(B, pos)) return CCE_OK;
    if (B->n_pins >= CCE_STM_LTM_PIN_MAX) return CCE_ERR_OOM;
    B->pins[B->n_pins++] = pos;
    return CCE_OK;
}

cce_result cce_stm_ltm_unpin(cce_stm_ltm_bridge *B, int pos) {
    int i, j;
    if (!B) return CCE_ERR_INVALID_ARG;
    for (i = 0; i < B->n_pins; i++) {
        if (B->pins[i] == pos) {
            for (j = i; j + 1 < B->n_pins; j++) B->pins[j] = B->pins[j + 1];
            B->n_pins--;
            return CCE_OK;
        }
    }
    return CCE_OK;
}

cce_result cce_stm_ltm_stm_active(const cce_stm_ltm_bridge *B, int *out,
                                  int out_cap, int *out_n) {
    int tmp[CCE_KV_STREAM_IDX_MAX + CCE_STM_LTM_PIN_MAX];
    int n = 0, i, j, m;
    if (!B || !out || !out_n || out_cap <= 0) return CCE_ERR_INVALID_ARG;
    if (cce_kv_stream_index_active(&B->stm, tmp, CCE_KV_STREAM_IDX_MAX, &n) != CCE_OK)
        return CCE_ERR_INVALID_ARG;
    for (i = 0; i < B->n_pins && n < (int)(sizeof tmp / sizeof tmp[0]); i++) {
        int p = B->pins[i], found = 0;
        for (j = 0; j < n; j++)
            if (tmp[j] == p) {
                found = 1;
                break;
            }
        if (!found) tmp[n++] = p;
    }
    /* simple insertion sort */
    for (i = 1; i < n; i++) {
        int v = tmp[i], k = i - 1;
        while (k >= 0 && tmp[k] > v) {
            tmp[k + 1] = tmp[k];
            k--;
        }
        tmp[k + 1] = v;
    }
    m = n < out_cap ? n : out_cap;
    for (i = 0; i < m; i++) out[i] = tmp[i];
    *out_n = m;
    return CCE_OK;
}

cce_recall_status cce_stm_ltm_request_recall(cce_stm_ltm_bridge *B, int pos,
                                             const char *query_opt) {
    stm_ltm_thr *T;
    int idx;
    if (!B || pos < 0) return CCE_RECALL_MISS;

    /* STM hit: already in index or pinned — no COLD */
    if (cce_kv_stream_index_contains(&B->stm, pos) || pin_has(B, pos)) {
        B->n_stm_hits++;
        return CCE_RECALL_STM_HIT;
    }

    T = thr_of(B);
    if (!T) return CCE_RECALL_MISS;

    pthread_mutex_lock(&T->mu);
    if (B->q_count >= CCE_STM_LTM_QMAX) {
        B->n_queue_full++;
        pthread_mutex_unlock(&T->mu);
        return CCE_RECALL_BUSY;
    }
    idx = B->q_tail;
    memset(&B->q[idx], 0, sizeof B->q[idx]);
    B->q[idx].active = 1;
    B->q[idx].pos = pos;
    B->q[idx].status = CCE_RECALL_LTM_QUEUED;
    B->q[idx].enqueue_ns = mono_ns();
    B->q[idx].simulated_cold_ms = B->simulated_cold_ms_default;
    if (query_opt) scopy(B->q[idx].query, sizeof B->q[idx].query, query_opt);
    B->q_tail = (B->q_tail + 1) % CCE_STM_LTM_QMAX;
    B->q_count++;
    B->n_ltm_queued++;
    pthread_cond_signal(&T->cv);
    pthread_mutex_unlock(&T->mu);

    /* Sync fallback: process inline if no worker (still called from non-HOT if possible) */
    if (!B->worker_live) {
        process_job(B, &B->q[idx]);
        return B->q[idx].status;
    }
    return CCE_RECALL_LTM_QUEUED;
}

cce_recall_status cce_stm_ltm_cert_probe(cce_stm_ltm_bridge *B, const char *query,
                                         char *skill_out, size_t skill_cap) {
    if (!B || !query) return CCE_RECALL_MISS;
    if (!B->cert_fn) return CCE_RECALL_MISS;
    if (B->cert_fn(query, skill_out, skill_cap, B->cert_ud)) {
        B->n_cert_hits++;
        return CCE_RECALL_CERT_HIT;
    }
    return CCE_RECALL_MISS;
}

int cce_stm_ltm_poll(cce_stm_ltm_bridge *B, cce_stm_ltm_job *out, int cap,
                     int *n) {
    stm_ltm_thr *T;
    int i, c = 0;
    if (!B || !out || !n || cap <= 0) return -1;
    T = thr_of(B);
    if (!T) {
        *n = 0;
        return 0;
    }
    pthread_mutex_lock(&T->mu);
    for (i = 0; i < CCE_STM_LTM_QMAX && c < cap; i++) {
        if (B->q[i].active && B->q[i].done) {
            out[c++] = B->q[i];
            if (B->q[i].done_ns > B->q[i].enqueue_ns) {
                double ms = (double)(B->q[i].done_ns - B->q[i].enqueue_ns) / 1e6;
                B->sum_ltm_wait_ms += ms;
            }
            /* retire */
            B->q[i].active = 0;
            B->q[i].done = 0;
            if (B->q_count > 0) B->q_count--;
        }
    }
    pthread_mutex_unlock(&T->mu);
    *n = c;
    return 0;
}

void cce_stm_ltm_sync(cce_stm_ltm_bridge *B) {
    int spins = 0;
    if (!B) return;
    /* Wait until no incomplete active jobs */
    while (spins < 100000) {
        int i, pending = 0;
        stm_ltm_thr *T = thr_of(B);
        if (!T) break;
        pthread_mutex_lock(&T->mu);
        for (i = 0; i < CCE_STM_LTM_QMAX; i++)
            if (B->q[i].active && !B->q[i].done) pending++;
        pthread_mutex_unlock(&T->mu);
        if (!pending) break;
        sleep_ms(0.05);
        spins++;
    }
}

int cce_stm_ltm_format(const cce_stm_ltm_bridge *B, char *buf, size_t cap) {
    double avg = 0;
    if (!B || !buf || !cap) return -1;
    if (B->n_ltm_done)
        avg = B->sum_ltm_wait_ms / (double)B->n_ltm_done;
    return snprintf(buf, cap,
                    "stm_ltm hot_steps=%llu stm_hits=%llu ltm_q=%llu ltm_done=%llu "
                    "cert=%llu qfull=%llu pins=%d stm_active=%d mem_r=%.4f avg_ltm_ms=%.3f",
                    (unsigned long long)B->n_hot_steps,
                    (unsigned long long)B->n_stm_hits,
                    (unsigned long long)B->n_ltm_queued,
                    (unsigned long long)B->n_ltm_done,
                    (unsigned long long)B->n_cert_hits,
                    (unsigned long long)B->n_queue_full, B->n_pins, B->stm.active_n,
                    cce_kv_stream_index_mem_ratio(&B->stm), avg);
}
