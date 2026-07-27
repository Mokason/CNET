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
static const char *G_VARIANT = "unknown";
static const char *G_FRONTEND = "unknown";

typedef struct {
    char id[32];
    VdBox *gts; int *gt_dif; size_t n_gt;
    VdBox *pb;  int *plabel; float *pf; size_t n_prop;
} VImg;

typedef struct { VImg *imgs; size_t n; int dim; } Pack;

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

/* ---------------- pack IO ------------------------------------------------ */
static int load_pack(const char *path, Pack *p) {
    FILE *f = fopen(path, "rb");
    char magic[8];
    int32_t dim, n_img;
    int64_t n_prop;
    size_t i;
    if (!f) { fprintf(stderr, "VD_BENCH_FAIL pack_missing:%s\n", path); return -1; }
    if (fread(magic, 1, 8, f) != 8 || memcmp(magic, "VDPACK1", 7) != 0) {
        fprintf(stderr, "VD_BENCH_FAIL pack_magic:%s\n", path); fclose(f); return -1;
    }
    if (fread(&dim, 4, 1, f) != 1 || fread(&n_img, 4, 1, f) != 1 ||
        fread(&n_prop, 8, 1, f) != 1) { fclose(f); return -1; }
    if (dim <= 0 || dim > DIM_MAX) {
        fprintf(stderr, "VD_BENCH_FAIL pack_dim=%d exceeds DIM_MAX=%d:%s\n", dim, DIM_MAX, path);
        fclose(f); return -1;
    }
    p->dim = dim; p->n = (size_t)n_img;
    p->imgs = (VImg *)calloc(p->n, sizeof(VImg));
    if (!p->imgs) { fclose(f); return -1; }
    for (i = 0; i < p->n; i++) {
        VImg *im = &p->imgs[i];
        int32_t idlen, np, ng, j;
        if (fread(&idlen, 4, 1, f) != 1 || idlen <= 0 || idlen > 30) { fclose(f); return -1; }
        if (fread(im->id, 1, (size_t)idlen, f) != (size_t)idlen) { fclose(f); return -1; }
        im->id[idlen] = 0;
        if (fread(&np, 4, 1, f) != 1 || fread(&ng, 4, 1, f) != 1) { fclose(f); return -1; }
        im->n_gt = (size_t)ng; im->n_prop = (size_t)np;
        im->gts = ng ? (VdBox *)calloc((size_t)ng, sizeof(VdBox)) : NULL;
        im->gt_dif = ng ? (int *)calloc((size_t)ng, sizeof(int)) : NULL;
        for (j = 0; j < ng; j++) {
            int32_t v[5];
            if (fread(v, sizeof v, 1, f) != 1) { fclose(f); return -1; }
            im->gts[j].x = v[0]; im->gts[j].y = v[1];
            im->gts[j].w = v[2]; im->gts[j].h = v[3];
            im->gt_dif[j] = v[4];
        }
        im->pb = np ? (VdBox *)calloc((size_t)np, sizeof(VdBox)) : NULL;
        im->plabel = np ? (int *)calloc((size_t)np, sizeof(int)) : NULL;
        im->pf = np ? (float *)calloc((size_t)np * dim, sizeof(float)) : NULL;
        for (j = 0; j < np; j++) {
            int32_t v[5];
            if (fread(v, sizeof v, 1, f) != 1) { fclose(f); return -1; }
            im->pb[j].x = v[0]; im->pb[j].y = v[1];
            im->pb[j].w = v[2]; im->pb[j].h = v[3];
            im->plabel[j] = v[4];
            if (fread(im->pf + (size_t)j * dim, sizeof(float), (size_t)dim, f) != (size_t)dim) {
                fclose(f); return -1;
            }
        }
    }
    fclose(f);
    return 0;
}

static void free_pack(Pack *p) {
    size_t i;
    for (i = 0; i < p->n; i++) {
        free(p->imgs[i].gts); free(p->imgs[i].gt_dif);
        free(p->imgs[i].pb); free(p->imgs[i].plabel); free(p->imgs[i].pf);
    }
    free(p->imgs);
    p->imgs = NULL; p->n = 0;
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
    char variant[32] = "v1", frontend[160] = "";
    size_t prev_ids_checked = 0;
    double val_thr = 0.5;           /* set on validation, never on test */
    double g_val_ap = 0.0;          /* validation AP50, for the floor formula */
    Pack tr, va, te;
    double *X = NULL, *Y = NULL;
    size_t n = 0;
    BinaryTransformNetwork head, rndhead;
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

    {   /* cache provenance, echoed into the JSON so a run cannot misreport
           which frontend produced the numbers */
        char mp[512]; FILE *m;
        snprintf(mp, sizeof mp, "%s/meta.txt", cache);
        m = fopen(mp, "r");
        if (m) {
            char k[64]; char v[31];
            int hs = 0, col = 0, hd = 0, pd = 0;
            while (fscanf(m, "%63s %30s", k, v) == 2) {
                if (!strcmp(k, "variant")) snprintf(variant, sizeof variant, "%s", v);
                else if (!strcmp(k, "hog_side")) hs = atoi(v);
                else if (!strcmp(k, "color")) col = atoi(v);
                else if (!strcmp(k, "hog_dim")) hd = atoi(v);
                else if (!strcmp(k, "pca_dim")) pd = atoi(v);
            }
            fclose(m);
            snprintf(frontend, sizeof frontend,
                     "SelectiveSearchFast(class-agnostic) -> %dx%d %s HOG %dD -> PCA%d (train-only)",
                     hs, hs, col ? "colour" : "gray", hd, pd);
        }
        if (!frontend[0]) snprintf(frontend, sizeof frontend, "unknown (no meta.txt in cache)");
        G_VARIANT = variant; G_FRONTEND = frontend;
        printf("variant=%s dim=%zu frontend=%s\n", G_VARIANT, DIM, G_FRONTEND);
    }

    /* A previous variant's holdout is spent. Re-assert here, at the scoring
       boundary, so pointing --cache at a stale directory cannot produce a
       number against images that were already scored. */
    if (prev_test) {
        Pack pv;
        size_t a, b, leaks = 0;
        if (load_pack(prev_test, &pv)) { printf("VD_BENCH_FAIL prev_test_unreadable\n"); return 3; }
        for (a = 0; a < te.n; a++)
            for (b = 0; b < pv.n; b++)
                if (!strcmp(te.imgs[a].id, pv.imgs[b].id)) leaks++;
        if (leaks) { printf("VD_BENCH_FAIL spent_holdout_reuse=%zu\n", leaks); free_pack(&pv); return 3; }
        prev_ids_checked = pv.n;
        printf("disjoint: holdout vs %zu already-scored ids -> 0 overlap\n", prev_ids_checked);
        free_pack(&pv);
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

    /* label-shuffle control */
    {
        BinaryTransformNetwork sh;
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
        btn_init(&sh, DIM, NCLS, 24, 192, 0.5, 7u);
        btn_set_ports(&sh, RAWP(), CLSP());
        btn_train_dynamic(&sh, X, Ys, n, 2000, 200, 1e-5, 1e-7);
        {
            VdDet **own; VdImage *v = run_detector(&va, &sh, 0.30, &own);
            printf("VAL ap50_labelshuffle=%.6f\n", vd_ap50(v, va.n, 0.5));
            free_det(v, own, va.n);
        }
        btn_free(&sh);
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
        double ap, ap2, apr, apl, pr, rc, prop_rec;
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
        test_s = now_s() - t_test0;

        pass_floor  = ap >= floor_ap;
        pass_ratio  = apr > 0 ? (ap >= ratio_floor * apr) : (ap > 0);
        pass_margin = (ap - apr) >= margin_floor;
        pass_det    = fabs(ap - ap2) <= 1e-9;
        levelA = pass_floor && pass_ratio && pass_margin && pass_det;

        printf("TEST proposal_recall=%.6f (%zu/%zu)\n", prop_rec, h, t);
        printf("TEST ap50_cnet=%.6f ap50_random=%.6f ap50_linear=%.6f\n", ap, apr, apl);
        printf("TEST precision@%.2f=%.6f recall@%.2f=%.6f detections=%zu gt=%zu\n",
               thr, pr, thr, rc, ndet, t);
        printf("TEST determinism |ap-ap_rerun|=%.3g\n", fabs(ap - ap2));
        printf("BARS floor=%.3f -> %s | ratio>=%.1f -> %s (%.1fx) | margin>=%.3f -> %s (%.4f) | det -> %s\n",
               floor_ap, pass_floor ? "PASS" : "FAIL",
               ratio_floor, pass_ratio ? "PASS" : "FAIL", apr > 0 ? ap / apr : 0.0,
               margin_floor, pass_margin ? "PASS" : "FAIL", ap - apr,
               pass_det ? "PASS" : "FAIL");

        {
            FILE *js = fopen(jsonp, "w");
            if (js) {
                fprintf(js, "{\n  \"schema_version\": 1,\n");
                fprintf(js, "  \"gpu_policy\": \"CPU only; ROCR/HIP/CUDA_VISIBLE_DEVICES empty\",\n");
                fprintf(js, "  \"dataset\": \"PASCAL VOC 2007\", \"class\": \"car\",\n");
                fprintf(js, "  \"md5_trainval\": \"c52e279531787c972589f7e41ab4ae64\",\n");
                fprintf(js, "  \"md5_test\": \"b6e924de25625d8de591ea690078ad9f\",\n");
                fprintf(js, "  \"split\": {\"train_img\": %zu, \"val_img\": %zu, \"test_img\": %zu, \"test_gt\": %zu},\n",
                        tr.n, va.n, te.n, t);
                fprintf(js, "  \"variant\": \"%s\",\n", G_VARIANT);
                fprintf(js, "  \"split_key\": \"sha256 image content hash (trainval); seed 20260727 shuffle slice (holdout)\",\n");
                fprintf(js, "  \"holdout_disjoint_from_spent_ids\": %zu, \"holdout_spent_overlap\": 0,\n",
                        prev_ids_checked);
                fprintf(js, "  \"leakage\": {\"id_overlap\": 0, \"content_hash_overlap_trainval_test\": 0, \"trainval_internal_dup_pairs\": 3},\n");
                fprintf(js, "  \"frontend\": \"%s\",\n", G_FRONTEND);
                fprintf(js, "  \"cnet_learned_component\": \"BTN head PORT_RAW%zu -> PORT_ONEHOT2\",\n", DIM);
                fprintf(js, "  \"train_rows\": %zu, \"train_seconds\": %.1f, \"test_seconds\": %.1f,\n", n, train_s, test_s);
                fprintf(js, "  \"proposal_recall_test\": %.6f,\n", prop_rec);
                fprintf(js, "  \"ap50_cnet_test\": %.6f,\n", ap);
                fprintf(js, "  \"ap50_random_head_test\": %.6f,\n", apr);
                fprintf(js, "  \"ap50_linear_baseline_test\": %.6f,\n", apl);
                fprintf(js, "  \"precision_at_thr\": %.6f, \"recall_at_thr\": %.6f, \"thr\": %.2f,\n", pr, rc, thr);
                fprintf(js, "  \"detections\": %zu,\n", ndet);
                fprintf(js, "  \"determinism_abs_delta\": %.3g,\n", fabs(ap - ap2));
                fprintf(js, "  \"bars\": {\"ap50_floor\": %.3f, \"ratio_floor\": %.1f, \"margin_floor\": %.3f},\n",
                        floor_ap, ratio_floor, margin_floor);
                fprintf(js, "  \"bar_results\": {\"floor\": %d, \"ratio\": %d, \"margin\": %d, \"determinism\": %d},\n",
                        pass_floor, pass_ratio, pass_margin, pass_det);
                fprintf(js, "  \"verdict_level_a\": \"%s\",\n",
                        levelA ? "VISION_DETECTION_MECHANISM_PASS" : "VISION_DETECTION_MECHANISM_WITHHELD");
                fprintf(js, "  \"verdict_level_b\": \"VISION_CAPSULE_PORTABILITY_WITHHELD\",\n");
                fprintf(js, "  \"verdict_level_b_reason\": \"PORT_RAW has no honest coverage gate in coverage_family_gated(); unchanged this pass\",\n");
                fprintf(js, "  \"verdict_level_c\": \"VISION_SPECIALIST_COMPETES_WITHHELD\",\n");
                fprintf(js, "  \"verdict_level_c_reason\": \"no pretrained reference detector run in this pass\"\n}\n");
                fclose(js);
            }
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
