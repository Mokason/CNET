#include "../../include/cce/cce_kv_page.h"

#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

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
    FILE *ledger;
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
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
            tmp[i] = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
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
    o->legal_max = legal_max > 0 ? legal_max : 8192;
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

/* Write cold file: ver=2 supports quant. Returns 0 and sets *digest_out. */
static int write_cold_file(cce_kv_pager *p, cce_kv_flush_job *job) {
    char path[400];
    FILE *f;
    uint32_t magic = 0x43564B32u; /* CVK2 */
    uint32_t ver = 2;
    int quant = job->quant ? 1 : 0;
    size_t npos, nk, nv;
    snprintf(path, sizeof path, "%s/page_%06d.kvc", p->archive_dir,
             job->page_id);
    f = fopen(path, "wb");
    if (!f) return -1;
    npos = (size_t)(job->pos_hi - job->pos_lo);
    nk = npos * (size_t)p->k_slot;
    nv = npos * (size_t)p->v_slot;
    fwrite(&magic, 4, 1, f);
    fwrite(&ver, 4, 1, f);
    fwrite(&job->page_id, 4, 1, f);
    fwrite(&job->pos_lo, 4, 1, f);
    fwrite(&job->pos_hi, 4, 1, f);
    fwrite(&p->k_slot, 4, 1, f);
    fwrite(&p->v_slot, 4, 1, f);
    fwrite(&quant, 4, 1, f);
    if (quant) {
        int8_t *kq = (int8_t *)malloc(nk);
        int8_t *vq = (int8_t *)malloc(nv);
        float sk = 1.f, sv = 1.f;
        uint64_t dig;
        if (!kq || !vq) {
            free(kq);
            free(vq);
            fclose(f);
            return -1;
        }
        quant_page_maxabs(job->k, nk, kq, &sk);
        quant_page_maxabs(job->v, nv, vq, &sv);
        dig = fnv1a_buf(kq, nk) ^ (fnv1a_buf(vq, nv) << 1);
        dig ^= (uint64_t)(sk * 1e6f) ^ ((uint64_t)(sv * 1e6f) << 32);
        job->digest = dig;
        fwrite(&dig, 8, 1, f);
        fwrite(&sk, 4, 1, f);
        fwrite(&sv, 4, 1, f);
        fwrite(kq, 1, nk, f);
        fwrite(vq, 1, nv, f);
        free(kq);
        free(vq);
    } else {
        fwrite(&job->digest, 8, 1, f);
        fwrite(job->k, 1, job->k_bytes, f);
        fwrite(job->v, 1, job->v_bytes, f);
    }
    fclose(f);
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
        pthread_cond_signal(&p->cv_space);
        pthread_mutex_unlock(&p->mu);

        if (!job.valid || !job.k || !job.v) continue;
        (void)write_cold_file(p, &job);
        pthread_mutex_lock(&p->mu);
        if (p->ledger) {
            fprintf(p->ledger,
                    "page_id=%d pos_lo=%d pos_hi=%d digest=0x%016llx "
                    "file=page_%06d.kvc tier=COLD quant=%d\n",
                    job.page_id, job.pos_lo, job.pos_hi,
                    (unsigned long long)job.digest, job.page_id, job.quant);
            fflush(p->ledger);
        }
        p->pages_flushed++;
        pthread_mutex_unlock(&p->mu);
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
    o = *opts;
    if (o.page_len < 16 || o.n_hot < 2 || o.k_slot < 1 || o.v_slot < 1)
        return CCE_ERR_INVALID_ARG;
    p = (cce_kv_pager *)calloc(1, sizeof *p);
    if (!p) return CCE_ERR_OOM;
    p->page_len = o.page_len;
    p->n_hot = o.n_hot;
    p->k_slot = o.k_slot;
    p->v_slot = o.v_slot;
    p->legal_max = o.legal_max > 0 ? o.legal_max : 8192;
    p->async = o.async;
    p->quant_cold = o.quant_cold;
    p->rehydrate = o.rehydrate;
    p->rehyd_lo = p->rehyd_hi = -1;
    p->rehyd_page_id = -1;
    p->capacity = p->page_len * p->n_hot;
    snprintf(p->archive_dir, sizeof p->archive_dir, "%s",
             o.archive_dir && o.archive_dir[0] ? o.archive_dir : "kv_archive");
    if (mkdir_p(p->archive_dir) != 0) {
        free(p);
        return CCE_ERR_IO;
    }
    snprintf(p->ledger_path, sizeof p->ledger_path, "%s/ledger.tsv",
             p->archive_dir);
    p->ledger = fopen(p->ledger_path, "a");
    if (!p->ledger) {
        free(p);
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

    pthread_mutex_init(&p->mu, NULL);
    pthread_cond_init(&p->cv_jobs, NULL);
    pthread_cond_init(&p->cv_space, NULL);
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
    free(p->k_ring);
    free(p->v_ring);
    free(p->slot_page_id);
    free(p->stage_k);
    free(p->stage_v);
    free(p->rehyd_k);
    free(p->rehyd_v);
    pthread_mutex_destroy(&p->mu);
    pthread_cond_destroy(&p->cv_jobs);
    pthread_cond_destroy(&p->cv_space);
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
    return p ? p->pages_flushed : 0;
}
int cce_kv_pager_pages_reused(const cce_kv_pager *p) {
    return p ? p->pages_reused : 0;
}
int cce_kv_pager_pages_rehydrated(const cce_kv_pager *p) {
    return p ? p->pages_rehydrated : 0;
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

int cce_kv_pager_clamp_jmin(const cce_kv_pager *p, int jmin) {
    if (!p) return jmin;
    if (jmin < p->window_start) return p->window_start;
    return jmin;
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
        if (write_cold_file(p, &job) == 0 && p->ledger) {
            fprintf(p->ledger,
                    "page_id=%d pos_lo=%d pos_hi=%d digest=0x%016llx "
                    "file=page_%06d.kvc tier=COLD quant=%d\n",
                    job.page_id, job.pos_lo, job.pos_hi,
                    (unsigned long long)job.digest, job.page_id, job.quant);
            fflush(p->ledger);
        }
        p->pages_flushed++;
        free(job.k);
        free(job.v);
        return 0;
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

    /* Double-buffer: copy to stage (WARM), then shift ring left, then
     * enqueue stage for async COLD write — writer continues into freed tail. */
    memcpy(p->stage_k, k_src, npos * (size_t)p->k_slot * sizeof(float));
    memcpy(p->stage_v, v_src, npos * (size_t)p->v_slot * sizeof(float));
    p->stage_busy = 1;

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

    if (enqueue_flush(p, page_id, pos_lo, pos_hi, p->stage_k, p->stage_v) !=
        0)
        return -1;
    p->stage_busy = 0;
    return 0;
}

int cce_kv_pager_prepare_write(cce_kv_pager *p, int pos) {
    if (!p || pos < 0) return -1;
    if (pos >= p->legal_max) return -1;
    /* Slide until pos is inside hot window end (writable). */
    while (pos >= p->window_start + p->capacity) {
        if (slide_one_page(p) != 0) return -1;
    }
    if (pos < p->window_start) return -1; /* would need cold rehydrate */
    if (pos + 1 > p->cur_pos) p->cur_pos = pos + 1;
    return 0;
}

int cce_kv_pager_write(cce_kv_pager *p, int pos, const float *k,
                       const float *v) {
    float *kr, *vr;
    if (!p || !k || !v) return -1;
    if (cce_kv_pager_prepare_write(p, pos) != 0) return -1;
    kr = cce_kv_pager_k_row(p, pos);
    vr = cce_kv_pager_v_row(p, pos);
    if (!kr || !vr) return -1;
    memcpy(kr, k, (size_t)p->k_slot * sizeof(float));
    memcpy(vr, v, (size_t)p->v_slot * sizeof(float));
    return 0;
}

void cce_kv_pager_sync(cce_kv_pager *p) {
    if (!p) return;
    if (!p->async || !p->thr_started) return;
    for (;;) {
        int c;
        pthread_mutex_lock(&p->mu);
        c = p->q_count;
        pthread_mutex_unlock(&p->mu);
        if (c == 0) break;
        usleep(1000);
    }
}

int cce_kv_pager_verify_cold(const cce_kv_pager *p, int page_id,
                             uint64_t expect_digest) {
    char path[400];
    FILE *f;
    uint32_t magic = 0, ver = 0;
    int pid = 0, lo = 0, hi = 0, ks = 0, vs = 0, quant = 0;
    uint64_t dig = 0;
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
    if (magic == 0x43564B32u) {
        if (fread(&quant, 4, 1, f) != 1 || fread(&dig, 8, 1, f) != 1) {
            fclose(f);
            return -1;
        }
    } else if (magic == 0x43564B31u) {
        if (fread(&dig, 8, 1, f) != 1) {
            fclose(f);
            return -1;
        }
    } else {
        fclose(f);
        return -1;
    }
    fclose(f);
    if (pid != page_id) return -1;
    if (expect_digest && dig != expect_digest) return -1;
    (void)ver;
    (void)lo;
    (void)hi;
    (void)ks;
    (void)vs;
    (void)quant;
    return 0;
}

int cce_kv_pager_write_slice(cce_kv_pager *p, int pos, size_t k_off,
                             const float *k, int k_dim, size_t v_off,
                             const float *v, int v_dim) {
    float *kr, *vr;
    if (!p || !k || !v || k_dim < 1 || v_dim < 1) return -1;
    if (cce_kv_pager_prepare_write(p, pos) != 0) return -1;
    kr = cce_kv_pager_k_row(p, pos);
    vr = cce_kv_pager_v_row(p, pos);
    if (!kr || !vr) return -1;
    if (k_off + (size_t)k_dim > (size_t)p->k_slot ||
        v_off + (size_t)v_dim > (size_t)p->v_slot)
        return -1;
    memcpy(kr + k_off, k, (size_t)k_dim * sizeof(float));
    memcpy(vr + v_off, v, (size_t)v_dim * sizeof(float));
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
    if (ks != p->k_slot || vs != p->v_slot || pid != page_id) {
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
    if (magic == 0x43564B32u) {
        float sk = 1.f, sv = 1.f;
        int8_t *kq, *vq;
        if (fread(&quant, 4, 1, f) != 1 || fread(&dig, 8, 1, f) != 1) {
            fclose(f);
            return -1;
        }
        if (quant) {
            if (fread(&sk, 4, 1, f) != 1 || fread(&sv, 4, 1, f) != 1) {
                fclose(f);
                return -1;
            }
            kq = (int8_t *)malloc(nk);
            vq = (int8_t *)malloc(nv);
            if (!kq || !vq || fread(kq, 1, nk, f) != nk ||
                fread(vq, 1, nv, f) != nv) {
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
            if (fread(p->rehyd_k, sizeof(float), nk, f) != nk ||
                fread(p->rehyd_v, sizeof(float), nv, f) != nv) {
                fclose(f);
                return -1;
            }
        }
    } else if (magic == 0x43564B31u) {
        if (fread(&dig, 8, 1, f) != 1 ||
            fread(p->rehyd_k, sizeof(float), nk, f) != nk ||
            fread(p->rehyd_v, sizeof(float), nv, f) != nv) {
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
    (void)ver;
    (void)dig;
    return 0;
}

int cce_kv_pager_rehydrate_pos(cce_kv_pager *p, int pos) {
    int page_id, lo;
    if (!p || !p->rehydrate || pos < 0) return -1;
    if (ring_index(p, pos) >= 0) return 0; /* already hot */
    if (p->rehyd_lo >= 0 && pos >= p->rehyd_lo && pos < p->rehyd_hi)
        return 0; /* already in rehyd cache */
    /* Cold page ids are assigned in order: page covering [i*page_len, ...)
     * has id = i for the first n_hot pages... actually first n_hot ids are
     * 0..n_hot-1 for initial ring, then next_page_id grows. Map pos→page_id
     * via floor(pos/page_len) only if we started at 0 with sequential ids. */
    page_id = pos / p->page_len;
    lo = page_id * p->page_len;
    /* Wait for flush if this page was just evicted */
    cce_kv_pager_sync(p);
    if (load_cold_page(p, page_id, NULL, NULL) != 0) return -1;
    if (pos < p->rehyd_lo || pos >= p->rehyd_hi) return -1;
    (void)lo;
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
