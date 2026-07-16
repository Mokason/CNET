/* Tiered runtime for store-backed transformers. See cce_tier_runtime.h.
 *
 * A3 (async readahead + learned hot-pinning): the fetch order of the first
 * cold pass is recorded (a dense forward's access sequence is deterministic).
 * With readahead enabled, a background worker prefetches the next `depth`
 * payloads in that order into a staging area while the forward computes;
 * the provider adopts staged payloads instead of blocking on the store.
 * The worker touches ONLY the store and the staging area — every forest
 * mutation stays on the forward thread (inside the provider callback), so
 * the math and the residency accounting are untouched.
 */

#include "../../include/cce/cce_tier_runtime.h"
#include "../../include/cce/cce_forest.h"
#include "../../include/cce/cce_cascade.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#define TIER_HAVE_THREADS 1
#include <pthread.h>
#include <time.h>
#endif

typedef struct { char name[96]; uint64_t digest; int fidx; } tier_entry;

/* readahead staging states (per mapped specialist) */
enum { RA_EMPTY = 0, RA_QUEUED = 1, RA_INFLIGHT = 2, RA_READY = 3 };

struct cce_tier_runtime {
    cce_weight_store* store;
    cce_forest* forest;
    tier_entry* map;
    int n_map;
    int rehydrations;

    /* learned access order: recorded on the first cold pass */
    int* seq;          /* map indices in first-fetch order */
    int* pos_of;       /* map idx -> seq position (-1 = unseen) */
    int  seq_len;
    int  seq_learned;
    int* fetch_count;  /* per map idx: lifetime fetches (learned pinning) */
    int  pinned;

    /* fetch stats (forward thread only) */
    int prefetch_hits, prefetch_waits, sync_fetches;
    double stall_sec;

#ifdef TIER_HAVE_THREADS
    /* async readahead: a worker pool (default size 1), a request ring,
       per-idx staging. Fetches are independent payloads, but they are
       memory-bandwidth-heavy — more workers contend with the forward's own
       matmuls (measured slower on real payloads), so 1 is the default and
       CNET_TIER_RA_WORKERS overrides for experiments. */
#define TIER_RA_MAX_WORKERS 4
    int ra_depth;             /* 0 = off */
    int worker_up, stop, gen; /* gen bumps on flush: late completions are discarded */
    pthread_t workers[TIER_RA_MAX_WORKERS];
    int n_workers;
    pthread_mutex_t mu;
    pthread_cond_t cv_work, cv_done;
    int* ra_state;            /* per map idx */
    cce_cascade** staged;     /* per map idx: READY payloads awaiting adoption */
    int* ring;                /* queued map indices */
    int ring_head, ring_tail, ring_cap;
    int staged_ready, staged_high_water;
#endif
};

static double tier_now(void) {
#ifdef TIER_HAVE_THREADS
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
#else
    return 0.0; /* stall accounting needs a monotonic clock; report 0 without one */
#endif
}

static int tier_lookup_idx(const cce_tier_runtime* rt, const char* name) {
    for (int i = 0; i < rt->n_map; i++)
        if (strcmp(rt->map[i].name, name) == 0) return i;
    return -1;
}

/* record the access order until one full pass has been seen; a refetch of an
 * already-seen specialist means the pass wrapped -> sequence is complete */
static void tier_record_fetch(cce_tier_runtime* rt, int mi) {
    rt->fetch_count[mi]++;
    if (rt->seq_learned) return;
    if (rt->pos_of[mi] >= 0) { rt->seq_learned = 1; return; }
    rt->pos_of[mi] = rt->seq_len;
    rt->seq[rt->seq_len++] = mi;
    if (rt->seq_len == rt->n_map) rt->seq_learned = 1;
}

#ifdef TIER_HAVE_THREADS

static void* ra_worker(void* arg) {
    cce_tier_runtime* rt = (cce_tier_runtime*)arg;
    pthread_mutex_lock(&rt->mu);
    while (!rt->stop) {
        if (rt->ring_head == rt->ring_tail) {
            pthread_cond_wait(&rt->cv_work, &rt->mu);
            continue;
        }
        int mi = rt->ring[rt->ring_head];
        rt->ring_head = (rt->ring_head + 1) % rt->ring_cap;
        if (rt->ra_state[mi] != RA_QUEUED) continue; /* flushed while queued */
        rt->ra_state[mi] = RA_INFLIGHT;
        int gen = rt->gen;
        uint64_t d = rt->map[mi].digest;
        pthread_mutex_unlock(&rt->mu);

        cce_cascade* cas = NULL;
        /* only store + heap: no forest access. RAW_QUANT: the streamed forward
           reads w_trit/w_q, never the FP tensor — skipping the dequant writes
           is the bulk of the fetch cost at scale */
        cce_weight_store_get_opt(rt->store, d, &cas, CCE_WS_GET_RAW_QUANT);

        pthread_mutex_lock(&rt->mu);
        if (gen != rt->gen) {                 /* flushed mid-fetch: discard */
            if (cas) cce_cascade_destroy(cas);
            if (rt->ra_state[mi] == RA_INFLIGHT) rt->ra_state[mi] = RA_EMPTY;
        } else if (cas) {
            rt->staged[mi] = cas;
            rt->ra_state[mi] = RA_READY;
            rt->staged_ready++;
            if (rt->staged_ready > rt->staged_high_water)
                rt->staged_high_water = rt->staged_ready;
        } else {
            rt->ra_state[mi] = RA_EMPTY;      /* fetch failed: forward falls back to sync */
        }
        pthread_cond_broadcast(&rt->cv_done);
    }
    pthread_mutex_unlock(&rt->mu);
    return NULL;
}

/* forward thread: queue the next `depth` sequence positions (wrapping) */
static void ra_schedule_ahead(cce_tier_runtime* rt, int pos) {
    if (rt->seq_len <= 0) return;
    pthread_mutex_lock(&rt->mu);
    for (int k = 1; k <= rt->ra_depth; k++) {
        int mi = rt->seq[(pos + k) % rt->seq_len];
        if (rt->ra_state[mi] != RA_EMPTY) continue;
        int fx = rt->map[mi].fidx;
        if (fx >= 0 && rt->forest->branches[fx].cascade) continue; /* already resident (e.g. pinned) */
        rt->ra_state[mi] = RA_QUEUED;
        rt->ring[rt->ring_tail] = mi;
        rt->ring_tail = (rt->ring_tail + 1) % rt->ring_cap;
        pthread_cond_signal(&rt->cv_work);
    }
    pthread_mutex_unlock(&rt->mu);
}

/* forward thread: drop queued + staged prefetches; in-flight results are
 * discarded by the generation check when the worker completes */
static void ra_flush(cce_tier_runtime* rt) {
    if (!rt->worker_up) return;
    pthread_mutex_lock(&rt->mu);
    rt->gen++;
    rt->ring_head = rt->ring_tail = 0;
    for (int i = 0; i < rt->n_map; i++) {
        if (rt->ra_state[i] == RA_READY && rt->staged[i]) {
            cce_cascade_destroy(rt->staged[i]);
            rt->staged[i] = NULL;
        }
        rt->ra_state[i] = RA_EMPTY;
    }
    rt->staged_ready = 0;
    pthread_cond_broadcast(&rt->cv_done);
    pthread_mutex_unlock(&rt->mu);
}

static void ra_stop_worker(cce_tier_runtime* rt) {
    if (!rt->worker_up) return;
    ra_flush(rt);
    pthread_mutex_lock(&rt->mu);
    rt->stop = 1;
    pthread_cond_broadcast(&rt->cv_work);
    pthread_mutex_unlock(&rt->mu);
    for (int i = 0; i < rt->n_workers; i++) pthread_join(rt->workers[i], NULL);
    /* a worker may have staged one last (pre-stop, same-gen) payload */
    for (int i = 0; i < rt->n_map; i++)
        if (rt->staged[i]) { cce_cascade_destroy(rt->staged[i]); rt->staged[i] = NULL; }
    pthread_mutex_destroy(&rt->mu);
    pthread_cond_destroy(&rt->cv_work);
    pthread_cond_destroy(&rt->cv_done);
    free(rt->ra_state); rt->ra_state = NULL;
    free(rt->staged);   rt->staged = NULL;
    free(rt->ring);     rt->ring = NULL;
    rt->worker_up = 0;
    rt->n_workers = 0;
    rt->stop = 0;
    rt->ra_depth = 0;
}

#endif /* TIER_HAVE_THREADS */

static cce_cascade* tier_provider(void* ctx, const char* branch_name) {
    cce_tier_runtime* rt = (cce_tier_runtime*)ctx;
    int mi = tier_lookup_idx(rt, branch_name);
    if (mi < 0) return NULL;
    double t0 = tier_now();
    tier_record_fetch(rt, mi);
    cce_cascade* cas = NULL;
#ifdef TIER_HAVE_THREADS
    if (rt->ra_depth > 0 && rt->seq_learned && rt->pos_of[mi] >= 0) {
        pthread_mutex_lock(&rt->mu);
        if (rt->ra_state[mi] == RA_READY) {
            cas = rt->staged[mi]; rt->staged[mi] = NULL;
            rt->ra_state[mi] = RA_EMPTY; rt->staged_ready--;
            rt->prefetch_hits++;
        } else if (rt->ra_state[mi] == RA_QUEUED || rt->ra_state[mi] == RA_INFLIGHT) {
            rt->prefetch_waits++;
            while (rt->ra_state[mi] == RA_QUEUED || rt->ra_state[mi] == RA_INFLIGHT)
                pthread_cond_wait(&rt->cv_done, &rt->mu);
            if (rt->ra_state[mi] == RA_READY) {  /* not READY = flushed/failed -> sync below */
                cas = rt->staged[mi]; rt->staged[mi] = NULL;
                rt->ra_state[mi] = RA_EMPTY; rt->staged_ready--;
            }
        }
        pthread_mutex_unlock(&rt->mu);
        ra_schedule_ahead(rt, rt->pos_of[mi]);  /* fetch ahead while this one computes */
    }
#endif
    if (!cas) {
        rt->sync_fetches++;
        if (cce_weight_store_get_opt(rt->store, rt->map[mi].digest, &cas,
                                     CCE_WS_GET_RAW_QUANT) != CCE_OK || !cas) {
            rt->stall_sec += tier_now() - t0;
            return NULL;
        }
    }
    rt->stall_sec += tier_now() - t0;
    rt->rehydrations++;
    return cas; /* forest adopts */
}

cce_result cce_tier_attach(cce_tier_runtime** out, cce_gguf_qwen2* m,
                           cce_weight_store* s, const char* manifest_path,
                           int hot_cap) {
    if (!out || !m || !m->forest || !s || !manifest_path) return CCE_ERR_INVALID_ARG;
    *out = NULL;

    FILE* f = fopen(manifest_path, "rb");
    if (!f) return CCE_ERR_IO;
    char line[512];
    if (!fgets(line, sizeof(line), f) || strncmp(line, "CNET_MANIFEST v1", 16) != 0) {
        fclose(f); return CCE_ERR_UNSUPPORTED;
    }

    cce_tier_runtime* rt = (cce_tier_runtime*)calloc(1, sizeof(*rt));
    if (!rt) { fclose(f); return CCE_ERR_OOM; }
    rt->store = s;
    rt->forest = m->forest;
    rt->map = (tier_entry*)calloc((size_t)m->forest->num_branches, sizeof(tier_entry));
    if (!rt->map) { free(rt); fclose(f); return CCE_ERR_OOM; }

    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "spec ", 5) != 0) continue;
        if (rt->n_map >= m->forest->num_branches) break;
        char name[96];
        unsigned long long d = 0;
        if (sscanf(line + 5, "%95s %llx", name, &d) == 2) {
            snprintf(rt->map[rt->n_map].name, sizeof(rt->map[0].name), "%s", name);
            rt->map[rt->n_map].digest = (uint64_t)d;
            rt->map[rt->n_map].fidx = -1;
            rt->n_map++;
        }
    }
    fclose(f);

    rt->seq         = (int*)calloc((size_t)(rt->n_map ? rt->n_map : 1), sizeof(int));
    rt->pos_of      = (int*)malloc((size_t)(rt->n_map ? rt->n_map : 1) * sizeof(int));
    rt->fetch_count = (int*)calloc((size_t)(rt->n_map ? rt->n_map : 1), sizeof(int));
    if (!rt->seq || !rt->pos_of || !rt->fetch_count) {
        free(rt->seq); free(rt->pos_of); free(rt->fetch_count);
        free(rt->map); free(rt);
        return CCE_ERR_OOM;
    }
    for (int i = 0; i < rt->n_map; i++) rt->pos_of[i] = -1;

    /* every forest branch must be restorable, and its payload present,
       before anything becomes evictable */
    for (int i = 0; i < m->forest->num_branches; i++) {
        int mi = tier_lookup_idx(rt, m->forest->branches[i].name);
        if (mi < 0 || !cce_weight_store_contains(s, rt->map[mi].digest)) {
            free(rt->seq); free(rt->pos_of); free(rt->fetch_count);
            free(rt->map); free(rt);
            return CCE_ERR_UNSUPPORTED;
        }
        rt->map[mi].fidx = i;
    }
    for (int i = 0; i < m->forest->num_branches; i++)
        m->forest->branches[i].evictable = 1;

    cce_forest_set_residency(m->forest, hot_cap, tier_provider, rt);
    *out = rt;
    return CCE_OK;
}

cce_result cce_tier_evict_all(cce_tier_runtime* rt) {
    if (!rt || !rt->forest) return CCE_ERR_INVALID_ARG;
#ifdef TIER_HAVE_THREADS
    ra_flush(rt); /* a cold start is cold: staged prefetches go too */
#endif
    for (int i = 0; i < rt->forest->num_branches; i++) {
        cce_branch* b = &rt->forest->branches[i];
        if (b->evictable && b->cascade && !b->is_view)
            cce_forest_evict_branch(rt->forest, i);
    }
    return CCE_OK;
}

cce_result cce_tier_set_readahead(cce_tier_runtime* rt, int depth) {
    if (!rt || depth < 0) return CCE_ERR_INVALID_ARG;
#ifdef TIER_HAVE_THREADS
    if (depth == 0) {
        if (rt->worker_up) ra_stop_worker(rt);
        rt->ra_depth = 0;
        return CCE_OK;
    }
    if (depth > rt->n_map - 1) depth = rt->n_map - 1;
    if (depth < 1) return CCE_ERR_INVALID_ARG;
    if (!rt->worker_up) {
        rt->ra_state = (int*)calloc((size_t)rt->n_map, sizeof(int));
        rt->staged   = (cce_cascade**)calloc((size_t)rt->n_map, sizeof(cce_cascade*));
        rt->ring_cap = rt->n_map + 1;
        rt->ring     = (int*)calloc((size_t)rt->ring_cap, sizeof(int));
        if (!rt->ra_state || !rt->staged || !rt->ring) {
            free(rt->ra_state); rt->ra_state = NULL;
            free(rt->staged);   rt->staged = NULL;
            free(rt->ring);     rt->ring = NULL;
            return CCE_ERR_OOM;
        }
        rt->ring_head = rt->ring_tail = 0;
        rt->staged_ready = 0;
        rt->stop = 0;
        if (pthread_mutex_init(&rt->mu, NULL) != 0 ||
            pthread_cond_init(&rt->cv_work, NULL) != 0 ||
            pthread_cond_init(&rt->cv_done, NULL) != 0) {
            free(rt->ra_state); rt->ra_state = NULL;
            free(rt->staged);   rt->staged = NULL;
            free(rt->ring);     rt->ring = NULL;
            return CCE_ERR_IO;
        }
        /* one worker by default: fetches are memory-bandwidth-heavy, and a
           pool contending with the forward's own matmuls measured SLOWER on
           real payloads. CNET_TIER_RA_WORKERS overrides for experiments. */
        int nw = 1;
        {
            const char* e = getenv("CNET_TIER_RA_WORKERS");
            if (e && *e) nw = atoi(e);
        }
        if (nw < 1) nw = 1;
        if (nw > TIER_RA_MAX_WORKERS) nw = TIER_RA_MAX_WORKERS;
        rt->n_workers = 0;
        for (int i = 0; i < nw; i++) {
            if (pthread_create(&rt->workers[i], NULL, ra_worker, rt) != 0) break;
            rt->n_workers++;
        }
        if (rt->n_workers == 0) {
            pthread_mutex_destroy(&rt->mu);
            pthread_cond_destroy(&rt->cv_work);
            pthread_cond_destroy(&rt->cv_done);
            free(rt->ra_state); rt->ra_state = NULL;
            free(rt->staged);   rt->staged = NULL;
            free(rt->ring);     rt->ring = NULL;
            return CCE_ERR_IO;
        }
        rt->worker_up = 1;
    }
    rt->ra_depth = depth;
    return CCE_OK;
#else
    (void)depth;
    return CCE_ERR_UNSUPPORTED;
#endif
}

cce_result cce_tier_pin_hot(cce_tier_runtime* rt, int n) {
    if (!rt || !rt->forest || n < 0) return CCE_ERR_INVALID_ARG;
    if (n > rt->n_map) n = rt->n_map;
    char* picked = (char*)calloc((size_t)(rt->n_map ? rt->n_map : 1), 1);
    double* score = (double*)malloc((size_t)(rt->n_map ? rt->n_map : 1) * sizeof(double));
    if (!picked || !score) { free(picked); free(score); return CCE_ERR_OOM; }
    /* score = expected refetch cost saved = fetches x payload bytes. On a
       dense model every specialist is fetched once per pass (counts tie), so
       the bytes factor decides — e.g. a huge FP tied lm_head payload
       dominates per-pass fetch cost and gets pinned first. */
    for (int i = 0; i < rt->n_map; i++) {
        long pb = cce_weight_store_payload_size(rt->store, rt->map[i].digest);
        score[i] = (double)rt->fetch_count[i] * (double)(pb > 0 ? pb : 1);
    }
    int newly_pinned = 0;
    for (int p = 0; p < n; p++) {
        /* highest score first; ties by learned order (earliest position wins) */
        int best = -1;
        for (int i = 0; i < rt->n_map; i++) {
            if (picked[i] || rt->map[i].fidx < 0) continue;
            if (!rt->forest->branches[rt->map[i].fidx].evictable) continue; /* already pinned */
            if (best < 0 ||
                score[i] > score[best] ||
                (score[i] == score[best] &&
                 rt->pos_of[i] >= 0 && (rt->pos_of[best] < 0 || rt->pos_of[i] < rt->pos_of[best])))
                best = i;
        }
        if (best < 0) break;
        picked[best] = 1;
        cce_cascade* cas = cce_forest_get_resident(rt->forest, rt->map[best].name);
        if (!cas) { free(picked); free(score); return CCE_ERR_IO; }
        rt->forest->branches[rt->map[best].fidx].evictable = 0; /* leaves the LRU pool */
        newly_pinned++;
    }
    free(picked);
    free(score);
    rt->pinned += newly_pinned;
    return CCE_OK;
}

cce_result cce_tier_unpin_all(cce_tier_runtime* rt) {
    if (!rt || !rt->forest) return CCE_ERR_INVALID_ARG;
    for (int i = 0; i < rt->n_map; i++)
        if (rt->map[i].fidx >= 0)
            rt->forest->branches[rt->map[i].fidx].evictable = 1;
    rt->pinned = 0;
    return CCE_OK;
}

int cce_tier_pinned(const cce_tier_runtime* rt) { return rt ? rt->pinned : 0; }
int cce_tier_seq_learned(const cce_tier_runtime* rt) { return rt ? rt->seq_learned : 0; }
int cce_tier_prefetch_hits(const cce_tier_runtime* rt) { return rt ? rt->prefetch_hits : 0; }
int cce_tier_prefetch_waits(const cce_tier_runtime* rt) { return rt ? rt->prefetch_waits : 0; }
int cce_tier_sync_fetches(const cce_tier_runtime* rt) { return rt ? rt->sync_fetches : 0; }
int cce_tier_staged_high_water(const cce_tier_runtime* rt) {
#ifdef TIER_HAVE_THREADS
    return rt ? rt->staged_high_water : 0;
#else
    (void)rt; return 0;
#endif
}
double cce_tier_stall_seconds(const cce_tier_runtime* rt) { return rt ? rt->stall_sec : 0.0; }

int cce_tier_rehydrations(const cce_tier_runtime* rt) { return rt ? rt->rehydrations : 0; }
int cce_tier_resident(const cce_tier_runtime* rt) {
    return rt ? cce_forest_resident_count(rt->forest) : 0;
}
int cce_tier_high_water(const cce_tier_runtime* rt) {
    return rt ? cce_forest_resident_high_water(rt->forest) : 0;
}

cce_result cce_tier_detach(cce_tier_runtime* rt) {
    if (!rt || !rt->forest) return CCE_ERR_INVALID_ARG;
#ifdef TIER_HAVE_THREADS
    ra_stop_worker(rt);
#endif
    cce_tier_unpin_all(rt); /* pinned branches go back to plain resident */
    /* rehydrate everything first so the model keeps working standalone;
       lift the cap so nothing gets evicted while restoring */
    rt->forest->hot_cap = rt->forest->num_branches + 1;
    for (int i = 0; i < rt->forest->num_branches; i++) {
        if (!rt->forest->branches[i].cascade) {
            if (!cce_forest_get_resident(rt->forest, rt->forest->branches[i].name)) {
                return CCE_ERR_IO; /* store lost a payload: refuse to detach */
            }
        }
    }
    cce_forest_set_residency(rt->forest, 0, NULL, NULL); /* clears evictable flags */
    free(rt->seq);
    free(rt->pos_of);
    free(rt->fetch_count);
    free(rt->map);
    free(rt);
    return CCE_OK;
}
