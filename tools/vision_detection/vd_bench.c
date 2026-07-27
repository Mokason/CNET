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
static int g_eval_fault = 0;
static long g_intra_dups = 0;   /* set by any non-finite metric or NMS refusal */
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

static int load_pack(const char *path, Pack *p) {
    int rc = vd_pack_load(path, p);
    if (rc != VD_PACK_OK)
        fprintf(stderr, "VD_BENCH_FAIL pack_invalid:%s (%s)\n", path, vd_pack_strerror(rc));
    return rc;
}
static void free_pack(Pack *p) { vd_pack_free(p); }

/* ---------------- evidence gate ------------------------------------------
   Identity comes from a protocol chosen by the scoring target and compiled into
   vd_protocol.c. The cache is checked against it. Nothing the cache says about
   itself is allowed to decide what is required of it -- that was the hole that
   let a cache declaring test_offset 0 skip the spent-holdout check entirely. */

static int hash_matches(const char *cache, const char *name, const char *want,
                        const char *label) {
    char path[VD_PATH_MAX], got[65];
    if (!want || !*want || !strcmp(want, "none")) {
        printf("VD_BENCH_FAIL missing_digest:%s\n", label);
        return -1;
    }
    if ((size_t)snprintf(path, sizeof path, "%s/%s", cache, name) >= sizeof path) return -1;
    if (vd_sha256_file(path, got) != 0) {
        printf("VD_BENCH_FAIL artefact_unreadable:%s\n", name);
        return -1;
    }
    if (strcmp(got, want) != 0) {
        printf("VD_BENCH_FAIL artefact_hash_mismatch:%s\n", name);
        return -1;
    }
    return 0;
}

/* Recompute a root from the sidecar itself and require it to equal BOTH the
   manifest's claim and the protocol's pinned constant. */
static int root_matches(const char *cache, const char *name, const char *declared,
                        const char *pinned, size_t expect_lines, const char *label) {
    char path[VD_PATH_MAX], got[65];
    size_t nlines = 0;
    if ((size_t)snprintf(path, sizeof path, "%s/%s", cache, name) >= sizeof path) return -1;
    if (vd_root_of_file(path, got, &nlines) != 0) {
        printf("VD_BENCH_FAIL sidecar_unreadable:%s\n", name);
        return -1;
    }
    if (expect_lines && nlines != expect_lines) {
        printf("VD_BENCH_FAIL sidecar_count:%s got=%zu want=%zu\n", label, nlines, expect_lines);
        return -1;
    }
    if (strcmp(got, declared) != 0) {
        printf("VD_BENCH_FAIL root_vs_manifest:%s\n", label);
        return -1;
    }
    if (!pinned || !*pinned) {
        printf("VD_BENCH_FAIL root_unpinned:%s (protocol has no committed root)\n", label);
        return -1;
    }
    if (strcmp(got, pinned) != 0) {
        printf("VD_BENCH_FAIL root_vs_protocol:%s\n", label);
        return -1;
    }
    return 0;
}

/* The sidecar must describe the same images the pack actually contains. */
static int ids_match_pack(const char *cache, const char *name, const Pack *pk,
                          const char *label) {
    char path[VD_PATH_MAX], line[256];
    FILE *f;
    size_t n = 0, i;
    char **have;
    int bad = 0;
    if ((size_t)snprintf(path, sizeof path, "%s/%s", cache, name) >= sizeof path) return -1;
    f = fopen(path, "rb");
    if (!f) return -1;
    have = (char **)calloc(pk->n ? pk->n : 1, sizeof(char *));
    if (!have) { fclose(f); return -1; }
    while (fgets(line, sizeof line, f)) {
        size_t l = strlen(line);
        if (l && line[l - 1] == '\n') line[--l] = 0;
        if (!l) continue;
        if (n >= pk->n) { bad = 1; break; }
        have[n] = strdup(line);
        if (!have[n]) { bad = 1; break; }
        n++;
    }
    fclose(f);
    if (!bad && n != pk->n) bad = 1;
    if (!bad) {
        for (i = 0; i < pk->n && !bad; i++) {
            size_t k;
            int found = 0;
            for (k = 0; k < n; k++)
                if (!strcmp(have[k], pk->imgs[i].id)) { found = 1; break; }
            if (!found) bad = 1;
        }
    }
    for (i = 0; i < n; i++) free(have[i]);
    free(have);
    if (bad) { printf("VD_BENCH_FAIL sidecar_pack_mismatch:%s\n", label); return -1; }
    return 0;
}


/* Leakage computed by the bench itself, from the packs and sidecars on disk --
   never taken from the manifest's own claim about itself. */
static int str_cmp(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static size_t count_dup_across(char **v, size_t n) {
    size_t i, dups = 0;
    qsort(v, n, sizeof *v, str_cmp);
    for (i = 1; i < n; i++)
        if (!strcmp(v[i - 1], v[i])) dups++;
    return dups;
}

/* IDs of all three packs pooled; any repeat is a split overlap. */
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

/* Same, for the source-content hashes carried by the sidecars.
   The question is CROSS-split overlap: a hash appearing under two different
   splits. A hash repeated inside one split is a duplicate image, not leakage --
   VOC2007 trainval genuinely contains 3 such pairs, and the content-addressed
   split deliberately keeps both copies on the same side. Those are counted
   separately and reported as a diagnostic. */
typedef struct { char *h; int split; } CHash;

static int chash_cmp(const void *a, const void *b) {
    const CHash *x = (const CHash *)a, *y = (const CHash *)b;
    int c = strcmp(x->h, y->h);
    if (c) return c;
    return x->split - y->split;
}

static long bench_content_overlap(const char *cache, long *intra_dups) {
    static const char *files[3] = {"content_train.txt", "content_val.txt", "content_test.txt"};
    char path[VD_PATH_MAX], line[256];
    CHash *v = NULL;
    size_t n = 0, cap = 0, i;
    long cross = 0, intra = 0;
    int f_i;
    if (intra_dups) *intra_dups = 0;
    for (f_i = 0; f_i < 3; f_i++) {
        FILE *f;
        if ((size_t)snprintf(path, sizeof path, "%s/%s", cache, files[f_i]) >= sizeof path) goto fail;
        f = fopen(path, "rb");
        if (!f) goto fail;
        while (fgets(line, sizeof line, f)) {
            size_t l = strlen(line);
            if (l && line[l - 1] == '\n') line[--l] = 0;
            if (!l) continue;
            if (n == cap) {
                CHash *nv;
                cap = cap ? cap * 2 : 4096;
                nv = (CHash *)realloc(v, cap * sizeof *v);
                if (!nv) { fclose(f); goto fail; }
                v = nv;
            }
            v[n].h = strdup(line);
            if (!v[n].h) { fclose(f); goto fail; }
            v[n].split = f_i;
            n++;
        }
        fclose(f);
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
/* softmax-ish confidence from the head's two raw outputs */
static double head_score(BinaryTransformNetwork *btn, const float *f) {
    double in[DIM_MAX];
    const double *o;
    size_t d;
    for (d = 0; d < DIM; d++) in[d] = f[d];
    o = btn_forward(btn, in);
    if (!o) return 0.0;
    {
        double a = o[0], b = o[1], m = a > b ? a : b;
        double ea = exp(a - m), eb = exp(b - m);
        return eb / (ea + eb);
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
}

static void free_det(VdImage *v, VdDet **own, size_t n) {
    size_t i;
    if (own) for (i = 0; i < n; i++) free(own[i]);
    free(own); free(v);
}

/* A detector arm that could not allocate returns NULL; scoring it as 0.0 would
   quietly turn an out-of-memory event into a benchmark number. */
static double ap_or_fault(VdImage *v, size_t n) {
    if (!v) { g_eval_fault = 1; return NAN; }
    return vd_ap50(v, n, 0.5);
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

int main(int argc, char **argv) {
    const char *cache = "data/vision_cache";
    const char *jsonp = "logs/vision_detection_bench.json";
    const char *prev_test = NULL;   /* pack whose holdout is already spent */
    char variant[80] = "", frontend[200] = "";
    size_t prev_ids_checked = 0;
    double val_thr = 0.5;           /* set on validation, never on test */
    double g_val_ap = 0.0;          /* validation AP50, for the floor formula */
    VdManifest man;
    const VdProtocol *PROTO = NULL;
    char mpath[VD_PATH_MAX];
    int evidence_manifest = 0, evidence_prev = 0, evidence_leak = 0;
    Pack tr, va, te;
    double *X = NULL, *Y = NULL;
    size_t n = 0;
    BinaryTransformNetwork head, rndhead, sh;
    Lin lin;
    double t0, train_s;
    int i;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--cache") && i + 1 < argc) cache = argv[++i];
        else if (!strcmp(argv[i], "--json") && i + 1 < argc) jsonp = argv[++i];
        else if (!strcmp(argv[i], "--prev-test") && i + 1 < argc) prev_test = argv[++i];
        else if (!strcmp(argv[i], "--protocol") && i + 1 < argc) {
            PROTO = vd_protocol_get(argv[++i]);
            if (!PROTO) { printf("VD_BENCH_FAIL unknown_protocol:%s\n", argv[i]); return 3; }
        }
    }

    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("vd_bench: CPU-only (ROCR/HIP/CUDA_VISIBLE_DEVICES empty)\n");
    {
        char p1[512], p2[512], p3[512];
        snprintf(p1, sizeof p1, "%s/train.pack", cache);
        snprintf(p2, sizeof p2, "%s/val.pack", cache);
        snprintf(p3, sizeof p3, "%s/test.pack", cache);
        if (load_pack(p1, &tr) || load_pack(p2, &va) || load_pack(p3, &te)) return 2;
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
        snprintf(mpath, sizeof mpath, "%s/manifest.txt", cache);
        if (vd_manifest_parse(mpath, &man, err, sizeof err) != 0) {
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
        if (hash_matches(cache, "train.pack", man.sha_train, "train.pack") != 0 ||
            hash_matches(cache, "val.pack",   man.sha_val,   "val.pack")   != 0 ||
            hash_matches(cache, "test.pack",  man.sha_test,  "test.pack")  != 0 ||
            hash_matches(cache, "pca.bin",    man.sha_pca,   "pca.bin")    != 0 ||
            hash_matches(cache, "ids_train.txt",     man.sha_ids_train,     "ids_train")     != 0 ||
            hash_matches(cache, "ids_val.txt",       man.sha_ids_val,       "ids_val")       != 0 ||
            hash_matches(cache, "ids_test.txt",      man.sha_ids_test,      "ids_test")      != 0 ||
            hash_matches(cache, "content_train.txt", man.sha_content_train, "content_train") != 0 ||
            hash_matches(cache, "content_val.txt",   man.sha_content_val,   "content_val")   != 0 ||
            hash_matches(cache, "content_test.txt",  man.sha_content_test,  "content_test")  != 0) {
            return 3;
        }
        if (root_matches(cache, "ids_test.txt", man.id_root_test, PROTO->id_root_test,
                         (size_t)PROTO->test_count, "id_root_test") != 0 ||
            root_matches(cache, "content_test.txt", man.content_root_test,
                         PROTO->content_root_test, (size_t)PROTO->test_count,
                         "content_root_test") != 0 ||
            root_matches(cache, "ids_train.txt", man.id_root_train, PROTO->id_root_train,
                         (size_t)PROTO->train_img, "id_root_train") != 0 ||
            root_matches(cache, "ids_val.txt", man.id_root_val, PROTO->id_root_val,
                         (size_t)PROTO->val_img, "id_root_val") != 0 ||
            root_matches(cache, "content_train.txt", man.content_root_train,
                         PROTO->content_root_train, (size_t)PROTO->train_img,
                         "content_root_train") != 0 ||
            root_matches(cache, "content_val.txt", man.content_root_val,
                         PROTO->content_root_val, (size_t)PROTO->val_img,
                         "content_root_val") != 0) {
            return 3;
        }
        if (ids_match_pack(cache, "ids_train.txt", &tr, "train") != 0 ||
            ids_match_pack(cache, "ids_val.txt",   &va, "val")   != 0 ||
            ids_match_pack(cache, "ids_test.txt",  &te, "test")  != 0) {
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
        long ct_dups = bench_content_overlap(cache, &intra_dups);
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
        if (load_pack(prev_test, &pv)) { printf("VD_BENCH_FAIL prev_test_unreadable\n"); return 3; }
        if ((long)pv.n != PROTO->prev_test_count) {
            printf("VD_BENCH_FAIL prev_test_count got=%zu want=%ld\n",
                   pv.n, PROTO->prev_test_count);
            free_pack(&pv); return 3;
        }
        if (vd_sha256_file(prev_test, pvsha) != 0 ||
            strcmp(pvsha, PROTO->prev_pack_sha) != 0) {
            printf("VD_BENCH_FAIL prev_test_digest_mismatch\n");
            free_pack(&pv); return 3;
        }
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
    memset(&head, 0, sizeof head);
    if (btn_init(&head, DIM, NCLS, 24, 192, 0.5, 7u) != 0) { printf("VD_BENCH_FAIL btn_init\n"); return 5; }
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
                    if (sc < 0.5) continue;                  /* only the FPs */
                    for (d = 0; d < DIM; d++) X[n * DIM + d] = im->pf[j2 * DIM + d];
                    Y[n * NCLS + 0] = 1.0; Y[n * NCLS + 1] = 0.0;
                    n++; added++; took++;
                }
            }
            printf("hard-negative round %d: added=%zu rows=%zu\n", round, added, n);
            if (added == 0) break;
            btn_free(&head);
            memset(&head, 0, sizeof head);
            btn_init(&head, DIM, NCLS, 24, 192, 0.5, 7u);
            btn_set_ports(&head, RAWP(), CLSP());
            btn_train_dynamic(&head, X, Y, n, 2000, 200, 1e-5, 1e-7);
        }
    }
    train_s = now_s() - t0;
    printf("head trained hidden=%lu train_s=%.1f\n", (unsigned long)head.hidden_count, train_s);

    /* randomized/untrained ablation: same shape, never trained */
    memset(&rndhead, 0, sizeof rndhead);
    btn_init(&rndhead, DIM, NCLS, 24, 192, 0.5, 999u);
    btn_set_ports(&rndhead, RAWP(), CLSP());

    /* linear logistic baseline on identical features */
    lin_train(&lin, X, Y, n, 12, 0.02);

    /* label-shuffle control: trained once here, then evaluated on validation AND
       on the holdout with the identical full pipeline. Nothing is tuned from it. */
    {
        double *Ys = (double *)malloc(n * NCLS * sizeof(double));
        size_t k;
        if (!Ys) { printf("VD_BENCH_FAIL shuffle_alloc\n"); return 6; }
        memcpy(Ys, Y, n * NCLS * sizeof(double));
        for (k = n; k > 1; k--) {
            size_t j = (size_t)(rnd() % k);
            double t;
            t = Ys[(k-1)*NCLS+0]; Ys[(k-1)*NCLS+0] = Ys[j*NCLS+0]; Ys[j*NCLS+0] = t;
            t = Ys[(k-1)*NCLS+1]; Ys[(k-1)*NCLS+1] = Ys[j*NCLS+1]; Ys[j*NCLS+1] = t;
        }
        memset(&sh, 0, sizeof sh);
        if (btn_init(&sh, DIM, NCLS, 24, 192, 0.5, 7u) != 0) { printf("VD_BENCH_FAIL btn_init_shuffle\n"); return 5; }
        btn_set_ports(&sh, RAWP(), CLSP());
        btn_train_dynamic(&sh, X, Ys, n, 2000, 200, 1e-5, 1e-7);
        {
            VdDet **own; VdImage *v = run_detector(&va, &sh, 0.30, &own);
            printf("VAL ap50_labelshuffle=%.6f\n", ap_or_fault(v, va.n));
            free_det(v, own, va.n);
        }
        free(Ys);
    }

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
            if (vd_pr_at(v, va.n, 0.5, thr, &pr, &rc) != 0) g_eval_fault = 1;
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
        if (vd_pr_at(v, te.n, 0.5, thr, &pr, &rc) != 0) g_eval_fault = 1;
        for (i2 = 0; i2 < te.n; i2++) ndet += v[i2].n_det;
        free_det(v, own, te.n);

        /* determinism: identical rerun of the same scored path */
        v = run_detector(&te, &head, 0.30, &own); ap2 = ap_or_fault(v, te.n); free_det(v, own, te.n);

        v = run_detector(&te, &rndhead, 0.30, &own); apr = ap_or_fault(v, te.n); free_det(v, own, te.n);
        v = run_linear(&te, &lin, 0.30, &own); apl = ap_or_fault(v, te.n); free_det(v, own, te.n);
        /* identical full evaluation of the frozen label-shuffle head */
        v = run_detector(&te, &sh, 0.30, &own); aps = ap_or_fault(v, te.n); free_det(v, own, te.n);
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
        {
            char *buf = (char *)malloc(8192);
            size_t cap = 8192, len = 0;
            int trunc = 0;
#define JS(...) do { \
    int _n = snprintf(buf + len, cap - len, __VA_ARGS__); \
    if (_n < 0 || (size_t)_n >= cap - len) trunc = 1; else len += (size_t)_n; \
} while (0)
            if (!buf) { printf("VD_BENCH_FAIL json_alloc\n"); return 7; }
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
            JS("  \"manifest\": {\"verified\": %d, \"sha256_train_pack\": \"%s\", \"sha256_val_pack\": \"%s\", \"sha256_test_pack\": \"%s\", \"sha256_pca_bin\": \"%s\", \"sha256_prev_test_pack\": \"%s\"},\n",
               evidence_manifest, man.sha_train, man.sha_val, man.sha_test, man.sha_pca, man.sha_prev);
            JS("  \"evidence\": {\"manifest\": %d, \"prev_holdout\": %d, \"leakage\": %d, \"eval_fault\": %d},\n",
               evidence_manifest, evidence_prev, evidence_leak, g_eval_fault);
            JS("  \"leakage\": {\"measured_cross_split_id_overlap\": 0, \"measured_cross_split_content_overlap\": 0, \"measured_intra_split_duplicate_images\": %ld},\n", g_intra_dups);
            JS("  \"frontend\": \"%s\",\n", G_FRONTEND);
            JS("  \"cnet_learned_component\": \"BTN head PORT_RAW%zu -> PORT_ONEHOT2\",\n", DIM);
            JS("  \"train_rows\": %zu, \"train_seconds\": %.1f, \"test_seconds\": %.1f,\n", n, train_s, test_s);
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
            if (trunc) { free(buf); printf("VD_BENCH_FAIL json_truncated\n"); return 7; }
            if (vd_publish_file(jsonp, buf, len) != 0) {
                free(buf);
                printf("VD_BENCH_FAIL json_publish_failed:%s\n", jsonp);
                return 7;
            }
            free(buf);
            printf("results published: %s (%zu bytes)\n", jsonp, len);
        }
        printf("%s\n", levelA ? "VISION_DETECTION_MECHANISM_PASS"
                               : "VISION_DETECTION_MECHANISM_WITHHELD");
        printf("VISION_CAPSULE_PORTABILITY_WITHHELD reason=port_raw_has_no_honest_coverage_gate\n");
        printf("VISION_SPECIALIST_COMPETES_WITHHELD reason=no_pretrained_reference_this_pass\n");
    }

    free(X); free(Y);
    free_pack(&tr); free_pack(&va);
    btn_free(&head); btn_free(&rndhead);
    free_pack(&te);
    printf("VD_BENCH_DONE\n");
    return 0;
}
