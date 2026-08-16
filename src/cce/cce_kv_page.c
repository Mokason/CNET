#include "../../include/cce/cce_kv_page.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "../../include/cnet_platform.h"  /* cnet_mkdir, cnet_fsync */

#define CCE_KV_QMAX 16

typedef struct {
    int page_id;
    int pos_lo, pos_hi;
    float *k; /* owned f32 copy for async (pre-quant) */
    float *v;
    size_t k_bytes, v_bytes;
    uint64_t digest;
    int quant; /* 1: write as int8 cold */
    int valid;
} cce_kv_flush_job;

struct cce_kv_pager {
    int page_len;
    int n_hot;
    int k_slot, v_slot;
    int legal_max;
    int async;
    int quant_cold;
    int rehydrate;
    char archive_dir[320];
    char ledger_path[384];

    int window_start;
    int cur_pos;
    int capacity;
    float *k_ring;
    float *v_ring;

    int *slot_page_id;
    int next_page_id;

    pthread_t thr;
    pthread_mutex_t mu;
    pthread_rwlock_t ring_rw;
    pthread_cond_t cv_jobs;
    pthread_cond_t cv_space;
    int thr_started;
    int stop;
    cce_kv_flush_job q[CCE_KV_QMAX];
    int q_head, q_tail, q_count;

    float *stage_k;
    float *stage_v;
    int stage_busy;

    /* Single-page rehydrate cache (COLD → float rows) */
    float *rehyd_k;
    float *rehyd_v;
    int rehyd_lo, rehyd_hi; /* half-open; -1 empty */
    int rehyd_page_id;

    int pages_flushed;
    int pages_reused;
    int pages_rehydrated;
    int writes_inflight;
    int write_failures;
    FILE *ledger;

    /* Telemetry for the ring_rw read leases below. */
    int reader_count;

    /* Neural KV validity epoch (MTK / weight cartridge). */
    uint64_t weight_epoch;
    uint64_t epoch_bumps;
    int cold_epoch_rejects;
};

static uint64_t fnv1a_buf(const void *data, size_t n) {
    const unsigned char *p = (const unsigned char *)data;
    uint64_t h = 14695981039346656037ULL;
    size_t i;
    for (i = 0; i < n; ++i) {
        h ^= (uint64_t)p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static int ledger_epoch_for_page(cce_kv_pager *p, int page_id, uint64_t *out_ep);

static int mkdir_p(const char *dir) {
    char tmp[320];
    size_t len, i;
    if (!dir || !dir[0]) return -1;
    snprintf(tmp, sizeof tmp, "%s", dir);
    len = strlen(tmp);
    if (len == 0) return -1;
    if (tmp[len - 1] == '/') tmp[len - 1] = 0;
    for (i = 1; tmp[i]; ++i) {
        if (tmp[i] == '/') {
            tmp[i] = 0;
            if (cnet_mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
            tmp[i] = '/';
        }
    }
    if (cnet_mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

int cce_ctx_legal_max(int model_ctx) {
    const char *page = getenv("CNET_KV_PAGE");
    const char *e = getenv("CNET_CTX_LEGAL");
    int legal;
    /* Paged: CNET opens 1M positions. Dense: model ctx (ops still cap 8192). */
    if (page && page[0] == '1')
        legal = CNET_CTX_LEGAL_MAX;
    else
        legal = model_ctx > 0 ? model_ctx : 8192;
    if (e && e[0]) {
        int v = atoi(e);
        if (v >= 8 && v <= CNET_CTX_LEGAL_MAX) legal = v;
    }
    if (legal > CNET_CTX_LEGAL_MAX) legal = CNET_CTX_LEGAL_MAX;
    if (legal < 8) legal = 8;
    return legal;
}

void cce_kv_pager_opts_default(cce_kv_pager_opts *o, int k_slot, int v_slot,
                               int legal_max) {
    const char *e;
    if (!o) return;
    memset(o, 0, sizeof *o);
    o->page_len = 256;
    o->n_hot = 4;
    o->k_slot = k_slot;
    o->v_slot = v_slot;
    o->legal_max = legal_max > 0 ? legal_max : CNET_CTX_LEGAL_MAX;
    if (o->legal_max > CNET_CTX_LEGAL_MAX) o->legal_max = CNET_CTX_LEGAL_MAX;
    o->archive_dir = NULL;
    o->async = 1;
    o->quant_cold = 1; /* default int8 COLD */
    o->rehydrate = 0;
    e = getenv("CNET_KV_PAGE_LEN");
    if (e && e[0]) {
        int v = atoi(e);
        if (v >= 16 && v <= 8192) o->page_len = v;
    }
    e = getenv("CNET_KV_HOT_PAGES");
    if (e && e[0]) {
        int v = atoi(e);
        if (v >= 2 && v <= 64) o->n_hot = v;
    }
    e = getenv("CNET_KV_ARCHIVE");
    if (e && e[0]) o->archive_dir = e;
    e = getenv("CNET_KV_ASYNC");
    if (e && e[0] == '0') o->async = 0;
    e = getenv("CNET_KV_QUANT");
    if (e && e[0] == '0') o->quant_cold = 0;
    if (e && e[0] == '1') o->quant_cold = 1;
    e = getenv("CNET_KV_REHYDRATE");
    if (e && e[0] == '1') o->rehydrate = 1;
}

static void quant_page_maxabs(const float *src, size_t n, int8_t *dst,
                              float *scale_out) {
    size_t i;
    float amax = 0.f, s;
    for (i = 0; i < n; ++i) {
        float a = src[i] < 0 ? -src[i] : src[i];
        if (a > amax) amax = a;
    }
    if (amax < 1e-12f) {
        *scale_out = 1.f;
        memset(dst, 0, n);
        return;
    }
    s = amax / 127.f;
    *scale_out = s;
    for (i = 0; i < n; ++i) {
        int v = (int)lrintf(src[i] / s);
        if (v > 127) v = 127;
        if (v < -127) v = -127;
        dst[i] = (int8_t)v;
    }
}

static void dequant_page(const int8_t *src, size_t n, float scale, float *dst) {
    size_t i;
    for (i = 0; i < n; ++i) dst[i] = (float)src[i] * scale;
}

static int write_exact(FILE *f, const void *buf, size_t size, size_t count) {
    if (count == 0) return 0;
    return fwrite(buf, size, count, f) == count ? 0 : -1;
}

/* Write cold file: ver=2 supports quant. Returns 0 only after the complete
 * body is visible and flushed; partial files are removed fail-closed. */
static int write_cold_file(cce_kv_pager *p, cce_kv_flush_job *job) {
    char path[400];
    FILE *f;
    uint32_t magic = 0x43564B32u; /* CVK2 */
    uint32_t ver = 2;
    int quant = job->quant ? 1 : 0;
    size_t npos, nk, nv;
    int ok = 0;

    snprintf(path, sizeof path, "%s/page_%06d.kvc", p->archive_dir,
             job->page_id);
    f = fopen(path, "wb");
    if (!f) return -1;
    npos = (size_t)(job->pos_hi - job->pos_lo);
    nk = npos * (size_t)p->k_slot;
    nv = npos * (size_t)p->v_slot;
    if (write_exact(f, &magic, 4, 1) || write_exact(f, &ver, 4, 1) ||
        write_exact(f, &job->page_id, 4, 1) ||
        write_exact(f, &job->pos_lo, 4, 1) ||
        write_exact(f, &job->pos_hi, 4, 1) ||
        write_exact(f, &p->k_slot, 4, 1) ||
        write_exact(f, &p->v_slot, 4, 1) ||
        write_exact(f, &quant, 4, 1))
        ok = -1;

    if (ok == 0 && quant) {
        int8_t *kq = (int8_t *)malloc(nk ? nk : 1);
        int8_t *vq = (int8_t *)malloc(nv ? nv : 1);
        float sk = 1.f, sv = 1.f;
        uint64_t dig;
        if (!kq || !vq) {
            free(kq);
            free(vq);
            ok = -1;
        } else {
            quant_page_maxabs(job->k, nk, kq, &sk);
            quant_page_maxabs(job->v, nv, vq, &sv);
            dig = fnv1a_buf(kq, nk) ^ (fnv1a_buf(vq, nv) << 1);
            dig ^= (uint64_t)(sk * 1e6f) ^ ((uint64_t)(sv * 1e6f) << 32);
            job->digest = dig;
            if (write_exact(f, &dig, 8, 1) || write_exact(f, &sk, 4, 1) ||
                write_exact(f, &sv, 4, 1) || write_exact(f, kq, 1, nk) ||
                write_exact(f, vq, 1, nv))
                ok = -1;
            free(kq);
            free(vq);
        }
    } else if (ok == 0) {
        if (write_exact(f, &job->digest, 8, 1) ||
            write_exact(f, job->k, 1, job->k_bytes) ||
            write_exact(f, job->v, 1, job->v_bytes))
            ok = -1;
    }
    if (ok == 0 && fflush(f) != 0) ok = -1;
    if (ok == 0 && cnet_fsync(fileno(f)) != 0) ok = -1;
    if (fclose(f) != 0) ok = -1;
    if (ok != 0) (void)remove(path);
    return ok;
}

static int record_cold_file(cce_kv_pager *p, const cce_kv_flush_job *job) {
    if (!p->ledger ||
        fprintf(p->ledger,
                "page_id=%d pos_lo=%d pos_hi=%d digest=0x%016llx "
                "file=page_%06d.kvc tier=COLD quant=%d weight_epoch=%llu\n",
                job->page_id, job->pos_lo, job->pos_hi,
                (unsigned long long)job->digest, job->page_id, job->quant,
                (unsigned long long)p->weight_epoch) < 0 ||
        fflush(p->ledger) != 0)
        return -1;
    return 0;
}

static void *kv_flush_thread(void *arg) {
    cce_kv_pager *p = (cce_kv_pager *)arg;
    for (;;) {
        cce_kv_flush_job job;
        memset(&job, 0, sizeof job);
        pthread_mutex_lock(&p->mu);
        while (!p->stop && p->q_count == 0)
            pthread_cond_wait(&p->cv_jobs, &p->mu);
        if (p->stop && p->q_count == 0) {
            pthread_mutex_unlock(&p->mu);
            break;
        }
        job = p->q[p->q_head];
        p->q[p->q_head].valid = 0;
        p->q_head = (p->q_head + 1) % CCE_KV_QMAX;
        p->q_count--;
        p->writes_inflight++;
        pthread_cond_broadcast(&p->cv_space);
        pthread_mutex_unlock(&p->mu);

        {
            int ok = (!job.valid || !job.k || !job.v) ? -1 : 0;
            int attempt;
            if (ok == 0) {
                for (attempt = 0; attempt < 3; ++attempt) {
                    ok = write_cold_file(p, &job);
                    if (ok == 0) break;
                    if (attempt < 2)
                        usleep((useconds_t)(10000u << attempt));
                }
            }
            pthread_mutex_lock(&p->mu);
            if (ok == 0) ok = record_cold_file(p, &job);
            if (ok == 0)
                p->pages_flushed++;
            else
                p->write_failures++;
            p->writes_inflight--;
            pthread_cond_broadcast(&p->cv_space);
            pthread_mutex_unlock(&p->mu);
        }
        free(job.k);
        free(job.v);
    }
    return NULL;
}

cce_result cce_kv_pager_open(cce_kv_pager **out, const cce_kv_pager_opts *opts) {
    cce_kv_pager *p;
    cce_kv_pager_opts o;
    size_t ring_k, ring_v;
    int i;
    if (!out || !opts) return CCE_ERR_INVALID_ARG;
    *out = NULL;
    o = *opts;
    if (o.page_len < 16 || o.n_hot < 2 || o.k_slot < 1 || o.v_slot < 1)
        return CCE_ERR_INVALID_ARG;
    if ((size_t)o.page_len > (size_t)INT_MAX / (size_t)o.n_hot)
        return CCE_ERR_INVALID_ARG;
    {
        size_t capacity = (size_t)o.page_len * (size_t)o.n_hot;
        if (capacity > SIZE_MAX / (size_t)o.k_slot / sizeof(float) ||
            capacity > SIZE_MAX / (size_t)o.v_slot / sizeof(float))
            return CCE_ERR_INVALID_ARG;
    }
    if (o.archive_dir && strlen(o.archive_dir) >= sizeof p->archive_dir)
        return CCE_ERR_INVALID_ARG;
    p = (cce_kv_pager *)calloc(1, sizeof *p);
    if (!p) return CCE_ERR_OOM;
    if (pthread_mutex_init(&p->mu, NULL) != 0) { free(p); return CCE_ERR_IO; }
    if (pthread_cond_init(&p->cv_jobs, NULL) != 0) {
        pthread_mutex_destroy(&p->mu); free(p); return CCE_ERR_IO;
    }
    if (pthread_cond_init(&p->cv_space, NULL) != 0) {
        pthread_cond_destroy(&p->cv_jobs); pthread_mutex_destroy(&p->mu);
        free(p); return CCE_ERR_IO;
    }
    if (pthread_rwlock_init(&p->ring_rw, NULL) != 0) {
        pthread_cond_destroy(&p->cv_space); pthread_cond_destroy(&p->cv_jobs);
        pthread_mutex_destroy(&p->mu); free(p); return CCE_ERR_IO;
    }
    p->page_len = o.page_len;
    p->n_hot = o.n_hot;
    p->k_slot = o.k_slot;
    p->v_slot = o.v_slot;
    p->legal_max = o.legal_max > 0 ? o.legal_max : CNET_CTX_LEGAL_MAX;
    if (p->legal_max > CNET_CTX_LEGAL_MAX) p->legal_max = CNET_CTX_LEGAL_MAX;
    p->async = o.async;
    p->quant_cold = o.quant_cold;
    p->rehydrate = o.rehydrate;
    p->rehyd_lo = p->rehyd_hi = -1;
    p->rehyd_page_id = -1;
    p->capacity = p->page_len * p->n_hot;
    p->weight_epoch = 0;
    p->epoch_bumps = 0;
    p->cold_epoch_rejects = 0;
    snprintf(p->archive_dir, sizeof p->archive_dir, "%s",
             o.archive_dir && o.archive_dir[0] ? o.archive_dir : "kv_archive");
    if (mkdir_p(p->archive_dir) != 0) {
        cce_kv_pager_close(p);
        return CCE_ERR_IO;
    }
    snprintf(p->ledger_path, sizeof p->ledger_path, "%s/ledger.tsv",
             p->archive_dir);
    p->ledger = fopen(p->ledger_path, "a");
    if (!p->ledger) {
        cce_kv_pager_close(p);
        return CCE_ERR_IO;
    }
    fprintf(p->ledger, "# cce_kv_page ledger page_len=%d n_hot=%d legal_max=%d\n",
            p->page_len, p->n_hot, p->legal_max);
    fflush(p->ledger);

    ring_k = (size_t)p->capacity * (size_t)p->k_slot;
    ring_v = (size_t)p->capacity * (size_t)p->v_slot;
    p->k_ring = (float *)calloc(ring_k, sizeof(float));
    p->v_ring = (float *)calloc(ring_v, sizeof(float));
    p->slot_page_id = (int *)calloc((size_t)p->n_hot, sizeof(int));
    p->stage_k =
        (float *)malloc((size_t)p->page_len * (size_t)p->k_slot * sizeof(float));
    p->stage_v =
        (float *)malloc((size_t)p->page_len * (size_t)p->v_slot * sizeof(float));
    p->rehyd_k =
        (float *)malloc((size_t)p->page_len * (size_t)p->k_slot * sizeof(float));
    p->rehyd_v =
        (float *)malloc((size_t)p->page_len * (size_t)p->v_slot * sizeof(float));
    if (!p->k_ring || !p->v_ring || !p->slot_page_id || !p->stage_k ||
        !p->stage_v || !p->rehyd_k || !p->rehyd_v) {
        cce_kv_pager_close(p);
        return CCE_ERR_OOM;
    }
    for (i = 0; i < p->n_hot; ++i) p->slot_page_id[i] = i;
    p->next_page_id = p->n_hot;


    if (p->async) {
        if (pthread_create(&p->thr, NULL, kv_flush_thread, p) == 0)
            p->thr_started = 1;
        else
            p->async = 0; /* soft-fail sync flush */
    }
    *out = p;
    return CCE_OK;
}

void cce_kv_pager_close(cce_kv_pager *p) {
    if (!p) return;
    cce_kv_pager_sync(p);
    if (p->thr_started) {
        pthread_mutex_lock(&p->mu);
        p->stop = 1;
        pthread_cond_broadcast(&p->cv_jobs);
        pthread_mutex_unlock(&p->mu);
        pthread_join(p->thr, NULL);
        p->thr_started = 0;
    }
    if (p->ledger) fclose(p->ledger);
    /* Wait for every acquired HOT row lease before releasing ring storage. */
    pthread_rwlock_wrlock(&p->ring_rw);
    free(p->k_ring);
    free(p->v_ring);
    free(p->slot_page_id);
    free(p->stage_k);
    free(p->stage_v);
    free(p->rehyd_k);
    free(p->rehyd_v);
    pthread_rwlock_unlock(&p->ring_rw);
    pthread_mutex_destroy(&p->mu);
    pthread_cond_destroy(&p->cv_jobs);
    pthread_cond_destroy(&p->cv_space);
    pthread_rwlock_destroy(&p->ring_rw);
    free(p);
}

int cce_kv_pager_hot_capacity(const cce_kv_pager *p) {
    return p ? p->capacity : 0;
}
int cce_kv_pager_window_start(const cce_kv_pager *p) {
    return p ? p->window_start : 0;
}
int cce_kv_pager_cur_pos(const cce_kv_pager *p) { return p ? p->cur_pos : 0; }
int cce_kv_pager_legal_max(const cce_kv_pager *p) {
    return p ? p->legal_max : 0;
}
int cce_kv_pager_page_len(const cce_kv_pager *p) {
    return p ? p->page_len : 0;
}
int cce_kv_pager_quant_cold(const cce_kv_pager *p) {
    return p ? p->quant_cold : 0;
}
int cce_kv_pager_rehydrate_enabled(const cce_kv_pager *p) {
    return p ? p->rehydrate : 0;
}
int cce_kv_pager_pages_flushed(const cce_kv_pager *p) {
    int n;
    if (!p) return 0;
    pthread_mutex_lock((pthread_mutex_t *)&p->mu);
    n = p->pages_flushed;
    pthread_mutex_unlock((pthread_mutex_t *)&p->mu);
    return n;
}
int cce_kv_pager_pages_reused(const cce_kv_pager *p) {
    return p ? p->pages_reused : 0;
}
int cce_kv_pager_pages_rehydrated(const cce_kv_pager *p) {
    return p ? p->pages_rehydrated : 0;
}
int cce_kv_pager_write_failures(const cce_kv_pager *p) {
    int n;
    if (!p) return 0;
    pthread_mutex_lock((pthread_mutex_t *)&p->mu);
    n = p->write_failures;
    pthread_mutex_unlock((pthread_mutex_t *)&p->mu);
    return n;
}
int cce_kv_pager_queue_depth(const cce_kv_pager *p) {
    int d;
    if (!p) return 0;
    pthread_mutex_lock((pthread_mutex_t *)&p->mu);
    d = p->q_count;
    pthread_mutex_unlock((pthread_mutex_t *)&p->mu);
    return d;
}

static int ring_index(const cce_kv_pager *p, int pos) {
    int rel;
    if (!p || pos < p->window_start || pos >= p->window_start + p->capacity)
        return -1;
    rel = pos - p->window_start;
    return rel;
}

float *cce_kv_pager_k_row(cce_kv_pager *p, int pos) {
    int ri = ring_index(p, pos);
    if (ri < 0 || !p->k_ring) return NULL;
    return p->k_ring + (size_t)ri * (size_t)p->k_slot;
}

float *cce_kv_pager_v_row(cce_kv_pager *p, int pos) {
    int ri = ring_index(p, pos);
    if (ri < 0 || !p->v_ring) return NULL;
    return p->v_ring + (size_t)ri * (size_t)p->v_slot;
}

int cce_kv_pager_reader_count(const cce_kv_pager *p) {
    int r;
    if (!p) return 0;
    pthread_mutex_lock((pthread_mutex_t *)&p->mu);
    r = p->reader_count;
    pthread_mutex_unlock((pthread_mutex_t *)&p->mu);
    return r;
}

float *cce_kv_pager_k_row_acquire(cce_kv_pager *p, int pos) {
    int ri;
    float *row;
    if (!p) return NULL;
    pthread_rwlock_rdlock(&p->ring_rw);
    ri = ring_index(p, pos);
    if (ri < 0 || !p->k_ring) {
        pthread_rwlock_unlock(&p->ring_rw);
        return NULL;
    }
    row = p->k_ring + (size_t)ri * (size_t)p->k_slot;
    pthread_mutex_lock(&p->mu);
    p->reader_count++;
    pthread_mutex_unlock(&p->mu);
    return row;
}

float *cce_kv_pager_v_row_acquire(cce_kv_pager *p, int pos) {
    int ri;
    float *row;
    if (!p) return NULL;
    pthread_rwlock_rdlock(&p->ring_rw);
    ri = ring_index(p, pos);
    if (ri < 0 || !p->v_ring) {
        pthread_rwlock_unlock(&p->ring_rw);
        return NULL;
    }
    row = p->v_ring + (size_t)ri * (size_t)p->v_slot;
    pthread_mutex_lock(&p->mu);
    p->reader_count++;
    pthread_mutex_unlock(&p->mu);
    return row;
}

void cce_kv_pager_row_release(cce_kv_pager *p, const float *row) {
    int held = 0;
    if (!p || !row) return;
    pthread_mutex_lock(&p->mu);
    if (p->reader_count > 0) {
        p->reader_count--;
        held = 1;
    }
    pthread_cond_broadcast(&p->cv_space);
    pthread_mutex_unlock(&p->mu);
    if (held) pthread_rwlock_unlock(&p->ring_rw);
}

int cce_kv_pager_clamp_jmin(const cce_kv_pager *p, int jmin) {
    if (!p) return jmin;
    if (jmin < p->window_start) return p->window_start;
    return jmin;
}

int cce_kv_pager_clear(cce_kv_pager *p) {
    int i;
    if (!p) return -1;
    cce_kv_pager_sync(p);
    pthread_rwlock_wrlock(&p->ring_rw);
    memset(p->k_ring, 0,
           (size_t)p->capacity * (size_t)p->k_slot * sizeof(float));
    memset(p->v_ring, 0,
           (size_t)p->capacity * (size_t)p->v_slot * sizeof(float));
    memset(p->stage_k, 0,
           (size_t)p->page_len * (size_t)p->k_slot * sizeof(float));
    memset(p->stage_v, 0,
           (size_t)p->page_len * (size_t)p->v_slot * sizeof(float));
    memset(p->rehyd_k, 0,
           (size_t)p->page_len * (size_t)p->k_slot * sizeof(float));
    memset(p->rehyd_v, 0,
           (size_t)p->page_len * (size_t)p->v_slot * sizeof(float));
    p->window_start = 0;
    p->cur_pos = 0;
    p->stage_busy = 0;
    p->rehyd_page_id = -1;
    for (i = 0; i < p->n_hot; ++i) p->slot_page_id[i] = i;
    p->next_page_id = p->n_hot;
    pthread_rwlock_unlock(&p->ring_rw);
    return 0;
}

static int enqueue_flush(cce_kv_pager *p, int page_id, int pos_lo, int pos_hi,
                         float *k_src, float *v_src) {
    cce_kv_flush_job job;
    size_t kb, vb;
    memset(&job, 0, sizeof job);
    kb = (size_t)(pos_hi - pos_lo) * (size_t)p->k_slot * sizeof(float);
    vb = (size_t)(pos_hi - pos_lo) * (size_t)p->v_slot * sizeof(float);
    job.k = (float *)malloc(kb);
    job.v = (float *)malloc(vb);
    if (!job.k || !job.v) {
        free(job.k);
        free(job.v);
        return -1;
    }
    memcpy(job.k, k_src, kb);
    memcpy(job.v, v_src, vb);
    job.k_bytes = kb;
    job.v_bytes = vb;
    job.page_id = page_id;
    job.pos_lo = pos_lo;
    job.pos_hi = pos_hi;
    job.digest = fnv1a_buf(job.k, kb) ^ (fnv1a_buf(job.v, vb) << 1);
    job.quant = p->quant_cold ? 1 : 0;
    job.valid = 1;

    if (!p->async || !p->thr_started) {
        int ok = write_cold_file(p, &job);
        if (ok == 0) ok = record_cold_file(p, &job);
        if (ok == 0)
            p->pages_flushed++;
        else
            p->write_failures++;
        free(job.k);
        free(job.v);
        return ok;
    }

    pthread_mutex_lock(&p->mu);
    while (p->q_count >= CCE_KV_QMAX && !p->stop)
        pthread_cond_wait(&p->cv_space, &p->mu);
    if (p->stop) {
        pthread_mutex_unlock(&p->mu);
        free(job.k);
        free(job.v);
        return -1;
    }
    p->q[p->q_tail] = job;
    p->q_tail = (p->q_tail + 1) % CCE_KV_QMAX;
    p->q_count++;
    pthread_cond_signal(&p->cv_jobs);
    pthread_mutex_unlock(&p->mu);
    return 0;
}

/* Slide window by one page: flush oldest page, reuse its ring slot. */
static int slide_one_page(cce_kv_pager *p) {
    int pos_lo, pos_hi, page_id;
    float *k_src, *v_src;
    size_t npos;

    if (p->window_start + p->capacity >= p->legal_max) return -1;
    /* oldest page sits at ring index 0 relative to window */
    pos_lo = p->window_start;
    pos_hi = p->window_start + p->page_len;
    page_id = p->slot_page_id[0];
    npos = (size_t)p->page_len;
    k_src = p->k_ring;
    v_src = p->v_ring;

    /* Double-buffer into WARM, then secure ownership of a complete COLD job
     * before recycling HOT. On synchronous storage failure the ring is left
     * untouched instead of silently losing the oldest page. */
    memcpy(p->stage_k, k_src, npos * (size_t)p->k_slot * sizeof(float));
    memcpy(p->stage_v, v_src, npos * (size_t)p->v_slot * sizeof(float));
    p->stage_busy = 1;
    if (enqueue_flush(p, page_id, pos_lo, pos_hi, p->stage_k, p->stage_v) !=
        0) {
        p->stage_busy = 0;
        return -1;
    }

    /* Shift hot ring left by one page (reuse storage). */
    if (p->n_hot > 1) {
        size_t move_k =
            (size_t)(p->capacity - p->page_len) * (size_t)p->k_slot;
        size_t move_v =
            (size_t)(p->capacity - p->page_len) * (size_t)p->v_slot;
        memmove(p->k_ring, p->k_ring + (size_t)p->page_len * (size_t)p->k_slot,
                move_k * sizeof(float));
        memmove(p->v_ring, p->v_ring + (size_t)p->page_len * (size_t)p->v_slot,
                move_v * sizeof(float));
        memmove(p->slot_page_id, p->slot_page_id + 1,
                (size_t)(p->n_hot - 1) * sizeof(int));
        p->slot_page_id[p->n_hot - 1] = p->next_page_id++;
        memset(p->k_ring +
                   (size_t)(p->capacity - p->page_len) * (size_t)p->k_slot,
               0, (size_t)p->page_len * (size_t)p->k_slot * sizeof(float));
        memset(p->v_ring +
                   (size_t)(p->capacity - p->page_len) * (size_t)p->v_slot,
               0, (size_t)p->page_len * (size_t)p->v_slot * sizeof(float));
    } else {
        /* Single hot page: whole ring is recycled after flush. */
        p->slot_page_id[0] = p->next_page_id++;
        memset(p->k_ring, 0,
               (size_t)p->capacity * (size_t)p->k_slot * sizeof(float));
        memset(p->v_ring, 0,
               (size_t)p->capacity * (size_t)p->v_slot * sizeof(float));
    }

    p->window_start += p->page_len;
    p->pages_reused++;

    p->stage_busy = 0;
    return 0;
}

static int pager_prepare_write_locked(cce_kv_pager *p, int pos) {
    if (!p || pos < 0) return -1;
    if (pos >= p->legal_max) return -1;
    /* Caller holds ring_rw for the complete slide/update transaction. */
    while (pos >= p->window_start + p->capacity) {
        if (slide_one_page(p) != 0) return -1;
    }
    if (pos < p->window_start) return -1; /* would need cold rehydrate */
    if (pos + 1 > p->cur_pos) p->cur_pos = pos + 1;
    return 0;
}

int cce_kv_pager_prepare_write(cce_kv_pager *p, int pos) {
    int rc;
    if (!p) return -1;
    pthread_rwlock_wrlock(&p->ring_rw);
    rc = pager_prepare_write_locked(p, pos);
    pthread_rwlock_unlock(&p->ring_rw);
    return rc;
}

int cce_kv_pager_write(cce_kv_pager *p, int pos, const float *k,
                       const float *v) {
    float *kr, *vr;
    if (!p || !k || !v) return -1;
    pthread_rwlock_wrlock(&p->ring_rw);
    if (pager_prepare_write_locked(p, pos) != 0) {
        pthread_rwlock_unlock(&p->ring_rw);
        return -1;
    }
    kr = cce_kv_pager_k_row(p, pos);
    vr = cce_kv_pager_v_row(p, pos);
    if (!kr || !vr) {
        pthread_rwlock_unlock(&p->ring_rw);
        return -1;
    }
    memcpy(kr, k, (size_t)p->k_slot * sizeof(float));
    memcpy(vr, v, (size_t)p->v_slot * sizeof(float));
    pthread_rwlock_unlock(&p->ring_rw);
    return 0;
}

void cce_kv_pager_sync(cce_kv_pager *p) {
    if (!p) return;
    if (!p->async || !p->thr_started) return;
    pthread_mutex_lock(&p->mu);
    while (p->q_count > 0 || p->writes_inflight > 0)
        pthread_cond_wait(&p->cv_space, &p->mu);
    pthread_mutex_unlock(&p->mu);
}

int cce_kv_pager_verify_cold(const cce_kv_pager *p, int page_id,
                             uint64_t expect_digest) {
    char path[400];
    FILE *f;
    uint32_t magic = 0, ver = 0;
    int pid = 0, lo = 0, hi = 0, ks = 0, vs = 0, quant = 0;
    uint64_t dig = 0;
    size_t npos, nk, nv;
    if (!p) return -1;
    snprintf(path, sizeof path, "%s/page_%06d.kvc", p->archive_dir, page_id);
    f = fopen(path, "rb");
    if (!f) return -1;
    if (fread(&magic, 4, 1, f) != 1 || fread(&ver, 4, 1, f) != 1 ||
        fread(&pid, 4, 1, f) != 1 || fread(&lo, 4, 1, f) != 1 ||
        fread(&hi, 4, 1, f) != 1 || fread(&ks, 4, 1, f) != 1 ||
        fread(&vs, 4, 1, f) != 1) {
        fclose(f);
        return -1;
    }
    if (page_id < 0 || page_id > INT_MAX / p->page_len ||
        ks != p->k_slot || vs != p->v_slot || pid != page_id || hi <= lo ||
        lo != page_id * p->page_len) {
        fclose(f);
        return -1;
    }
    /* Slot shape must match the pager that wrote the page; reject otherwise
     * (e.g. page_len was changed between runs). npos bounds the body size. */
    npos = (size_t)(hi - lo);
    if (npos > (size_t)p->page_len) {
        fclose(f);
        return -1;
    }
    nk = npos * (size_t)ks;
    nv = npos * (size_t)vs;

    if (magic == 0x43564B32u && ver == 2u) {
        float sk = 1.f, sv = 1.f;
        int8_t *kq = NULL, *vq = NULL;
        if (fread(&quant, 4, 1, f) != 1 || fread(&dig, 8, 1, f) != 1 ||
            (quant != 0 && quant != 1)) {
            fclose(f);
            return -1;
        }
        /* Re-hash the BODY (quant bytes or f32 bytes) and compare against
         * the digest stored in the header. A flipped body bit must fail. */
        if (quant) {
            uint64_t body_dig;
            if (fread(&sk, 4, 1, f) != 1 || fread(&sv, 4, 1, f) != 1) {
                fclose(f);
                return -1;
            }
            kq = (int8_t *)malloc(nk ? nk : 1);
            vq = (int8_t *)malloc(nv ? nv : 1);
            if (!kq || !vq ||
                fread(kq, 1, nk, f) != nk || fread(vq, 1, nv, f) != nv) {
                free(kq); free(vq); fclose(f);
                return -1;
            }
            body_dig = fnv1a_buf(kq, nk) ^ (fnv1a_buf(vq, nv) << 1);
            body_dig ^= (uint64_t)(sk * 1e6f) ^ ((uint64_t)(sv * 1e6f) << 32);
            free(kq); free(vq);
            if (body_dig != dig) { fclose(f); return -1; }
        } else {
            size_t kb_bytes = nk * sizeof(float);
            size_t vb_bytes = nv * sizeof(float);
            float *kb = (float *)malloc(kb_bytes ? kb_bytes : 1);
            float *vb = (float *)malloc(vb_bytes ? vb_bytes : 1);
            uint64_t body_dig;
            if (!kb || !vb ||
                fread(kb, sizeof(float), nk, f) != nk ||
                fread(vb, sizeof(float), nv, f) != nv) {
                free(kb); free(vb); fclose(f);
                return -1;
            }
            body_dig = fnv1a_buf(kb, kb_bytes) ^
                       (fnv1a_buf(vb, vb_bytes) << 1);
            free(kb); free(vb);
            if (body_dig != dig) { fclose(f); return -1; }
        }
    } else if (magic == 0x43564B31u && ver == 1u) {
        size_t kb_bytes = nk * sizeof(float);
        size_t vb_bytes = nv * sizeof(float);
        float *kb, *vb;
        uint64_t body_dig;
        if (fread(&dig, 8, 1, f) != 1) { fclose(f); return -1; }
        kb = (float *)malloc(kb_bytes ? kb_bytes : 1);
        vb = (float *)malloc(vb_bytes ? vb_bytes : 1);
        if (!kb || !vb ||
            fread(kb, sizeof(float), nk, f) != nk ||
            fread(vb, sizeof(float), nv, f) != nv) {
            free(kb); free(vb); fclose(f);
            return -1;
        }
        body_dig = fnv1a_buf(kb, kb_bytes) ^
                   (fnv1a_buf(vb, vb_bytes) << 1);
        free(kb); free(vb);
        if (body_dig != dig) { fclose(f); return -1; }
    } else {
        fclose(f);
        return -1;
    }
    fclose(f);
    if (expect_digest && dig != expect_digest) return -1;
    (void)ver;
    (void)quant;
    return 0;
}

int cce_kv_pager_write_slice(cce_kv_pager *p, int pos, size_t k_off,
                             const float *k, int k_dim, size_t v_off,
                             const float *v, int v_dim) {
    float *kr, *vr;
    if (!p || !k || !v || k_dim < 1 || v_dim < 1) return -1;
    pthread_rwlock_wrlock(&p->ring_rw);
    if (pager_prepare_write_locked(p, pos) != 0) {
        pthread_rwlock_unlock(&p->ring_rw);
        return -1;
    }
    kr = cce_kv_pager_k_row(p, pos);
    vr = cce_kv_pager_v_row(p, pos);
    if (!kr || !vr || k_off + (size_t)k_dim > (size_t)p->k_slot ||
        v_off + (size_t)v_dim > (size_t)p->v_slot) {
        pthread_rwlock_unlock(&p->ring_rw);
        return -1;
    }
    memcpy(kr + k_off, k, (size_t)k_dim * sizeof(float));
    memcpy(vr + v_off, v, (size_t)v_dim * sizeof(float));
    pthread_rwlock_unlock(&p->ring_rw);
    return 0;
}

/* Load cold page file into rehyd_* buffers. */
static int load_cold_page(cce_kv_pager *p, int page_id, int *out_lo,
                          int *out_hi) {
    char path[400];
    FILE *f;
    uint32_t magic = 0, ver = 0;
    int pid = 0, lo = 0, hi = 0, ks = 0, vs = 0, quant = 0;
    uint64_t dig = 0;
    size_t npos, nk, nv;
    snprintf(path, sizeof path, "%s/page_%06d.kvc", p->archive_dir, page_id);
    f = fopen(path, "rb");
    if (!f) return -1;
    if (fread(&magic, 4, 1, f) != 1 || fread(&ver, 4, 1, f) != 1 ||
        fread(&pid, 4, 1, f) != 1 || fread(&lo, 4, 1, f) != 1 ||
        fread(&hi, 4, 1, f) != 1 || fread(&ks, 4, 1, f) != 1 ||
        fread(&vs, 4, 1, f) != 1) {
        fclose(f);
        return -1;
    }
    if (page_id < 0 || page_id > INT_MAX / p->page_len ||
        ks != p->k_slot || vs != p->v_slot || pid != page_id || hi <= lo ||
        lo != page_id * p->page_len) {
        fclose(f);
        return -1;
    }
    npos = (size_t)(hi - lo);
    if (npos > (size_t)p->page_len) {
        fclose(f);
        return -1;
    }
    nk = npos * (size_t)ks;
    nv = npos * (size_t)vs;
    if (magic == 0x43564B32u && ver == 2u) {
        float sk = 1.f, sv = 1.f;
        int8_t *kq, *vq;
        if (fread(&quant, 4, 1, f) != 1 || fread(&dig, 8, 1, f) != 1 ||
            (quant != 0 && quant != 1)) {
            fclose(f);
            return -1;
        }
        if (quant) {
            uint64_t body_dig;
            if (fread(&sk, 4, 1, f) != 1 || fread(&sv, 4, 1, f) != 1) {
                fclose(f);
                return -1;
            }
            kq = (int8_t *)malloc(nk ? nk : 1);
            vq = (int8_t *)malloc(nv ? nv : 1);
            if (!kq || !vq || fread(kq, 1, nk, f) != nk ||
                fread(vq, 1, nv, f) != nv) {
                free(kq);
                free(vq);
                fclose(f);
                return -1;
            }
            body_dig = fnv1a_buf(kq, nk) ^ (fnv1a_buf(vq, nv) << 1);
            body_dig ^= (uint64_t)(sk * 1e6f) ^
                        ((uint64_t)(sv * 1e6f) << 32);
            if (body_dig != dig) {
                free(kq);
                free(vq);
                fclose(f);
                return -1;
            }
            dequant_page(kq, nk, sk, p->rehyd_k);
            dequant_page(vq, nv, sv, p->rehyd_v);
            free(kq);
            free(vq);
        } else {
            uint64_t body_dig;
            if (fread(p->rehyd_k, sizeof(float), nk, f) != nk ||
                fread(p->rehyd_v, sizeof(float), nv, f) != nv) {
                fclose(f);
                return -1;
            }
            body_dig = fnv1a_buf(p->rehyd_k, nk * sizeof(float)) ^
                       (fnv1a_buf(p->rehyd_v, nv * sizeof(float)) << 1);
            if (body_dig != dig) {
                fclose(f);
                return -1;
            }
        }
    } else if (magic == 0x43564B31u && ver == 1u) {
        uint64_t body_dig;
        if (fread(&dig, 8, 1, f) != 1 ||
            fread(p->rehyd_k, sizeof(float), nk, f) != nk ||
            fread(p->rehyd_v, sizeof(float), nv, f) != nv) {
            fclose(f);
            return -1;
        }
        body_dig = fnv1a_buf(p->rehyd_k, nk * sizeof(float)) ^
                   (fnv1a_buf(p->rehyd_v, nv * sizeof(float)) << 1);
        if (body_dig != dig) {
            fclose(f);
            return -1;
        }
    } else {
        fclose(f);
        return -1;
    }
    fclose(f);
    p->rehyd_lo = lo;
    p->rehyd_hi = hi;
    p->rehyd_page_id = page_id;
    p->pages_rehydrated++;
    if (out_lo) *out_lo = lo;
    if (out_hi) *out_hi = hi;
    return 0;
}

int cce_kv_pager_rehydrate_pos(cce_kv_pager *p, int pos) {
    int page_id, lo;
    uint64_t page_epoch = 0;
    if (!p || !p->rehydrate || pos < 0) return -1;
    if (ring_index(p, pos) >= 0) return 0; /* already hot */
    /* Only positions evicted from the current generation can be COLD. After
     * clear(), window_start is reset to zero; this rejects stale files left by
     * the previous generation until the corresponding current page is really
     * evicted and rewritten. */
    if (pos >= p->window_start) return -1;
    if (p->rehyd_lo >= 0 && pos >= p->rehyd_lo && pos < p->rehyd_hi)
        return 0; /* already in rehyd cache */
    page_id = pos / p->page_len;
    lo = page_id * p->page_len;
    /* Wait for flush if this page was just evicted.
     *
     * THIS MUST STAY ABOVE THE EPOCH GATE. Flushes are async by default
     * (o->async = 1), and the `page_id=N ... weight_epoch=E` ledger line is
     * written by record_cold_file() from inside kv_flush_thread once the job
     * drains. A page that has been evicted but whose flush has not yet
     * completed therefore has NO ledger line, so ledger_epoch_for_page()
     * fails, and the `else if (p->weight_epoch > 0)` arm below discards it as
     * a stale-epoch legacy file -- silently, and counted as the gate working.
     * Syncing first makes the page's own stamp visible before we judge it, and
     * also stops this thread from scanning the ledger while the flush thread is
     * still appending to it. */
    cce_kv_pager_sync(p);

    /* Epoch gate: COLD neural pages stamped under a prior weight_epoch are
     * mathematical garbage under the new tensors — refuse rehydrate. */
    if (ledger_epoch_for_page(p, page_id, &page_epoch) == 0) {
        if (page_epoch != p->weight_epoch) {
            p->cold_epoch_rejects++;
            return -1;
        }
    } else if (p->weight_epoch > 0) {
        /* No ledger stamp (legacy file) after an epoch bump → refuse. */
        p->cold_epoch_rejects++;
        return -1;
    }
    if (load_cold_page(p, page_id, NULL, NULL) != 0) return -1;
    if (pos < p->rehyd_lo || pos >= p->rehyd_hi) return -1;
    (void)lo;
    return 0;
}

uint64_t cce_kv_pager_weight_epoch(const cce_kv_pager *p) {
    return p ? p->weight_epoch : 0;
}

int cce_kv_pager_bump_weight_epoch(cce_kv_pager *p) {
    if (!p) return -1;
    /* Clear HOT/WARM/rehyd so no W0 activations remain resident. */
    if (cce_kv_pager_clear(p) != 0) return -1;
    p->weight_epoch++;
    p->epoch_bumps++;
    if (p->ledger) {
        fprintf(p->ledger,
                "# weight_epoch_bump epoch=%llu bumps=%llu (neural COLD "
                "stamped below this is invalid)\n",
                (unsigned long long)p->weight_epoch,
                (unsigned long long)p->epoch_bumps);
        fflush(p->ledger);
    }
    return 0;
}

/* Scan ledger for last weight_epoch stamp of page_id. Returns 0 if found. */
static int ledger_epoch_for_page(cce_kv_pager *p, int page_id, uint64_t *out_ep) {
    FILE *f;
    char line[512];
    int found = 0;
    uint64_t ep = 0;
    if (!p || !out_ep || page_id < 0) return -1;
    f = fopen(p->ledger_path, "r");
    if (!f) return -1;
    while (fgets(line, sizeof line, f)) {
        int pid = -1;
        unsigned long long e = 0;
        if (sscanf(line, "page_id=%d", &pid) != 1 || pid != page_id) continue;
        {
            const char *we = strstr(line, "weight_epoch=");
            if (we && sscanf(we, "weight_epoch=%llu", &e) == 1) {
                ep = (uint64_t)e;
                found = 1;
            } else {
                /* Legacy line without epoch → treat as epoch 0 */
                ep = 0;
                found = 1;
            }
        }
    }
    fclose(f);
    if (!found) return -1;
    *out_ep = ep;
    return 0;
}

const float *cce_kv_pager_k_row_ex(cce_kv_pager *p, int pos) {
    float *h;
    if (!p) return NULL;
    h = cce_kv_pager_k_row(p, pos);
    if (h) return h;
    if (!p->rehydrate) return NULL;
    if (cce_kv_pager_rehydrate_pos(p, pos) != 0) return NULL;
    if (pos < p->rehyd_lo || pos >= p->rehyd_hi) return NULL;
    return p->rehyd_k + (size_t)(pos - p->rehyd_lo) * (size_t)p->k_slot;
}

const float *cce_kv_pager_v_row_ex(cce_kv_pager *p, int pos) {
    float *h;
    if (!p) return NULL;
    h = cce_kv_pager_v_row(p, pos);
    if (h) return h;
    if (!p->rehydrate) return NULL;
    if (cce_kv_pager_rehydrate_pos(p, pos) != 0) return NULL;
    if (pos < p->rehyd_lo || pos >= p->rehyd_hi) return NULL;
    return p->rehyd_v + (size_t)(pos - p->rehyd_lo) * (size_t)p->v_slot;
}
