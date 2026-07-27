/* Evaluator fixtures with analytically known IoU / NMS / AP outcomes.
 *
 * The evaluator is the only thing standing between a detector and a number, so
 * it is tested independently of any training code and against cases whose
 * answers can be derived by hand.
 *
 * make vision_detection_eval_test -> VISION_DETECTION_EVAL_PASS
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../tools/vision_detection/vd_eval.h"

static int failures, checks;
static void check(int ok, const char *name) {
    checks++;
    printf("  %-58s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}
static int near(double a, double b) { return fabs(a - b) < 1e-9; }

int main(void) {
    printf("== detection evaluator fixtures (analytic) ==\n");

    /* ---- IoU ------------------------------------------------------------ */
    {
        VdBox a = {0, 0, 10, 10}, b = {0, 0, 10, 10};
        check(near(vd_iou(a, b), 1.0), "iou: identical boxes = 1");
        VdBox c = {20, 20, 10, 10};
        check(near(vd_iou(a, c), 0.0), "iou: disjoint = 0");
        /* half overlap: inter 5x10=50, union 100+100-50=150 -> 1/3 */
        VdBox d = {5, 0, 10, 10};
        check(near(vd_iou(a, d), 50.0 / 150.0), "iou: half overlap = 1/3");
        /* contained: inter 25, union 100 -> 0.25 */
        VdBox e = {0, 0, 5, 5};
        check(near(vd_iou(a, e), 25.0 / 100.0), "iou: contained = 0.25");
        /* touching edges only -> 0 */
        VdBox f = {10, 0, 10, 10};
        check(near(vd_iou(a, f), 0.0), "iou: edge-touching = 0");
    }

    /* ---- NMS ------------------------------------------------------------ */
    {
        VdDet d[3];
        int keep[3], n;
        d[0] = (VdDet){{0, 0, 10, 10}, 0.9, 0};
        d[1] = (VdDet){{1, 1, 10, 10}, 0.8, 0};   /* heavy overlap with d0 */
        d[2] = (VdDet){{50, 50, 10, 10}, 0.7, 0}; /* disjoint */
        n = vd_nms(d, 3, 0.3, keep);
        check(n == 2, "nms: suppresses the overlapping lower-score box");
        check(keep[0] == 0 && keep[1] == 2, "nms: keeps highest score + disjoint");
        n = vd_nms(d, 3, 0.99, keep);
        check(n == 3, "nms: threshold 0.99 keeps all three");
    }

    /* ---- AP: perfect single detection ----------------------------------- */
    {
        VdImage im;
        VdBox gt = {0, 0, 10, 10};
        VdDet det = {{0, 0, 10, 10}, 0.9, 0};
        int dif = 0;
        im.gts = &gt; im.gt_difficult = &dif; im.n_gt = 1;
        im.dets = &det; im.n_det = 1;
        double ap = vd_ap50(&im, 1, 0.5);
        check(near(ap, 1.0), "ap: one perfect detection = 1.0");
    }

    /* ---- AP: no detections at all --------------------------------------- */
    {
        VdImage im;
        VdBox gt = {0, 0, 10, 10};
        int dif = 0;
        im.gts = &gt; im.gt_difficult = &dif; im.n_gt = 1;
        im.dets = NULL; im.n_det = 0;
        check(near(vd_ap50(&im, 1, 0.5), 0.0), "ap: no detections = 0.0");
    }

    /* ---- AP: detections but no ground truth ----------------------------- */
    {
        VdImage im;
        VdDet det = {{0, 0, 10, 10}, 0.9, 0};
        im.gts = NULL; im.gt_difficult = NULL; im.n_gt = 0;
        im.dets = &det; im.n_det = 1;
        check(near(vd_ap50(&im, 1, 0.5), 0.0), "ap: all-FP with no GT = 0.0");
    }

    /* ---- AP: duplicate detections on one GT -----------------------------
       Two detections on one GT: first TP, second is a duplicate FP.
       Precision at rank1 = 1.0 (recall 1.0). All-points AP = 1.0 because full
       recall is reached at precision 1; the duplicate cannot raise recall. */
    {
        VdImage im;
        VdBox gt = {0, 0, 10, 10};
        VdDet det[2];
        int dif = 0;
        det[0] = (VdDet){{0, 0, 10, 10}, 0.9, 0};
        det[1] = (VdDet){{0, 0, 10, 10}, 0.8, 0};
        im.gts = &gt; im.gt_difficult = &dif; im.n_gt = 1;
        im.dets = det; im.n_det = 2;
        check(near(vd_ap50(&im, 1, 0.5), 1.0),
              "ap: duplicate on same GT is FP, AP still 1.0");
    }

    /* ---- AP: one-to-one matching across two GTs -------------------------
       2 GT, 2 correct detections -> AP 1.0. */
    {
        VdImage im;
        VdBox gt[2] = {{0, 0, 10, 10}, {50, 50, 10, 10}};
        int dif[2] = {0, 0};
        VdDet det[2];
        det[0] = (VdDet){{0, 0, 10, 10}, 0.9, 0};
        det[1] = (VdDet){{50, 50, 10, 10}, 0.8, 0};
        im.gts = gt; im.gt_difficult = dif; im.n_gt = 2;
        im.dets = det; im.n_det = 2;
        check(near(vd_ap50(&im, 1, 0.5), 1.0), "ap: two GT, two TP = 1.0");
    }

    /* ---- AP: half recall -------------------------------------------------
       2 GT, only 1 detected correctly. Precision 1 at recall 0.5, nothing
       beyond -> all-points AP = 0.5. */
    {
        VdImage im;
        VdBox gt[2] = {{0, 0, 10, 10}, {50, 50, 10, 10}};
        int dif[2] = {0, 0};
        VdDet det = {{0, 0, 10, 10}, 0.9, 0};
        im.gts = gt; im.gt_difficult = dif; im.n_gt = 2;
        im.dets = &det; im.n_det = 1;
        check(near(vd_ap50(&im, 1, 0.5), 0.5), "ap: 1 of 2 GT found = 0.5");
    }

    /* ---- AP: difficult GT is ignored ------------------------------------
       1 normal GT + 1 difficult GT; one detection hits the difficult one.
       The difficult match is neither TP nor FP, and the difficult GT is not in
       the recall denominator -> AP 0.0 (the real GT was never found), and
       critically NOT penalised as a false positive. */
    {
        VdImage im;
        VdBox gt[2] = {{0, 0, 10, 10}, {50, 50, 10, 10}};
        int dif[2] = {0, 1};
        VdDet det = {{50, 50, 10, 10}, 0.9, 0};
        im.gts = gt; im.gt_difficult = dif; im.n_gt = 2;
        im.dets = &det; im.n_det = 1;
        check(near(vd_ap50(&im, 1, 0.5), 0.0),
              "ap: detection on difficult GT is ignored, not FP");
    }

    /* ---- AP: confidence ranking matters ---------------------------------
       2 GT. A wrong high-confidence box first, then a correct one.
       ranks: FP (p=0, r=0), TP (p=0.5, r=0.5) -> all-points AP = 0.5*0.5 = 0.25 */
    {
        VdImage im;
        VdBox gt[2] = {{0, 0, 10, 10}, {50, 50, 10, 10}};
        int dif[2] = {0, 0};
        VdDet det[2];
        det[0] = (VdDet){{200, 200, 10, 10}, 0.95, 0};  /* FP, ranked first */
        det[1] = (VdDet){{0, 0, 10, 10}, 0.90, 0};      /* TP */
        im.gts = gt; im.gt_difficult = dif; im.n_gt = 2;
        im.dets = det; im.n_det = 2;
        check(near(vd_ap50(&im, 1, 0.5), 0.25),
              "ap: high-confidence FP first costs precision (0.25)");
    }

    /* ---- proposal recall ------------------------------------------------- */
    {
        VdImage im;
        VdBox gt[2] = {{0, 0, 10, 10}, {50, 50, 10, 10}};
        int dif[2] = {0, 0};
        VdBox props[2] = {{0, 0, 10, 10}, {300, 300, 10, 10}};
        size_t hit = 0, tot = 0;
        im.gts = gt; im.gt_difficult = dif; im.n_gt = 2;
        vd_proposal_recall(&im, 1, props, 2, 0.5, &hit, &tot);
        check(hit == 1 && tot == 2, "recall: 1 of 2 GT covered by proposals");
    }

    printf("checks=%d failures=%d\n", checks, failures);
    if (failures == 0) { printf("VISION_DETECTION_EVAL_PASS checks=%d\n", checks); return 0; }
    printf("VISION_DETECTION_EVAL_FAIL failures=%d\n", failures);
    return 1;
}
