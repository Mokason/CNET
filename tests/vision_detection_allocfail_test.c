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
static long clean_allocs(void) { return alloc_count; }

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

    /* vd_ap50: every allocation it actually performs is failed in turn, and the
       result must be EXACTLY NaN -- not merely "plausible". */
    {
        int bad = 0;
        long reach;
        no_inject();
        (void)vd_ap50(IMGS, 2, 0.5);
        reach = clean_allocs();
        check(reach > 0, "vd_ap50: performs allocations to inject into");
        for (i = 1; i <= reach; i++) {
            double ap;
            inject(i);
            ap = vd_ap50(IMGS, 2, 0.5);
            no_inject();
            if (!isnan(ap)) bad++;   /* every reachable failure must be NaN */
        }
        check(bad == 0, "vd_ap50: every reachable allocation failure returns exactly NaN");
        for (i = reach + 1; i <= reach + 4; i++) {
            double ap;
            inject(i);
            ap = vd_ap50(IMGS, 2, 0.5);
            no_inject();
            if (!(isfinite(ap) && ap > 0.0)) bad++;   /* unreachable: must still work */
        }
        check(bad == 0, "vd_ap50: unreached injection still yields the real score");
        report("vd_ap50 exact injection");
    }

    /* vd_pr_at: every reachable failure must report -1 AND leave NaN outputs */
    {
        int bad = 0;
        long reach;
        double pr0 = 0, rc0 = 0;
        no_inject();
        (void)vd_pr_at(IMGS, 2, 0.5, 0.5, &pr0, &rc0);
        reach = clean_allocs();
        check(reach > 0, "vd_pr_at: performs allocations to inject into");
        for (i = 1; i <= reach; i++) {
            double pr = 123.0, rc = 123.0;
            int st;
            inject(i);
            st = vd_pr_at(IMGS, 2, 0.5, 0.5, &pr, &rc);
            no_inject();
            if (st != -1) bad++;
            if (isfinite(pr) || isfinite(rc)) bad++;
        }
        check(bad == 0, "vd_pr_at: every reachable failure is -1 with NaN outputs");
        report("vd_pr_at exact injection");
    }

    /* vd_nms: every reachable failure must be exactly VD_NMS_FAIL */
    {
        int bad = 0, keep[3];
        long reach;
        no_inject();
        (void)vd_nms(DET, 3, 0.3, keep);
        reach = clean_allocs();
        check(reach > 0, "vd_nms: performs allocations to inject into");
        for (i = 1; i <= reach; i++) {
            size_t nn;
            inject(i);
            nn = vd_nms(DET, 3, 0.3, keep);
            no_inject();
            if (nn != VD_NMS_FAIL) bad++;
        }
        check(bad == 0, "vd_nms: every reachable failure returns exactly VD_NMS_FAIL");
        report("vd_nms exact injection");
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
        {
            long reach;
            no_inject();
            if (vd_pack_load(path, &pk) == VD_PACK_OK) vd_pack_free(&pk);
            reach = clean_allocs();
            check(reach > 0, "vd_pack_load: performs allocations to inject into");
            for (i = 1; i <= reach; i++) {
                int rc;
                inject(i);
                rc = vd_pack_load(path, &pk);
                no_inject();
                if (rc != VD_PACK_E_ALLOC) bad++;             /* exact status */
                if (pk.imgs != NULL || pk.n != 0) bad++;      /* fully zeroed */
            }
            check(bad == 0, "vd_pack_load: every reachable failure is E_ALLOC and zeroed");
        }
        report("vd_pack_load exact injection");
        unlink(path);
    }

    /* manifest parser: allocation failure must refuse, never half-populate */
    {
        char mp[256];
        FILE *mf;
        VdManifest m;
        char err[256];
        long reach;
        int bad = 0;
        snprintf(mp, sizeof mp, "/tmp/vd_allocfail_%d.manifest", (int)getpid());
        mf = fopen(mp, "wb");
        if (mf) { fputs("manifest_version 1\n", mf); fclose(mf); }
        no_inject();
        (void)vd_manifest_parse(mp, &m, err, sizeof err);
        reach = clean_allocs();
        for (i = 1; i <= reach; i++) {
            inject(i);
            if (vd_manifest_parse(mp, &m, err, sizeof err) == 0) bad++;
            no_inject();
        }
        check(bad == 0, "vd_manifest_parse: allocation failure never yields a parsed manifest");
        report("vd_manifest_parse exact injection");
        unlink(mp);
    }

    printf("checks=%d failures=%d\n", checks, failures);
    if (failures == 0) {
        printf("VISION_DETECTION_ALLOCFAIL_PASS checks=%d\n", checks);
        return 0;
    }
    printf("VISION_DETECTION_ALLOCFAIL_FAIL failures=%d\n", failures);
    return 1;
}
