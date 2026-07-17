/* The REAL flagship run: a CCE-loaded transformer as the acquisition oracle.
 *
 * Extracts conditional next-token slices (V-restricted greedy argmax) from
 * the model into certified, sealed units in a unified base — duty-cycled,
 * thermally governed, crash-resumable (re-run the same command to resume;
 * create <base>.stop to stop cleanly).
 *
 * Usage:
 *   flagship_run <model> [V] [max_units] [temp_C] [duty] [wall_s] [base.cnb]
 * Defaults: V=64, max_units=8 (smoke-sized), temp 80C, duty 0.75, wall 0
 * (unbounded), base flagship.cnb. Vocab = token ids 2000..2000+V-1.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _OPENMP
#include <omp.h>
#endif

#include "../include/flagship.h"
#include "../include/base.h"
#include "../include/cce/cce_detect.h"
#include "../include/cce/cce_clgemm.h"
#include "../include/cce/cce_campaign_provenance.h"
#include "../include/cce/cce_oracle_prefix_cache.h"
#include "window_discover.h"

#define FS_MAX_LANES 8

/* One oracle LANE: a model instance + logits scratch (+ its own GPU handle
   in pool mode). Lanes share nothing mutable — no queue, no KV cache — so
   the mining loop can call the oracle width-wide, one lane per GPU. */
typedef struct {
    cce_anymodel *am;      /* owned; NULL for lane 0 (borrows main's model) */
    cce_gguf_qwen2 *m;
    float *logits;         /* model vocab_size scratch */
    cce_clgemm *gpu;       /* per-lane handle in pool mode; NULL otherwise */
    cce_oracle_prefix_cache prefix_cache;
    float *batch_logits;   /* batched-probe scratch: rows x vocab_size */
    size_t batch_rows;
} OracleLane;

typedef struct {
    OracleLane lane[FS_MAX_LANES];
    size_t nlanes;
    int inner_threads;     /* per-lane CPU team for the forward's OMP tiles */
    int force_lane;        /* >= 0: pin a lane (serial checks); -1 = by thread */
    const int *vocab;
    size_t v;
    int t;                 /* conditioning token of the CURRENT unit */
    double margin_eps;     /* >0: margin-aware certification. A point whose
                              decision margin (smallest logit gap that would
                              change the ordered answer) is below this is
                              ABSTAINED (oracle returns +1) — the teacher's
                              own coin-flips are excluded from the certified
                              domain instead of poisoning training. 0 = off. */
} CceOracleCtx;

/* Smallest logit gap whose crossing would change the ordered top-`depth`
   window answer (gaps rank0-1, 1-2, ..., depth-1..depth). A tiny value means
   the teacher is nearly tied at some decision boundary — a point no student
   can be expected to memorize because the model itself is choosing at random
   there. Returns +inf-ish (large) when the window is smaller than depth+1. */
static double fs_decision_margin(const float *logits, const int *vocab,
                                 size_t V, int depth) {
    int taken[4096] = {0};
    double prev = 0.0, minfg = 1e30;
    int r;
    if ((size_t)(depth + 1) > V) return 1e30;
    for (r = 0; r <= depth; ++r) {
        size_t i, best = (size_t)-1;
        double bl = 0.0;
        for (i = 0; i < V; ++i) {
            if (taken[i]) continue;
            if (best == (size_t)-1 || (double)logits[vocab[i]] > bl) {
                bl = (double)logits[vocab[i]];
                best = i;
            }
        }
        taken[best] = 1;
        if (r > 0) { double g = prev - bl; if (g < minfg) minfg = g; }
        prev = bl;
    }
    return minfg;
}

/* Set-valued top-k (CNET_TOPK_SET=1): certify the teacher's top-3 SET
   instead of its knife-edge ordering. Near-ties INSIDE the top-3 are the
   dominant refusal cause on interactive-frequency tokens, yet the set
   membership is stable — the ambiguity is real model knowledge, so
   certify it rather than abstain. Mechanically: picks are written in
   canonical (ascending window-index) order, so students train on and
   certification replays a representation that order flips cannot change;
   the abstention margin narrows to the only gap that still matters, the
   rank-3/rank-4 boundary. */
static int fs_topk_set_on(void) {
    static int cached = -1;
    if (cached < 0) {
        const char *e = getenv("CNET_TOPK_SET");
        cached = (e && e[0] == '1') ? 1 : 0;
    }
    return cached;
}

/* Smallest gap whose crossing would change top-`depth` MEMBERSHIP:
   logit[rank depth] - logit[rank depth+1] over the window. */
static double fs_set_margin(const float *logits, const int *vocab,
                            size_t v, size_t depth) {
    double top[8];
    size_t i, r, n = depth + 1;
    if (n > 8) n = 8;
    for (r = 0; r < n; ++r) top[r] = -1e30;
    for (i = 0; i < v; ++i) {
        double l = (double)logits[vocab[i]];
        for (r = 0; r < n; ++r) {
            if (l > top[r]) {
                size_t s;
                for (s = n - 1; s > r; --s) top[s] = top[s - 1];
                top[r] = l;
                break;
            }
        }
    }
    return top[depth - 1] - top[depth];
}


/* Sum of a contract's port totals in doubles (mirrors soul_host). */
static size_t fs_ports_total(const Port *ports, size_t n) {
    size_t t = 0, i;
    for (i = 0; i < n; ++i) t += ports[i].field_width * ports[i].field_count;
    return t;
}
/* Lane for THIS call: pinned when force_lane is set, else by OMP thread id
   (the mining loop runs one thread per lane). Also scopes the calling
   thread's nested-OMP team so `lanes x inner` matches the machine. */
static OracleLane *fs_lane(CceOracleCtx *c) {
    size_t li = 0;
#ifdef _OPENMP
    if (c->force_lane >= 0) li = (size_t)c->force_lane % c->nlanes;
    else li = (size_t)omp_get_thread_num() % c->nlanes;
    if (c->inner_threads > 0) omp_set_num_threads(c->inner_threads);
#endif
    return &c->lane[li];
}

/* All calls of one unit share conditioning token t. Its KV row is a pure
   function of the token and the weights, so computing it ONCE per lane per
   unit and letting every call attend to the cached row 0 is BIT-IDENTICAL
   to recomputing it inside each call — while removing 1 of 2 (argmax/topk)
   or 1 of 3 (pair) tokens from every layer traversal. Suffix calls write
   KV rows >= 1 only; row 0 stays pinned until t changes.
   CNET_ORACLE_PREFIX=0 disables (A/B; both paths are gated by the digest
   audit and the determinism spot check either way). */
static int fs_prefix_off(void) {
    const char *e = getenv("CNET_ORACLE_PREFIX");
    return e && e[0] == '0';
}

/* BOS anchor (default ON, CNET_ORACLE_BOS=0 opts out): gemma-family models
   collapse to <pad> on BOS-less raw-token contexts — measured 1 distinct
   full-vocab argmax over 128 probes on BOTH real models, which made every
   window degenerate and every mined unit constant. Anchored contexts are
   [bos, t, ...suffix]; with prefix reuse the anchor costs nothing per call. */
static int fs_bos(const cce_gguf_qwen2 *m) {
    const char *e = getenv("CNET_ORACLE_BOS");
    if (e && e[0] == '0') return -1;
    return m->bos_token_id;
}

static int fs_prefix(OracleLane *L, int t) {
    int toks[2];
    int n = 0, bos = fs_bos(L->m);
    if (cce_oracle_prefix_cache_hit(&L->prefix_cache, t)) return 0;
    cce_oracle_prefix_cache_begin_fill(&L->prefix_cache);
    if (bos >= 0) toks[n++] = bos;
    toks[n++] = t;
    L->m->cur_pos = 0;
    if (cce_gguf_qwen2_forward(L->m, toks, n, L->logits,
                               L->m->vocab_size) != CCE_OK) {
        return -1;
    }
    cce_oracle_prefix_cache_commit(&L->prefix_cache, t);
    return 0;
}

/* prefix length = rows pinned in the KV cache = where each suffix starts */
static int fs_prefix_len(const cce_gguf_qwen2 *m) {
    return (fs_bos(m) >= 0) ? 2 : 1;
}

static int cce_cond_next(const double *in, double *out, void *ctx) {
    CceOracleCtx *c = (CceOracleCtx *)ctx;
    OracleLane *L = fs_lane(c);
    size_t w = 0, i, best = 0;
    int tokens[2];
    float bl;

    for (i = 0; i < c->v; ++i)
        if (in[i] > 0.5) w = i;
    if (fs_prefix_off()) {
        int n = 0, bos = fs_bos(L->m);
        int toks[3];
        if (bos >= 0) toks[n++] = bos;
        toks[n++] = c->t;
        toks[n++] = c->vocab[w];
        L->m->cur_pos = 0;   /* fresh context: prefix recomputed every call */
        if (cce_gguf_qwen2_forward(L->m, toks, n, L->logits,
                                   L->m->vocab_size) != CCE_OK) {
            return -1;
        }
    } else {
        if (fs_prefix(L, c->t) != 0) return -1;
        tokens[0] = c->vocab[w];
        L->m->cur_pos = fs_prefix_len(L->m);   /* attend to the pinned prefix */
        if (cce_gguf_qwen2_forward(L->m, tokens, 1, L->logits,
                                   L->m->vocab_size) != CCE_OK) {
            return -1;
        }
    }
    if (c->margin_eps > 0.0 &&
        fs_decision_margin(L->logits, c->vocab, c->v, 1) < c->margin_eps)
        return 1;   /* teacher-ambiguous argmax: abstain */
    bl = L->logits[c->vocab[0]];
    for (i = 1; i < c->v; ++i) {
        if (L->logits[c->vocab[i]] > bl) {
            bl = L->logits[c->vocab[i]];
            best = i;
        }
    }
    for (i = 0; i < c->v; ++i) out[i] = (i == best) ? 1.0 : 0.0;
    return 0;
}

/* PAIR: argmax over V of P(v | [t, w_prev, w_cur]) — 3-token context. */
static int cce_cond_pair(const double *in, double *out, void *ctx) {
    CceOracleCtx *c = (CceOracleCtx *)ctx;
    OracleLane *L = fs_lane(c);
    size_t wp = 0, wc = 0, i, best = 0;
    int tokens[3];
    float bl;

    for (i = 0; i < c->v; ++i)
        if (in[i] > 0.5) wp = i;
    for (i = 0; i < c->v; ++i)
        if (in[c->v + i] > 0.5) wc = i;
    if (fs_prefix_off()) {
        int n = 0, bos = fs_bos(L->m);
        int toks[4];
        if (bos >= 0) toks[n++] = bos;
        toks[n++] = c->t;
        toks[n++] = c->vocab[wp];
        toks[n++] = c->vocab[wc];
        L->m->cur_pos = 0;
        if (cce_gguf_qwen2_forward(L->m, toks, n, L->logits,
                                   L->m->vocab_size) != CCE_OK) {
            return -1;
        }
    } else {
        if (fs_prefix(L, c->t) != 0) return -1;
        tokens[0] = c->vocab[wp];
        tokens[1] = c->vocab[wc];
        L->m->cur_pos = fs_prefix_len(L->m);
        if (cce_gguf_qwen2_forward(L->m, tokens, 2, L->logits,
                                   L->m->vocab_size) != CCE_OK) {
            return -1;
        }
    }
    if (c->margin_eps > 0.0 &&
        fs_decision_margin(L->logits, c->vocab, c->v, 1) < c->margin_eps)
        return 1;   /* teacher-ambiguous argmax: abstain */
    bl = L->logits[c->vocab[0]];
    for (i = 1; i < c->v; ++i) {
        if (L->logits[c->vocab[i]] > bl) {
            bl = L->logits[c->vocab[i]];
            best = i;
        }
    }
    for (i = 0; i < c->v; ++i) out[i] = (i == best) ? 1.0 : 0.0;
    return 0;
}

/* TOPK: the model's ordered top-3 within V, as 3 one-hot fields (the soul). */
/* Margin-gate + encode the top-3 answer from `logits` for one probe.
   Shared verbatim by the serial and batched topk oracles so the batch
   path cannot drift semantically. Returns 0 (encoded) or 1 (abstain). */
static int fs_topk_encode(const CceOracleCtx *c, const float *logits,
                          double *out) {
    char taken[4096] = {0};
    size_t i, r, best;
    if (c->margin_eps > 0.0) {
        double m = fs_topk_set_on()
                       ? fs_set_margin(logits, c->vocab, c->v, 3)
                       : fs_decision_margin(logits, c->vocab, c->v, 3);
        if (m < c->margin_eps)
            return 1;   /* teacher-ambiguous: abstain from the cert domain */
    }
    for (i = 0; i < 3u * c->v; ++i) out[i] = 0.0;
    {
        size_t picks[3];
        for (r = 0; r < 3; ++r) {
            float bl;
            best = (size_t)-1;
            bl = 0.0f;
            for (i = 0; i < c->v; ++i) {
                if (taken[i]) continue;
                if (best == (size_t)-1 || logits[c->vocab[i]] > bl) {
                    bl = logits[c->vocab[i]];
                    best = i;
                }
            }
            taken[best] = 1;
            picks[r] = best;
        }
        if (fs_topk_set_on()) {
            /* canonical set encoding: ascending window index */
            size_t tmp;
            if (picks[0] > picks[1]) { tmp = picks[0]; picks[0] = picks[1]; picks[1] = tmp; }
            if (picks[1] > picks[2]) { tmp = picks[1]; picks[1] = picks[2]; picks[2] = tmp; }
            if (picks[0] > picks[1]) { tmp = picks[0]; picks[0] = picks[1]; picks[1] = tmp; }
        }
        for (r = 0; r < 3; ++r) out[r * c->v + picks[r]] = 1.0;
    }
    return 0;
}

static int cce_cond_topk(const double *in, double *out, void *ctx) {
    CceOracleCtx *c = (CceOracleCtx *)ctx;
    OracleLane *L = fs_lane(c);
    size_t w = 0, i, r, best;
    int tokens[2];
    int taken[4096] = {0};

    for (i = 0; i < c->v; ++i)
        if (in[i] > 0.5) w = i;
    if (fs_prefix_off()) {
        int n = 0, bos = fs_bos(L->m);
        int toks[3];
        if (bos >= 0) toks[n++] = bos;
        toks[n++] = c->t;
        toks[n++] = c->vocab[w];
        L->m->cur_pos = 0;
        if (cce_gguf_qwen2_forward(L->m, toks, n, L->logits,
                                   L->m->vocab_size) != CCE_OK) {
            return -1;
        }
    } else {
        if (fs_prefix(L, c->t) != 0) return -1;
        tokens[0] = c->vocab[w];
        L->m->cur_pos = fs_prefix_len(L->m);
        if (cce_gguf_qwen2_forward(L->m, tokens, 1, L->logits,
                                   L->m->vocab_size) != CCE_OK) {
            return -1;
        }
    }
    return fs_topk_encode(c, L->logits, out);
}


/* Batched probes (CNET_ORACLE_BATCH=N): answer N w-continuations of the
   pinned prefix in ONE forward — the projections run as N-row GEMMs
   instead of N GEMVs. Encode/margin semantics are fs_topk_encode, shared
   with the serial path; the startup equivalence gate refuses mining if
   the two paths ever disagree. */
static size_t fs_batch_hint(void) {
    static long cached = -1;
    if (cached < 0) {
        const char *e = getenv("CNET_ORACLE_BATCH");
        cached = e ? atol(e) : 0;
        if (cached < 0) cached = 0;
        if (cached > 256) cached = 256;
    }
    return (size_t)cached;
}

static int cce_cond_topk_batch(const double *in, double *out, int *rcs,
                               size_t count, void *ctx) {
    CceOracleCtx *c = (CceOracleCtx *)ctx;
    OracleLane *L = fs_lane(c);
    int toks[256];
    size_t b, i;
    if (count == 0) return 0;
    if (count > 256) return -1;
    if (fs_prefix_off()) return -1;   /* batch mode rides the pinned prefix */
    if (fs_prefix(L, c->t) != 0) return -1;
    if (L->batch_rows < count) {
        float *nb = (float *)realloc(
            L->batch_logits,
            (size_t)count * (size_t)L->m->vocab_size * sizeof *nb);
        if (!nb) return -1;
        L->batch_logits = nb;
        L->batch_rows = count;
    }
    for (b = 0; b < count; ++b) {
        size_t w = 0;
        const double *row = in + b * c->v;
        for (i = 0; i < c->v; ++i)
            if (row[i] > 0.5) w = i;
        toks[b] = c->vocab[w];
    }
    L->m->cur_pos = fs_prefix_len(L->m);
    if (cce_gguf_qwen2_forward_probes(L->m, toks, (int)count, L->batch_logits,
                                      L->m->vocab_size) != CCE_OK)
        return -1;
    for (b = 0; b < count; ++b) {
        const float *lg =
            L->batch_logits + b * (size_t)L->m->vocab_size;
        rcs[b] = fs_topk_encode(c, lg, out + b * 3u * c->v);
    }
    return 0;
}

static CnetOracleFn cce_task_fn = cce_cond_next;   /* set by main per mode */

static int cce_maker(void *maker_ctx, size_t k, int token_id,
                     FlagshipOracle *out) {
    CceOracleCtx *c = (CceOracleCtx *)maker_ctx;
    (void)k;
    c->t = token_id;
    if (cce_task_fn == cce_cond_topk && fs_batch_hint() > 0 &&
        !fs_prefix_off()) {
        out->fn_batch = cce_cond_topk_batch;
        out->batch_hint = fs_batch_hint();
    }
    out->fn = cce_task_fn;
    out->ctx = c;
    out->width = c->nlanes;   /* pool mode: mining fans out one thread/lane */
    return 0;
}

/* KV-state-leak guard: the same query must answer identically before and
   after an unrelated query. A leak here would poison every mined exemplar.
   Task-aware: PAIR has a 2-field input, TOPK a 3-field output.
   Runs per LANE, and additionally requires every lane's answer to be
   identical to lane 0's — divergent lanes would make mined knowledge depend
   on scheduling, which is exactly what the digests must never do. */
static int determinism_spot_check(CceOracleCtx *c, FlagshipTask task) {
    static double in[8192], out1[12288], out2[12288], out_ref[12288];
    size_t i, lane;
    size_t in_total = (task == FLAGSHIP_TASK_PAIR) ? 2 * c->v : c->v;
    size_t out_total = (task == FLAGSHIP_TASK_TOPK) ? 3 * c->v : c->v;
    int rc = 0;
    /* determinism, not certification: never abstain here (a probe point that
       happens to be teacher-ambiguous would return +1 and read as failure). */
    double saved_eps = c->margin_eps;
    c->margin_eps = 0.0;

    c->t = c->vocab[0];
    for (lane = 0; lane < c->nlanes && rc == 0; ++lane) {
        c->force_lane = (int)lane;

        memset(in, 0, in_total * sizeof *in);
        in[0] = 1.0;
        if (task == FLAGSHIP_TASK_PAIR) in[c->v] = 1.0;
        if (cce_task_fn(in, out1, c) != 0) { rc = -1; break; }

        memset(in, 0, in_total * sizeof *in);  /* unrelated query in between */
        in[c->v - 1] = 1.0;
        if (task == FLAGSHIP_TASK_PAIR) in[in_total - 1] = 1.0;
        if (cce_task_fn(in, out2, c) != 0) { rc = -1; break; }

        memset(in, 0, in_total * sizeof *in);  /* repeat the first */
        in[0] = 1.0;
        if (task == FLAGSHIP_TASK_PAIR) in[c->v] = 1.0;
        if (cce_task_fn(in, out2, c) != 0) { rc = -1; break; }

        for (i = 0; i < out_total && rc == 0; ++i)
            if (out1[i] != out2[i]) rc = -1;

        if (lane == 0) {
            memcpy(out_ref, out1, out_total * sizeof *out_ref);
        } else {
            for (i = 0; i < out_total && rc == 0; ++i)
                if (out1[i] != out_ref[i]) rc = -1;   /* cross-lane divergence */
        }
    }
    c->force_lane = -1;
    c->margin_eps = saved_eps;
    return rc;
}


/* ---- Manifest replay (CNET_MANIFEST=<base>.manifest.json) ---------------
   manifest-out without manifest-in is half a loop: a run must be
   RE-RUNNABLE from its artifact. Every recorded knob is applied via
   setenv(..., overwrite=0) — explicit environment always wins — and the
   argv-shaped fields (model, V, max_units, task, base) become defaults
   for positions the caller leaves off. Parser is a scanner for our own
   fprintf format, not a general JSON reader. */
static int mf_scan_str(const char *text, const char *key, char *out,
                       size_t cap) {
    char pat[96];
    const char *at, *q1, *q2;
    snprintf(pat, sizeof pat, "\"%s\": \"", key);
    at = strstr(text, pat);
    if (!at) return -1;
    q1 = at + strlen(pat);
    q2 = strchr(q1, '"');
    if (!q2 || (size_t)(q2 - q1) >= cap) return -1;
    memcpy(out, q1, (size_t)(q2 - q1));
    out[q2 - q1] = 0;
    return 0;
}

static int mf_scan_num(const char *text, const char *key, double *out) {
    char pat[96];
    const char *at;
    snprintf(pat, sizeof pat, "\"%s\": ", key);
    at = strstr(text, pat);
    if (!at) return -1;
    *out = atof(at + strlen(pat));
    return 0;
}

static void mf_setenv_num(const char *text, const char *key,
                          const char *envname, int as_int) {
    double v;
    char buf[64];
    if (mf_scan_num(text, key, &v) != 0) return;
    if (as_int) snprintf(buf, sizeof buf, "%ld", (long)v);
    else snprintf(buf, sizeof buf, "%.6f", v);
    setenv(envname, buf, 0);
}

static int mf_write_window_sidecar(const char *base_path, const int *vocab,
                                   size_t v, char *out, size_t out_cap) {
    FILE *f;
    size_t i;
    if (snprintf(out, out_cap, "%s.window.txt", base_path) < 0) return -1;
    f = fopen(out, "w");
    if (!f) return -1;
    for (i = 0; i < v; ++i) {
        if (fprintf(f, "%d\n", vocab[i]) < 0) { fclose(f); return -1; }
    }
    return fclose(f) == 0 ? 0 : -1;
}

static int mf_write_run_manifest(const char *base_path,
                                 const char *executable_path,
                                 const char *model_path,
                                 FlagshipTask task, size_t v,
                                 size_t max_units, const int *vocab,
                                 const CceOracleCtx *ctx,
                                 const FlagshipConfig *cfg,
                                 const cce_anymodel *am) {
    char mpath[600], mtmp[640], spath[600], stmp[640];
    char generated_window[600];
    char exe_sha[65], model_sha[65], window_sha[65];
    char golden_sha[65] = "", base_sha[65];
    const char *window_path = getenv("CNET_WINDOW_FILE");
    const char *golden_path = getenv("CNET_ORACLE_GOLDEN");
    unsigned long long wfnv = 1469598103934665603ULL;
    FILE *mf = NULL, *sf = NULL;
    size_t wi;
    long msz = 0;

    if (!window_path || !*window_path) {
        if (mf_write_window_sidecar(base_path, vocab, v, generated_window,
                                    sizeof generated_window) != 0) {
            fprintf(stderr, "manifest: cannot write canonical window sidecar\n");
            return -1;
        }
        window_path = generated_window;
    }
    if (!golden_path) golden_path = "";
    if (cce_sha256_file_hex(executable_path, exe_sha) != 0 ||
        cce_sha256_file_hex(model_path, model_sha) != 0 ||
        cce_sha256_file_hex(window_path, window_sha) != 0 ||
        cce_sha256_file_hex(base_path, base_sha) != 0 ||
        (*golden_path && cce_sha256_file_hex(golden_path, golden_sha) != 0)) {
        fprintf(stderr, "manifest: cannot fingerprint executable/model/window/"
                        "golden/base artifacts\n");
        return -1;
    }
    {
        FILE *mfp = fopen(model_path, "rb");
        if (mfp) {
            if (fseek(mfp, 0, SEEK_END) == 0) msz = ftell(mfp);
            fclose(mfp);
        }
    }
    for (wi = 0; wi < v; ++wi) {
        wfnv ^= (unsigned long long)(unsigned)vocab[wi];
        wfnv *= 1099511628211ULL;
    }

    snprintf(mpath, sizeof mpath, "%s.manifest.json", base_path);
    snprintf(mtmp, sizeof mtmp, "%s.tmp", mpath);
    mf = fopen(mtmp, "w");
    if (!mf) return -1;
    fprintf(mf, "{\n");
    fprintf(mf, "  \"provenance_version\": 1,\n");
    fprintf(mf, "  \"build_rev\": \"%s\",\n",
#ifdef CNET_BUILD_REV
            CNET_BUILD_REV
#else
            "unknown"
#endif
    );
    fprintf(mf, "  \"source_dirty\": %d,\n",
#ifdef CNET_SOURCE_DIRTY
            CNET_SOURCE_DIRTY ? 1 : 0
#else
            1
#endif
    );
    fprintf(mf, "  \"executable_sha256\": \"%s\",\n", exe_sha);
    fprintf(mf, "  \"model\": \"%s\",\n", model_path);
    fprintf(mf, "  \"model_bytes\": %ld,\n", msz);
    fprintf(mf, "  \"model_sha256\": \"%s\",\n", model_sha);
    fprintf(mf, "  \"tokenizer_model\": \"%s\",\n",
            am->transformer->tokenizer_model);
    fprintf(mf, "  \"bos_token_id\": %d,\n",
            am->transformer->bos_token_id);
    fprintf(mf, "  \"task\": \"%s\",\n",
            task == FLAGSHIP_TASK_PAIR ? "pair"
            : task == FLAGSHIP_TASK_TOPK ? "topk" : "argmax");
    fprintf(mf, "  \"target_semantics\": \"%s\",\n",
            (task == FLAGSHIP_TASK_TOPK && fs_topk_set_on())
                ? "top3-set-canonical" : "ordered");
    fprintf(mf, "  \"V\": %lu,\n", (unsigned long)v);
    fprintf(mf, "  \"max_units\": %lu,\n", (unsigned long)max_units);
    fprintf(mf, "  \"window_source\": \"%s\",\n", window_path);
    fprintf(mf, "  \"window_fnv\": \"%016llx\",\n", wfnv);
    fprintf(mf, "  \"window_sha256\": \"%s\",\n", window_sha);
    fprintf(mf, "  \"margin_eps\": %.6f,\n", ctx->margin_eps);
    fprintf(mf, "  \"cert_sampled\": %d,\n",
            getenv("CNET_CERT_SAMPLED") ? 1 : 0);
    fprintf(mf, "  \"sample_count\": %lu,\n",
            (unsigned long)cfg->acq.sample_count);
    fprintf(mf, "  \"min_accuracy_bound\": %.4f,\n",
            cfg->acq.min_accuracy_bound);
    fprintf(mf, "  \"student\": {\"init_hidden\": %lu, "
                "\"max_hidden\": %lu, \"max_epochs\": %lu, "
                "\"seed\": %u},\n",
            (unsigned long)cfg->acq.init_hidden,
            (unsigned long)cfg->acq.max_hidden,
            (unsigned long)cfg->acq.max_epochs, cfg->acq.seed);
    fprintf(mf, "  \"adaptive\": %d,\n",
            getenv("CNET_ACQ_ADAPTIVE") ? 1 : 0);
    fprintf(mf, "  \"warmstart\": %d,\n",
            getenv("CNET_ACQ_WARMSTART") ? 1 : 0);
    fprintf(mf, "  \"oracle_int8\": %d,\n",
            getenv("CNET_ORACLE_INT8") ? 1 : 0);
    fprintf(mf, "  \"oracle_golden\": \"%s\",\n", golden_path);
    fprintf(mf, "  \"oracle_golden_sha256\": \"%s\",\n", golden_sha);
    fprintf(mf, "  \"base\": \"%s\",\n", base_path);
    fprintf(mf, "  \"base_sha256\": \"%s\",\n", base_sha);
    fprintf(mf, "  \"lanes\": %lu\n", (unsigned long)ctx->nlanes);
    fprintf(mf, "}\n");
    if (fclose(mf) != 0 || rename(mtmp, mpath) != 0) {
        remove(mtmp); return -1;
    }

    snprintf(spath, sizeof spath, "%s.sha256", base_path);
    snprintf(stmp, sizeof stmp, "%s.tmp", spath);
    sf = fopen(stmp, "w");
    if (!sf) return -1;
    fprintf(sf, "%s  %s\n", base_sha, base_path);
    if (fclose(sf) != 0 || rename(stmp, spath) != 0) {
        remove(stmp); return -1;
    }
    printf("manifest: %s (strict provenance v1)\n", mpath);
    printf("base digest: %s\n", spath);
    return 0;
}

int main(int argc, char **argv) {
    const char *model_path;
    size_t V = 64, max_units = 8;
    int temp_c = 80;
    double duty = 0.75, wall = 0.0;
    const char *base_path = "flagship.cnb";
    char ledger_path[512];
    cce_anymodel *am = NULL;
    CceOracleCtx ctx;
    int *vocab;
    size_t i;
    int window_from_file;
    FlagshipConfig cfg;
    FlagshipReport rep;

    FlagshipTask task = FLAGSHIP_TASK_ARGMAX;

    /* Line-buffer stdout: mining runs live for hours under service managers
       with stdout redirected to a file; block buffering hides every
       progress line until exit. */
    setvbuf(stdout, NULL, _IOLBF, 0);

    /* Manifest replay: apply the recorded recipe before anything reads
       env or argv. Filename convention <base>.manifest.json also supplies
       the base path and teacher when the caller omits them. */
    {
        const char *mfp = getenv("CNET_MANIFEST");
        if (mfp) {
            char provenance_error[256];
#ifdef CNET_BUILD_REV
            const char *live_build_rev = CNET_BUILD_REV;
#else
            const char *live_build_rev = "unknown";
#endif
#ifdef CNET_SOURCE_DIRTY
            const int live_source_dirty = CNET_SOURCE_DIRTY;
#else
            const int live_source_dirty = 1;
#endif
            if (cce_campaign_provenance_verify(
                    mfp, argv[0], live_build_rev, live_source_dirty,
                    provenance_error, sizeof provenance_error) != 0) {
                fprintf(stderr, "CNET_MANIFEST %s: provenance refusal: %s\n",
                        mfp, provenance_error);
                return 1;
            }
            FILE *f = fopen(mfp, "rb");
            if (!f) {
                fprintf(stderr, "CNET_MANIFEST %s: cannot open\n", mfp);
                return 1;
            }
            {
                static char mtext[16384];
                static char m_model[512], m_task[16], m_win[512];
                static char m_base[560];
                size_t got = fread(mtext, 1, sizeof mtext - 1, f);
                double num;
                mtext[got] = 0;
                fclose(f);
                if (mf_scan_str(mtext, "window_source", m_win,
                                sizeof m_win) == 0 &&
                    strcmp(m_win, "argmax-discovery") != 0)
                    setenv("CNET_WINDOW_FILE", m_win, 0);
                mf_setenv_num(mtext, "margin_eps", "CNET_CERT_MARGIN", 0);
                if (mf_scan_num(mtext, "cert_sampled", &num) == 0 && num >= 1)
                    setenv("CNET_CERT_SAMPLED", "1", 0);
                mf_setenv_num(mtext, "sample_count",
                              "CNET_CERT_SAMPLE_COUNT", 1);
                mf_setenv_num(mtext, "init_hidden", "CNET_ACQ_HIDDEN", 1);
                mf_setenv_num(mtext, "max_hidden", "CNET_ACQ_MAXHIDDEN", 1);
                mf_setenv_num(mtext, "max_epochs", "CNET_ACQ_EPOCHS", 1);
                mf_setenv_num(mtext, "seed", "CNET_ACQ_SEED", 1);
                if (mf_scan_num(mtext, "adaptive", &num) == 0 && num >= 1)
                    setenv("CNET_ACQ_ADAPTIVE", "1", 0);
                if (mf_scan_num(mtext, "warmstart", &num) == 0 && num >= 1)
                    setenv("CNET_ACQ_WARMSTART", "1", 0);
                if (mf_scan_num(mtext, "oracle_int8", &num) == 0 && num >= 1)
                    setenv("CNET_ORACLE_INT8", "1", 0);
                mf_setenv_num(mtext, "lanes", "CNET_ORACLE_LANES", 1);
                {
                    static char m_sem[40];
                    if (mf_scan_str(mtext, "target_semantics", m_sem,
                                    sizeof m_sem) == 0 &&
                        strcmp(m_sem, "top3-set-canonical") == 0)
                        setenv("CNET_TOPK_SET", "1", 0);
                }
                {
                    static char m_gold[512];
                    if (mf_scan_str(mtext, "oracle_golden", m_gold,
                                    sizeof m_gold) == 0 && m_gold[0])
                        setenv("CNET_ORACLE_GOLDEN", m_gold, 0);
                }
                if (mf_scan_str(mtext, "model", m_model, sizeof m_model) == 0)
                    setenv("CNET_MANIFEST_MODEL", m_model, 1);
                if (mf_scan_str(mtext, "task", m_task, sizeof m_task) == 0)
                    setenv("CNET_MANIFEST_TASK", m_task, 1);
                if (mf_scan_num(mtext, "V", &num) == 0)
                    mf_setenv_num(mtext, "V", "CNET_MANIFEST_V", 1);
                if (mf_scan_num(mtext, "max_units", &num) == 0)
                    mf_setenv_num(mtext, "max_units", "CNET_MANIFEST_UNITS", 1);
                {
                    size_t blen = strlen(mfp);
                    const char *suffix = ".manifest.json";
                    size_t slen = strlen(suffix);
                    if (blen > slen &&
                        strcmp(mfp + blen - slen, suffix) == 0 &&
                        blen - slen < sizeof m_base) {
                        memcpy(m_base, mfp, blen - slen);
                        m_base[blen - slen] = 0;
                        setenv("CNET_MANIFEST_BASE", m_base, 1);
                    }
                }
                printf("manifest replay: %s\n", mfp);
            }
        }
    }

    if (argc < 2 && !getenv("CNET_MANIFEST_MODEL")) {
        fprintf(stderr, "usage: %s <model> [V] [max_units] [temp_C] [duty] "
                        "[wall_s] [base.cnb] [argmax|pair|topk]\n", argv[0]);
        return 2;
    }
    model_path = argc > 1 ? argv[1] : getenv("CNET_MANIFEST_MODEL");
    if (getenv("CNET_MANIFEST_V")) V = (size_t)atoi(getenv("CNET_MANIFEST_V"));
    if (getenv("CNET_MANIFEST_UNITS"))
        max_units = (size_t)atoi(getenv("CNET_MANIFEST_UNITS"));
    if (getenv("CNET_MANIFEST_BASE")) base_path = getenv("CNET_MANIFEST_BASE");
    if (getenv("CNET_MANIFEST_TASK")) {
        const char *mt = getenv("CNET_MANIFEST_TASK");
        if (strcmp(mt, "pair") == 0) task = FLAGSHIP_TASK_PAIR;
        else if (strcmp(mt, "topk") == 0) task = FLAGSHIP_TASK_TOPK;
    }
    if (argc > 2) V = (size_t)atoi(argv[2]);
    if (argc > 3) max_units = (size_t)atoi(argv[3]);
    if (argc > 4) temp_c = atoi(argv[4]);
    if (argc > 5) duty = atof(argv[5]);
    if (argc > 6) wall = atof(argv[6]);
    if (argc > 7) base_path = argv[7];
    if (argc > 8) {
        if (strcmp(argv[8], "pair") == 0) task = FLAGSHIP_TASK_PAIR;
        else if (strcmp(argv[8], "topk") == 0) task = FLAGSHIP_TASK_TOPK;
        else if (strcmp(argv[8], "argmax") != 0) {
            fprintf(stderr, "unknown task %s\n", argv[8]);
            return 2;
        }
    }
    if (V < 16 || V > 4096) { fprintf(stderr, "V must be 16..4096\n"); return 2; }

    if (cce_anymodel_open(&am, model_path) != CCE_OK || am->transformer == NULL) {
        fprintf(stderr, "cannot open a transformer runner for %s "
                        "(see: make detect FILE=%s)\n", model_path, model_path);
        return 1;
    }
    printf("model: %s  layers=%d hidden=%d vocab=%d\n", model_path,
           am->transformer->n_layer, am->transformer->n_embd,
           am->transformer->vocab_size);
    if ((int)(2000 + V) >= am->transformer->vocab_size) {
        fprintf(stderr, "vocab window exceeds model vocab\n");
        return 1;
    }

    vocab = (int *)malloc(V * sizeof *vocab);
    for (i = 0; i < V; ++i) vocab[i] = 2000 + (int)i;

    memset(&ctx, 0, sizeof ctx);
    {
        size_t li0;
        for (li0 = 0; li0 < FS_MAX_LANES; ++li0)
            cce_oracle_prefix_cache_init(&ctx.lane[li0].prefix_cache);
    }
    ctx.lane[0].m = am->transformer;
    ctx.nlanes = 1;
    ctx.force_lane = -1;
    /* margin-aware certification: CNET_CERT_MARGIN=<logit gap>. A domain
       point whose decision margin (smallest logit gap that would change the
       ordered answer) falls below this is ABSTAINED — the model's own
       near-ties are excluded from the certified domain instead of poisoning
       training and blocking certification. 0 (default) = off. */
    ctx.margin_eps = getenv("CNET_CERT_MARGIN")
                         ? atof(getenv("CNET_CERT_MARGIN")) : 0.0;
#ifdef _OPENMP
    ctx.inner_threads = omp_get_max_threads();
#endif
    ctx.vocab = vocab;
    ctx.v = V;
    ctx.lane[0].logits = (float *)malloc((size_t)am->transformer->vocab_size *
                                         sizeof *ctx.lane[0].logits);
    if (!ctx.lane[0].logits) { fprintf(stderr, "oom\n"); return 1; }

    flagship_config_defaults(&cfg);
    cfg.task = task;
    cfg.vocab_tokens = vocab;
    cfg.vocab_size = V;
    cfg.max_units = max_units;
    cfg.base_path = base_path;
    snprintf(ledger_path, sizeof ledger_path, "%s.gaps.txt", base_path);
    cfg.ledger_path = ledger_path;
    cfg.gpu_temp_limit_c = temp_c;
    cfg.duty_fraction = duty;
    cfg.max_wall_seconds = wall;
    window_from_file = 0;

    /* Recert windows come from the BASE'S OWN ledger: gaps order is
       window order, so an old base audits against exactly the tokens it
       was mined with — discovery under a changed oracle would produce a
       different window and audit nothing. CNET_WINDOW_FILE still
       overrides when a ledger is absent. */
    if (getenv("CNET_RECERT") && getenv("CNET_RECERT")[0] == '1' &&
        !getenv("CNET_WINDOW_FILE")) {
        char gpath[600];
        FILE *gf;
        snprintf(gpath, sizeof gpath, "%s.gaps.txt", base_path);
        gf = fopen(gpath, "r");
        if (gf) {
            char line[512];
            size_t got = 0;
            while (got < V && fgets(line, sizeof line, gf)) {
                const char *at = strstr(line, " tk");
                if (!at) continue;
                vocab[got] = atoi(at + 3);
                if (vocab[got] >= 0 &&
                    vocab[got] < am->transformer->vocab_size)
                    got++;
            }
            fclose(gf);
            if (got > 0) {
                V = got;
                window_from_file = 1;
                printf("recert window: %lu tokens from %s (ledger order)\n",
                       (unsigned long)V, gpath);
            }
        }
    }

    /* Explicit window: CNET_WINDOW_FILE=<path> supplies the V token ids
       (one decimal id per line) instead of argmax discovery — e.g. an
       English word window so downstream claim verification covers English
       text instead of the model's multilingual argmax attractors. Ids must
       be unique and inside the model vocab; the run aborts on a malformed
       file rather than silently mining the wrong units. Applies to every
       task including PAIR — an English pair window mines bigram
       conditionals over meaningful tokens. */
    if (!window_from_file && getenv("CNET_WINDOW_FILE")) {
        const char *wf = getenv("CNET_WINDOW_FILE");
        FILE *f = fopen(wf, "r");
        size_t got = 0, j;
        if (!f) {
            fprintf(stderr, "CNET_WINDOW_FILE %s: cannot open\n", wf);
            return 1;
        }
        while (got < V && fscanf(f, "%d", &vocab[got]) == 1) {
            /* -1 marks an untranslatable slot in a cross-recert window:
               kept to preserve slot alignment, skipped by the audit. Only
               legal in recert mode — mining needs every slot real. */
            if (vocab[got] == -1 &&
                getenv("CNET_RECERT") && getenv("CNET_RECERT")[0] == '1') {
                got++;
                continue;
            }
            if (vocab[got] < 0 ||
                vocab[got] >= am->transformer->vocab_size) {
                fprintf(stderr, "CNET_WINDOW_FILE: id %d outside model "
                                "vocab\n", vocab[got]);
                fclose(f);
                return 1;
            }
            got++;
        }
        fclose(f);
        if (got != V) {
            fprintf(stderr, "CNET_WINDOW_FILE: need %lu ids, got %lu\n",
                    (unsigned long)V, (unsigned long)got);
            return 1;
        }
        for (i = 0; i < V; ++i)
            for (j = i + 1; j < V; ++j)
                if (vocab[i] == vocab[j]) {
                    fprintf(stderr, "CNET_WINDOW_FILE: duplicate id %d\n",
                            vocab[i]);
                    return 1;
                }
        window_from_file = 1;
        printf("window: %lu tokens from %s (first: %d %d %d %d)\n",
               (unsigned long)V, wf, vocab[0], vocab[1], vocab[2], vocab[3]);
    }


    switch (task) {
    case FLAGSHIP_TASK_PAIR:
        cce_task_fn = cce_cond_pair;
        /* memorization-scale student for entangled pair lookups (the one
           documented tuning pass; certify_failed rates are the RESULT) */
        cfg.acq.init_hidden = 64;
        cfg.acq.max_hidden = 256;
        cfg.acq.max_epochs = 12000;
        cfg.acq.growth_window = 400;
        cfg.acq.holdout_fraction = 0.0;  /* exactness-on-sample is the bar */
        if (!window_from_file) {
            size_t placed = cnet_window_discover(am->transformer,
                                                 ctx.lane[0].logits,
                                                 vocab, V, 3,
                                                 fs_bos(am->transformer));
            printf("pair window: %lu/%lu tokens from the model's own "
                   "argmax distribution\n",
                   (unsigned long)placed, (unsigned long)V);
        } else {
            printf("pair window: %lu tokens from CNET_WINDOW_FILE\n",
                   (unsigned long)V);
        }
        break;
    case FLAGSHIP_TASK_TOPK:
        cce_task_fn = cce_cond_topk;
        /* memorization-scale student (same documented precedent as PAIR):
           a REAL oracle's top-3 slices are entangled ~40-class lookups —
           the lean-teacher default (hidden<=64, 4000 epochs) certify_fails
           on every unit. The NaN-era oracle "certified" only because its
           units were constant functions. */
        cfg.acq.init_hidden = 64;
        cfg.acq.max_hidden = 256;
        cfg.acq.max_epochs = 12000;
        cfg.acq.growth_window = 400;
        break;
    default:
        cce_task_fn = cce_cond_next;
        cfg.acq.init_hidden = 64;      /* same reasoning as TOPK above */
        cfg.acq.max_hidden = 256;
        cfg.acq.max_epochs = 12000;
        cfg.acq.growth_window = 400;
        break;
    }

    /* Certification tier. Default (exhaustive/PROVEN) demands EXACT match on
       every domain point — measured on the real gemma4 oracle, students
       reach 254-255/256, blocked from PROVEN by the model's OWN near-tie
       points (worst_margin ~0.001: irreducible residue, the doc's
       "no sharp boundary" wall). CNET_CERT_SAMPLED=1 engages the SAMPLED
       tier: mine a strict subset of the domain so coverage is SAMPLED, then
       certify with a Wilson lower bound (min_accuracy_bound, default 0.95) —
       high-confidence units instead of exact proofs. Exactness-on-sample is
       the bar (holdout 0), the PAIR precedent. */
    if (getenv("CNET_CERT_SAMPLED") && getenv("CNET_CERT_SAMPLED")[0] == '1') {
        size_t samp = (size_t)((double)V * 0.8);   /* 80% subsample -> SAMPLED */
        /* CNET_CERT_SAMPLE_COUNT: explicit subsample size. Wilson >= 0.95
           needs only ~96 all-correct points; the 0.8V default (204 at
           V=256) both doubles the oracle probing cost AND plants more of
           the teacher's coin-flip points in the sample than the bound
           requires. Smaller samples are faster and pass more often, at a
           lower (still >= min_accuracy_bound) statistical floor. */
        if (getenv("CNET_CERT_SAMPLE_COUNT")) {
            long sc = atol(getenv("CNET_CERT_SAMPLE_COUNT"));
            if (sc >= 16 && (size_t)sc < V) samp = (size_t)sc;
        }
        if (samp < 16) samp = (V < 16 ? V : 16);
        cfg.acq.sample_count = samp;
        cfg.acq.mine_budget = (samp > 1) ? samp - 1 : 1;  /* card>budget => sampled */
        cfg.acq.holdout_fraction = 0.0;
        printf("cert tier: SAMPLED (Wilson >= %.2f over a %lu/%lu-point "
               "subsample; exactness-on-sample)\n",
               cfg.acq.min_accuracy_bound, (unsigned long)samp,
               (unsigned long)V);
    }

    /* Student-capacity overrides (sweep the representational limit without
       recompiling): CNET_ACQ_HIDDEN / CNET_ACQ_MAXHIDDEN / CNET_ACQ_EPOCHS.
       Diagnostic for whether cert failures are the student's ceiling (they
       shrink with capacity) or the oracle's ambiguity (they don't). */
    if (getenv("CNET_ACQ_HIDDEN"))    cfg.acq.init_hidden = (size_t)atoi(getenv("CNET_ACQ_HIDDEN"));
    if (getenv("CNET_ACQ_MAXHIDDEN")) cfg.acq.max_hidden  = (size_t)atoi(getenv("CNET_ACQ_MAXHIDDEN"));
    if (getenv("CNET_ACQ_EPOCHS"))    cfg.acq.max_epochs  = (size_t)atoi(getenv("CNET_ACQ_EPOCHS"));
    /* Student seed override: the default (42) is FIXED, so re-running a
       deferred unit reproduces the identical near-miss deterministically.
       Retry passes over deferred units need a different seed per pass to
       give each student a fresh draw at exactness-on-sample. */
    if (getenv("CNET_ACQ_SEED"))      cfg.acq.seed = (unsigned)atoi(getenv("CNET_ACQ_SEED"));
    if (getenv("CNET_ACQ_HIDDEN") || getenv("CNET_ACQ_MAXHIDDEN") ||
        getenv("CNET_ACQ_EPOCHS"))
        printf("student: init_hidden=%lu max_hidden=%lu max_epochs=%lu\n",
               (unsigned long)cfg.acq.init_hidden,
               (unsigned long)cfg.acq.max_hidden,
               (unsigned long)cfg.acq.max_epochs);

    /* Window discovery for ARGMAX/TOPK (PAIR always discovered): the fixed
       2000..2000+V window is a MEASURED constant slice on both real models
       (window_discover.h) — every unit mined from it is one constant
       function. Discovery derives the window from the model's own argmax
       attractors, deterministically (resume-safe: same window, same tags).
       CNET_WINDOW_DISCOVER=0 restores the fixed window. */
    if (task != FLAGSHIP_TASK_PAIR && !window_from_file &&
        !(getenv("CNET_WINDOW_DISCOVER") &&
          getenv("CNET_WINDOW_DISCOVER")[0] == '0')) {
        size_t placed = cnet_window_discover(am->transformer,
                                             ctx.lane[0].logits, vocab, V, 2,
                                             fs_bos(am->transformer));
        printf("window: %lu/%lu tokens from the model's own argmax "
               "distribution (first: %d %d %d %d)\n", (unsigned long)placed,
               (unsigned long)V, vocab[0], vocab[1], vocab[2], vocab[3]);
        if (placed < V / 2)
            fprintf(stderr, "WARNING: only %lu distinct attractor tokens — "
                            "window padded with defaults beyond that\n",
                    (unsigned long)placed);
    }

    /* CNET_GPU=1: OpenCL forward, gated by an in-process equivalence sweep —
       a diverging oracle silently changes the meaning of extracted knowledge,
       so any decision mismatch refuses GPU mining outright.
       With >1 discrete GPU (and an OpenMP build), an oracle POOL is built:
       one model instance pinned per GPU, and mining calls the oracle
       width-wide (CNET_ORACLE_LANES caps it; 1 restores the single path). */
    if (getenv("CNET_GPU") && strcmp(getenv("CNET_GPU"), "1") == 0) {
        char dev[128] = {0};
        size_t lanes = 1;
        size_t ndev = 1;
        cce_clgemm *gpu = cce_clgemm_open(NULL, dev, sizeof dev);
        if (!gpu) {
            fprintf(stderr, "CNET_GPU=1 but no OpenCL GPU available\n");
            return 1;
        }
#ifdef _OPENMP
        lanes = cce_clgemm_device_count(gpu);
        ndev = lanes;
        if (getenv("CNET_ORACLE_LANES")) {
            long wl = atol(getenv("CNET_ORACLE_LANES"));
            /* may EXCEED the device count: with an int8 oracle
               (CNET_ORACLE_INT8) instances are small enough to pin
               several per GPU, and probe throughput scales with lanes */
            if (wl >= 1) lanes = (size_t)wl;
        }
        if (lanes > FS_MAX_LANES) lanes = FS_MAX_LANES;
#endif
        if (lanes > 1) {
            /* pool mode: per-lane single-device handles; the probe handle
               (which spans ALL devices) is not used for mining */
            size_t li;
            cce_clgemm_close(gpu);
            gpu = NULL;
            for (li = 0; li < lanes; ++li) {
                char dn[128] = {0};
                ctx.lane[li].gpu =
                    cce_clgemm_open_device(NULL, (int)(li % ndev), dn,
                                           sizeof dn);
                if (!ctx.lane[li].gpu) {
                    fprintf(stderr, "lane %lu: cannot open GPU device\n",
                            (unsigned long)li);
                    return 1;
                }
                if (li > 0) {
                    cce_anymodel *am2 = NULL;
                    if (cce_anymodel_open(&am2, model_path) != CCE_OK ||
                        am2->transformer == NULL) {
                        fprintf(stderr, "lane %lu: cannot re-open %s\n",
                                (unsigned long)li, model_path);
                        return 1;
                    }
                    ctx.lane[li].am = am2;
                    ctx.lane[li].m = am2->transformer;
                    ctx.lane[li].logits = (float *)malloc(
                        (size_t)am2->transformer->vocab_size *
                        sizeof *ctx.lane[li].logits);
                    if (!ctx.lane[li].logits) { fprintf(stderr, "oom\n"); return 1; }
                }
                printf("lane %lu: %s\n", (unsigned long)li, dn);
            }
            ctx.nlanes = lanes;
        }
        {
            /* per-lane sweep: every lane's argmax must match the CPU path
               (CPU reference computed BEFORE any handle is attached) */
            float *lg = (float *)malloc(
                (size_t)am->transformer->vocab_size * sizeof *lg);
            size_t cpu_am[16];
            size_t jj, li, mismatch = 0;
            if (!lg) return 1;
            for (jj = 0; jj < 16; ++jj) {
                int tk[3];
                int nt = 0;
                size_t a, am_c = 0;
                if (fs_bos(am->transformer) >= 0)
                    tk[nt++] = fs_bos(am->transformer);
                tk[nt++] = 2000 + (int)((jj * 37u) % 4096u);
                tk[nt++] = 2000 + (int)((jj * 91u + 17u) % 4096u);
                am->transformer->cur_pos = 0;
                cce_gguf_qwen2_forward(am->transformer, tk, nt,
                                       ctx.lane[0].logits,
                                       am->transformer->vocab_size);
                for (a = 0; a < (size_t)am->transformer->vocab_size; ++a) {
                    if (ctx.lane[0].logits[a] != ctx.lane[0].logits[a]) {
                        fprintf(stderr, "NaN logit in CPU reference — "
                                        "forward is broken; refusing to "
                                        "mine\n");
                        return 1;
                    }
                    if (ctx.lane[0].logits[a] > ctx.lane[0].logits[am_c])
                        am_c = a;
                }
                cpu_am[jj] = am_c;
            }
            /* attach handles (pool: per instance; single: process-global) */
            if (ctx.nlanes > 1) {
                for (li = 0; li < ctx.nlanes; ++li)
                    cce_gguf_qwen2_set_clgemm(ctx.lane[li].m,
                                              ctx.lane[li].gpu);
            } else {
                cce_gguf_set_clgemm(gpu);
            }
            for (li = 0; li < ctx.nlanes && !mismatch; ++li) {
                for (jj = 0; jj < 16 && !mismatch; ++jj) {
                    int tk[3];
                    int nt = 0;
                    size_t a, am_g = 0;
                    if (fs_bos(ctx.lane[li].m) >= 0)
                        tk[nt++] = fs_bos(ctx.lane[li].m);
                    tk[nt++] = 2000 + (int)((jj * 37u) % 4096u);
                    tk[nt++] = 2000 + (int)((jj * 91u + 17u) % 4096u);
                    ctx.lane[li].m->cur_pos = 0;
                    cce_gguf_qwen2_forward(ctx.lane[li].m, tk, nt, lg,
                                           ctx.lane[li].m->vocab_size);
                    for (a = 0; a < (size_t)ctx.lane[li].m->vocab_size; ++a) {
                        if (lg[a] != lg[a]) {
                            fprintf(stderr, "NaN logit on lane %lu — "
                                            "forward is broken; refusing "
                                            "to mine\n", (unsigned long)li);
                            return 1;
                        }
                        if (lg[a] > lg[am_g]) am_g = a;
                    }
                    if (am_g != cpu_am[jj]) mismatch = 1;
                }
            }
            free(lg);
            if (mismatch) {
                fprintf(stderr, "GPU EQUIVALENCE FAILED — refusing to mine "
                                "with --gpu (run gpu_equiv for details)\n");
                return 1;
            }
        }
#ifdef _OPENMP
        if (ctx.nlanes > 1) {
            /* nested teams: `lanes` outer oracle threads x `inner` CPU tile
               threads per forward — sized to the machine, not stacked */
            omp_set_max_active_levels(2);
            ctx.inner_threads = omp_get_max_threads() / (int)ctx.nlanes;
            if (ctx.inner_threads < 1) ctx.inner_threads = 1;
        }
#endif
        if (ctx.nlanes > 1)
            printf("gpu: %s — oracle pool of %lu lanes (equivalence sweep OK "
                   "on every lane, inner CPU teams %d)\n",
                   dev, (unsigned long)ctx.nlanes, ctx.inner_threads);
        else
            printf("gpu: %s (equivalence sweep OK, %0.1f MB will go resident)\n",
                   dev, 0.0);
    }

    if (determinism_spot_check(&ctx, task) != 0) {
        fprintf(stderr, "DETERMINISM CHECK FAILED: the forward pass is not "
                        "reproducible across calls (KV state leak?) — refusing "
                        "to mine from a nondeterministic oracle\n");
        return 1;
    }
    printf("determinism spot check: OK\n");

    /* Oracle self-check gate (the Q4_K lesson): certification guarantees
       student == oracle, but nothing above guarantees oracle == model — a
       silent dequant/forward regression mines confidently against garbage
       and every seal downstream is worthless. CNET_ORACLE_GOLDEN=<file>
       replays a fixed 32-probe battery (full-vocab argmax) BEFORE mining
       and refuses the run on any mismatch. Regenerate the goldens on a
       TRUSTED build with CNET_ORACLE_GOLDEN_WRITE=1. */
    if (getenv("CNET_ORACLE_GOLDEN")) {
        const char *gf = getenv("CNET_ORACLE_GOLDEN");
        int gwrite = getenv("CNET_ORACLE_GOLDEN_WRITE") &&
                     getenv("CNET_ORACLE_GOLDEN_WRITE")[0] == '1';
        float *lg = ctx.lane[0].logits;
        int bos = fs_bos(am->transformer);
        FILE *gfp = fopen(gf, gwrite ? "w" : "r");
        size_t jj, bad = 0, total = 0;
        if (!gfp) {
            fprintf(stderr, "oracle golden %s: cannot open\n", gf);
            return 1;
        }
        if (gwrite) {
            for (jj = 0; jj < 32; ++jj) {
                int tk[3];
                int nt = 0, vi, bestid = 0;
                int a = 2000 + (int)((jj * 37u) % 4096u);
                int b = 2000 + (int)((jj * 101u + 13u) % 4096u);
                if (bos >= 0) tk[nt++] = bos;
                tk[nt++] = a;
                tk[nt++] = b;
                am->transformer->cur_pos = 0;
                if (cce_gguf_qwen2_forward(am->transformer, tk, nt, lg,
                                           am->transformer->vocab_size)
                    != CCE_OK) {
                    fprintf(stderr, "oracle golden: forward failed\n");
                    fclose(gfp);
                    return 1;
                }
                for (vi = 1; vi < am->transformer->vocab_size; ++vi)
                    if (lg[vi] > lg[bestid]) bestid = vi;
                fprintf(gfp, "%d %d -> %d\n", a, b, bestid);
            }
            printf("oracle golden: wrote 32 probes to %s\n", gf);
        } else {
            int a, b, want;
            while (fscanf(gfp, "%d %d -> %d", &a, &b, &want) == 3) {
                int tk[3];
                int nt = 0, vi, bestid = 0;
                if (bos >= 0) tk[nt++] = bos;
                tk[nt++] = a;
                tk[nt++] = b;
                am->transformer->cur_pos = 0;
                if (cce_gguf_qwen2_forward(am->transformer, tk, nt, lg,
                                           am->transformer->vocab_size)
                    != CCE_OK) {
                    fprintf(stderr, "oracle golden: forward failed\n");
                    fclose(gfp);
                    return 1;
                }
                for (vi = 1; vi < am->transformer->vocab_size; ++vi)
                    if (lg[vi] > lg[bestid]) bestid = vi;
                total++;
                if (bestid != want) bad++;
            }
            if (total == 0) {
                fprintf(stderr, "oracle golden: %s holds no probes — refusing "
                                "to mine unchecked\n", gf);
                fclose(gfp);
                return 1;
            }
            if (bad > 0) {
                fprintf(stderr, "oracle golden: %lu/%lu probes MISMATCH — the "
                                "oracle no longer reproduces its trusted "
                                "behavior, refusing to mine\n",
                        (unsigned long)bad, (unsigned long)total);
                fclose(gfp);
                return 1;
            }
            printf("oracle golden: %lu/%lu probes match\n",
                   (unsigned long)total, (unsigned long)total);
        }
        fclose(gfp);
        /* The battery's DIRECT forwards clobbered the model stream, but the
           lane still remembers the determinism check's prefix — and the
           check's t is vocab[0], the SAME token unit 1 mines. A stale
           prefix cache makes fs_prefix skip the recompute and mine unit 1
           against the LAST GOLDEN PAIR's state (KV row 0 + recurrent
           state): a self-consistent chimera that certifies its own student.
           Every goldens-enabled campaign to date poisoned exactly its first
           unit this way (caught 2026-07-15 by the qwen35 runner's rewind
           refusal turning the silent corruption into oracle_unfit).
           Invalidate the cache: any foreign forward = prefix gone. */
        {
            size_t li;
            for (li = 0; li < ctx.nlanes; ++li)
                cce_oracle_prefix_cache_invalidate(
                    &ctx.lane[li].prefix_cache);
        }
    }

    /* Window decisiveness screening (CNET_WINDOW_SCREEN=<candidates>):
       measure how decisive the teacher is on each candidate token BEFORE
       spending training budget — for each candidate t, probe a fixed
       stride of contexts [BOS, t, w] and score the mean top-3 boundary
       margin over the candidate pool. The V highest scorers are written
       to CNET_WINDOW_SCREEN_OUT (default <candidates>.screened) and the
       process exits: mine them with CNET_WINDOW_FILE=<that file>. The
       pool-relative margin is a heuristic for any final subwindow, but a
       token indecisive against the pool never becomes decisive inside
       it. */
    if (getenv("CNET_WINDOW_SCREEN")) {
        const char *cf = getenv("CNET_WINDOW_SCREEN");
        const char *of = getenv("CNET_WINDOW_SCREEN_OUT");
        char ofbuf[512];
        int *cand = NULL;
        double *score = NULL;
        size_t ncand = 0, ci, wi, probes;
        FILE *cfp;
        if (!of) {
            snprintf(ofbuf, sizeof ofbuf, "%s.screened", cf);
            of = ofbuf;
        }
        probes = getenv("CNET_SCREEN_PROBES")
                     ? (size_t)atoi(getenv("CNET_SCREEN_PROBES")) : 12;
        if (probes < 4) probes = 4;
        cfp = fopen(cf, "r");
        if (!cfp) {
            fprintf(stderr, "window screen %s: cannot open\n", cf);
            return 1;
        }
        cand = (int *)malloc(8192 * sizeof *cand);
        score = (double *)malloc(8192 * sizeof *score);
        if (!cand || !score) { fclose(cfp); return 1; }
        while (ncand < 8192 && fscanf(cfp, "%d", &cand[ncand]) == 1) {
            if (cand[ncand] >= 0 &&
                cand[ncand] < am->transformer->vocab_size)
                ncand++;
        }
        fclose(cfp);
        if (ncand < V) {
            fprintf(stderr, "window screen: %lu candidates < V=%lu\n",
                    (unsigned long)ncand, (unsigned long)V);
            return 1;
        }
        printf("window screen: %lu candidates, %lu probes each\n",
               (unsigned long)ncand, (unsigned long)probes);
        cce_gguf_qwen2_set_head_window(am->transformer, cand, (int)ncand);
        {
            float *lg = ctx.lane[0].logits;
            int bos = fs_bos(am->transformer);
            for (ci = 0; ci < ncand; ++ci) {
                double sum = 0.0;
                for (wi = 0; wi < probes; ++wi) {
                    int tk[3];
                    int nt = 0;
                    size_t w = (ci * 7919u + wi * (ncand / probes | 1))
                               % ncand;
                    if (bos >= 0) tk[nt++] = bos;
                    tk[nt++] = cand[ci];
                    tk[nt++] = cand[w];
                    am->transformer->cur_pos = 0;
                    if (cce_gguf_qwen2_forward(am->transformer, tk, nt, lg,
                                               am->transformer->vocab_size)
                        != CCE_OK)
                        continue;
                    sum += fs_set_margin(lg, cand, ncand, 3);
                }
                score[ci] = sum / (double)probes;
            }
        }
        {
            /* selection sort of the top V by score (V is small) */
            FILE *ofp = fopen(of, "w");
            size_t r2;
            if (!ofp) {
                fprintf(stderr, "window screen: cannot write %s\n", of);
                return 1;
            }
            for (r2 = 0; r2 < V; ++r2) {
                size_t bi = 0;
                for (ci = 1; ci < ncand; ++ci)
                    if (score[ci] > score[bi]) bi = ci;
                fprintf(ofp, "%d\n", cand[bi]);
                score[bi] = -1e30;
            }
            fclose(ofp);
        }
        printf("window screen: wrote the %lu most decisive tokens to %s — "
               "mine them with CNET_WINDOW_FILE=%s\n",
               (unsigned long)V, of, of);
        free(cand);
        free(score);
        return 0;
    }

    /* Restrict every lane's head to the mined window now that the full-head
       startup gates (equivalence sweep, determinism check) have passed:
       mining forwards then skip the [D x 262144] head GEMM and compute only
       the |V| window logits the oracle reads — bit-identical, ~1/4 off each
       forward. CNET_HEAD_WINDOW=0 keeps the full head. */
    if (!(getenv("CNET_HEAD_WINDOW") && getenv("CNET_HEAD_WINDOW")[0] == '0')) {
        size_t li;
        for (li = 0; li < ctx.nlanes; ++li)
            cce_gguf_qwen2_set_head_window(ctx.lane[li].m, vocab, (int)V);
        printf("head window: restricted to %lu mined tokens (full-vocab head "
               "off for mining)\n", (unsigned long)V);
    }

    /* Margin-distribution sweep (CNET_MARGIN_SWEEP=<out.tsv>): the v2
       lever measurement. For every (unit t, continuation w) mining probe —
       the exact pinned-prefix context cce_cond_topk sees — record BOTH
       margins that decide abstention: the ordered top-3 decision margin
       (today's criterion) and the top-3 SET margin (the rank-3/rank-4 gap,
       CNET_TOPK_SET's criterion). No training budget is spent and the base
       is never touched: one row per probe, then exit. Offline analysis
       reproduces the acquire gate exactly (usable/attempts >= evidence
       threshold at any eps), so ordered-vs-set yield AND the decisiveness
       ranking window screening would sort by come from ONE dataset.
       CNET_SWEEP_UNITS=<n> limits to the first n units (timing samples).
       CNET_SWEEP_START=<u> resumes an interrupted sweep: appends to the
       TSV starting at unit index u (the caller strips any partial trailing
       unit first — the analyzer refuses partial units either way). */
    if (getenv("CNET_MARGIN_SWEEP")) {
        const char *of = getenv("CNET_MARGIN_SWEEP");
        OracleLane *L = &ctx.lane[0];
        FILE *ofp;
        float *bl;
        size_t nu = V, ui, u0 = 0;
        double t0;
        if (fs_prefix_off()) {
            fprintf(stderr, "margin sweep: rides the pinned prefix "
                            "(CNET_ORACLE_PREFIX=0 unsupported)\n");
            return 1;
        }
        if (getenv("CNET_SWEEP_UNITS")) {
            long n = atol(getenv("CNET_SWEEP_UNITS"));
            if (n > 0 && (size_t)n < nu) nu = (size_t)n;
        }
        if (getenv("CNET_SWEEP_START")) {
            long n = atol(getenv("CNET_SWEEP_START"));
            if (n > 0 && (size_t)n < nu) u0 = (size_t)n;
        }
        ofp = fopen(of, u0 ? "a" : "w");
        if (!ofp) {
            fprintf(stderr, "margin sweep: cannot write %s\n", of);
            return 1;
        }
        /* scratch sized to the probe CHUNK (<= 256 rows, the batch API's
           cap), not V: at the V=4096 ceiling a V-row buffer would be a
           multi-GiB request of which only 256 rows are ever touched */
        bl = (float *)malloc((V < 256 ? V : 256)
                             * (size_t)L->m->vocab_size * sizeof *bl);
        if (!bl) { fclose(ofp); return 1; }
        if (u0 == 0) {
            /* int8 stamp mirrors gguf_oracle_int8_mode ('1' enables), not
               mere env presence — CNET_ORACLE_INT8=0 must stamp int8=0 */
            const char *i8 = getenv("CNET_ORACLE_INT8");
            fprintf(ofp, "# margin_sweep model=%s V=%lu units=%lu eps=%.6f "
                         "prefix=on int8=%s\n",
                    model_path, (unsigned long)V, (unsigned long)nu,
                    ctx.margin_eps, (i8 && i8[0] == '1') ? "1" : "0");
            fprintf(ofp, "unit_idx\tunit_token\tw_idx\tw_token\t"
                         "ordered_margin\tset_margin\n");
        } else {
            printf("margin sweep: resuming at unit %lu (append)\n",
                   (unsigned long)u0);
        }
        t0 = omp_get_wtime();
        for (ui = u0; ui < nu; ++ui) {
            int t = vocab[ui];
            size_t done = 0;
            while (done < V) {
                size_t nb = V - done, b;
                int toks[256];
                if (nb > 256) nb = 256;
                for (b = 0; b < nb; ++b) toks[b] = vocab[done + b];
                if (fs_prefix(L, t) != 0) {
                    fprintf(stderr, "margin sweep: prefix failed at unit "
                                    "%lu\n", (unsigned long)ui);
                    fclose(ofp); free(bl);
                    return 1;
                }
                L->m->cur_pos = fs_prefix_len(L->m);
                if (cce_gguf_qwen2_forward_probes(L->m, toks, (int)nb, bl,
                                                  L->m->vocab_size)
                    != CCE_OK) {
                    fprintf(stderr, "margin sweep: probe failed at unit "
                                    "%lu\n", (unsigned long)ui);
                    fclose(ofp); free(bl);
                    return 1;
                }
                for (b = 0; b < nb; ++b) {
                    const float *lg = bl + b * (size_t)L->m->vocab_size;
                    fprintf(ofp, "%lu\t%d\t%lu\t%d\t%.9g\t%.9g\n",
                            (unsigned long)ui, t,
                            (unsigned long)(done + b), toks[b],
                            fs_decision_margin(lg, vocab, V, 3),
                            fs_set_margin(lg, vocab, V, 3));
                }
                done += nb;
            }
            {
                double el = omp_get_wtime() - t0;
                /* the TSV flushes per unit so an external kill loses at
                   most the in-flight unit, and the progress line can never
                   claim rows the file does not hold */
                fflush(ofp);
                printf("margin sweep: unit %lu/%lu (tk%d) elapsed %.0fs "
                       "eta %.0fs\n", (unsigned long)(ui + 1),
                       (unsigned long)nu, t, el,
                       el / (double)(ui + 1 - u0)
                           * (double)(nu - ui - 1));
                fflush(stdout);
            }
        }
        fclose(ofp);
        free(bl);
        printf("margin sweep: wrote %lu units x %lu probes to %s\n",
               (unsigned long)nu, (unsigned long)V, of);
        return 0;
    }

    /* Drift audit (CNET_RECERT=1): certification froze student == oracle
       at MINING time; nothing checks that the oracle still stands behind
       those answers after a loader fix or a teacher swap. Replay every
       acquired unit's sealed exemplar table against the CURRENT oracle and
       report per-unit agreement. The base's gaps ledger supplies the
       window (ledger order IS window order), so old bases audit without
       their original environment. Margin abstentions count as excluded
       domain, not drift. Report: <base>.recert.txt + stdout; no mining. */
    if (getenv("CNET_RECERT") && getenv("CNET_RECERT")[0] == '1') {
        CnetBase rbase;
        FILE *rout;
        char rpath[600];
        size_t ui;
        int *name_ids = vocab;
        int *name_buf = NULL;
        /* Cross-recert: unit NAMES belong to the source teacher's vocab,
           probe ids to the current one. CNET_RECERT_NAMES=<source window>
           supplies the naming ids slot-for-slot; probe slots translated to
           -1 (no such token in this vocab) are skipped as untranslatable. */
        if (getenv("CNET_RECERT_NAMES")) {
            FILE *nf = fopen(getenv("CNET_RECERT_NAMES"), "r");
            size_t got = 0;
            name_buf = (int *)malloc(V * sizeof *name_buf);
            if (!nf || !name_buf) {
                fprintf(stderr, "recert: cannot read CNET_RECERT_NAMES\n");
                return 1;
            }
            while (got < V && fscanf(nf, "%d", &name_buf[got]) == 1) got++;
            fclose(nf);
            if (got != V) {
                fprintf(stderr, "recert: name window has %lu ids, need "
                                "%lu\n", (unsigned long)got,
                        (unsigned long)V);
                return 1;
            }
            name_ids = name_buf;
        }
        size_t units_seen = 0, units_clean = 0, units_drift = 0;
        size_t ex_match = 0, ex_miss = 0, ex_abst = 0, ex_err = 0;
        size_t ex_overlap = 0;
        cnb_init(&rbase);
        if (cnb_load(&rbase, base_path) != 0) {
            fprintf(stderr, "recert: cannot load base %s\n", base_path);
            return 1;
        }
        snprintf(rpath, sizeof rpath, "%s.recert.txt", base_path);
        rout = fopen(rpath, "w");
        printf("recert: auditing %s against the current oracle\n", base_path);
        for (ui = 0; ui < V; ++ui) {
            char uname[64];
            BinaryTransformNetwork ubtn;
            Contract uc;
            size_t e, in_total, out_total;
            size_t u_match = 0, u_miss = 0, u_abst = 0, u_err = 0;
            size_t u_overlap = 0;
            double *got;
            if (vocab[ui] < 0) continue;   /* untranslatable slot */
            snprintf(uname, sizeof uname, "acq_tk%dq%d",
                     name_ids[ui], name_ids[ui]);
            memset(&ubtn, 0, sizeof ubtn);
            memset(&uc, 0, sizeof uc);
            if (cnb_get_unit(&rbase, uname, &ubtn, &uc) != 0) continue;
            units_seen++;
            in_total = fs_ports_total(uc.input_ports, uc.input_port_count);
            out_total = fs_ports_total(uc.output_ports, uc.output_port_count);
            got = (double *)malloc(out_total * sizeof *got);
            ctx.t = vocab[ui];
            for (e = 0; e < uc.exemplar_count && got; ++e) {
                const double *irow = uc.inputs + e * in_total;
                const double *orow = uc.outputs + e * out_total;
                int rc2 = cce_task_fn(irow, got, &ctx);
                if (rc2 > 0) { u_abst++; continue; }
                if (rc2 < 0) { u_err++; continue; }
                /* CNET_RECERT_SETCMP=1: compare the top-3 as UNORDERED sets.
                   Same-teacher audits want exact replay (default memcmp);
                   cross-teacher audits ask a different question — do two
                   independent models even agree on WHICH tokens follow —
                   and exact rank order across models is near-zero by
                   construction. */
                if (getenv("CNET_RECERT_SETCMP") &&
                    getenv("CNET_RECERT_SETCMP")[0] == '1' &&
                    out_total % 3 == 0) {
                    size_t fw = out_total / 3, fi, gi;
                    size_t gp[3], op[3], tmp2;
                    for (fi = 0; fi < 3; ++fi) {
                        gp[fi] = 0; op[fi] = 0;
                        for (gi = 1; gi < fw; ++gi) {
                            if (got[fi * fw + gi] > got[fi * fw + gp[fi]])
                                gp[fi] = gi;
                            if (orow[fi * fw + gi] > orow[fi * fw + op[fi]])
                                op[fi] = gi;
                        }
                    }
                    for (fi = 0; fi < 2; ++fi)
                        for (gi = 0; gi < 2 - fi; ++gi) {
                            if (gp[gi] > gp[gi + 1]) { tmp2 = gp[gi]; gp[gi] = gp[gi + 1]; gp[gi + 1] = tmp2; }
                            if (op[gi] > op[gi + 1]) { tmp2 = op[gi]; op[gi] = op[gi + 1]; op[gi + 1] = tmp2; }
                        }
                    {
                        /* overlap = |got-set ∩ stored-set| in 0..3 — the
                           real cross-teacher agreement signal. Full match
                           counts as before; partial overlap accumulates
                           for the mean reported per unit. */
                        size_t ov = 0, ai, bi2;
                        for (ai = 0; ai < 3; ++ai)
                            for (bi2 = 0; bi2 < 3; ++bi2)
                                if (gp[ai] == op[bi2]) { ov++; break; }
                        u_overlap += ov;
                        if (ov == 3) u_match++; else u_miss++;
                    }
                    continue;
                }
                if (memcmp(got, orow, out_total * sizeof *got) == 0) u_match++;
                else u_miss++;
            }
            free(got);
            if (u_miss == 0 && u_err == 0) units_clean++; else units_drift++;
            ex_match += u_match; ex_miss += u_miss;
            ex_abst += u_abst; ex_err += u_err;
            ex_overlap += u_overlap;
            if (rout)
                fprintf(rout, "%s match=%lu miss=%lu abstain=%lu err=%lu "
                              "mean_overlap=%.3f\n",
                        uname, (unsigned long)u_match, (unsigned long)u_miss,
                        (unsigned long)u_abst, (unsigned long)u_err,
                        (u_match + u_miss) > 0
                            ? (double)u_overlap / (double)(u_match + u_miss)
                            : 0.0);
            if (u_miss > 0 || u_err > 0)
                printf("recert DRIFT %s: match=%lu miss=%lu abstain=%lu "
                       "err=%lu\n", uname, (unsigned long)u_match,
                       (unsigned long)u_miss, (unsigned long)u_abst,
                       (unsigned long)u_err);
            btn_free(&ubtn);
            contract_free(&uc);
        }
        printf("recert: %lu units audited — %lu clean, %lu drifted; "
               "exemplars match=%lu miss=%lu abstain=%lu err=%lu "
               "mean_top3_overlap=%.3f\n",
               (unsigned long)units_seen, (unsigned long)units_clean,
               (unsigned long)units_drift, (unsigned long)ex_match,
               (unsigned long)ex_miss, (unsigned long)ex_abst,
               (unsigned long)ex_err,
               (ex_match + ex_miss) > 0
                   ? (double)ex_overlap / (double)(ex_match + ex_miss)
                   : 0.0);
        if (rout) {
            fprintf(rout, "TOTAL units=%lu clean=%lu drifted=%lu "
                          "match=%lu miss=%lu abstain=%lu err=%lu\n",
                    (unsigned long)units_seen, (unsigned long)units_clean,
                    (unsigned long)units_drift, (unsigned long)ex_match,
                    (unsigned long)ex_miss, (unsigned long)ex_abst,
                    (unsigned long)ex_err);
            fclose(rout);
            printf("recert report: %s\n", rpath);
        }
        cnb_free(&rbase);
        free(name_buf);
        return units_drift > 0 ? 2 : 0;
    }

    /* Batched-oracle equivalence gate: the same 16 probes through the
       serial oracle and ONE batched call must agree bit-exactly — verdicts
       and encoded rows. Bit-equality is the design property (every row's
       math is independent of its neighbors); any drift means the batch
       path is broken and mining with it would certify the wrong teacher.
       Refused loudly, like every other oracle gate. */
    if (task == FLAGSHIP_TASK_TOPK && fs_batch_hint() > 0 &&
        !fs_prefix_off()) {
        size_t np = 16, pi, mismatches = 0;
        double *g_in = (double *)calloc(np * V, sizeof(double));
        double *g_ser = (double *)calloc(np * 3u * V, sizeof(double));
        double *g_bat = (double *)calloc(np * 3u * V, sizeof(double));
        int r_ser[16], r_bat[16];
        if (!g_in || !g_ser || !g_bat) {
            fprintf(stderr, "batch gate: OOM\n");
            return 1;
        }
        ctx.t = vocab[0];
        ctx.force_lane = 0;
        for (pi = 0; pi < np; ++pi) {
            g_in[pi * V + (pi * 17u) % V] = 1.0;
            r_ser[pi] = cce_cond_topk(g_in + pi * V, g_ser + pi * 3u * V,
                                      &ctx);
        }
        if (cce_cond_topk_batch(g_in, g_bat, r_bat, np, &ctx) != 0) {
            fprintf(stderr, "batch gate: batched call FAILED — refusing "
                            "to mine with CNET_ORACLE_BATCH\n");
            return 1;
        }
        for (pi = 0; pi < np; ++pi) {
            if (r_ser[pi] != r_bat[pi]) { mismatches++; continue; }
            if (r_ser[pi] == 0 &&
                memcmp(g_ser + pi * 3u * V, g_bat + pi * 3u * V,
                       3u * V * sizeof(double)) != 0)
                mismatches++;
        }
        ctx.force_lane = -1;
        free(g_in); free(g_ser); free(g_bat);
        if (mismatches > 0) {
            fprintf(stderr, "batch gate: %lu/%lu probes DIVERGE between "
                            "serial and batched oracles — refusing\n",
                    (unsigned long)mismatches, (unsigned long)np);
            return 1;
        }
        printf("batch oracle: equivalence OK (%lu probes, batch<=%lu)\n",
               (unsigned long)np, (unsigned long)fs_batch_hint());
    }

    printf("run: task=%s V=%lu max_units=%lu temp<=%dC duty=%.2f wall=%.0fs base=%s\n",
           task == FLAGSHIP_TASK_PAIR ? "pair"
           : task == FLAGSHIP_TASK_TOPK ? "topk" : "argmax",
           (unsigned long)V, (unsigned long)max_units, temp_c, duty, wall,
           base_path);
    printf("stop cleanly at any time:  echo stop > %s.stop\n", base_path);

    if (flagship_run(&cfg, cce_maker, &ctx, &rep) != 0) {
        fprintf(stderr, "flagship_run failed to start\n");
        return 1;
    }
    /* Only a completed run publishes a manifest. The manifest fingerprints
       the resulting base, not the pre-run input, and is atomically replaced. */
    if (mf_write_run_manifest(base_path, argv[0], model_path, task, V,
                              max_units, vocab, &ctx, &cfg, am) != 0) {
        fprintf(stderr, "flagship_run completed but strict provenance "
                        "publication failed\n");
        return 1;
    }
    flagship_print_report(&rep, stdout);

    /* the decisive number */
    if (rep.attempted > 0) {
        printf("extraction rate this run: %lu/%lu (%.1f%%)\n",
               (unsigned long)rep.acquired, (unsigned long)rep.attempted,
               100.0 * (double)rep.acquired / (double)rep.attempted);
    }

    {
        size_t li;
        for (li = 1; li < ctx.nlanes; ++li) {
            free(ctx.lane[li].logits);
            if (ctx.lane[li].am) cce_anymodel_free(ctx.lane[li].am);
        }
        for (li = 0; li < ctx.nlanes; ++li)
            if (ctx.lane[li].gpu) cce_clgemm_close(ctx.lane[li].gpu);
    }
    free(ctx.lane[0].logits);
    free(vocab);
    cce_anymodel_free(am);
    return 0;
}
