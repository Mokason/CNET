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

/* ---------------- manifest ----------------------------------------------- */
/* Binds a cache directory to the prep run that produced it. Without this the
   bench will happily score whatever packs it is pointed at. */
typedef struct {
    int present;
    char variant[80];
    long pca_dim, test_offset, test_count;
    long train_img, val_img, test_img;
    long train_prop, val_prop, test_prop;
    long hog_side, color, hog_dim;
    long prev_ids_checked, prev_sha_checked;
    long prev_id_overlap, prev_content_overlap;
    long tv_id_overlap, tv_content_overlap;
    char sha_train[80], sha_val[80], sha_test[80], sha_pca[80], sha_prev[80];
} Manifest;

static int man_read(const char *cache, Manifest *m) {
    char path[VD_PATH_MAX], k[64], v[80];
    FILE *f;
    memset(m, 0, sizeof *m);
    if ((size_t)snprintf(path, sizeof path, "%s/manifest.txt", cache) >= sizeof path) return -1;
    f = fopen(path, "r");
    if (!f) return -1;
    while (fscanf(f, "%63s %79s", k, v) == 2) {
        if (!strcmp(k, "variant")) snprintf(m->variant, sizeof m->variant, "%s", v);
        else if (!strcmp(k, "pca_dim")) m->pca_dim = atol(v);
        else if (!strcmp(k, "hog_side")) m->hog_side = atol(v);
        else if (!strcmp(k, "color")) m->color = atol(v);
        else if (!strcmp(k, "hog_dim")) m->hog_dim = atol(v);
        else if (!strcmp(k, "test_offset")) m->test_offset = atol(v);
        else if (!strcmp(k, "test_count")) m->test_count = atol(v);
        else if (!strcmp(k, "train_img")) m->train_img = atol(v);
        else if (!strcmp(k, "val_img")) m->val_img = atol(v);
        else if (!strcmp(k, "test_img")) m->test_img = atol(v);
        else if (!strcmp(k, "train_prop")) m->train_prop = atol(v);
        else if (!strcmp(k, "val_prop")) m->val_prop = atol(v);
        else if (!strcmp(k, "test_prop")) m->test_prop = atol(v);
        else if (!strcmp(k, "prev_test_ids_checked")) m->prev_ids_checked = atol(v);
        else if (!strcmp(k, "prev_test_sha_checked")) m->prev_sha_checked = atol(v);
        else if (!strcmp(k, "prev_test_id_overlap")) m->prev_id_overlap = atol(v);
        else if (!strcmp(k, "prev_test_content_overlap")) m->prev_content_overlap = atol(v);
        else if (!strcmp(k, "trainval_id_overlap")) m->tv_id_overlap = atol(v);
        else if (!strcmp(k, "trainval_content_overlap")) m->tv_content_overlap = atol(v);
        else if (!strcmp(k, "sha256_train_pack")) snprintf(m->sha_train, sizeof m->sha_train, "%s", v);
        else if (!strcmp(k, "sha256_val_pack")) snprintf(m->sha_val, sizeof m->sha_val, "%s", v);
        else if (!strcmp(k, "sha256_test_pack")) snprintf(m->sha_test, sizeof m->sha_test, "%s", v);
        else if (!strcmp(k, "sha256_pca_bin")) snprintf(m->sha_pca, sizeof m->sha_pca, "%s", v);
        else if (!strcmp(k, "sha256_prev_test_pack")) snprintf(m->sha_prev, sizeof m->sha_prev, "%s", v);
    }
    fclose(f);
    m->present = 1;
    return 0;
}

/* Recompute the artefact hash and compare. A manifest that does not match the
   bytes on disk is worse than no manifest, so a mismatch is fatal. */
static int man_check_file(const char *cache, const char *name, const char *want) {
    char path[VD_PATH_MAX], got[65];
    if (!want || !*want) return -1;
    if ((size_t)snprintf(path, sizeof path, "%s/%s", cache, name) >= sizeof path) return -1;
    if (vd_sha256_file(path, got) != 0) return -1;
    if (strcmp(got, want) != 0) {
        printf("VD_BENCH_FAIL artefact_hash_mismatch:%s\n", name);
        return -1;
    }
    return 0;
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
    VdImage *out = (VdImage *)calloc(p->n, sizeof(VdImage));
    VdDet **keepbuf = (VdDet **)calloc(p->n, sizeof(VdDet *));
    size_t i, j;
    for (i = 0; i < p->n; i++) {
        const VImg *im = &p->imgs[i];
        VdDet *raw = im->n_prop ? (VdDet *)calloc(im->n_prop, sizeof(VdDet)) : NULL;
        int *keep = im->n_prop ? (int *)calloc(im->n_prop, sizeof(int)) : NULL;
        size_t nk = 0;
        for (j = 0; j < im->n_prop; j++) {
            raw[j].box = im->pb[j];
            raw[j].cls = 0;
            raw[j].score = head_score(btn, im->pf + j * DIM);
        }
        if (im->n_prop) nk = vd_nms(raw, im->n_prop, nms_iou, keep);
        if (nk == VD_NMS_FAIL) { g_eval_fault = 1; nk = 0; }
        keepbuf[i] = nk ? (VdDet *)calloc(nk, sizeof(VdDet)) : NULL;
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
    for (i = 0; i < n; i++) free(own[i]);
    free(own); free(v);
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
    VdImage *out = (VdImage *)calloc(p->n, sizeof(VdImage));
    VdDet **kb = (VdDet **)calloc(p->n, sizeof(VdDet *));
    size_t i, j;
    for (i = 0; i < p->n; i++) {
        const VImg *im = &p->imgs[i];
        VdDet *raw = im->n_prop ? (VdDet *)calloc(im->n_prop, sizeof(VdDet)) : NULL;
        int *keep = im->n_prop ? (int *)calloc(im->n_prop, sizeof(int)) : NULL;
        size_t nk = 0;
        for (j = 0; j < im->n_prop; j++) {
            raw[j].box = im->pb[j]; raw[j].cls = 0;
            raw[j].score = lin_score(L, im->pf + j * DIM);
        }
        if (im->n_prop) nk = vd_nms(raw, im->n_prop, nms_iou, keep);
        if (nk == VD_NMS_FAIL) { g_eval_fault = 1; nk = 0; }
        kb[i] = nk ? (VdDet *)calloc(nk, sizeof(VdDet)) : NULL;
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
    char variant[80] = "v1", frontend[200] = "";
    size_t prev_ids_checked = 0;
    double val_thr = 0.5;           /* set on validation, never on test */
    double g_val_ap = 0.0;          /* validation AP50, for the floor formula */
    Manifest man;
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

    /* ---- evidence gate: the cache must prove where it came from ---------
       Every predicate below is part of Level A. A run that cannot establish
       them does not get to print PASS. */
    if (man_read(cache, &man) != 0) {
        printf("VD_BENCH_FAIL manifest_missing:%s/manifest.txt\n", cache);
        return 3;
    }
    if (man_check_file(cache, "train.pack", man.sha_train) != 0 ||
        man_check_file(cache, "val.pack",   man.sha_val)   != 0 ||
        man_check_file(cache, "test.pack",  man.sha_test)  != 0 ||
        man_check_file(cache, "pca.bin",    man.sha_pca)   != 0) {
        printf("VD_BENCH_FAIL manifest_artefact_mismatch\n");
        return 3;
    }
    if (man.pca_dim != (long)DIM ||
        man.train_img != (long)tr.n || man.val_img != (long)va.n || man.test_img != (long)te.n ||
        man.train_prop != (long)tr.total_prop || man.val_prop != (long)va.total_prop ||
        man.test_prop != (long)te.total_prop || man.test_count != (long)te.n) {
        printf("VD_BENCH_FAIL manifest_counts_mismatch dim=%ld/%zu imgs=%ld/%zu,%ld/%zu,%ld/%zu\n",
               man.pca_dim, DIM, man.train_img, tr.n, man.val_img, va.n, man.test_img, te.n);
        return 3;
    }
    if (man.tv_id_overlap != 0 || man.tv_content_overlap != 0 ||
        man.prev_id_overlap != 0 || man.prev_content_overlap != 0) {
        printf("VD_BENCH_FAIL manifest_declares_leakage\n");
        return 3;
    }
    snprintf(variant, sizeof variant, "%s", man.variant);
    snprintf(frontend, sizeof frontend,
             "SelectiveSearchFast(class-agnostic) -> %ldx%ld %s HOG %ldD -> PCA%ld (train-only)",
             man.hog_side, man.hog_side, man.color ? "colour" : "gray", man.hog_dim, man.pca_dim);
    G_VARIANT = variant; G_FRONTEND = frontend;
    printf("variant=%s dim=%zu frontend=%s\n", G_VARIANT, DIM, G_FRONTEND);
    printf("manifest: verified sha256 of train/val/test/pca artefacts\n");
    evidence_manifest = 1;

    /* A previous variant's holdout is spent. Re-assert here, at the scoring
       boundary, so pointing --cache at a stale directory cannot produce a
       number against images that were already scored. */
    if (man.test_offset > 0 && !prev_test) {
        printf("VD_BENCH_FAIL prev_test_required variant=%s test_offset=%ld\n",
               G_VARIANT, man.test_offset);
        return 3;
    }
    if (man.test_offset > 0 &&
        (man.prev_ids_checked != man.test_count || man.prev_sha_checked != man.test_count)) {
        printf("VD_BENCH_FAIL prev_evidence_incomplete ids=%ld sha=%ld expected=%ld\n",
               man.prev_ids_checked, man.prev_sha_checked, man.test_count);
        return 3;
    }
    if (prev_test) {
        Pack pv;
        size_t a, b, leaks = 0;
        char pvsha[65];
        if (load_pack(prev_test, &pv)) { printf("VD_BENCH_FAIL prev_test_unreadable\n"); return 3; }
        /* the spent pack must be the exact one prep checked against */
        if (vd_sha256_file(prev_test, pvsha) != 0 || strcmp(pvsha, man.sha_prev) != 0) {
            printf("VD_BENCH_FAIL prev_test_hash_mismatch\n");
            free_pack(&pv); return 3;
        }
        for (a = 0; a < te.n; a++)
            for (b = 0; b < pv.n; b++)
                if (!strcmp(te.imgs[a].id, pv.imgs[b].id)) leaks++;
        if (leaks) { printf("VD_BENCH_FAIL spent_holdout_reuse=%zu\n", leaks); free_pack(&pv); return 3; }
        prev_ids_checked = pv.n;
        printf("disjoint: holdout vs %zu already-scored ids -> 0 overlap (pack hash bound)\n",
               prev_ids_checked);
        free_pack(&pv);
        evidence_prev = 1;
    } else if (man.test_offset == 0) {
        evidence_prev = 1;   /* first slice: nothing was spent before it */
    }

    /* leakage re-assertion at bench level: IDs must be disjoint */
    {
        size_t a, b, leaks = 0;
        for (a = 0; a < te.n; a++) {
            for (b = 0; b < tr.n; b++) if (!strcmp(te.imgs[a].id, tr.imgs[b].id)) leaks++;
            for (b = 0; b < va.n; b++) if (!strcmp(te.imgs[a].id, va.imgs[b].id)) leaks++;
        }
        if (leaks) { printf("VD_BENCH_FAIL id_leak=%zu\n", leaks); return 3; }
        printf("leakage: test-vs-train/val id overlap = 0\n");
        evidence_leak = 1;
    }

    n = collect(&tr, &X, &Y, 20);  /* validation-time cost decision */
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
            printf("VAL ap50_labelshuffle=%.6f\n", vd_ap50(v, va.n, 0.5));
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
        ap = vd_ap50(v, va.n, 0.5);
        for (s = 1; s < 20; s++) {
            double thr = s * 0.05, pr, rc, f1;
            vd_pr_at(v, va.n, 0.5, thr, &pr, &rc);
            f1 = (pr + rc) > 0 ? 2 * pr * rc / (pr + rc) : 0.0;
            if (f1 > bestf) { bestf = f1; bestthr = thr; }
        }
        free_det(v, own, va.n);
        v = run_detector(&va, &rndhead, 0.30, &own); apr = vd_ap50(v, va.n, 0.5); free_det(v, own, va.n);
        v = run_linear(&va, &lin, 0.30, &own); apl = vd_ap50(v, va.n, 0.5); free_det(v, own, va.n);
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
        ap = vd_ap50(v, te.n, 0.5);
        vd_pr_at(v, te.n, 0.5, thr, &pr, &rc);
        for (i2 = 0; i2 < te.n; i2++) ndet += v[i2].n_det;
        free_det(v, own, te.n);

        /* determinism: identical rerun of the same scored path */
        v = run_detector(&te, &head, 0.30, &own); ap2 = vd_ap50(v, te.n, 0.5); free_det(v, own, te.n);

        v = run_detector(&te, &rndhead, 0.30, &own); apr = vd_ap50(v, te.n, 0.5); free_det(v, own, te.n);
        v = run_linear(&te, &lin, 0.30, &own); apl = vd_ap50(v, te.n, 0.5); free_det(v, own, te.n);
        /* identical full evaluation of the frozen label-shuffle head */
        v = run_detector(&te, &sh, 0.30, &own); aps = vd_ap50(v, te.n, 0.5); free_det(v, own, te.n);
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
            JS("  \"dataset\": \"PASCAL VOC 2007\", \"class\": \"car\",\n");
            JS("  \"md5_trainval\": \"c52e279531787c972589f7e41ab4ae64\",\n");
            JS("  \"md5_test\": \"b6e924de25625d8de591ea690078ad9f\",\n");
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
            JS("  \"leakage\": {\"id_overlap\": 0, \"content_hash_overlap_trainval_test\": 0, \"trainval_internal_dup_pairs\": 3},\n");
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
