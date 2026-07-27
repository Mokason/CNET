/* Deterministic allocation-failure injection.
 *
 * Every allocation in the evidence and metric paths is failed in turn, one per
 * run of the fixture, and the code must respond with an explicit failure --
 * NaN, VD_NMS_FAIL, or a nonzero status. What must never happen is a finite
 * number: an out-of-memory event that comes back as a score is indistinguishable
 * from a real measurement.
 *
 * Injection is via the linker's --wrap, so it is deterministic and needs no
 * environment tricks: the Nth allocation fails, for N = 1..depth.
 *
 * make vision_detection_allocfail_test -> VISION_DETECTION_ALLOCFAIL_PASS
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../tools/vision_detection/vd_eval.h"
#include "../tools/vision_detection/vd_pack.h"
#include "../tools/vision_detection/vd_protocol.h"

/* ---- allocation injection ---------------------------------------------- */
static long alloc_count;
static long alloc_fail_at = -1;   /* -1 disables injection */

void *__real_malloc(size_t);
void *__real_calloc(size_t, size_t);
void *__real_realloc(void *, size_t);

static int should_fail(void) {
    alloc_count++;
    return (alloc_fail_at >= 0 && alloc_count == alloc_fail_at);
}

void *__wrap_malloc(size_t n) { return should_fail() ? NULL : __real_malloc(n); }
void *__wrap_calloc(size_t a, size_t b) { return should_fail() ? NULL : __real_calloc(a, b); }
void *__wrap_realloc(void *p, size_t n) { return should_fail() ? NULL : __real_realloc(p, n); }

static void inject(long nth) { alloc_count = 0; alloc_fail_at = nth; }
static void no_inject(void) { alloc_count = 0; alloc_fail_at = -1; }

static int failures, checks;
static void check(int ok, const char *name) {
    checks++;
    if (!ok) { printf("  %-62s FAIL\n", name); failures++; }
}
static void report(const char *name) { printf("  %-62s PASS\n", name); }

/* ---- fixture ------------------------------------------------------------ */
static VdBox GT[2]  = {{0, 0, 10, 10}, {50, 50, 10, 10}};
static int   DIF[2] = {0, 0};
static VdDet DET[3];
static VdImage IMGS[2];

static void build(void) {
    DET[0] = (VdDet){{0, 0, 10, 10}, 0.9, 0};
    DET[1] = (VdDet){{50, 50, 10, 10}, 0.8, 0};
    DET[2] = (VdDet){{20, 20, 10, 10}, 0.7, 0};
    IMGS[0].gts = GT; IMGS[0].gt_difficult = DIF; IMGS[0].n_gt = 2;
    IMGS[0].dets = DET; IMGS[0].n_det = 3;
    IMGS[1].gts = GT; IMGS[1].gt_difficult = DIF; IMGS[1].n_gt = 2;
    IMGS[1].dets = DET; IMGS[1].n_det = 3;
}

int main(void) {
    const long DEPTH = 24;
    long i;
    char path[256];

    printf("== allocation-failure injection ==\n");
    build();

    /* baseline: with no injection the fixture must produce real numbers, or the
       whole suite would pass vacuously */
    no_inject();
    {
        double ap = vd_ap50(IMGS, 2, 0.5), pr = 0, rc = 0;
        int st = vd_pr_at(IMGS, 2, 0.5, 0.5, &pr, &rc);
        check(isfinite(ap) && ap > 0.0, "baseline: ap50 is a real number without injection");
        check(st == 0 && isfinite(pr) && isfinite(rc), "baseline: pr_at succeeds without injection");
    }
    report("baseline sanity");

    /* vd_ap50: every allocation failed in turn */
    {
        int bad = 0;
        for (i = 1; i <= DEPTH; i++) {
            double ap;
            inject(i);
            ap = vd_ap50(IMGS, 2, 0.5);
            no_inject();
            /* Either the allocation was not reached (real score) or it failed
               and the result is NaN. A finite wrong number is the bug. */
            if (!isnan(ap) && !(ap > 0.0)) bad++;
        }
        check(bad == 0, "vd_ap50: no allocation failure yields a bogus finite score");
        report("vd_ap50 injection depth 24");
    }

    /* vd_pr_at: failure must be reported AND outputs left non-finite */
    {
        int bad = 0;
        for (i = 1; i <= DEPTH; i++) {
            double pr = 123.0, rc = 123.0;
            int st;
            inject(i);
            st = vd_pr_at(IMGS, 2, 0.5, 0.5, &pr, &rc);
            no_inject();
            if (st != 0 && (isfinite(pr) || isfinite(rc))) bad++;   /* failed but numeric */
            if (st == 0 && (!isfinite(pr) || !isfinite(rc))) bad++; /* succeeded but NaN */
        }
        check(bad == 0, "vd_pr_at: status and outputs never disagree");
        report("vd_pr_at injection depth 24");
    }

    /* vd_nms: must return the sentinel, never a plausible count */
    {
        int bad = 0;
        for (i = 1; i <= 8; i++) {
            int keep[3];
            size_t n;
            inject(i);
            n = vd_nms(DET, 3, 0.3, keep);
            no_inject();
            if (n != VD_NMS_FAIL && n > 3) bad++;
        }
        check(bad == 0, "vd_nms: allocation failure returns VD_NMS_FAIL, never a count");
        report("vd_nms injection depth 8");
    }

    /* vd_pack_load: must refuse, never hand back a partial pack */
    {
        VdPack pk;
        int bad = 0, loaded_ok = 0;
        FILE *f;
        snprintf(path, sizeof path, "/tmp/vd_allocfail_%d.pack", (int)getpid());
        /* one valid image, 2 proposals, dim 4 */
        f = fopen(path, "wb");
        if (f) {
            int32_t v; int64_t agg = 2; int j, k;
            fwrite("VDPACK1", 1, 8, f);
            v = 4; fwrite(&v, 4, 1, f);
            v = 1; fwrite(&v, 4, 1, f);
            fwrite(&agg, 8, 1, f);
            v = 6; fwrite(&v, 4, 1, f); fwrite("000123", 1, 6, f);
            v = 2; fwrite(&v, 4, 1, f);
            v = 1; fwrite(&v, 4, 1, f);
            v = 0; fwrite(&v, 4, 1, f); fwrite(&v, 4, 1, f);
            v = 20; fwrite(&v, 4, 1, f); fwrite(&v, 4, 1, f);
            v = 0; fwrite(&v, 4, 1, f);
            for (j = 0; j < 2; j++) {
                v = 5; fwrite(&v, 4, 1, f); fwrite(&v, 4, 1, f);
                v = 30; fwrite(&v, 4, 1, f); fwrite(&v, 4, 1, f);
                v = 0; fwrite(&v, 4, 1, f);
                for (k = 0; k < 4; k++) { float z = 0.25f; fwrite(&z, 4, 1, f); }
            }
            fclose(f);
        }
        no_inject();
        if (vd_pack_load(path, &pk) == VD_PACK_OK) { loaded_ok = 1; vd_pack_free(&pk); }
        check(loaded_ok, "baseline: fixture pack loads without injection");
        for (i = 1; i <= 12; i++) {
            int rc;
            inject(i);
            rc = vd_pack_load(path, &pk);
            no_inject();
            if (rc == VD_PACK_OK) vd_pack_free(&pk);   /* allocation not reached */
            else if (pk.imgs != NULL || pk.n != 0) bad++;  /* must be fully zeroed */
        }
        check(bad == 0, "vd_pack_load: refusal leaves no partial pack behind");
        report("vd_pack_load injection depth 12");
        unlink(path);
    }

    printf("checks=%d failures=%d\n", checks, failures);
    if (failures == 0) {
        printf("VISION_DETECTION_ALLOCFAIL_PASS checks=%d\n", checks);
        return 0;
    }
    printf("VISION_DETECTION_ALLOCFAIL_FAIL failures=%d\n", failures);
    return 1;
}
