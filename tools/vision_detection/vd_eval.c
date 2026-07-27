#include "vd_eval.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Geometry is evaluated in int64: every int32 corner sum is then exactly
   representable, so a box near INT_MAX cannot wrap into a bogus overlap. The
   only thing an int32 box cannot express usefully is a non-positive extent. */
int vd_box_valid(VdBox b) { return (b.w > 0 && b.h > 0) ? 0 : -1; }

double vd_iou(VdBox a, VdBox b) {
    int64_t ax = a.x, ay = a.y, bx = b.x, by = b.y;
    int64_t ax2 = ax + (int64_t)a.w, ay2 = ay + (int64_t)a.h;
    int64_t bx2 = bx + (int64_t)b.w, by2 = by + (int64_t)b.h;
    int64_t x1 = ax > bx ? ax : bx, y1 = ay > by ? ay : by;
    int64_t x2 = ax2 < bx2 ? ax2 : bx2, y2 = ay2 < by2 ? ay2 : by2;
    int64_t iw = x2 - x1, ih = y2 - y1;
    double inter, uni;
    if (vd_box_valid(a) != 0 || vd_box_valid(b) != 0) return NAN;
    if (iw <= 0 || ih <= 0) return 0.0;
    inter = (double)iw * (double)ih;
    uni = (double)a.w * (double)a.h + (double)b.w * (double)b.h - inter;
    if (!(uni > 0.0)) return 0.0;
    return inter / uni;
}

/* Every box that will be touched must be sane before any scoring starts;
   scoring half a malformed set and reporting the number is the failure mode
   this guards. */
static int dets_valid(const VdDet *d, size_t n) {
    size_t i;
    if (!d && n) return -1;
    for (i = 0; i < n; i++)
        if (vd_box_valid(d[i].box) != 0 || !isfinite(d[i].score)) return -1;
    return 0;
}
static int imgs_valid(const VdImage *imgs, size_t n_img) {
    size_t i, j;
    if (!imgs && n_img) return -1;
    for (i = 0; i < n_img; i++) {
        if (dets_valid(imgs[i].dets, imgs[i].n_det) != 0) return -1;
        if (!imgs[i].gts && imgs[i].n_gt) return -1;
        for (j = 0; j < imgs[i].n_gt; j++)
            if (vd_box_valid(imgs[i].gts[j]) != 0) return -1;
    }
    return 0;
}

/* index + score, sorted by descending score with a stable index tiebreak so
   equal scores never make the result depend on qsort's internals. */
typedef struct { size_t i; double s; } Rank;

static int rank_cmp(const void *pa, const void *pb) {
    const Rank *a = (const Rank *)pa, *b = (const Rank *)pb;
    if (a->s > b->s) return -1;
    if (a->s < b->s) return 1;
    return a->i < b->i ? -1 : (a->i > b->i ? 1 : 0);
}

static Rank *rank_dets(const VdDet *d, size_t n) {
    Rank *r = (Rank *)malloc(n * sizeof *r);
    size_t i;
    if (!r) return NULL;
    for (i = 0; i < n; i++) { r[i].i = i; r[i].s = d[i].score; }
    qsort(r, n, sizeof *r, rank_cmp);
    return r;
}

size_t vd_nms(const VdDet *dets, size_t n, double iou_thr, int *keep_out) {
    Rank *r;
    char *dead;
    size_t i, j, k = 0;
    if (n == 0) return 0;
    if (!dets || !keep_out) return VD_NMS_FAIL;
    if (dets_valid(dets, n) != 0) return VD_NMS_FAIL;
    r = rank_dets(dets, n);
    if (!r) return VD_NMS_FAIL;
    dead = (char *)calloc(n, 1);
    if (!dead) { free(r); return VD_NMS_FAIL; }
    for (i = 0; i < n; i++) {
        size_t ii = r[i].i;
        if (dead[ii]) continue;
        keep_out[k++] = (int)ii;
        for (j = i + 1; j < n; j++) {
            size_t jj = r[j].i;
            if (dead[jj]) continue;
            if (dets[ii].cls == dets[jj].cls &&
                vd_iou(dets[ii].box, dets[jj].box) > iou_thr)
                dead[jj] = 1;
        }
    }
    free(dead);
    free(r);
    return k;
}

/* Flatten every detection across images, ranked globally by confidence — AP is
   a ranking metric over the whole set, not a per-image average. */
typedef struct { size_t img, det; double score; } GRank;

static int grank_cmp(const void *pa, const void *pb) {
    const GRank *a = (const GRank *)pa, *b = (const GRank *)pb;
    if (a->score > b->score) return -1;
    if (a->score < b->score) return 1;
    if (a->img != b->img) return a->img < b->img ? -1 : 1;
    return a->det < b->det ? -1 : (a->det > b->det ? 1 : 0);
}

static size_t count_real_gt(const VdImage *imgs, size_t n_img) {
    size_t i, j, t = 0;
    for (i = 0; i < n_img; i++)
        for (j = 0; j < imgs[i].n_gt; j++)
            if (!imgs[i].gt_difficult || !imgs[i].gt_difficult[j]) t++;
    return t;
}

double vd_ap50(const VdImage *imgs, size_t n_img, double iou_thr) {
    size_t npos = count_real_gt(imgs, n_img);
    size_t total_det = 0, i, j, k = 0;
    GRank *g;
    char **matched;
    double *prec, *rec, ap = 0.0, prev_rec = 0.0;
    size_t tp = 0, fp = 0, npts = 0;

    if (imgs_valid(imgs, n_img) != 0) return NAN;
    if (npos == 0) return 0.0;
    for (i = 0; i < n_img; i++) total_det += imgs[i].n_det;
    if (total_det == 0) return 0.0;

    g = (GRank *)malloc(total_det * sizeof *g);
    matched = (char **)calloc(n_img, sizeof *matched);
    prec = (double *)malloc(total_det * sizeof *prec);
    rec = (double *)malloc(total_det * sizeof *rec);
    if (!g || !matched || !prec || !rec) {
        free(g); free(prec); free(rec);
        if (matched) free(matched);
        return NAN;   /* allocation failure must never look like a score of 0 */
    }
    for (i = 0; i < n_img; i++) {
        matched[i] = imgs[i].n_gt ? (char *)calloc(imgs[i].n_gt, 1) : NULL;
        for (j = 0; j < imgs[i].n_det; j++) {
            g[k].img = i; g[k].det = j; g[k].score = imgs[i].dets[j].score; k++;
        }
    }
    qsort(g, total_det, sizeof *g, grank_cmp);

    for (k = 0; k < total_det; k++) {
        const VdImage *im = &imgs[g[k].img];
        const VdDet *d = &im->dets[g[k].det];
        double best = 0.0;
        long best_j = -1;
        for (j = 0; j < im->n_gt; j++) {
            double v = vd_iou(d->box, im->gts[j]);
            if (v > best) { best = v; best_j = (long)j; }
        }
        if (best >= iou_thr && best_j >= 0) {
            int dif = im->gt_difficult ? im->gt_difficult[best_j] : 0;
            if (dif) continue;                    /* ignored: neither TP nor FP */
            if (matched[g[k].img] && !matched[g[k].img][best_j]) {
                matched[g[k].img][best_j] = 1;
                tp++;
            } else {
                fp++;                              /* duplicate on a matched GT */
            }
        } else {
            fp++;
        }
        prec[npts] = (double)tp / (double)(tp + fp);
        rec[npts] = (double)tp / (double)npos;
        npts++;
    }

    /* all-points interpolation: precision envelope, then integrate over recall */
    if (npts > 0) {
        for (i = npts - 1; i > 0; i--)
            if (prec[i] > prec[i - 1]) prec[i - 1] = prec[i];
        prev_rec = 0.0;
        for (i = 0; i < npts; i++) {
            if (rec[i] > prev_rec) {
                ap += (rec[i] - prev_rec) * prec[i];
                prev_rec = rec[i];
            }
        }
    }

    for (i = 0; i < n_img; i++) free(matched[i]);
    free(matched);
    free(g); free(prec); free(rec);
    return ap;
}

void vd_pr_at(const VdImage *imgs, size_t n_img, double iou_thr,
              double score_thr, double *precision, double *recall) {
    size_t npos = count_real_gt(imgs, n_img);
    size_t i, j, tp = 0, fp = 0;
    char **matched;
    if (imgs_valid(imgs, n_img) != 0) {
        if (precision) *precision = NAN;
        if (recall) *recall = NAN;
        return;
    }
    matched = (char **)calloc(n_img, sizeof *matched);
    if (!matched) { if (precision) *precision = NAN; if (recall) *recall = NAN; return; }
    for (i = 0; i < n_img; i++)
        matched[i] = imgs[i].n_gt ? (char *)calloc(imgs[i].n_gt, 1) : NULL;
    for (i = 0; i < n_img; i++) {
        const VdImage *im = &imgs[i];
        Rank *r = im->n_det ? rank_dets(im->dets, im->n_det) : NULL;
        if (im->n_det && !r) { if (precision) *precision = NAN; if (recall) *recall = NAN; }
        for (j = 0; j < im->n_det; j++) {
            size_t di = r ? r[j].i : j;
            const VdDet *d = &im->dets[di];
            double best = 0.0;
            long best_j = -1;
            size_t q;
            if (d->score < score_thr) continue;
            for (q = 0; q < im->n_gt; q++) {
                double v = vd_iou(d->box, im->gts[q]);
                if (v > best) { best = v; best_j = (long)q; }
            }
            if (best >= iou_thr && best_j >= 0) {
                int dif = im->gt_difficult ? im->gt_difficult[best_j] : 0;
                if (dif) continue;
                if (matched[i] && !matched[i][best_j]) { matched[i][best_j] = 1; tp++; }
                else fp++;
            } else {
                fp++;
            }
        }
        free(r);
    }
    if (precision) *precision = (tp + fp) ? (double)tp / (double)(tp + fp) : 0.0;
    if (recall) *recall = npos ? (double)tp / (double)npos : 0.0;
    for (i = 0; i < n_img; i++) free(matched[i]);
    free(matched);
}

void vd_proposal_recall(const VdImage *imgs, size_t n_img, const VdBox *props,
                        size_t n_props, double iou_thr, size_t *hit,
                        size_t *total) {
    size_t i, j, k, h = 0, t = 0;
    for (i = 0; i < n_img; i++) {
        for (j = 0; j < imgs[i].n_gt; j++) {
            int covered = 0;
            if (imgs[i].gt_difficult && imgs[i].gt_difficult[j]) continue;
            if (vd_box_valid(imgs[i].gts[j]) != 0) continue;
            t++;
            for (k = 0; k < n_props; k++) {
                double v;
                if (vd_box_valid(props[k]) != 0) continue;   /* diagnostic only */
                v = vd_iou(props[k], imgs[i].gts[j]);
                if (isfinite(v) && v >= iou_thr) { covered = 1; break; }
            }
            if (covered) h++;
        }
    }
    if (hit) *hit = h;
    if (total) *total = t;
}
