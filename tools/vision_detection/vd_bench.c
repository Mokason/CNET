/* vd_bench — CNET-native detection head on real VOC2007 proposals.
 *
 * The learned component under test is a CNET BTN (PORT_RAW 64-D features ->
 * PORT_ONEHOT 2 background/car). Everything upstream (Selective Search
 * proposals, HOG, train-only PCA) is fixed by vd_prep and identical across every
 * arm, so an arm-to-arm difference isolates the head.
 *
 * Controls: randomized/untrained head, a linear logistic head on the SAME
 * features, a label-shuffle control, and the proposal recall ceiling.
 *
 * Protocol: plans/cnet_vision_object_detection_20260727.md
 * CPU only. No GPU calls anywhere in this path.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "vd_eval.h"
#include "vd_io.h"
#include "vd_pack.h"
#include "vd_protocol.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/wait.h>
#include <poll.h>
#include <signal.h>
#include <errno.h>
#include "vd_sha256.h"
#include "../../include/nn.h"
#include "../../include/router.h"
#include "../../include/contract/contract.h"
#include "../../include/specialist.h"

/* Feature width is a property of the cache, not of this file: V1 packs are
   64-D (32x32 gray HOG), V2 packs are 256-D (64x64 colour HOG). Read from the
   pack header and asserted against DIM_MAX. */
#define DIM_MAX 256
#define NCLS 2

static size_t DIM = 0;
static int g_eval_fault = 0;   /* set by any non-finite metric or NMS refusal */
static long g_intra_dups = 0;
static char g_artifact_root[65] = "";
static int g_jobs = 1;
static double g_t_fork = 0.0, g_final_train_s = 0.0, g_t_start = 0.0;
/* Production deadline: the measured shuffle arm is ~1117 s, so 3600 s is more
   than 3x headroom. Tests override it with a short value. */
static double g_shuffle_deadline_s = 3600.0;
static double g_worker_deadline = 0.0;
static const char *g_worker_fault = "";
static int g_parent_fault_after_fork = 0;
static int g_fail_random_init = 0;
static int g_sig_in_window = 0;      /* test hook: raise inside the fork window */
static int g_sig_after_reap = 0;     /* test hook: raise once the child is collected */
static int g_sig_before_publish = 0; /* test hook: recorded signal before publication */
static const char *G_VARIANT = "unknown";
static const char *G_FRONTEND = "unknown";

typedef VdPackImg VImg;
typedef VdPack Pack;

static uint64_t g_seed = 20260727ULL;
static uint64_t rnd(void) {
    g_seed += 0x9E3779B97F4A7C15ULL;
    uint64_t z = g_seed;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

static double now_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static void free_pack(Pack *p) { vd_pack_free(p); }


/* ---------------- one-snapshot cache reader ------------------------------
   The cache directory is traversed once with component-wise no-follow
   semantics, and every scored member is opened once from that dirfd. All
   parsing, hashing, root computation and ID/content checking then happens
   through those descriptors. Nothing scored is ever reopened by pathname, so a
   directory exchanged underneath the run cannot let it load one generation and
   validate another -- it either sees one internally consistent snapshot or
   refuses. */
typedef struct {
    int dir_fd;
    int fd[VD_N_MEMBERS];
    off_t size[VD_N_MEMBERS];
    char sha[VD_N_MEMBERS][65];
    int manifest_fd;
} Snapshot;

static void snap_close(Snapshot *s) {
    int i;
    if (!s) return;
    for (i = 0; i < VD_N_MEMBERS; i++) if (s->fd[i] >= 0) { close(s->fd[i]); s->fd[i] = -1; }
    if (s->manifest_fd >= 0) { close(s->manifest_fd); s->manifest_fd = -1; }
    if (s->dir_fd >= 0) { close(s->dir_fd); s->dir_fd = -1; }
}

static int snap_open(const char *cache, Snapshot *s) {
    int i;
    memset(s, 0, sizeof *s);
    s->dir_fd = -1; s->manifest_fd = -1;
    for (i = 0; i < VD_N_MEMBERS; i++) s->fd[i] = -1;

    s->dir_fd = vd_open_dir_nofollow(cache);
    if (s->dir_fd < 0) { printf("VD_BENCH_FAIL cache_dir_refused:%s\n", cache); return -1; }
    for (i = 0; i < VD_N_MEMBERS; i++) {
        s->fd[i] = vd_openat_regular(s->dir_fd, VD_MEMBERS[i], &s->size[i]);
        if (s->fd[i] < 0) {
            printf("VD_BENCH_FAIL member_unopenable:%s\n", VD_MEMBERS[i]);
            snap_close(s); return -1;
        }
        if (vd_sha256_fd(s->fd[i], s->sha[i]) != 0) {
            printf("VD_BENCH_FAIL member_unhashable:%s\n", VD_MEMBERS[i]);
            snap_close(s); return -1;
        }
    }
    s->manifest_fd = vd_openat_regular(s->dir_fd, "manifest.txt", NULL);
    if (s->manifest_fd < 0) {
        printf("VD_BENCH_FAIL manifest_unreadable\n");
        snap_close(s); return -1;
    }
    return 0;
}

static int snap_index(const char *name) {
    int i;
    for (i = 0; i < VD_N_MEMBERS; i++) if (!strcmp(VD_MEMBERS[i], name)) return i;
    return -1;
}

/* Digest of a member as captured in the snapshot, compared to what the manifest
   claims. The protocol's artifact root is the authority; this catches a
   manifest that disagrees with the bytes it sits beside. */
static int snap_member_matches(const Snapshot *s, const char *name, const char *declared) {
    int i = snap_index(name);
    if (i < 0) return -1;
    if (!declared || strlen(declared) != 64) {
        printf("VD_BENCH_FAIL missing_digest:%s\n", name);
        return -1;
    }
    if (strcmp(s->sha[i], declared) != 0) {
        printf("VD_BENCH_FAIL artefact_hash_mismatch:%s\n", name);
        return -1;
    }
    return 0;
}

static int snap_root_matches(const Snapshot *s, const char *name, const char *declared,
                             const char *pinned, size_t expect_lines, const char *label) {
    int i = snap_index(name);
    char got[65];
    size_t nlines = 0;
    if (i < 0) return -1;
    if (vd_root_of_fd(s->fd[i], got, &nlines) != 0) {
        printf("VD_BENCH_FAIL sidecar_unreadable:%s\n", name); return -1;
    }
    if (expect_lines && nlines != expect_lines) {
        printf("VD_BENCH_FAIL sidecar_count:%s got=%zu want=%zu\n", label, nlines, expect_lines);
        return -1;
    }
    if (strcmp(got, declared) != 0) { printf("VD_BENCH_FAIL root_vs_manifest:%s\n", label); return -1; }
    if (!pinned || !*pinned) {
        printf("VD_BENCH_FAIL root_unpinned:%s\n", label); return -1;
    }
    if (strcmp(got, pinned) != 0) { printf("VD_BENCH_FAIL root_vs_protocol:%s\n", label); return -1; }
    return 0;
}

static int snap_ids_match_pack(const Snapshot *s, const char *name, const Pack *pk,
                               const char *label) {
    int idx = snap_index(name);
    char **have = NULL;
    size_t n = 0, i;
    int bad = 0;
    if (idx < 0) return -1;
    if (vd_lines_of_fd(s->fd[idx], &have, &n) != 0) return -1;
    if (n != pk->n) bad = 1;
    for (i = 0; i < pk->n && !bad; i++) {
        size_t k;
        int found = 0;
        for (k = 0; k < n; k++) if (!strcmp(have[k], pk->imgs[i].id)) { found = 1; break; }
        if (!found) bad = 1;
    }
    vd_lines_free(have, n);
    if (bad) { printf("VD_BENCH_FAIL sidecar_pack_mismatch:%s\n", label); return -1; }
    return 0;
}

/* ---------------- evidence gate ------------------------------------------
   Identity comes from a protocol chosen by the scoring target and compiled into
   vd_protocol.c. The cache is checked against it. Nothing the cache says about
   itself is allowed to decide what is required of it -- that was the hole that
   let a cache declaring test_offset 0 skip the spent-holdout check entirely. */

/* Leakage computed by the bench itself, from the held descriptors -- never
   taken from the manifest's own claim about itself. */
static int str_cmp(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static size_t count_dup_across(char **v, size_t n) {
    size_t i, dups = 0;
    qsort(v, n, sizeof *v, str_cmp);
    for (i = 1; i < n; i++) if (!strcmp(v[i - 1], v[i])) dups++;
    return dups;
}

static long bench_id_overlap(const Pack *a, const Pack *b, const Pack *c) {
    size_t n = a->n + b->n + c->n, k = 0, i;
    char **v = (char **)malloc(n * sizeof *v);
    long dups;
    if (!v) return -1;
    for (i = 0; i < a->n; i++) v[k++] = a->imgs[i].id;
    for (i = 0; i < b->n; i++) v[k++] = b->imgs[i].id;
    for (i = 0; i < c->n; i++) v[k++] = c->imgs[i].id;
    dups = (long)count_dup_across(v, n);
    free(v);
    return dups;
}

/* Cross-split content overlap is leakage. A hash repeated INSIDE one split is a
   duplicate image, not leakage: VOC2007 trainval genuinely contains 3 such
   pairs and the content-addressed split keeps both copies on the same side.
   Those are counted separately and reported as a diagnostic. */
typedef struct { char *h; int split; } CHash;

static int chash_cmp(const void *a, const void *b) {
    const CHash *x = (const CHash *)a, *y = (const CHash *)b;
    int c = strcmp(x->h, y->h);
    if (c) return c;
    return x->split - y->split;
}

static long bench_content_overlap(const Snapshot *s, long *intra_dups) {
    static const char *files[3] = {"content_train.txt", "content_val.txt", "content_test.txt"};
    CHash *v = NULL;
    size_t n = 0, cap = 0, i;
    long cross = 0, intra = 0;
    int f_i;
    if (intra_dups) *intra_dups = 0;
    for (f_i = 0; f_i < 3; f_i++) {
        char **ln = NULL;
        size_t k, nl = 0;
        int idx = snap_index(files[f_i]);
        if (idx < 0 || vd_lines_of_fd(s->fd[idx], &ln, &nl) != 0) goto fail;
        for (k = 0; k < nl; k++) {
            if (n == cap) {
                CHash *nv;
                cap = cap ? cap * 2 : 4096;
                nv = (CHash *)realloc(v, cap * sizeof *v);
                if (!nv) { vd_lines_free(ln, nl); goto fail; }
                v = nv;
            }
            v[n].h = strdup(ln[k]);
            if (!v[n].h) { vd_lines_free(ln, nl); goto fail; }
            v[n].split = f_i;
            n++;
        }
        vd_lines_free(ln, nl);
    }
    qsort(v, n, sizeof *v, chash_cmp);
    for (i = 1; i < n; i++) {
        if (strcmp(v[i - 1].h, v[i].h) != 0) continue;
        if (v[i - 1].split != v[i].split) cross++;
        else intra++;
    }
    for (i = 0; i < n; i++) free(v[i].h);
    free(v);
    if (intra_dups) *intra_dups = intra;
    return cross;
fail:
    for (i = 0; i < n; i++) free(v[i].h);
    free(v);
    return -1;
}

/* ---------------- training set assembly --------------------------------- */
/* label -1 = ignore (0.3<=IoU<0.5 or difficult); never used for training. */
static size_t collect(const Pack *p, double **X, double **Y, size_t max_neg_per_img) {
    size_t i, j, n = 0, cap = 0;
    double *x = NULL, *y = NULL;
    for (i = 0; i < p->n; i++)
        for (j = 0; j < p->imgs[i].n_prop; j++)
            if (p->imgs[i].plabel[j] >= 0) cap++;
    x = (double *)malloc(cap * DIM * sizeof(double));
    y = (double *)malloc(cap * NCLS * sizeof(double));
    if (!x || !y) { free(x); free(y); return 0; }
    for (i = 0; i < p->n; i++) {
        const VImg *im = &p->imgs[i];
        size_t neg = 0;
        for (j = 0; j < im->n_prop; j++) {
            int lab = im->plabel[j];
            size_t d;
            if (lab < 0) continue;
            if (lab == 0) {
                if (neg >= max_neg_per_img) continue;
                neg++;
            }
            for (d = 0; d < DIM; d++) x[n * DIM + d] = im->pf[j * DIM + d];
            y[n * NCLS + 0] = lab ? 0.0 : 1.0;
            y[n * NCLS + 1] = lab ? 1.0 : 0.0;
            n++;
        }
    }
    *X = x; *Y = y;
    return n;
}

/* ---------------- scoring ------------------------------------------------ */
/* Deterministic BTN fault injection for the failure-path tests. -1 disables;
   counted over every guarded init and every forward. */
static long g_btn_fail_at = -1;
static long g_btn_calls;

static int vd_btn_init_guarded(BinaryTransformNetwork *btn, size_t in, size_t out,
                               size_t h0, size_t hmax, double lr, unsigned seed,
                               const char *what) {
    g_btn_calls++;
    if (g_btn_fail_at >= 0 && g_btn_calls == g_btn_fail_at) {
        printf("VD_BENCH_FAIL btn_init_failed:%s (injected)\n", what);
        g_eval_fault = 1;
        return -1;
    }
    memset(btn, 0, sizeof *btn);
    if (btn_init(btn, in, out, h0, hmax, lr, seed) != 0) {
        printf("VD_BENCH_FAIL btn_init_failed:%s\n", what);
        g_eval_fault = 1;
        return -1;
    }
    return 0;
}

/* softmax-ish confidence from the head's two raw outputs */
static double head_score(BinaryTransformNetwork *btn, const float *f) {
    double in[DIM_MAX];
    const double *o;
    size_t d;
    for (d = 0; d < DIM; d++) in[d] = f[d];
    g_btn_calls++;
    if (g_btn_fail_at >= 0 && g_btn_calls == g_btn_fail_at) {
        /* A control whose forward fails must never silently score 0: that would
           LOWER the random-head AP and INFLATE both ratio and margin. */
        g_eval_fault = 1;
        return NAN;
    }
    o = btn_forward(btn, in);
    if (!o) { g_eval_fault = 1; return NAN; }
    {
        double a = o[0], b = o[1], m = a > b ? a : b;
        double ea, eb, r;
        if (!isfinite(a) || !isfinite(b)) { g_eval_fault = 1; return NAN; }
        ea = exp(a - m); eb = exp(b - m);
        r = eb / (ea + eb);
        if (!isfinite(r)) { g_eval_fault = 1; return NAN; }
        return r;
    }
}

/* Build VdImage detections for a whole split, NMS applied per image. */
static VdImage *run_detector(const Pack *p, BinaryTransformNetwork *btn,
                             double nms_iou, VdDet ***own) {
    VdImage *out = (VdImage *)calloc(p->n ? p->n : 1, sizeof(VdImage));
    VdDet **keepbuf = (VdDet **)calloc(p->n ? p->n : 1, sizeof(VdDet *));
    size_t i, j;
    if (!out || !keepbuf) { g_eval_fault = 1; free(out); free(keepbuf); *own = NULL; return NULL; }
    for (i = 0; i < p->n; i++) {
        const VImg *im = &p->imgs[i];
        VdDet *raw = im->n_prop ? (VdDet *)calloc(im->n_prop, sizeof(VdDet)) : NULL;
        int *keep = im->n_prop ? (int *)calloc(im->n_prop, sizeof(int)) : NULL;
        size_t nk = 0;
        if (im->n_prop && (!raw || !keep)) { g_eval_fault = 1; free(raw); free(keep); continue; }
        for (j = 0; j < im->n_prop; j++) {
            raw[j].box = im->pb[j];
            raw[j].cls = 0;
            raw[j].score = head_score(btn, im->pf + j * DIM);
            if (!isfinite(raw[j].score)) { g_eval_fault = 1; free(raw); free(keep); goto arm_fault; }
        }
        if (im->n_prop) nk = vd_nms(raw, im->n_prop, nms_iou, keep);
        if (nk == VD_NMS_FAIL) { g_eval_fault = 1; nk = 0; }
        keepbuf[i] = nk ? (VdDet *)calloc(nk, sizeof(VdDet)) : NULL;
        if (nk && !keepbuf[i]) { g_eval_fault = 1; nk = 0; }
        for (j = 0; j < nk; j++) keepbuf[i][j] = raw[keep[j]];
        out[i].gts = im->gts; out[i].gt_difficult = im->gt_dif; out[i].n_gt = im->n_gt;
        out[i].dets = keepbuf[i]; out[i].n_det = nk;
        free(raw); free(keep);
    }
    *own = keepbuf;
    return out;

arm_fault:
    for (i = 0; i < p->n; i++) free(keepbuf[i]);
    free(keepbuf); free(out);
    *own = NULL;
    return NULL;
}

static void free_det(VdImage *v, VdDet **own, size_t n) {
    size_t i;
    if (own) for (i = 0; i < n; i++) free(own[i]);
    free(own); free(v);
}

/* A detector arm that could not allocate returns NULL; scoring it as 0.0 would
   quietly turn an out-of-memory event into a benchmark number. */
static double ap_or_fault(VdImage *v, size_t n) {
    double ap;
    if (!v) { g_eval_fault = 1; return NAN; }
    ap = vd_ap50(v, n, 0.5);
    if (!isfinite(ap)) g_eval_fault = 1;
    return ap;
}

static Port RAWP(void) {
    Port p; memset(&p, 0, sizeof p);
    p.family = PORT_RAW; p.field_width = (uint32_t)DIM; p.field_count = 1;
    snprintf(p.tag, sizeof p.tag, "vd_hogpca%d", (int)DIM);
    return p;
}
static Port CLSP(void) {
    Port p; memset(&p, 0, sizeof p);
    p.family = PORT_ONEHOT; p.field_width = NCLS; p.field_count = 1;
    snprintf(p.tag, sizeof p.tag, "vd_carcls");
    return p;
}

/* ---------------- label-shuffle worker lifecycle -------------------------
   The worker is a child process, so every failure mode has to be handled
   explicitly: a hang, an early exit, a partial write, a crash, or a parent
   error after the fork. There is exactly ONE parent cleanup path -- nothing
   returns while the child is live -- the read is bounded by a monotonic
   deadline, and the child is terminated and reaped on any fault. */
typedef struct { int ok; double ap_val, ap_test, elapsed; } ShufResult;

static volatile sig_atomic_t g_child_pid_sig;   /* async-signal-safe copy */
static volatile sig_atomic_t g_signal_caught;   /* set by the handler, read by control */

/* Two async-signal-safe states, chosen by whether a child is still outstanding.
 *
 *  - A child is live (g_child_pid_sig > 0): record the signal and TERM the
 *    child, then return, so normal control performs the bounded reap. Exiting
 *    here would abandon the worker.
 *  - The child has already been collected (g_child_pid_sig == 0): there is
 *    nothing left to reap, so exit immediately. This is what makes it
 *    impossible for a results file or a verdict marker to be emitted after an
 *    interruption -- the window between reaping the worker and restoring the
 *    original handlers previously allowed exactly that. */
static void child_signal_cleanup(int sig) {
    if (g_child_pid_sig > 0) {
        g_signal_caught = sig;
        kill((pid_t)g_child_pid_sig, SIGTERM);
        return;
    }
    _exit(128 + sig);
}

/* A recorded interruption must never leave a bar-bearing artefact behind.
   Checked after collection, before accepting the worker result, before handler
   restoration, before building the JSON, before publishing it, and before any
   verdict marker. */
static int interrupted(void) { return g_signal_caught != 0; }

/* Signal state saved across the fork window. SIGINT/SIGTERM are blocked and the
   parent handlers installed BEFORE fork(), so there is no interval in which a
   signal can arrive with no handler, or with a handler but no child PID to act
   on. The mask is restored only after the PID and the deadline are published,
   which means a signal that arrives inside the window is merely pending and is
   delivered to a fully initialised handler. */
typedef struct {
    sigset_t old_mask;
    struct sigaction old_int, old_term;
    int armed;
} SigWindow;

static int sigwindow_enter(SigWindow *w) {
    sigset_t block;
    struct sigaction sa;
    memset(w, 0, sizeof *w);
    sigemptyset(&block);
    sigaddset(&block, SIGINT);
    sigaddset(&block, SIGTERM);
    if (sigprocmask(SIG_BLOCK, &block, &w->old_mask) != 0) return -1;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = child_signal_cleanup;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;                       /* no SA_RESTART: poll must see EINTR */
    if (sigaction(SIGINT, &sa, &w->old_int) != 0) {
        (void)sigprocmask(SIG_SETMASK, &w->old_mask, NULL);
        return -1;
    }
    if (sigaction(SIGTERM, &sa, &w->old_term) != 0) {
        (void)sigaction(SIGINT, &w->old_int, NULL);
        (void)sigprocmask(SIG_SETMASK, &w->old_mask, NULL);
        return -1;
    }
    w->armed = 1;
    return 0;
}

/* Restore the mask only; handlers stay armed while the worker is live. */
static void sigwindow_unblock(SigWindow *w) {
    if (w->armed) (void)sigprocmask(SIG_SETMASK, &w->old_mask, NULL);
}

/* Restore the original actions and mask exactly. */
static void sigwindow_leave(SigWindow *w) {
    if (!w->armed) return;
    (void)sigaction(SIGINT, &w->old_int, NULL);
    (void)sigaction(SIGTERM, &w->old_term, NULL);
    (void)sigprocmask(SIG_SETMASK, &w->old_mask, NULL);
    w->armed = 0;
}

/* Child: default dispositions and the caller's original mask, before any work. */
static void sigwindow_child_reset(SigWindow *w) {
    struct sigaction dfl;
    memset(&dfl, 0, sizeof dfl);
    dfl.sa_handler = SIG_DFL;
    sigemptyset(&dfl.sa_mask);
    (void)sigaction(SIGINT, &dfl, NULL);
    (void)sigaction(SIGTERM, &dfl, NULL);
    (void)sigprocmask(SIG_SETMASK, &w->old_mask, NULL);
}

/* Bounded, non-blocking collection against an absolute monotonic deadline:
   TERM, WNOHANG poll, escalate to KILL, WNOHANG poll again. Never uses a
   blocking waitpid. Returns 0 only if the child was actually collected. */
static int reap_child_bounded(pid_t pid, double term_deadline, double kill_deadline) {
    int status = 0;
    if (pid <= 0) return 0;
    kill(pid, SIGTERM);
    while (now_s() < term_deadline) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) { g_child_pid_sig = 0; return 0; }
        if (r < 0 && errno != EINTR) break;
        usleep(20000);
    }
    kill(pid, SIGKILL);
    while (now_s() < kill_deadline) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) { g_child_pid_sig = 0; return 0; }
        if (r < 0 && errno != EINTR) break;
        usleep(20000);
    }
    return -1;   /* could not collect: caller must fail loud and report the PID */
}

/* Collect an already-exited child without ever blocking indefinitely. */
static int wait_child_deadline(pid_t pid, int *status, double deadline) {
    for (;;) {
        pid_t r = waitpid(pid, status, WNOHANG);
        if (r == pid) { g_child_pid_sig = 0; return 0; }
        if (r < 0 && errno != EINTR) return -1;
        if (now_s() >= deadline) return -1;
        usleep(20000);
    }
}

/* Read exactly n bytes before the deadline, tolerating EINTR and short reads.
   Returns 0 on a complete message, -1 on timeout/EOF/error. */
static int read_exact_deadline(int fd, void *buf, size_t n, double deadline_s) {
    unsigned char *p = (unsigned char *)buf;
    size_t got = 0;
    while (got < n) {
        struct pollfd pfd;
        double left = deadline_s - now_s();
        int pr;
        ssize_t r;
        if (left <= 0) return -1;
        pfd.fd = fd; pfd.events = POLLIN; pfd.revents = 0;
        pr = poll(&pfd, 1, (int)(left * 1000.0 > 1000.0 ? 1000.0 : left * 1000.0));
        if (pr < 0) { if (errno == EINTR) continue; return -1; }
        if (pr == 0) continue;                      /* re-check the deadline */
        if (!(pfd.revents & (POLLIN | POLLHUP))) return -1;
        r = read(fd, p + got, n - got);
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        if (r == 0) return -1;                      /* EOF before full message */
        got += (size_t)r;
    }
    return 0;
}

/* Trains the shuffled-label head and evaluates it with the identical full
   pipeline on validation and the holdout. Returns 1 on success. */
static int run_shuffle_arm(const Pack *va, const Pack *te,
                           const double *X, const double *Y, size_t n,
                           double *ap_val, double *ap_test) {
    BinaryTransformNetwork sh;
    double *Ys = (double *)malloc(n * NCLS * sizeof(double));
    size_t k;
    *ap_val = *ap_test = NAN;
    if (!Ys) return 0;
    memcpy(Ys, Y, n * NCLS * sizeof(double));
    for (k = n; k > 1; k--) {
        size_t j = (size_t)(rnd() % k);
        double t;
        t = Ys[(k-1)*NCLS+0]; Ys[(k-1)*NCLS+0] = Ys[j*NCLS+0]; Ys[j*NCLS+0] = t;
        t = Ys[(k-1)*NCLS+1]; Ys[(k-1)*NCLS+1] = Ys[j*NCLS+1]; Ys[j*NCLS+1] = t;
    }
    if (vd_btn_init_guarded(&sh, DIM, NCLS, 24, 192, 0.5, 7u, "shuffle") != 0) { free(Ys); return 0; }
    btn_set_ports(&sh, RAWP(), CLSP());
    btn_train_dynamic(&sh, X, Ys, n, 2000, 200, 1e-5, 1e-7);
    free(Ys);
    {
        VdDet **own; VdImage *v;
        v = run_detector(va, &sh, 0.30, &own); *ap_val = ap_or_fault(v, va->n); free_det(v, own, va->n);
        v = run_detector(te, &sh, 0.30, &own); *ap_test = ap_or_fault(v, te->n); free_det(v, own, te->n);
    }
    btn_free(&sh);
    return (isfinite(*ap_val) && isfinite(*ap_test) && !g_eval_fault) ? 1 : 0;
}

/* ---------------- linear logistic baseline (same features) --------------- */
typedef struct { double w[DIM_MAX]; double b; } Lin;

static void lin_train(Lin *L, const double *X, const double *Y, size_t n,
                      int epochs, double lr) {
    size_t i, d;
    int e;
    memset(L, 0, sizeof *L);
    for (e = 0; e < epochs; e++) {
        for (i = 0; i < n; i++) {
            double z = L->b, pr, g;
            for (d = 0; d < DIM; d++) z += L->w[d] * X[i * DIM + d];
            pr = 1.0 / (1.0 + exp(-z));
            g = (Y[i * NCLS + 1] - pr) * lr;
            for (d = 0; d < DIM; d++) L->w[d] += g * X[i * DIM + d];
            L->b += g;
        }
    }
}
static double lin_score(const Lin *L, const float *f) {
    double z = L->b;
    size_t d;
    for (d = 0; d < DIM; d++) z += L->w[d] * f[d];
    return 1.0 / (1.0 + exp(-z));
}
static VdImage *run_linear(const Pack *p, const Lin *L, double nms_iou, VdDet ***own) {
    VdImage *out = (VdImage *)calloc(p->n ? p->n : 1, sizeof(VdImage));
    VdDet **kb = (VdDet **)calloc(p->n ? p->n : 1, sizeof(VdDet *));
    size_t i, j;
    if (!out || !kb) { g_eval_fault = 1; free(out); free(kb); *own = NULL; return NULL; }
    for (i = 0; i < p->n; i++) {
        const VImg *im = &p->imgs[i];
        VdDet *raw = im->n_prop ? (VdDet *)calloc(im->n_prop, sizeof(VdDet)) : NULL;
        int *keep = im->n_prop ? (int *)calloc(im->n_prop, sizeof(int)) : NULL;
        size_t nk = 0;
        if (im->n_prop && (!raw || !keep)) { g_eval_fault = 1; free(raw); free(keep); continue; }
        for (j = 0; j < im->n_prop; j++) {
            raw[j].box = im->pb[j]; raw[j].cls = 0;
            raw[j].score = lin_score(L, im->pf + j * DIM);
        }
        if (im->n_prop) nk = vd_nms(raw, im->n_prop, nms_iou, keep);
        if (nk == VD_NMS_FAIL) { g_eval_fault = 1; nk = 0; }
        kb[i] = nk ? (VdDet *)calloc(nk, sizeof(VdDet)) : NULL;
        if (nk && !kb[i]) { g_eval_fault = 1; nk = 0; }
        for (j = 0; j < nk; j++) kb[i][j] = raw[keep[j]];
        out[i].gts = im->gts; out[i].gt_difficult = im->gt_dif; out[i].n_gt = im->n_gt;
        out[i].dets = kb[i]; out[i].n_det = nk;
        free(raw); free(keep);
    }
    *own = kb;
    return out;
}

/* proposal recall over a split (ceiling the head cannot exceed) */
static void split_prop_recall(const Pack *p, double thr, size_t *hit, size_t *tot) {
    size_t i, j, k, h = 0, t = 0;
    for (i = 0; i < p->n; i++) {
        const VImg *im = &p->imgs[i];
        for (j = 0; j < im->n_gt; j++) {
            int cov = 0;
            if (im->gt_dif[j]) continue;
            t++;
            for (k = 0; k < im->n_prop; k++)
                if (vd_iou(im->pb[k], im->gts[j]) >= thr) { cov = 1; break; }
            if (cov) h++;
        }
    }
    *hit = h; *tot = t;
}

int main(int argc, char **argv) {
    const char *cache = "data/vision_cache";
    const char *jsonp = "logs/vision_detection_bench.json";
    const char *prev_test = NULL;   /* pack whose holdout is already spent */
    char variant[80] = "", frontend[200] = "";
    size_t prev_ids_checked = 0;
    double val_thr = 0.5;           /* set on validation, never on test */
    double g_val_ap = 0.0;          /* validation AP50, for the floor formula */
    VdManifest man;
    Snapshot snap;
    const VdProtocol *PROTO = NULL;
    int evidence_manifest = 0, evidence_prev = 0, evidence_leak = 0;
    Pack tr, va, te;
    double *X = NULL, *Y = NULL;
    size_t n = 0;
    BinaryTransformNetwork head, rndhead;   /* zeroed below: the cleanup path may
                                               run before either is initialised */
    Lin lin;
    double t0, train_s;
    int i;
    int need_final_train = 0;
    int shuf_pipe[2] = {-1, -1};
    pid_t shuf_pid = -1;
    ShufResult shuf = {0, 0.0, 0.0, 0.0};
    int rc_final = 0;
    SigWindow sigwin;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--cache") && i + 1 < argc) cache = argv[++i];
        else if (!strcmp(argv[i], "--json") && i + 1 < argc) jsonp = argv[++i];
        else if (!strcmp(argv[i], "--prev-test") && i + 1 < argc) prev_test = argv[++i];
        else if (!strcmp(argv[i], "--btn-fail-at") && i + 1 < argc) g_btn_fail_at = atol(argv[++i]);
        else if (!strcmp(argv[i], "--worker-fault") && i + 1 < argc) g_worker_fault = argv[++i];
        else if (!strcmp(argv[i], "--parent-fault-after-fork")) g_parent_fault_after_fork = 1;
        else if (!strcmp(argv[i], "--fail-random-init")) g_fail_random_init = 1;
        else if (!strcmp(argv[i], "--signal-after-reap") && i + 1 < argc) {
            const char *v = argv[++i];
            g_sig_after_reap = !strcmp(v, "TERM") ? SIGTERM : SIGINT;
        }
        else if (!strcmp(argv[i], "--signal-before-publish") && i + 1 < argc) {
            const char *v = argv[++i];
            g_sig_before_publish = !strcmp(v, "TERM") ? SIGTERM : SIGINT;
        }
        else if (!strcmp(argv[i], "--signal-in-fork-window") && i + 1 < argc) {
            const char *v = argv[++i];
            g_sig_in_window = !strcmp(v, "TERM") ? SIGTERM : SIGINT;
        }
        else if (!strcmp(argv[i], "--shuffle-deadline-s") && i + 1 < argc)
            g_shuffle_deadline_s = atof(argv[++i]);
        else if (!strcmp(argv[i], "--jobs") && i + 1 < argc) {
            g_jobs = atoi(argv[++i]);
            if (g_jobs < 1) g_jobs = 1;
            if (g_jobs > 2) g_jobs = 2;   /* only one independent arm exists */
        }
        else if (!strcmp(argv[i], "--protocol") && i + 1 < argc) {
            PROTO = vd_protocol_get(argv[++i]);
            if (!PROTO) { printf("VD_BENCH_FAIL unknown_protocol:%s\n", argv[i]); return 3; }
        }
    }

    g_t_start = now_s();
    memset(&sigwin, 0, sizeof sigwin);
    memset(&head, 0, sizeof head);
    memset(&rndhead, 0, sizeof rndhead);
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("vd_bench: CPU-only (ROCR/HIP/CUDA_VISIBLE_DEVICES empty)\n");
    if (snap_open(cache, &snap) != 0) return 3;
    {
        int rc1 = vd_pack_load_fd(snap.fd[snap_index("train.pack")], &tr);
        int rc2 = vd_pack_load_fd(snap.fd[snap_index("val.pack")], &va);
        int rc3 = vd_pack_load_fd(snap.fd[snap_index("test.pack")], &te);
        if (rc1 || rc2 || rc3) {
            printf("VD_BENCH_FAIL pack_invalid (%s/%s/%s)\n",
                   vd_pack_strerror(rc1), vd_pack_strerror(rc2), vd_pack_strerror(rc3));
            return 3;
        }
    }
    printf("packs: train_img=%zu val_img=%zu test_img=%zu\n", tr.n, va.n, te.n);

    if (tr.dim != va.dim || tr.dim != te.dim) {
        printf("VD_BENCH_FAIL dim_mismatch train=%d val=%d test=%d\n", tr.dim, va.dim, te.dim);
        return 2;
    }
    DIM = (size_t)tr.dim;

    /* ---- evidence gate --------------------------------------------------
       Identity comes from a protocol chosen by the scoring target and compiled
       into vd_protocol.c. The cache is checked against it; nothing the cache
       says about itself decides what is required of it. That was the hole that
       let a cache declaring test_offset 0 skip the spent-holdout check. */
    {
        char err[256];
        if (!PROTO) {
            printf("VD_BENCH_FAIL protocol_required (pass --protocol <name>)\n");
            return 3;
        }
        if (vd_manifest_parse_fd(snap.manifest_fd, &man, err, sizeof err) != 0) {
            printf("VD_BENCH_FAIL %s\n", err); return 3;
        }
        if (vd_manifest_check(&man, PROTO, err, sizeof err) != 0) {
            printf("VD_BENCH_FAIL %s\n", err); return 3;
        }
        if (man.pca_dim != (long)DIM ||
            man.train_img != (long)tr.n || man.val_img != (long)va.n ||
            man.test_img != (long)te.n ||
            man.train_prop != (long)tr.total_prop || man.val_prop != (long)va.total_prop ||
            man.test_prop != (long)te.total_prop) {
            printf("VD_BENCH_FAIL manifest_counts_mismatch\n"); return 3;
        }
        if (snap_member_matches(&snap, "train.pack", man.sha_train) != 0 ||
            snap_member_matches(&snap, "val.pack",   man.sha_val)   != 0 ||
            snap_member_matches(&snap, "test.pack",  man.sha_test)  != 0 ||
            snap_member_matches(&snap, "pca.bin",    man.sha_pca)   != 0 ||
            snap_member_matches(&snap, "ids_train.txt",     man.sha_ids_train)     != 0 ||
            snap_member_matches(&snap, "ids_val.txt",       man.sha_ids_val)       != 0 ||
            snap_member_matches(&snap, "ids_test.txt",      man.sha_ids_test)      != 0 ||
            snap_member_matches(&snap, "content_train.txt", man.sha_content_train) != 0 ||
            snap_member_matches(&snap, "content_val.txt",   man.sha_content_val)   != 0 ||
            snap_member_matches(&snap, "content_test.txt",  man.sha_content_test)  != 0) {
            return 3;
        }
        /* ---- artifact authority ------------------------------------------
           The protocol pins one root over the exact bytes, sizes, names and
           ordering of every scored member. Canonical sidecars with substituted
           features, GT, proposals or PCA change it, so the manifest alone is
           never authority. */
        {
            VdMember mem[VD_N_MEMBERS];
            char root[65];
            int mi;
            for (mi = 0; mi < VD_N_MEMBERS; mi++) {
                mem[mi].name = VD_MEMBERS[mi];
                mem[mi].size = (long long)snap.size[mi];
                memcpy(mem[mi].sha, snap.sha[mi], 65);
            }
            if (vd_artifact_root(mem, VD_N_MEMBERS, man.manifest_version, root) != 0) {
                printf("VD_BENCH_FAIL artifact_root_compute_failed\n"); return 3;
            }
            if (!PROTO->artifact_root || !*PROTO->artifact_root) {
                printf("VD_BENCH_FAIL artifact_root_unpinned protocol=%s\n", PROTO->name);
                return 3;
            }
            if (strcmp(root, man.artifact_root) != 0) {
                printf("VD_BENCH_FAIL artifact_root_vs_manifest\n"); return 3;
            }
            if (strcmp(root, PROTO->artifact_root) != 0) {
                printf("VD_BENCH_FAIL artifact_root_vs_protocol got=%s\n", root);
                return 3;
            }
            snprintf(g_artifact_root, sizeof g_artifact_root, "%s", root);
            printf("artifact root verified from held descriptors: %s\n", root);
        }
        if (snap_root_matches(&snap, "ids_test.txt", man.id_root_test, PROTO->id_root_test,
                              (size_t)PROTO->test_count, "id_root_test") != 0 ||
            snap_root_matches(&snap, "content_test.txt", man.content_root_test,
                              PROTO->content_root_test, (size_t)PROTO->test_count,
                              "content_root_test") != 0 ||
            snap_root_matches(&snap, "ids_train.txt", man.id_root_train, PROTO->id_root_train,
                              (size_t)PROTO->train_img, "id_root_train") != 0 ||
            snap_root_matches(&snap, "ids_val.txt", man.id_root_val, PROTO->id_root_val,
                              (size_t)PROTO->val_img, "id_root_val") != 0 ||
            snap_root_matches(&snap, "content_train.txt", man.content_root_train,
                              PROTO->content_root_train, (size_t)PROTO->train_img,
                              "content_root_train") != 0 ||
            snap_root_matches(&snap, "content_val.txt", man.content_root_val,
                              PROTO->content_root_val, (size_t)PROTO->val_img,
                              "content_root_val") != 0) {
            return 3;
        }
        if (snap_ids_match_pack(&snap, "ids_train.txt", &tr, "train") != 0 ||
            snap_ids_match_pack(&snap, "ids_val.txt",   &va, "val")   != 0 ||
            snap_ids_match_pack(&snap, "ids_test.txt",  &te, "test")  != 0) {
            return 3;
        }
        snprintf(variant, sizeof variant, "%s", man.variant);
        snprintf(frontend, sizeof frontend,
                 "SelectiveSearchFast(class-agnostic) -> %ldx%ld %s HOG %ldD -> PCA%ld (train-only)",
                 man.hog_side, man.hog_side, man.color ? "colour" : "gray",
                 man.hog_dim, man.pca_dim);
        G_VARIANT = variant; G_FRONTEND = frontend;
        printf("protocol=%s (pre-registered, not cache-declared)\n", PROTO->name);
        printf("variant=%s dim=%zu frontend=%s\n", G_VARIANT, DIM, G_FRONTEND);
        printf("manifest: schema ok; artefact+sidecar digests verified; "
               "id/content roots match pinned protocol roots\n");
        evidence_manifest = 1;
    }

    /* Leakage: measured here, from the artefacts, not read off the manifest. */
    {
        long id_dups = bench_id_overlap(&tr, &va, &te);
        long intra_dups = 0;
        long ct_dups = bench_content_overlap(&snap, &intra_dups);
        if (id_dups < 0 || ct_dups < 0) {
            printf("VD_BENCH_FAIL leakage_check_failed\n");
            return 3;
        }
        if (id_dups != 0 || ct_dups != 0) {
            printf("VD_BENCH_FAIL measured_leakage id=%ld content=%ld\n", id_dups, ct_dups);
            return 3;
        }
        printf("leakage: measured across train/val/holdout -> cross-split id overlap 0, "
               "cross-split content overlap 0 (intra-split duplicate images: %ld)\n", intra_dups);
        g_intra_dups = intra_dups;
        evidence_leak = 1;
    }

    /* The spent-holdout requirement comes from the PROTOCOL. */
    if (PROTO->requires_prev) {
        Pack pv;
        size_t a, b, leaks = 0;
        char pvsha[65];
        if (!prev_test) {
            printf("VD_BENCH_FAIL prev_test_required protocol=%s\n", PROTO->name);
            return 3;
        }
        /* The spent holdout is ONE held object: opened once with component-wise
           no-follow traversal, then parsed and hashed from that same descriptor.
           It is never reopened by pathname, so the pack that is counted is the
           pack that is digested. */
        off_t psz = 0;
        int pfd = vd_open_file_nofollow(prev_test, &psz, (off_t)1 << 31);
        int prc;
        if (pfd < 0) { printf("VD_BENCH_FAIL prev_test_unopenable\n"); return 3; }
        prc = vd_pack_load_fd(pfd, &pv);
        if (prc != VD_PACK_OK) {
            printf("VD_BENCH_FAIL prev_test_invalid (%s)\n", vd_pack_strerror(prc));
            close(pfd); return 3;
        }
        if ((long)pv.n != PROTO->prev_test_count) {
            printf("VD_BENCH_FAIL prev_test_count got=%zu want=%ld\n",
                   pv.n, PROTO->prev_test_count);
            free_pack(&pv); close(pfd); return 3;
        }
        if (vd_sha256_fd(pfd, pvsha) != 0 ||
            strcmp(pvsha, PROTO->prev_pack_sha) != 0) {
            printf("VD_BENCH_FAIL prev_test_digest_mismatch\n");
            free_pack(&pv); close(pfd); return 3;
        }
        close(pfd);
        for (a = 0; a < te.n; a++)
            for (b = 0; b < pv.n; b++)
                if (!strcmp(te.imgs[a].id, pv.imgs[b].id)) leaks++;
        if (leaks) {
            printf("VD_BENCH_FAIL spent_holdout_reuse=%zu\n", leaks);
            free_pack(&pv); return 3;
        }
        prev_ids_checked = pv.n;
        printf("disjoint: holdout vs %zu already-scored ids -> 0 overlap "
               "(count and digest pinned by protocol)\n", prev_ids_checked);
        free_pack(&pv);
        evidence_prev = 1;
    } else {
        evidence_prev = 1;
    }

    n = collect(&tr, &X, &Y, 20);  /* validation-time cost decision */
    if (!X || !Y) { printf("VD_BENCH_FAIL collect_alloc\n"); return 6; }
    printf("train rows=%zu dim=%zu\n", n, DIM);
    if (n < 1000) { printf("VD_BENCH_FAIL too_few_train_rows=%zu\n", n); return 4; }

    t0 = now_s();
    if (vd_btn_init_guarded(&head, DIM, NCLS, 24, 192, 0.5, 7u, "main") != 0) return 5;
    btn_set_ports(&head, RAWP(), CLSP());
    btn_train_dynamic(&head, X, Y, n, 2000, 200, 1e-5, 1e-7);

    /* Hard-negative mining, TRAIN ONLY (never val/test). The first pass sees a
       thin random sample of background, so the confident false positives it
       makes are exactly the rows worth adding. Two bounded rounds. */
    {
        int round;
        for (round = 0; round < 2; round++) {
            size_t i2, j2, added = 0, cap = n + 60000;
            double *X2 = (double *)realloc(X, cap * DIM * sizeof(double));
            double *Y2 = (double *)realloc(Y, cap * NCLS * sizeof(double));
            if (!X2 || !Y2) { printf("VD_BENCH_FAIL hnm_oom\n"); return 6; }
            X = X2; Y = Y2;
            for (i2 = 0; i2 < tr.n && n < cap; i2++) {
                const VImg *im = &tr.imgs[i2];
                size_t took = 0;
                for (j2 = 0; j2 < im->n_prop && n < cap && took < 15; j2++) {
                    double sc;
                    size_t d;
                    if (im->plabel[j2] != 0) continue;      /* negatives only */
                    sc = head_score(&head, im->pf + j2 * DIM);
                    if (!isfinite(sc)) { printf("VD_BENCH_FAIL hnm_forward_failed\n"); return 5; }
                    if (sc < 0.5) continue;                  /* only the FPs */
                    for (d = 0; d < DIM; d++) X[n * DIM + d] = im->pf[j2 * DIM + d];
                    Y[n * NCLS + 0] = 1.0; Y[n * NCLS + 1] = 0.0;
                    n++; added++; took++;
                }
            }
            printf("hard-negative round %d: added=%zu rows=%zu\n", round, added, n);
            if (added == 0) break;
            if (round + 1 < 2) {
                /* this model is what the NEXT scan uses, so it must exist now */
                btn_free(&head);
                if (vd_btn_init_guarded(&head, DIM, NCLS, 24, 192, 0.5, 7u, "hnm") != 0) return 5;
                btn_set_ports(&head, RAWP(), CLSP());
                btn_train_dynamic(&head, X, Y, n, 2000, 200, 1e-5, 1e-7);
            } else {
                /* Nothing scans this model, so its training is deferred past the
                   fork below. Same matrix, same seed, same call -- identical
                   result, just overlapped with the shuffle control. */
                need_final_train = 1;
            }
        }
    }

    /* ---- process-level overlap ------------------------------------------
       The training matrix is frozen at this point. The final main-head
       training and the label-shuffle control training both consume it and are
       independent of each other, so the control runs in a forked worker while
       the parent does the final main training. The child inherits the frozen
       matrix copy-on-write, uses the same RNG state and the same seed, and
       performs the identical training call -- the BTN update order is
       untouched. Nothing else is parallelised. */
    if (g_jobs > 1) {
        if (pipe(shuf_pipe) != 0) { printf("VD_BENCH_FAIL shuffle_pipe\n"); return 6; }
        if (sigwindow_enter(&sigwin) != 0) {
            printf("VD_BENCH_FAIL signal_setup\n");
            close(shuf_pipe[0]); close(shuf_pipe[1]);
            return 6;
        }
        shuf_pid = fork();                 /* forked with SIGINT/SIGTERM blocked */
        if (shuf_pid < 0) {
            printf("VD_BENCH_FAIL shuffle_fork\n");
            sigwindow_leave(&sigwin);
            close(shuf_pipe[0]); close(shuf_pipe[1]);
            return 6;
        }
        if (shuf_pid == 0) {
            sigwindow_child_reset(&sigwin);
            ShufResult r;
            close(shuf_pipe[0]);              /* child never reads */
            memset(&r, 0, sizeof r);
            if (!strcmp(g_worker_fault, "hang")) { for (;;) pause(); }
            if (!strcmp(g_worker_fault, "exit")) _exit(3);
            if (!strcmp(g_worker_fault, "crash")) abort();
            {
                double c0 = now_s();
                r.ok = run_shuffle_arm(&va, &te, X, Y, n, &r.ap_val, &r.ap_test);
                r.elapsed = now_s() - c0;
            }
            if (!strcmp(g_worker_fault, "partial")) {
                (void)!write(shuf_pipe[1], &r, sizeof r / 2);
                close(shuf_pipe[1]);
                for (;;) pause();             /* hold the pipe open, no EOF */
            }
            if (write(shuf_pipe[1], &r, sizeof r) != (ssize_t)sizeof r) _exit(2);
            close(shuf_pipe[1]);
            if (!strcmp(g_worker_fault, "nowait")) for (;;) pause();   /* message, no exit */
            _exit(0);
        }
        close(shuf_pipe[1]);                  /* parent never writes */
        /* Test hook: raise inside the exact window after fork and before the
           PID is published, while signals are still blocked. */
        if (g_sig_in_window) raise(g_sig_in_window);
        /* ONE absolute monotonic deadline, fixed at the successful fork. The
           message read and the post-message wait both consume it, so parent
           training time cannot silently extend the worker's budget. */
        g_worker_deadline = now_s() + g_shuffle_deadline_s;
        g_child_pid_sig = (sig_atomic_t)shuf_pid;
        /* PID and deadline are now published; only here does a pending signal
           become deliverable. */
        sigwindow_unblock(&sigwin);
        if (g_signal_caught) {
            printf("VD_BENCH_FAIL interrupted_in_fork_window=%d\n", (int)g_signal_caught);
            rc_final = 128 + (int)g_signal_caught;
            goto worker_cleanup;
        }
    }

    g_t_fork = now_s();
    if (need_final_train) {
        btn_free(&head);
        if (vd_btn_init_guarded(&head, DIM, NCLS, 24, 192, 0.5, 7u, "final") != 0)
            goto worker_cleanup;              /* never return while the child lives */
        btn_set_ports(&head, RAWP(), CLSP());
        btn_train_dynamic(&head, X, Y, n, 2000, 200, 1e-5, 1e-7);
    }
    if (g_signal_caught) {   /* checked again after the parent's own training */
        printf("VD_BENCH_FAIL interrupted_by_signal=%d\n", (int)g_signal_caught);
        rc_final = 128 + (int)g_signal_caught;
        goto worker_cleanup;
    }
    if (g_parent_fault_after_fork) {
        printf("VD_BENCH_FAIL parent_fault_after_fork (injected)\n");
        g_eval_fault = 1;
        goto worker_cleanup;
    }
    g_final_train_s = now_s() - g_t_fork;
    train_s = now_s() - t0;
    printf("head trained hidden=%lu train_s=%.1f\n", (unsigned long)head.hidden_count, train_s);

    /* randomized/untrained ablation: same shape, never trained */
    if (g_fail_random_init) {
        printf("VD_BENCH_FAIL btn_init_failed:random (injected)\n");
        g_eval_fault = 1;
        rc_final = 5;
        goto worker_cleanup;
    }
    if (vd_btn_init_guarded(&rndhead, DIM, NCLS, 24, 192, 0.5, 999u, "random") != 0) {
        rc_final = 5;
        goto worker_cleanup;
    }
    btn_set_ports(&rndhead, RAWP(), CLSP());

    /* linear logistic baseline on identical features */
    lin_train(&lin, X, Y, n, 12, 0.02);

    /* Collect the shuffle control: from the worker when forked, otherwise run
       it here. Either way it is the same computation. */
    if (shuf_pid > 0) {
        ShufResult r;
        int status = 0, bad = 0;
        memset(&r, 0, sizeof r);
        if (read_exact_deadline(shuf_pipe[0], &r, sizeof r, g_worker_deadline) != 0) {
            printf("VD_BENCH_FAIL shuffle_worker_no_message (timeout/EOF/partial)\n");
            bad = 1;
        }
        close(shuf_pipe[0]);
        shuf_pipe[0] = -1;
        if (g_signal_caught) {
            printf("VD_BENCH_FAIL interrupted_by_signal=%d\n", (int)g_signal_caught);
            bad = 1;
        }
        if (!bad) {
            /* The message arriving does not mean the child exited: the wait is
               bounded by the SAME absolute deadline and never blocks. */
            if (wait_child_deadline(shuf_pid, &status, g_worker_deadline) != 0) {
                printf("VD_BENCH_FAIL shuffle_worker_wait_timeout\n");
                bad = 1;
            } else if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
                printf("VD_BENCH_FAIL shuffle_worker_status=%d\n", status);
                bad = 1;
            } else if (r.ok != 1 || !isfinite(r.ap_val) || !isfinite(r.ap_test) ||
                       !isfinite(r.elapsed)) {
                printf("VD_BENCH_FAIL shuffle_worker_bad_result\n");
                bad = 1;
            } else {
                shuf_pid = -1;              /* collected */
                if (g_sig_after_reap) raise(g_sig_after_reap);
                if (interrupted()) {
                    printf("VD_BENCH_FAIL interrupted_after_reap=%d\n", (int)g_signal_caught);
                    bad = 1;
                } else {
                    shuf = r;               /* accepted only under zero signal */
                }
            }
        }
        if (bad) { g_eval_fault = 1; if (!rc_final) rc_final = 6; goto worker_cleanup; }
        if (interrupted()) { rc_final = 128 + (int)g_signal_caught; goto worker_cleanup; }
        /* handlers restored only now, with the result accepted and no signal */
        sigwindow_leave(&sigwin);
    } else {
        double c0 = now_s();
        shuf.ok = run_shuffle_arm(&va, &te, X, Y, n, &shuf.ap_val, &shuf.ap_test);
        shuf.elapsed = now_s() - c0;
        if (!shuf.ok) g_eval_fault = 1;
    }
    printf("VAL ap50_labelshuffle=%.6f\n", shuf.ap_val);
    /* The concurrent window is NOT the time saved: the two trainings contend for
       memory bandwidth and both dilate, so the honest figure is the measured
       wall reduction against a serial run, not this window. */
    printf("PERF jobs=%d final_train_s=%.1f shuffle_s=%.1f overlap_window_s=%.1f\n",
           g_jobs, g_final_train_s, shuf.elapsed,
           g_jobs > 1 ? (shuf.elapsed < g_final_train_s ? shuf.elapsed : g_final_train_s) : 0.0);

    /* ---- validation (threshold + bar setting happen HERE, never on test) -- */
    {
        VdDet **own; VdImage *v;
        size_t h, t;
        double ap, apr, apl, bestf = -1, bestthr = 0.5;
        int s;
        split_prop_recall(&va, 0.5, &h, &t);
        printf("VAL proposal_recall=%.6f (%zu/%zu)\n", t ? (double)h/t : 0.0, h, t);
        v = run_detector(&va, &head, 0.30, &own);
        ap = ap_or_fault(v, va.n);
        for (s = 1; s < 20; s++) {
            double thr = s * 0.05, pr, rc, f1;
            if (!v || vd_pr_at(v, va.n, 0.5, thr, &pr, &rc) != 0) g_eval_fault = 1;
            f1 = (pr + rc) > 0 ? 2 * pr * rc / (pr + rc) : 0.0;
            if (f1 > bestf) { bestf = f1; bestthr = thr; }
        }
        free_det(v, own, va.n);
        v = run_detector(&va, &rndhead, 0.30, &own); apr = ap_or_fault(v, va.n); free_det(v, own, va.n);
        v = run_linear(&va, &lin, 0.30, &own); apl = ap_or_fault(v, va.n); free_det(v, own, va.n);
        printf("VAL ap50_cnet=%.6f ap50_random=%.6f ap50_linear=%.6f best_thr=%.2f best_f1=%.4f\n",
               ap, apr, apl, bestthr, bestf);
        val_thr = bestthr;      /* the one parameter chosen on validation */
        g_val_ap = ap;          /* feeds the protocol floor formula below */
        printf("VAL_FROZEN_BARS ap50_floor=%.6f ratio_floor=3.0 margin_floor=0.05 thr=%.2f\n",
               ap * 0.5 > 0.10 ? ap * 0.5 : 0.10, val_thr);
    }
    /* ================== SINGLE SCORED TEST RUN ==========================
       Bars were frozen from validation above. Nothing below feeds back into
       any hyperparameter, threshold, seed or architecture choice. */
    {
        VdDet **own; VdImage *v;
        size_t h, t, ndet = 0, i2;
        double ap, ap2, apr, apl, aps, pr, rc, prop_rec;
        double thr = val_thr;   /* frozen on validation above; no test feedback */
        /* protocol floor: max(0.10, 0.50 x AP50_val), computed rather than
           hardcoded so a stronger validation run raises the bar as specified. */
        double floor_ap = 0.5 * g_val_ap > 0.10 ? 0.5 * g_val_ap : 0.10;
        double ratio_floor = 3.0, margin_floor = 0.05;
        int pass_floor, pass_ratio, pass_margin, pass_det, levelA;
        double t_test0 = now_s(), test_s;

        split_prop_recall(&te, 0.5, &h, &t);
        prop_rec = t ? (double)h / t : 0.0;

        v = run_detector(&te, &head, 0.30, &own);
        ap = ap_or_fault(v, te.n);
        if (!v || vd_pr_at(v, te.n, 0.5, thr, &pr, &rc) != 0) g_eval_fault = 1;
        if (v) for (i2 = 0; i2 < te.n; i2++) ndet += v[i2].n_det;
        free_det(v, own, te.n);

        /* determinism: identical rerun of the same scored path */
        v = run_detector(&te, &head, 0.30, &own); ap2 = ap_or_fault(v, te.n); free_det(v, own, te.n);

        v = run_detector(&te, &rndhead, 0.30, &own); apr = ap_or_fault(v, te.n); free_det(v, own, te.n);
        v = run_linear(&te, &lin, 0.30, &own); apl = ap_or_fault(v, te.n); free_det(v, own, te.n);
        /* identical full evaluation of the frozen label-shuffle head */
        aps = shuf.ap_test;   /* identical full evaluation, computed above */
        test_s = now_s() - t_test0;

        if (!isfinite(ap) || !isfinite(ap2) || !isfinite(apr) || !isfinite(apl) ||
            !isfinite(aps) || !isfinite(pr) || !isfinite(rc)) g_eval_fault = 1;
        pass_floor  = isfinite(ap) && ap >= floor_ap;
        pass_ratio  = apr > 0 ? (ap >= ratio_floor * apr) : (ap > 0);
        pass_margin = (ap - apr) >= margin_floor;
        pass_det    = fabs(ap - ap2) <= 1e-9;
        /* Level A is the metric bars AND the evidence predicates. A stale cache,
           an unverified manifest, or a missing spent-holdout check cannot yield a
           PASS however good the numbers look. */
        levelA = pass_floor && pass_ratio && pass_margin && pass_det &&
                 evidence_manifest && evidence_prev && evidence_leak && !g_eval_fault;

        printf("TEST proposal_recall=%.6f (%zu/%zu)\n", prop_rec, h, t);
        printf("TEST ap50_cnet=%.6f ap50_random=%.6f ap50_linear=%.6f ap50_labelshuffle=%.6f\n",
               ap, apr, apl, aps);
        printf("TEST precision@%.2f=%.6f recall@%.2f=%.6f detections=%zu gt=%zu\n",
               thr, pr, thr, rc, ndet, t);
        printf("TEST determinism |ap-ap_rerun|=%.3g\n", fabs(ap - ap2));
        printf("BARS floor=%.3f -> %s | ratio>=%.1f -> %s (%.1fx) | margin>=%.3f -> %s (%.4f) | det -> %s\n",
               floor_ap, pass_floor ? "PASS" : "FAIL",
               ratio_floor, pass_ratio ? "PASS" : "FAIL", apr > 0 ? ap / apr : 0.0,
               margin_floor, pass_margin ? "PASS" : "FAIL", ap - apr,
               pass_det ? "PASS" : "FAIL");
        printf("EVIDENCE manifest=%d prev_holdout=%d leakage=%d eval_fault=%d\n",
               evidence_manifest, evidence_prev, evidence_leak, g_eval_fault);

        /* Results are evidence: build the document in memory, then publish it
           atomically. If publication fails the run has produced no record, so
           it must fail -- printing PASS with no artefact is not an option. */
        if (g_sig_before_publish) g_signal_caught = g_sig_before_publish;
        if (interrupted()) {
            printf("VD_BENCH_FAIL interrupted_before_publication=%d\n", (int)g_signal_caught);
            rc_final = 128 + (int)g_signal_caught;
            goto worker_cleanup;
        }
        {
            char *buf = (char *)malloc(8192);
            size_t cap = 8192, len = 0;
            int trunc = 0;
#define JS(...) do { \
    int _n = snprintf(buf + len, cap - len, __VA_ARGS__); \
    if (_n < 0 || (size_t)_n >= cap - len) trunc = 1; else len += (size_t)_n; \
} while (0)
            if (!buf) { printf("VD_BENCH_FAIL json_alloc\n"); rc_final = 7; goto worker_cleanup; }
            JS("{\n  \"schema_version\": 2,\n");
            JS("  \"gpu_policy\": \"CPU only; ROCR/HIP/CUDA_VISIBLE_DEVICES empty\",\n");
            JS("  \"dataset\": \"%s\", \"class\": \"%s\",\n", PROTO->dataset, PROTO->cls);
            JS("  \"protocol\": \"%s\", \"protocol_source\": \"pre-registered in vd_protocol.c; not cache-declared\",\n", PROTO->name);
            JS("  \"note_md5\": \"VOC tar MD5s are verified by the fetch step, not by this binary; not asserted here\",\n");
            JS("  \"variant\": \"%s\",\n", G_VARIANT);
            JS("  \"split\": {\"train_img\": %zu, \"val_img\": %zu, \"test_img\": %zu, \"test_gt\": %zu},\n",
               tr.n, va.n, te.n, t);
            JS("  \"split_key\": \"sha256 image content hash (trainval); seed 20260727 shuffle slice (holdout)\",\n");
            JS("  \"holdout_slice_offset\": %ld,\n", man.test_offset);
            JS("  \"holdout_disjoint_from_spent_ids\": %zu, \"holdout_spent_overlap\": 0,\n",
               prev_ids_checked);
            JS("  \"artifact_root\": \"%s\", \"artifact_root_source\": \"recomputed from held descriptors; equals pinned protocol root\",\n", g_artifact_root);
            JS("  \"manifest\": {\"verified\": %d, \"sha256_train_pack\": \"%s\", \"sha256_val_pack\": \"%s\", \"sha256_test_pack\": \"%s\", \"sha256_pca_bin\": \"%s\", \"sha256_prev_test_pack\": \"%s\"},\n",
               evidence_manifest, man.sha_train, man.sha_val, man.sha_test, man.sha_pca, man.sha_prev);
            JS("  \"evidence\": {\"manifest\": %d, \"prev_holdout\": %d, \"leakage\": %d, \"eval_fault\": %d},\n",
               evidence_manifest, evidence_prev, evidence_leak, g_eval_fault);
            JS("  \"leakage\": {\"measured_cross_split_id_overlap\": 0, \"measured_cross_split_content_overlap\": 0, \"measured_intra_split_duplicate_images\": %ld},\n", g_intra_dups);
            JS("  \"frontend\": \"%s\",\n", G_FRONTEND);
            JS("  \"cnet_learned_component\": \"BTN head PORT_RAW%zu -> PORT_ONEHOT2\",\n", DIM);
            JS("  \"train_rows\": %zu, \"train_seconds\": %.1f, \"test_seconds\": %.1f,\n", n, train_s, test_s);
            JS("  \"perf\": {\"jobs\": %d, \"final_train_s\": %.1f, \"shuffle_s\": %.1f, \"wall_s\": %.1f},\n",
               g_jobs, g_final_train_s, shuf.elapsed, now_s() - g_t_start);
            JS("  \"proposal_recall_test\": %.6f,\n", prop_rec);
            JS("  \"ap50_cnet_test\": %.6f,\n", ap);
            JS("  \"ap50_random_head_test\": %.6f,\n", apr);
            JS("  \"ap50_linear_baseline_test\": %.6f,\n", apl);
            JS("  \"ap50_label_shuffle_test\": %.6f,\n", aps);
            JS("  \"precision_at_thr\": %.6f, \"recall_at_thr\": %.6f, \"thr\": %.2f,\n", pr, rc, thr);
            JS("  \"detections\": %zu,\n", ndet);
            JS("  \"determinism_abs_delta\": %.3g,\n", fabs(ap - ap2));
            JS("  \"bars\": {\"ap50_floor\": %.3f, \"ratio_floor\": %.1f, \"margin_floor\": %.3f},\n",
               floor_ap, ratio_floor, margin_floor);
            JS("  \"bar_results\": {\"floor\": %d, \"ratio\": %d, \"margin\": %d, \"determinism\": %d},\n",
               pass_floor, pass_ratio, pass_margin, pass_det);
            JS("  \"verdict_level_a\": \"%s\",\n",
               levelA ? "VISION_DETECTION_MECHANISM_PASS" : "VISION_DETECTION_MECHANISM_WITHHELD");
            JS("  \"verdict_level_b\": \"VISION_CAPSULE_PORTABILITY_WITHHELD\",\n");
            JS("  \"verdict_level_b_reason\": \"PORT_RAW has no honest coverage gate in coverage_family_gated(); unchanged this pass\",\n");
            JS("  \"verdict_level_c\": \"VISION_SPECIALIST_COMPETES_WITHHELD\",\n");
            JS("  \"verdict_level_c_reason\": \"no pretrained reference detector run in this pass\"\n}\n");
#undef JS
            if (trunc) { free(buf); printf("VD_BENCH_FAIL json_truncated\n"); rc_final = 7; goto worker_cleanup; }
            if (interrupted()) {
                free(buf);
                printf("VD_BENCH_FAIL interrupted_before_publication=%d\n", (int)g_signal_caught);
                rc_final = 128 + (int)g_signal_caught;
                goto worker_cleanup;
            }
            if (vd_publish_file(jsonp, buf, len) != 0) {
                free(buf);
                printf("VD_BENCH_FAIL json_publish_failed:%s\n", jsonp);
                rc_final = 7;
                goto worker_cleanup;
            }
            free(buf);
            printf("results published: %s (%zu bytes)\n", jsonp, len);
        }
        if (interrupted()) {
            printf("VD_BENCH_FAIL interrupted_before_verdict=%d\n", (int)g_signal_caught);
            rc_final = 128 + (int)g_signal_caught;
            goto worker_cleanup;
        }
        printf("%s\n", levelA ? "VISION_DETECTION_MECHANISM_PASS"
                               : "VISION_DETECTION_MECHANISM_WITHHELD");
        printf("VISION_CAPSULE_PORTABILITY_WITHHELD reason=port_raw_has_no_honest_coverage_gate\n");
        printf("VISION_SPECIALIST_COMPETES_WITHHELD reason=no_pretrained_reference_this_pass\n");
    }

worker_cleanup:
    /* The single exit route while a worker may be live. Every post-fork failure
       path -- training, random-head init, allocation, evaluation, signal,
       publication -- arrives here rather than returning. */
    if (shuf_pid > 0) {
        if (shuf_pipe[0] >= 0) { close(shuf_pipe[0]); shuf_pipe[0] = -1; }
        if (reap_child_bounded(shuf_pid, now_s() + 2.0, now_s() + 4.0) != 0) {
            printf("VD_BENCH_FAIL shuffle_worker_uncollectable pid=%d\n", (int)shuf_pid);
        }
        shuf_pid = -1;
        if (!rc_final) rc_final = 6;
        g_eval_fault = 1;
    }
    sigwindow_leave(&sigwin);
    if (g_signal_caught && !rc_final) rc_final = 128 + (int)g_signal_caught;
    free(X); free(Y);
    free_pack(&tr); free_pack(&va);
    btn_free(&head); btn_free(&rndhead);
    free_pack(&te);
    snap_close(&snap);
    if (rc_final) return rc_final;
    printf("PERF wall_s=%.1f\n", now_s() - g_t_start);
    printf("btn_calls_total=%ld\n", g_btn_calls);
    printf("VD_BENCH_DONE\n");
    return 0;
}
