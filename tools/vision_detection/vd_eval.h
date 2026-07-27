/* Detection evaluator — VOC-style AP50, NMS, IoU, proposal recall.
 *
 * Deliberately independent of any training code: it takes boxes and scores and
 * returns a number, so a bug in the learner cannot quietly become a bug in the
 * metric. Fixtures with analytically known answers live in
 * tests/vision_detection_eval_test.c.
 *
 * Matching is greedy one-to-one by descending confidence at IoU >= thr. Each GT
 * matches at most once; later detections on a matched GT are false positives.
 * VOC `difficult` GTs are ignored: a detection matching one is neither TP nor
 * FP, and difficult GTs are excluded from the recall denominator.
 * AP integration is all-points (VOC2010+), not the 11-point interpolation.
 */
#ifndef VD_EVAL_H
#define VD_EVAL_H

#include <stddef.h>

typedef struct { int x, y, w, h; } VdBox;

typedef struct {
    VdBox box;
    double score;
    int cls;
} VdDet;

typedef struct {
    const VdBox *gts;
    const int *gt_difficult;
    size_t n_gt;
    const VdDet *dets;
    size_t n_det;
} VdImage;

double vd_iou(VdBox a, VdBox b);

/* Greedy NMS. Writes kept indices (into dets) in score order; returns count. */
size_t vd_nms(const VdDet *dets, size_t n, double iou_thr, int *keep_out);

/* AP50 over a set of images. */
double vd_ap50(const VdImage *imgs, size_t n_img, double iou_thr);

/* Precision/recall at a score threshold (same matching rules). */
void vd_pr_at(const VdImage *imgs, size_t n_img, double iou_thr,
              double score_thr, double *precision, double *recall);

/* How many non-difficult GTs are covered by at least one proposal. */
void vd_proposal_recall(const VdImage *imgs, size_t n_img, const VdBox *props,
                        size_t n_props, double iou_thr, size_t *hit,
                        size_t *total);

#endif
