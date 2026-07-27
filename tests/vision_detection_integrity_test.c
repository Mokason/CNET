/* Integrity fixtures for the vision benchmark's evidence path.
 *
 * Everything here is a negative control: a benchmark that will happily parse a
 * malformed pack, publish a half-written result file, or hash a file through a
 * symlink can produce a number that looks like a PASS and is not one. Each case
 * crafts a hostile input and asserts the code REFUSES it with a nonzero status.
 *
 * No network, no dataset, no GPU. Everything is built in a temp directory.
 *
 * make vision_detection_integrity_test -> VISION_DETECTION_INTEGRITY_PASS
 */
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../tools/vision_detection/vd_eval.h"
#include "../tools/vision_detection/vd_io.h"
#include "../tools/vision_detection/vd_pack.h"
#include "../tools/vision_detection/vd_protocol.h"
#include "../tools/vision_detection/vd_sha256.h"

static int failures, checks;
static char DIR[64];   /* small enough that every derived path is provably bounded */

static void check(int ok, const char *name) {
    checks++;
    printf("  %-64s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

/* ---------- pack builder ------------------------------------------------- */
typedef struct { unsigned char *b; size_t n, cap; } Buf;

static void bput(Buf *b, const void *p, size_t n) {
    if (b->n + n > b->cap) {
        b->cap = (b->n + n) * 2 + 256;
        b->b = (unsigned char *)realloc(b->b, b->cap);
        if (!b->b) { fprintf(stderr, "oom\n"); exit(9); }
    }
    memcpy(b->b + b->n, p, n);
    b->n += n;
}
static void b32(Buf *b, int32_t v) { bput(b, &v, 4); }
static void b64(Buf *b, int64_t v) { bput(b, &v, 8); }

/* A structurally valid one-image pack, then knobs to corrupt it. */
typedef struct {
    const char *magic;
    int32_t dim, n_img;
    int64_t agg;             /* declared aggregate proposal count */
    int32_t idlen;           /* 0 = use strlen(id) */
    const char *id;
    int32_t n_prop, n_gt;
    int32_t prop_w;          /* proposal box width */
    int32_t label;
    float feat_fill;
    int truncate_bytes;      /* chop N bytes off the end */
    int trailing_bytes;      /* append N junk bytes */
} PackSpec;

static PackSpec good(void) {
    PackSpec s;
    memset(&s, 0, sizeof s);
    s.magic = "VDPACK1"; s.dim = 4; s.n_img = 1; s.agg = 2;
    s.idlen = 0; s.id = "000123"; s.n_prop = 2; s.n_gt = 1;
    s.prop_w = 10; s.label = 1; s.feat_fill = 0.5f;
    return s;
}

static void write_pack(const char *path, PackSpec s) {
    Buf b; int32_t i, j; FILE *f;
    char m[8];
    memset(&b, 0, sizeof b);
    memset(m, 0, 8);
    memcpy(m, s.magic, strlen(s.magic) < 8 ? strlen(s.magic) : 8);
    bput(&b, m, 8);
    b32(&b, s.dim); b32(&b, s.n_img); b64(&b, s.agg);
    for (i = 0; i < (s.n_img > 0 ? s.n_img : 0); i++) {
        int32_t il = s.idlen ? s.idlen : (int32_t)strlen(s.id);
        b32(&b, il);
        if (il > 0) bput(&b, s.id, (size_t)il < strlen(s.id) ? (size_t)il : strlen(s.id));
        b32(&b, s.n_prop); b32(&b, s.n_gt);
        for (j = 0; j < (s.n_gt > 0 ? s.n_gt : 0); j++) {
            b32(&b, 0); b32(&b, 0); b32(&b, 20); b32(&b, 20); b32(&b, 0);
        }
        for (j = 0; j < (s.n_prop > 0 ? s.n_prop : 0); j++) {
            int32_t k;
            b32(&b, 0); b32(&b, 0); b32(&b, s.prop_w); b32(&b, 10); b32(&b, s.label);
            for (k = 0; k < s.dim; k++) bput(&b, &s.feat_fill, 4);
        }
    }
    if (s.truncate_bytes > 0 && (size_t)s.truncate_bytes < b.n) b.n -= (size_t)s.truncate_bytes;
    for (i = 0; i < s.trailing_bytes; i++) { unsigned char z = 0xAB; bput(&b, &z, 1); }
    f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", path); exit(9); }
    fwrite(b.b, 1, b.n, f);
    fclose(f);
    free(b.b);
}

static int load_rc(PackSpec s) {
    char p[600];
    VdPack pk;
    int rc;
    snprintf(p, sizeof p, "%s/t.pack", DIR);
    write_pack(p, s);
    rc = vd_pack_load(p, &pk);
    if (rc == VD_PACK_OK) vd_pack_free(&pk);
    return rc;
}

/* Swaps the destination directory for another one at a chosen publication
   phase, so the identity checks are exercised deterministically. */
static const char *g_hook_swap_from, *g_hook_swap_to;
static int g_hook_phase_to_fire;

static void swap_hook(VdStageHookPhase ph, void *ctx) {
    char tmp[700];
    (void)ctx;
    if ((int)ph != g_hook_phase_to_fire) return;
    snprintf(tmp, sizeof tmp, "%s.swapaside", g_hook_swap_from);
    if (rename(g_hook_swap_from, tmp) != 0) return;
    if (rename(g_hook_swap_to, g_hook_swap_from) != 0) { (void)rename(tmp, g_hook_swap_from); return; }
    (void)rename(tmp, g_hook_swap_to);
}

/* The required members of a recognised cache, so a replace test operates on a
   destination the publisher will actually accept. */
static int write_full_cache(VdStage *st) {
    static const char *names[] = {
        "manifest.txt", "train.pack", "val.pack", "test.pack", "pca.bin",
        "ids_train.txt", "ids_val.txt", "ids_test.txt",
        "content_train.txt", "content_val.txt", "content_test.txt"
    };
    size_t i;
    for (i = 0; i < sizeof names / sizeof names[0]; i++) {
        VdOut o;
        const char *body = strcmp(names[i], "manifest.txt") ? "x\n" : "manifest_version 1\n";
        if (vd_out_open(st, names[i], &o) != 0) return -1;
        if (vd_out_write(&o, body, strlen(body)) != 0) { vd_out_finish(&o); return -1; }
        if (vd_out_finish(&o) != 0) return -1;
    }
    return 0;
}

int main(void) {
    printf("== vision benchmark integrity fixtures ==\n");
    snprintf(DIR, sizeof DIR, "/tmp/vd_integrity_%d", (int)getpid());
    if (vd_mkdir_p(DIR) != 0) { printf("cannot create %s\n", DIR); return 9; }

    /* ---- SHA-256 known answers ------------------------------------------ */
    {
        VdSha256 c; char h[65];
        vd_sha256_init(&c); vd_sha256_hex(&c, h);
        check(!strcmp(h, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"),
              "sha256: empty string known answer");
        vd_sha256_init(&c); vd_sha256_update(&c, "abc", 3); vd_sha256_hex(&c, h);
        check(!strcmp(h, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"),
              "sha256: \"abc\" known answer");
    }

    /* ---- pack: the valid case must still load --------------------------- */
    check(load_rc(good()) == VD_PACK_OK, "pack: well-formed pack loads");

    /* ---- pack: hostile headers ------------------------------------------ */
    { PackSpec s = good(); s.magic = "XXXXXXX";
      check(load_rc(s) == VD_PACK_E_MAGIC, "pack: bad magic refused"); }
    { PackSpec s = good(); s.dim = 0;
      check(load_rc(s) != VD_PACK_OK, "pack: dim 0 refused"); }
    { PackSpec s = good(); s.dim = -1;
      check(load_rc(s) != VD_PACK_OK, "pack: negative dim refused"); }
    { PackSpec s = good(); s.dim = VD_MAX_DIM + 1;
      check(load_rc(s) != VD_PACK_OK, "pack: oversize dim refused"); }
    { PackSpec s = good(); s.n_img = -1;
      check(load_rc(s) != VD_PACK_OK, "pack: negative image count refused"); }
    { PackSpec s = good(); s.n_img = VD_MAX_IMAGES + 1;
      check(load_rc(s) != VD_PACK_OK, "pack: oversize image count refused"); }
    { PackSpec s = good(); s.agg = -5;
      check(load_rc(s) != VD_PACK_OK, "pack: negative aggregate refused"); }

    /* ---- pack: hostile per-image counts --------------------------------- */
    { PackSpec s = good(); s.n_prop = -1;
      check(load_rc(s) != VD_PACK_OK, "pack: negative proposal count refused"); }
    { PackSpec s = good(); s.n_prop = VD_MAX_PROPS_PER_IMAGE + 1; s.agg = s.n_prop;
      check(load_rc(s) != VD_PACK_OK, "pack: oversize proposals/image refused"); }
    { PackSpec s = good(); s.n_gt = -1;
      check(load_rc(s) != VD_PACK_OK, "pack: negative GT count refused"); }
    { PackSpec s = good(); s.n_gt = VD_MAX_GT_PER_IMAGE + 1;
      check(load_rc(s) != VD_PACK_OK, "pack: oversize GT/image refused"); }
    { PackSpec s = good(); s.idlen = 0 - 1;
      check(load_rc(s) != VD_PACK_OK, "pack: negative id length refused"); }
    { PackSpec s = good(); s.idlen = VD_MAX_ID_LEN + 1;
      check(load_rc(s) != VD_PACK_OK, "pack: oversize id length refused"); }

    /* ---- pack: structural integrity ------------------------------------- */
    { PackSpec s = good(); s.truncate_bytes = 6;
      check(load_rc(s) == VD_PACK_E_TRUNC, "pack: truncated mid-feature refused"); }
    { PackSpec s = good(); s.truncate_bytes = 1;
      check(load_rc(s) != VD_PACK_OK, "pack: one byte short refused"); }
    { PackSpec s = good(); s.trailing_bytes = 16;
      check(load_rc(s) == VD_PACK_E_TRAILING, "pack: trailing data refused"); }
    { PackSpec s = good(); s.agg = 999;
      check(load_rc(s) == VD_PACK_E_AGGREGATE, "pack: aggregate count mismatch refused"); }

    /* ---- pack: field-level validation ----------------------------------- */
    { PackSpec s = good(); s.prop_w = 0;
      check(load_rc(s) == VD_PACK_E_FIELD, "pack: zero-width proposal box refused"); }
    { PackSpec s = good(); s.prop_w = -10;
      check(load_rc(s) == VD_PACK_E_FIELD, "pack: negative-width proposal box refused"); }
    { PackSpec s = good(); s.prop_w = VD_MAX_COORD + 1;
      check(load_rc(s) == VD_PACK_E_FIELD, "pack: absurd proposal width refused"); }
    { PackSpec s = good(); s.label = 7;
      check(load_rc(s) == VD_PACK_E_FIELD, "pack: label outside {-1,0,1} refused"); }
    { PackSpec s = good(); s.feat_fill = (float)NAN;
      check(load_rc(s) == VD_PACK_E_FIELD, "pack: NaN feature refused"); }
    { PackSpec s = good(); s.feat_fill = (float)INFINITY;
      check(load_rc(s) == VD_PACK_E_FIELD, "pack: infinite feature refused"); }

    /* read_ids must not be weaker than the loader */
    { PackSpec s = good(); s.trailing_bytes = 8;
      char p[600], **ids = NULL; size_t n = 0; int rc;
      snprintf(p, sizeof p, "%s/t.pack", DIR);
      write_pack(p, s);
      rc = vd_pack_read_ids(p, &ids, &n);
      if (rc == VD_PACK_OK) vd_pack_free_ids(ids, n);
      check(rc != VD_PACK_OK, "pack: read_ids refuses what load refuses"); }

    /* ---- path validation ------------------------------------------------- */
    check(vd_path_ok("") != 0, "path: empty rejected");
    check(vd_path_ok(NULL) != 0, "path: NULL rejected");
    check(vd_path_ok("a/../../etc") != 0, "path: .. component rejected");
    check(vd_path_ok("out; rm -rf /") != 0, "path: shell metacharacter rejected");
    check(vd_path_ok("out$(id)") != 0, "path: command substitution rejected");
    check(vd_path_ok("data/vision_cache_v2") == 0, "path: ordinary path accepted");
    {
        char big[VD_PATH_MAX + 16];
        memset(big, 'a', sizeof big - 1); big[sizeof big - 1] = 0;
        check(vd_path_ok(big) != 0, "path: over-length rejected");
    }

    /* ---- mkdir_p --------------------------------------------------------- */
    {
        char nest[600]; struct stat st;
        snprintf(nest, sizeof nest, "%s/a/b/c", DIR);
        check(vd_mkdir_p(nest) == 0 && stat(nest, &st) == 0 && S_ISDIR(st.st_mode),
              "mkdir_p: creates nested directories");
        check(vd_mkdir_p(nest) == 0, "mkdir_p: existing directory is not an error");
        check(vd_mkdir_p("bad;path") != 0, "mkdir_p: refuses metacharacter path");
    }
    {   /* an existing regular file must not be mistaken for a directory */
        char fp[600]; FILE *f;
        snprintf(fp, sizeof fp, "%s/plainfile", DIR);
        f = fopen(fp, "wb"); if (f) { fputc('x', f); fclose(f); }
        check(vd_mkdir_p(fp) != 0, "mkdir_p: existing non-directory refused");
    }

    /* ---- transactional publication --------------------------------------- */
    {
        char fp[600]; FILE *f; char rb[64]; size_t got;
        snprintf(fp, sizeof fp, "%s/out.json", DIR);
        check(vd_publish_file(fp, "{\"a\":1}", 7) == 0, "publish: writes file");
        f = fopen(fp, "rb"); got = f ? fread(rb, 1, sizeof rb, f) : 0; if (f) fclose(f);
        check(got == 7 && !memcmp(rb, "{\"a\":1}", 7), "publish: content is exact");
        check(vd_publish_file(fp, "{\"a\":2}", 7) == 0, "publish: overwrites atomically");
        f = fopen(fp, "rb"); got = f ? fread(rb, 1, sizeof rb, f) : 0; if (f) fclose(f);
        check(got == 7 && !memcmp(rb, "{\"a\":2}", 7), "publish: replacement is exact");
    }
    {   /* the failure that matters: publication must report failure, not succeed quietly */
        char bad[600];
        snprintf(bad, sizeof bad, "%s/nonexistent_dir/out.json", DIR);
        check(vd_publish_file(bad, "x", 1) != 0, "publish: missing directory reports failure");
        check(vd_publish_file("/proc/cnet_nope/out.json", "x", 1) != 0,
              "publish: unwritable destination reports failure");
        check(vd_publish_file("bad;name.json", "x", 1) != 0,
              "publish: metacharacter destination refused");
    }
    {   /* a symlink planted at the temp path must not be written through */
        char fp[600], tp[600];
        struct stat st;
        snprintf(fp, sizeof fp, "%s/link_target.json", DIR);
        snprintf(tp, sizeof tp, "%s/link_target.json.tmp", DIR);
        (void)unlink(tp);
        if (symlink("/tmp/vd_should_never_be_created", tp) == 0) {
            int rc = vd_publish_file(fp, "x", 1);
            int leaked = (stat("/tmp/vd_should_never_be_created", &st) == 0);
            check(!leaked, "publish: does not write through a planted symlink");
            (void)rc;
            (void)unlink(tp);
        } else {
            check(1, "publish: symlink case skipped (symlink unavailable)");
        }
    }

    /* ---- abort never deletes; it quarantines ----------------------------- */
    {
        char qdest[600], stagepath[VD_PATH_MAX + 128];
        VdStage st;
        struct stat sb;
        const char *q;

        snprintf(qdest, sizeof qdest, "%s/quarantined", DIR);
        check(vd_stage_begin(qdest, &st) == 0, "quarantine: staging begins");
        check(write_full_cache(&st) == 0, "quarantine: staging is complete");
        {
            int sn = snprintf(stagepath, sizeof stagepath, "%s", st.stage_full);
            check(sn > 0 && (size_t)sn < sizeof stagepath, "quarantine: stage path bounded");
        }
        vd_stage_abort(&st);
        q = vd_stage_quarantine(&st);
        check(q != NULL && *q, "quarantine: abort reports the staging path");
        check(stat(stagepath, &sb) == 0 && S_ISDIR(sb.st_mode),
              "quarantine: abort did NOT delete the staging directory");
        check(stat(qdest, &sb) != 0, "quarantine: the destination was never created");

        {   /* refused publish: destination intact, staging preserved+reported */
            char occupied[600];
            struct stat b1, b2;
            snprintf(occupied, sizeof occupied, "%s/occupied", DIR);
            check(vd_stage_begin(occupied, &st) == 0 && write_full_cache(&st) == 0
                  && vd_stage_commit(&st) == 0, "quarantine: first publish succeeds");
            check(stat(occupied, &b1) == 0, "quarantine: destination stat");
            check(vd_stage_begin(occupied, &st) == 0, "quarantine: second staging begins");
            check(write_full_cache(&st) == 0, "quarantine: second staging complete");
            {
                int sn = snprintf(stagepath, sizeof stagepath, "%s", st.stage_full);
                check(sn > 0 && (size_t)sn < sizeof stagepath, "quarantine: path bounded");
            }
            check(vd_stage_commit(&st) != 0, "quarantine: publish over existing is refused");
            q = vd_stage_quarantine(&st);
            check(q != NULL && *q, "quarantine: refused publish reports the staging path");
            check(stat(stagepath, &sb) == 0 && S_ISDIR(sb.st_mode),
                  "quarantine: refused publish preserved the staging directory");
            check(stat(occupied, &b2) == 0 && b1.st_dev == b2.st_dev && b1.st_ino == b2.st_ino,
                  "quarantine: destination keeps its exact device+inode");
        }
    }

    /* ---- evaluator geometry under extreme boxes -------------------------- */
    {
        VdBox big = {2147483000, 2147483000, 1000, 1000};
        VdBox b2  = {2147483000, 2147483000, 1000, 1000};
        double v = vd_iou(big, b2);
        check(v >= 0.0 && v <= 1.0 && !isnan(v),
              "eval: iou on near-INT_MAX boxes stays in [0,1]");
    }
    {
        VdBox a = {0, 0, 10, 10}, bad = {0, 0, -5, 10};
        double v = vd_iou(a, bad);
        check(v == 0.0 || isnan(v), "eval: iou with negative extent is not a positive score");
    }
    {   /* invalid geometry must not be silently scored as a detection */
        VdImage im;
        VdBox gt = {0, 0, 10, 10};
        VdDet det = {{0, 0, -10, -10}, 0.9, 0};
        int dif = 0;
        double ap;
        im.gts = &gt; im.gt_difficult = &dif; im.n_gt = 1;
        im.dets = &det; im.n_det = 1;
        ap = vd_ap50(&im, 1, 0.5);
        check(isnan(ap) || ap == 0.0, "eval: malformed detection box yields no credit");
    }
    {
        VdDet d[2];
        int keep[2];
        size_t n;
        d[0] = (VdDet){{0, 0, 10, 10}, 0.9, 0};
        d[1] = (VdDet){{0, 0, -1, 10}, 0.8, 0};
        n = vd_nms(d, 2, 0.3, keep);
        check(n == VD_NMS_FAIL, "eval: nms refuses a malformed box");
    }

    /* ---- strict manifest schema ------------------------------------------
       A manifest is an identity claim; anything ambiguous about it must be
       refused rather than interpreted generously. */
    {
        char mp[600], err[256];
        VdManifest m;
        FILE *f;
        const char *base =
            "manifest_version 1\nvariant v2\ndataset PASCAL_VOC_2007\nclass car\n"
            "split_key sha256_content_hash_trainval\nseed 20260727\n"
            "hog_side 64\ncolor 1\nhog_dim 1764\npca_dim 256\n"
            "ss_width 300\nmax_prop 300\nmin_side 16\n"
            "nms_iou_x100 30\nmatch_iou_x100 50\n"
            "test_offset 1000\ntest_count 1000\ntrain_img 4042\nval_img 969\ntest_img 1000\n"
            "train_prop 10\nval_prop 10\ntest_prop 10\n"
            "pca_fit_images 600\npca_fit_rows 162983\n"
            "trainval_id_overlap 0\ntrainval_content_overlap 0\n"
            "prev_test_ids_checked 1000\nprev_test_sha_checked 1000\n"
            "prev_test_id_overlap 0\nprev_test_content_overlap 0\n"
            "sha256_prev_test_pack 1111111111111111111111111111111111111111111111111111111111111111\n"
            "sha256_train_pack 2222222222222222222222222222222222222222222222222222222222222222\n"
            "sha256_val_pack 3333333333333333333333333333333333333333333333333333333333333333\n"
            "sha256_test_pack 4444444444444444444444444444444444444444444444444444444444444444\n"
            "sha256_pca_bin 5555555555555555555555555555555555555555555555555555555555555555\n"
            "sha256_ids_train 6666666666666666666666666666666666666666666666666666666666666666\n"
            "sha256_ids_val 7777777777777777777777777777777777777777777777777777777777777777\n"
            "sha256_ids_test 8888888888888888888888888888888888888888888888888888888888888888\n"
            "sha256_content_train 9999999999999999999999999999999999999999999999999999999999999999\n"
            "sha256_content_val aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n"
            "sha256_content_test bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\n"
            "id_root_train cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc\n"
            "id_root_val dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd\n"
            "id_root_test eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee\n"
            "content_root_train ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff\n"
            "content_root_val 0000000000000000000000000000000000000000000000000000000000000000\n"
            "content_root_test 1010101010101010101010101010101010101010101010101010101010101010\n"
            "artifact_root 2020202020202020202020202020202020202020202020202020202020202020\n";
        snprintf(mp, sizeof mp, "%s/manifest.txt", DIR);

#define WRITE_MAN(body) do { f = fopen(mp, "wb"); fputs((body), f); fclose(f); } while (0)
        WRITE_MAN(base);
        check(vd_manifest_parse(mp, &m, err, sizeof err) == 0,
              "manifest: complete well-formed manifest parses");

        {   /* unknown key */
            char b[8192]; snprintf(b, sizeof b, "%sextra_key 1\n", base);
            WRITE_MAN(b);
            check(vd_manifest_parse(mp, &m, err, sizeof err) != 0, "manifest: unknown key refused");
        }
        {   /* duplicate key */
            char b[8192]; snprintf(b, sizeof b, "%sseed 20260727\n", base);
            WRITE_MAN(b);
            check(vd_manifest_parse(mp, &m, err, sizeof err) != 0, "manifest: duplicate key refused");
        }
        {   /* missing key */
            char b[8192]; const char *cut = strstr(base, "seed 20260727\n");
            size_t pre = (size_t)(cut - base);
            snprintf(b, sizeof b, "%.*s%s", (int)pre, base, cut + strlen("seed 20260727\n"));
            WRITE_MAN(b);
            check(vd_manifest_parse(mp, &m, err, sizeof err) != 0, "manifest: missing key refused");
        }
        {   /* whitespace smuggling */
            char b[8192]; snprintf(b, sizeof b, " %s", base);
            WRITE_MAN(b);
            check(vd_manifest_parse(mp, &m, err, sizeof err) != 0, "manifest: leading space refused");
        }
        {   /* tab-separated */
            char b[8192]; snprintf(b, sizeof b, "%s", base);
            { char *t = strstr(b, "seed 20260727"); if (t) t[4] = '\t'; }
            WRITE_MAN(b);
            check(vd_manifest_parse(mp, &m, err, sizeof err) != 0, "manifest: tab separator refused");
        }
        {   /* trailing bytes without newline */
            char b[8192]; snprintf(b, sizeof b, "%strailing", base);
            WRITE_MAN(b);
            check(vd_manifest_parse(mp, &m, err, sizeof err) != 0, "manifest: trailing bytes refused");
        }
        {   /* malformed number */
            char b[8192]; snprintf(b, sizeof b, "%s", base);
            { char *t = strstr(b, "seed 20260727"); if (t) memcpy(t + 5, "2026072x", 8); }
            WRITE_MAN(b);
            check(vd_manifest_parse(mp, &m, err, sizeof err) != 0, "manifest: non-numeric value refused");
        }
        {   /* overflow */
            char b[8192]; snprintf(b, sizeof b, "%s", base);
            { char *t = strstr(b, "train_prop 10"); if (t) memcpy(t + 11, "99999999999999999999", 20); }
            WRITE_MAN(b);
            check(vd_manifest_parse(mp, &m, err, sizeof err) != 0, "manifest: overflowing number refused");
        }
        {   /* short digest */
            char b[8192]; snprintf(b, sizeof b, "%s", base);
            { char *t = strstr(b, "id_root_test ee"); if (t) memcpy(t + 13, "eeee\n", 5); }
            WRITE_MAN(b);
            check(vd_manifest_parse(mp, &m, err, sizeof err) != 0, "manifest: malformed digest refused");
        }
        {   /* blank line */
            char b[8192]; snprintf(b, sizeof b, "%s\n", base);
            WRITE_MAN(b);
            check(vd_manifest_parse(mp, &m, err, sizeof err) != 0, "manifest: blank line refused");
        }
#undef WRITE_MAN
    }

    /* ---- protocol identity is not negotiable by the cache ---------------- */
    {
        const VdProtocol *p = vd_protocol_get("v2");
        check(p != NULL, "protocol: v2 is registered");
        if (p) {
            check(p->requires_prev == 1,
                  "protocol: v2 requires the spent-holdout check unconditionally");
            check(p->test_offset == 1000 && p->test_count == 1000,
                  "protocol: v2 holdout slice is pinned at [1000,2000)");
            check(p->seed == 20260727 && !strcmp(p->cls, "car"),
                  "protocol: v2 seed and class are pinned");
            check(p->train_img == 4042 && p->val_img == 969,
                  "protocol: v2 train/val counts are pinned");
            check(p->pca_fit_images == 600 && p->pca_fit_rows == 162983,
                  "protocol: v2 PCA-fit provenance is pinned");
            check(p->hog_side == 64 && p->color == 1 && p->hog_dim == 1764 && p->pca_dim == 256,
                  "protocol: v2 descriptor config is pinned");
            check(p->ss_width == 300 && p->max_prop == 300 && p->min_side == 16,
                  "protocol: v2 proposal parameters are pinned");
            check(p->prev_test_count == 1000 && strlen(p->prev_pack_sha) == 64,
                  "protocol: v2 previous-holdout count and digest are pinned");
        }
        check(vd_protocol_get("v0") == NULL, "protocol: unknown name is refused");
    }

    /* A manifest that self-declares a weaker protocol must not satisfy v2.
       This is Codex's bypass expressed at the unit level. */
    {
        const VdProtocol *p = vd_protocol_get("v2");
        VdManifest m;
        char err[256];
        memset(&m, 0, sizeof m);
        m.manifest_version = 1;
        snprintf(m.variant, sizeof m.variant, "v2");
        snprintf(m.dataset, sizeof m.dataset, "PASCAL_VOC_2007");
        snprintf(m.cls, sizeof m.cls, "car");
        snprintf(m.split_key, sizeof m.split_key, "sha256_content_hash_trainval");
        m.seed = 20260727;
        m.hog_side = 64; m.color = 1; m.hog_dim = 1764; m.pca_dim = 256;
        m.ss_width = 300; m.max_prop = 300; m.min_side = 16;
        m.nms_iou_x100 = 30; m.match_iou_x100 = 50;
        m.test_offset = 0;            /* <-- the bypass */
        m.test_count = 1000; m.test_img = 1000;
        m.train_img = 4042; m.val_img = 969;
        m.pca_fit_images = 600; m.pca_fit_rows = 162983;
        check(vd_manifest_check(&m, p, err, sizeof err) != 0,
              "protocol: cache declaring test_offset 0 cannot satisfy v2");
        m.test_offset = 1000;
        m.cls[0] = 'b';
        check(vd_manifest_check(&m, p, err, sizeof err) != 0, "protocol: wrong class refused");
        snprintf(m.cls, sizeof m.cls, "car");
        m.seed = 1;
        check(vd_manifest_check(&m, p, err, sizeof err) != 0, "protocol: wrong seed refused");
        m.seed = 20260727; m.pca_fit_rows = 5;
        check(vd_manifest_check(&m, p, err, sizeof err) != 0,
              "protocol: wrong PCA-fit provenance refused");
        m.pca_fit_rows = 162983; m.train_img = 10;
        check(vd_manifest_check(&m, p, err, sizeof err) != 0, "protocol: wrong train count refused");
        m.train_img = 4042; m.trainval_id_overlap = 1;
        check(vd_manifest_check(&m, p, err, sizeof err) != 0,
              "protocol: nonzero declared leakage refused");
    }

    /* ---- directory-atomic cache publication ------------------------------
       A cache is only meaningful whole, and a planted symlink at any member
       path must never be written through. */
    {
        char dest[600], ext[600], link[700], probe[700];
        struct stat sb;
        VdStage st;
        VdOut o;
        FILE *f;

        snprintf(dest, sizeof dest, "%s/cachepub", DIR);
        check(vd_stage_begin(dest, &st) == 0, "stage: begins on a clean destination");
        check(write_full_cache(&st) == 0, "stage: creates a complete cache");
        check(vd_out_open(&st, "a.pack", &o) == 0, "stage: creates a member");
        check(vd_out_write(&o, "hello", 5) == 0, "stage: writes a member");
        check(vd_out_finish(&o) == 0, "stage: finishes a member");
        check(stat(dest, &sb) != 0, "stage: destination is invisible before commit");
        check(vd_stage_commit(&st) == 0, "stage: commits");
        check(stat(dest, &sb) == 0 && S_ISDIR(sb.st_mode), "stage: destination appears whole");

        /* publishing again over the same destination is always refused */
        {
            struct stat b1, b2;
            check(stat(dest, &b1) == 0, "stage: existing destination stat");
            check(vd_stage_begin(dest, &st) == 0, "stage: begins again");
            check(write_full_cache(&st) == 0, "stage: second staging is complete");
            check(vd_stage_commit(&st) != 0, "stage: refuses to publish over an existing cache");
            check(stat(dest, &b2) == 0 && b1.st_dev == b2.st_dev && b1.st_ino == b2.st_ino,
                  "stage: the existing cache keeps its exact device+inode");
        }

        /* incomplete staging must leave nothing behind */
        snprintf(ext, sizeof ext, "%s/aborted", DIR);
        check(vd_stage_begin(ext, &st) == 0, "stage: begins for abort case");
        (void)write_full_cache(&st);
        check(vd_out_open(&st, "partial.pack", &o) == 0 && vd_out_write(&o, "zz", 2) == 0
              && vd_out_finish(&o) == 0, "stage: writes into aborted staging");
        vd_stage_abort(&st);
        check(stat(ext, &sb) != 0, "stage: aborted staging never becomes visible");

        /* a symlinked destination is refused outright */
        snprintf(probe, sizeof probe, "%s/external_target", DIR);
        if (vd_mkdir_p(probe) == 0) {
            snprintf(link, sizeof link, "%s/linked_cache", DIR);
            (void)unlink(link);
            if (symlink(probe, link) == 0) {
                check(vd_stage_begin(link, &st) != 0, "stage: symlinked destination refused");
            } else check(1, "stage: symlink destination case skipped");
        }
        /* a symlinked PARENT is refused */
        {
            char linkp[700], inner[900];
            snprintf(linkp, sizeof linkp, "%s/linked_parent", DIR);
            (void)unlink(linkp);
            if (symlink(probe, linkp) == 0) {
                snprintf(inner, sizeof inner, "%s/child", linkp);
                check(vd_stage_begin(inner, &st) != 0, "stage: symlinked parent refused");
            } else check(1, "stage: symlink parent case skipped");
        }
        /* a planted member symlink must not be written through */
        {
            char target[700];
            char staged[VD_PATH_MAX + 128];   /* st.stage is VD_PATH_MAX */
            snprintf(target, sizeof target, "%s/never_touch", DIR);
            f = fopen(target, "wb"); if (f) { fputs("ORIGINAL", f); fclose(f); }
            snprintf(dest, sizeof dest, "%s/plantcache", DIR);
            if (vd_stage_begin(dest, &st) == 0) {
                int sn = snprintf(staged, sizeof staged, "%s/%s/c.pack", DIR, st.stage);
                if (sn > 0 && (size_t)sn < sizeof staged && symlink(target, staged) == 0) {
                    check(vd_out_open(&st, "c.pack", &o) != 0,
                          "stage: refuses to open through a planted member symlink");
                } else check(1, "stage: planted member case skipped");
                vd_stage_abort(&st);
            }
            {   /* the external file must be byte-identical */
                char rb[32] = {0};
                size_t got = 0;
                f = fopen(target, "rb");
                if (f) { got = fread(rb, 1, sizeof rb - 1, f); fclose(f); }
                check(got == 8 && !memcmp(rb, "ORIGINAL", 8),
                      "stage: external symlink target is unmodified");
            }
        }
    }

    /* ---- artifact root binds bytes, names, sizes and order --------------- */
    {
        VdMember m[VD_N_MEMBERS], m2[VD_N_MEMBERS];
        char r1[65], r2[65];
        int k;
        for (k = 0; k < VD_N_MEMBERS; k++) {
            m[k].name = VD_MEMBERS[k];
            m[k].size = 100 + k;
            memset(m[k].sha, 'a', 64); m[k].sha[63] = (char)('0' + k); m[k].sha[64] = 0;
        }
        check(vd_artifact_root(m, VD_N_MEMBERS, 1, r1) == 0, "artifact: root computes");
        memcpy(m2, m, sizeof m);
        m2[0].sha[0] = 'b';
        check(vd_artifact_root(m2, VD_N_MEMBERS, 1, r2) == 0 && strcmp(r1, r2) != 0,
              "artifact: changed member bytes change the root");
        memcpy(m2, m, sizeof m);
        m2[2].size += 1;
        check(vd_artifact_root(m2, VD_N_MEMBERS, 1, r2) == 0 && strcmp(r1, r2) != 0,
              "artifact: changed member size changes the root");
        memcpy(m2, m, sizeof m);
        check(vd_artifact_root(m2, VD_N_MEMBERS, 2, r2) == 0 && strcmp(r1, r2) != 0,
              "artifact: changed schema changes the root");
        memcpy(m2, m, sizeof m);
        { VdMember t = m2[0]; m2[0] = m2[1]; m2[1] = t; }
        check(vd_artifact_root(m2, VD_N_MEMBERS, 1, r2) != 0,
              "artifact: reordered members are refused, not silently hashed");
        memcpy(m2, m, sizeof m);
        m2[4].name = "not_a_member.txt";
        check(vd_artifact_root(m2, VD_N_MEMBERS, 1, r2) != 0,
              "artifact: renamed member is refused");
        check(vd_artifact_root(m, VD_N_MEMBERS - 1, 1, r2) != 0,
              "artifact: wrong member count is refused");
        {   /* the pinned v2 root must actually be pinned */
            const VdProtocol *pv = vd_protocol_get("v2");
            check(pv && pv->artifact_root && strlen(pv->artifact_root) == 64,
                  "artifact: v2 protocol carries a pinned artifact root");
        }
    }

    /* ---- component-wise no-follow traversal ------------------------------ */
    {
        char base[600], real[700], link[700], via[900];
        int fd;
        snprintf(base, sizeof base, "%s/trav", DIR);
        snprintf(real, sizeof real, "%s/trav/a/b", DIR);
        check(vd_mkdir_p_nofollow(real) == 0, "traverse: creates a nested path");
        fd = vd_open_dir_nofollow(real);
        check(fd >= 0, "traverse: opens a clean nested path");
        if (fd >= 0) close(fd);
        /* symlink at an INTERMEDIATE component, not the leaf */
        snprintf(link, sizeof link, "%s/trav/a_link", DIR);
        (void)unlink(link);
        if (symlink("a", link) == 0) {
            snprintf(via, sizeof via, "%s/trav/a_link/b", DIR);
            check(vd_open_dir_nofollow(via) < 0,
                  "traverse: symlink at an intermediate component refused");
            check(vd_mkdir_p_nofollow(via) != 0,
                  "traverse: mkdir through an intermediate symlink refused");
        } else check(1, "traverse: intermediate symlink case skipped");
        /* symlink at the FIRST component */
        {
            char l2[700], v2[900];
            snprintf(l2, sizeof l2, "%s/trav_link", DIR);
            (void)unlink(l2);
            if (symlink("trav", l2) == 0) {
                snprintf(v2, sizeof v2, "%s/trav_link/a", DIR);
                check(vd_open_dir_nofollow(v2) < 0,
                      "traverse: symlink at the first component refused");
            } else check(1, "traverse: first-component symlink case skipped");
        }
        /* a non-directory in the middle */
        {
            char fp[700], v3[900];
            FILE *g;
            snprintf(fp, sizeof fp, "%s/trav/plain", DIR);
            g = fopen(fp, "wb"); if (g) { fputc('x', g); fclose(g); }
            snprintf(v3, sizeof v3, "%s/trav/plain/x", DIR);
            check(vd_open_dir_nofollow(v3) < 0, "traverse: non-directory component refused");
        }
        {
            char up[700];
            snprintf(up, sizeof up, "%s/trav/../trav", DIR);
            check(vd_open_dir_nofollow(up) < 0, "traverse: .. component refused");
        }
    }

    /* ---- publication is fresh-only, and never destroys anything ----------- */
    {
        char dest[600], decoy[600];
        VdStage st;
        struct stat before, after, dbefore, dafter;
        FILE *g;

        /* an existing cache destination is a hard refusal, byte- and
           inode-identical afterwards */
        snprintf(dest, sizeof dest, "%s/fresh", DIR);
        check(vd_stage_begin(dest, &st) == 0 && write_full_cache(&st) == 0
              && vd_stage_commit(&st) == 0, "fresh: publishes into an absent destination");
        check(stat(dest, &before) == 0 && S_ISDIR(before.st_mode),
              "fresh: destination exists after publish");
        check(vd_stage_begin(dest, &st) == 0, "fresh: staging begins over an existing cache");
        check(write_full_cache(&st) == 0, "fresh: staging is complete");
        check(vd_stage_commit(&st) != 0, "fresh: existing destination is refused");
        check(stat(dest, &after) == 0 && after.st_dev == before.st_dev
              && after.st_ino == before.st_ino,
              "fresh: existing cache keeps its exact device+inode");

        /* an arbitrary directory is equally safe */
        snprintf(decoy, sizeof decoy, "%s/arbitrary", DIR);
        check(vd_mkdir_p_nofollow(decoy) == 0, "fresh: creates an arbitrary directory");
        {
            char fp[800];
            snprintf(fp, sizeof fp, "%s/precious.txt", decoy);
            g = fopen(fp, "wb"); if (g) { fputs("KEEP", g); fclose(g); }
        }
        check(stat(decoy, &dbefore) == 0, "fresh: arbitrary directory stat");
        check(vd_stage_begin(decoy, &st) == 0 && write_full_cache(&st) == 0
              && vd_stage_commit(&st) != 0,
              "fresh: arbitrary existing directory is refused");
        check(stat(decoy, &dafter) == 0 && dafter.st_dev == dbefore.st_dev
              && dafter.st_ino == dbefore.st_ino,
              "fresh: arbitrary directory keeps its exact device+inode");
        {
            char fp[800], rb[16] = {0};
            size_t got = 0;
            snprintf(fp, sizeof fp, "%s/precious.txt", decoy);
            g = fopen(fp, "rb");
            if (g) { got = fread(rb, 1, sizeof rb - 1, g); fclose(g); }
            check(got == 4 && !memcmp(rb, "KEEP", 4),
                  "fresh: arbitrary directory contents are byte-identical");
        }
        /* a plain file destination */
        {
            char fdst[600];
            struct stat fb, fa;
            snprintf(fdst, sizeof fdst, "%s/plainfile_dest2", DIR);
            g = fopen(fdst, "wb"); if (g) { fputs("F", g); fclose(g); }
            check(stat(fdst, &fb) == 0, "fresh: plain-file destination stat");
            if (vd_stage_begin(fdst, &st) == 0) {
                check(vd_stage_commit(&st) != 0, "fresh: plain-file destination refused");
            } else check(1, "fresh: plain-file destination refused at begin");
            check(stat(fdst, &fa) == 0 && fa.st_ino == fb.st_ino && S_ISREG(fa.st_mode),
                  "fresh: plain-file destination keeps its exact inode");
        }
        /* a deterministic change at the pre-publication hook must not destroy
           any unrelated inode */
        {
            char h1[600];
            struct stat hb, ha, db2;
            snprintf(h1, sizeof h1, "%s/hooked", DIR);
            check(vd_stage_begin(h1, &st) == 0 && write_full_cache(&st) == 0
                  && vd_stage_commit(&st) == 0, "hook: publishes a cache to swap in");
            check(stat(h1, &hb) == 0, "hook: baseline stat");
            g_hook_swap_from = decoy;
            g_hook_swap_to = h1;
            g_hook_phase_to_fire = VD_HOOK_BEFORE_PUBLISH;
            vd_stage_set_hook(swap_hook, NULL);
            {
                char h2[600];
                snprintf(h2, sizeof h2, "%s/hooked_target", DIR);
                check(vd_stage_begin(h2, &st) == 0 && write_full_cache(&st) == 0,
                      "hook: staging for the hooked publish");
                (void)vd_stage_commit(&st);
            }
            vd_stage_set_hook(NULL, NULL);
            check(stat(h1, &ha) == 0 && stat(decoy, &db2) == 0,
                  "hook: both directories still exist");
            check((ha.st_ino == hb.st_ino) || (db2.st_ino == hb.st_ino),
                  "hook: the original inode was moved, never deleted");
        }
    }

    printf("checks=%d failures=%d\n", checks, failures);
    {   /* best-effort cleanup; never fatal */
        char cmd[700];
        snprintf(cmd, sizeof cmd, "%s/t.pack", DIR); unlink(cmd);
    }
    if (failures == 0) {
        printf("VISION_DETECTION_INTEGRITY_PASS checks=%d\n", checks);
        return 0;
    }
    printf("VISION_DETECTION_INTEGRITY_FAIL failures=%d\n", failures);
    return 1;
}
