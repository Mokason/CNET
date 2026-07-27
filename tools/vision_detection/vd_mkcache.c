/* Builds a miniature but structurally valid cache directory (packs + pca.bin +
 * manifest) so the bench's evidence gate can be exercised without the 1.1 GB
 * real cache or any VOC data. Test-support only: it is never part of a scored
 * run, and the caches it writes are far too small to train on.
 *
 * Used by scripts/vision_detection_evidence_test.sh.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "vd_io.h"
#include "vd_pack.h"
#include "vd_protocol.h"
#include "vd_sha256.h"

static void w32(FILE *f, int32_t v) { fwrite(&v, 4, 1, f); }
static void w64(FILE *f, int64_t v) { fwrite(&v, 8, 1, f); }

/* id_base keeps the "spent" holdout disjoint from the new one by construction */
static int write_pack(const char *path, int n_img, int dim, int n_prop, int n_gt,
                      int id_base) {
    FILE *f = fopen(path, "wb");
    int i, j, k;
    if (!f) return -1;
    fwrite("VDPACK1", 1, 8, f);
    w32(f, dim);
    w32(f, n_img);
    w64(f, (int64_t)n_img * n_prop);
    for (i = 0; i < n_img; i++) {
        char id[16];
        int idlen = snprintf(id, sizeof id, "%06d", id_base + i);
        w32(f, idlen);
        fwrite(id, 1, (size_t)idlen, f);
        w32(f, n_prop);
        w32(f, n_gt);
        for (j = 0; j < n_gt; j++) {
            w32(f, 10); w32(f, 10); w32(f, 40); w32(f, 40); w32(f, 0);
        }
        for (j = 0; j < n_prop; j++) {
            w32(f, 5 + j); w32(f, 5 + j); w32(f, 30); w32(f, 30);
            w32(f, (j % 4 == 0) ? 1 : 0);
            for (k = 0; k < dim; k++) {
                float v = (float)((j + k) % 7) * 0.125f;
                fwrite(&v, 4, 1, f);
            }
        }
    }
    fclose(f);
    return 0;
}

int main(int argc, char **argv) {
    const char *out = NULL, *prev = NULL, *variant = "v2";
    /* Large enough for a COMPLETE bench run (train rows > the 1000 minimum) so
       the publication path is exercised end to end, small enough that the run
       takes about a second at dim 8. */
    int dim = 8, n_tr = 400, n_va = 40, n_te = 40, n_prop = 4, n_gt = 1;
    int test_offset = 1000, prev_count = 40, prev_base = 900000;
    int i;
    char p[VD_PATH_MAX], sh_tr[65], sh_va[65], sh_te[65], sh_pca[65], sh_prev[65];
    char sh_side[6][65], rt[6][65], aroot[65];
    char body[8192];
    size_t len;
    FILE *f;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "--prev") && i + 1 < argc) prev = argv[++i];
        else if (!strcmp(argv[i], "--variant") && i + 1 < argc) variant = argv[++i];
        else if (!strcmp(argv[i], "--test-offset") && i + 1 < argc) test_offset = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--prev-base") && i + 1 < argc) prev_base = atoi(argv[++i]);
    }
    if (!out) { fprintf(stderr, "need --out\n"); return 2; }
    if (vd_mkdir_p(out) != 0) { fprintf(stderr, "mkdir failed\n"); return 2; }

    snprintf(p, sizeof p, "%s/train.pack", out);
    if (write_pack(p, n_tr, dim, n_prop, n_gt, 100000)) return 3;
    snprintf(p, sizeof p, "%s/val.pack", out);
    if (write_pack(p, n_va, dim, n_prop, n_gt, 200000)) return 3;
    snprintf(p, sizeof p, "%s/test.pack", out);
    if (write_pack(p, n_te, dim, n_prop, n_gt, 300000)) return 3;

    snprintf(p, sizeof p, "%s/pca.bin", out);
    f = fopen(p, "wb");
    if (!f) return 3;
    { int32_t a = 16, b = dim; float z = 0.0f; int k;
      fwrite(&a, 4, 1, f); fwrite(&b, 4, 1, f);
      for (k = 0; k < 16 + 16 * dim; k++) fwrite(&z, 4, 1, f); }
    fclose(f);

    strcpy(sh_prev, "none");
    if (prev) {
        if (write_pack(prev, prev_count, dim, n_prop, n_gt, prev_base)) return 3;
        if (vd_sha256_file(prev, sh_prev) != 0) return 3;
    }

    snprintf(p, sizeof p, "%s/train.pack", out); if (vd_sha256_file(p, sh_tr) != 0) return 3;
    snprintf(p, sizeof p, "%s/val.pack", out);   if (vd_sha256_file(p, sh_va) != 0) return 3;
    snprintf(p, sizeof p, "%s/test.pack", out);  if (vd_sha256_file(p, sh_te) != 0) return 3;
    snprintf(p, sizeof p, "%s/pca.bin", out);    if (vd_sha256_file(p, sh_pca) != 0) return 3;

    {   /* sidecars, so the gate has something to recompute roots from */
        const char *names[6] = {"ids_train.txt","ids_val.txt","ids_test.txt",
                                "content_train.txt","content_val.txt","content_test.txt"};
        int counts[6]; int bases[6]; int k2;
        counts[0]=n_tr; counts[1]=n_va; counts[2]=n_te;
        counts[3]=n_tr; counts[4]=n_va; counts[5]=n_te;
        bases[0]=100000; bases[1]=200000; bases[2]=300000;
        bases[3]=100000; bases[4]=200000; bases[5]=300000;
        for (k2 = 0; k2 < 6; k2++) {
            int q;
            snprintf(p, sizeof p, "%s/%s", out, names[k2]);
            f = fopen(p, "wb");
            if (!f) return 3;
            for (q = 0; q < counts[k2]; q++) {
                if (k2 < 3) fprintf(f, "%06d\n", bases[k2] + q);
                else fprintf(f, "%064d\n", bases[k2] + q);
            }
            fclose(f);
        }
    }
    {
        const char *names[6] = {"ids_train.txt","ids_val.txt","ids_test.txt",
                                "content_train.txt","content_val.txt","content_test.txt"};
        int k2;
        for (k2 = 0; k2 < 6; k2++) {
            snprintf(p, sizeof p, "%s/%s", out, names[k2]);
            if (vd_sha256_file(p, sh_side[k2]) != 0) return 3;
            if (vd_root_of_file(p, rt[k2], NULL) != 0) return 3;
        }
    }

    {   /* artifact root over this cache's own members */
        VdMember mem[VD_N_MEMBERS];
        const char *dg[VD_N_MEMBERS] = {sh_tr, sh_va, sh_te, sh_pca,
                                        sh_side[0], sh_side[1], sh_side[2],
                                        sh_side[3], sh_side[4], sh_side[5]};
        struct stat sb;
        int k2;
        for (k2 = 0; k2 < VD_N_MEMBERS; k2++) {
            snprintf(p, sizeof p, "%s/%s", out, VD_MEMBERS[k2]);
            if (stat(p, &sb) != 0) return 3;
            mem[k2].name = VD_MEMBERS[k2];
            mem[k2].size = (long long)sb.st_size;
            snprintf(mem[k2].sha, sizeof mem[k2].sha, "%s", dg[k2]);
        }
        if (vd_artifact_root(mem, VD_N_MEMBERS, 1, aroot) != 0) return 3;
    }
    len = (size_t)snprintf(body, sizeof body,
        "manifest_version 1\nvariant %s\ndataset SYNTHETIC_TEST\nclass car\n"
        "split_key sha256_content_hash_trainval\nseed 20260727\n"
        "hog_side 64\ncolor 1\nhog_dim 1764\npca_dim %d\n"
        "ss_width 300\nmax_prop 300\nmin_side 16\n"
        "nms_iou_x100 30\nmatch_iou_x100 50\n"
        "test_offset %d\ntest_count %d\n"
        "train_img %d\nval_img %d\ntest_img %d\n"
        "train_prop %d\nval_prop %d\ntest_prop %d\n"
        "pca_fit_images 2\npca_fit_rows 8\n"
        "trainval_id_overlap 0\ntrainval_content_overlap 0\n"
        "prev_test_ids_checked %d\nprev_test_sha_checked %d\n"
        "prev_test_id_overlap 0\nprev_test_content_overlap 0\n"
        "sha256_prev_test_pack %s\n"
        "sha256_train_pack %s\nsha256_val_pack %s\n"
        "sha256_test_pack %s\nsha256_pca_bin %s\n"
        "sha256_ids_train %s\nsha256_ids_val %s\nsha256_ids_test %s\n"
        "sha256_content_train %s\nsha256_content_val %s\nsha256_content_test %s\n"
        "id_root_train %s\nid_root_val %s\nid_root_test %s\n"
        "content_root_train %s\ncontent_root_val %s\ncontent_root_test %s\n"
        "artifact_root %s\n",
        variant, dim, test_offset, n_te, n_tr, n_va, n_te,
        n_tr * n_prop, n_va * n_prop, n_te * n_prop,
        test_offset > 0 ? prev_count : 0, test_offset > 0 ? prev_count : 0,
        sh_prev, sh_tr, sh_va, sh_te, sh_pca,
        sh_side[0], sh_side[1], sh_side[2], sh_side[3], sh_side[4], sh_side[5],
        rt[0], rt[1], rt[2], rt[3], rt[4], rt[5], aroot);
    if (len >= sizeof body) return 3;

    {   /* roots of the synthetic sidecars, so the test protocol can pin them */
        const char *names[6] = {"ids_train.txt","ids_val.txt","ids_test.txt",
                                "content_train.txt","content_val.txt","content_test.txt"};
        const char *labels[6] = {"id_train","id_val","id_test",
                                 "content_train","content_val","content_test"};
        char hex[65];
        int k2;
        for (k2 = 0; k2 < 6; k2++) {
            snprintf(p, sizeof p, "%s/%s", out, names[k2]);
            if (vd_root_of_file(p, hex, NULL) == 0)
                printf("VD_MKCACHE_ROOT %s=%s\n", labels[k2], hex);
        }
        if (prev && vd_sha256_file(prev, hex) == 0)
            printf("VD_MKCACHE_ROOT prev_pack=%s\n", hex);
        printf("VD_MKCACHE_ROOT artifact=%s\n", aroot);
    }
    snprintf(p, sizeof p, "%s/manifest.txt", out);
    if (vd_publish_file(p, body, len) != 0) return 3;
    printf("VD_MKCACHE_OK %s\n", out);
    return 0;
}
