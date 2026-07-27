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
#include "../tools/vision_detection/vd_sha256.h"

static int failures, checks;
static char DIR[512];

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
