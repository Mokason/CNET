/* Schema-2 capsule: one manifest-bound sidecar asset travelling with the unit.
 *
 * A visual specialist's head is useless without the frontend that produced its
 * features — the PCA basis, the HOG and Selective Search configuration and
 * identity, the class map and the thresholds. Schema 2 carries exactly one
 * extra blob for that, bound the same way unit.cnb already is: declared size
 * and FNV, inside the region the trailing manifest_fnv already covers.
 *
 * These fixtures assert the two properties that make it safe to move:
 *   1. a complete package round-trips and the asset arrives byte-identical;
 *   2. every incomplete, mixed, corrupted or version-mismatched package is
 *      REFUSED, and a refused import leaves the destination base untouched.
 *
 * Same trust boundary as the schema-1 capsule: unkeyed checksums detect
 * ACCIDENT, never a motivated forger.
 *
 * make vision_capsule_asset -> VISION_CAPSULE_ASSET_PASS
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../include/cnet_capsule.h"
#include "../include/base.h"
#include "../include/hybrid_ai.h"
#include "../include/nn.h"
#include "../include/contract/contract.h"

#define SYM 8

static int failures, checks;

static void check(int ok, const char *name) {
    checks++;
    printf("  %-64s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static Port P(const char *tag) {
    Port p;
    memset(&p, 0, sizeof p);
    p.family = PORT_ONEHOT;
    p.field_width = SYM;
    p.field_count = 1;
    snprintf(p.tag, sizeof p.tag, "%s", tag);
    return p;
}

static void oh(double *r, int hot) {
    int i;
    for (i = 0; i < SYM; i++) r[i] = (i == hot) ? 1.0 : 0.0;
}

/* A small trained unit, so the capsule under test is a real certified one. */
static int build_base(CnetBase *b, BinaryTransformNetwork *btn) {
    double x[SYM * SYM], y[SYM * SYM];
    int i;
    Contract c;
    cnb_init(b);
    memset(btn, 0, sizeof *btn);
    if (btn_init(btn, SYM, SYM, 16, 64, 0.5, 7u) != 0) return -1;
    btn_set_ports(btn, P("vca_in"), P("vca_goal"));
    for (i = 0; i < SYM; i++) { oh(x + i * SYM, i); oh(y + i * SYM, (i + 3) % SYM); }
    btn_train_dynamic(btn, x, y, SYM, 20000, 200, 1e-6, 1e-8);
    btn_train(btn, x, y, SYM, 4000);
    memset(&c, 0, sizeof c);
    if (contract_init_borrowed(&c, "vca_unit", btn, x, y, SYM) != 0) return -1;
    if (cnb_add_unit(b, btn, &c, NULL) != 0) { contract_free(&c); return -1; }
    contract_free(&c);
    return 0;
}

static void rm_pack(const char *dir) {
    char p[512];
    snprintf(p, sizeof p, "%s/unit.cnb", dir);          (void)remove(p);
    snprintf(p, sizeof p, "%s/manifest.cknow", dir);    (void)remove(p);
    snprintf(p, sizeof p, "%s/%s", dir, CNET_CAPSULE_ASSET_FILE); (void)remove(p);
    (void)rmdir(dir);
}

static int copy_file(const char *from, const char *to) {
    FILE *a = fopen(from, "rb"), *b;
    char buf[4096];
    size_t n;
    if (!a) return -1;
    b = fopen(to, "wb");
    if (!b) { fclose(a); return -1; }
    while ((n = fread(buf, 1, sizeof buf, a)) > 0)
        if (fwrite(buf, 1, n, b) != n) { fclose(a); fclose(b); return -1; }
    fclose(a);
    return fclose(b);
}

static int clone_pack(const char *src, const char *dst) {
    char p[512], q[512];
    (void)mkdir(dst, 0777);
    snprintf(p, sizeof p, "%s/unit.cnb", src);
    snprintf(q, sizeof q, "%s/unit.cnb", dst);
    if (copy_file(p, q) != 0) return -1;
    snprintf(p, sizeof p, "%s/manifest.cknow", src);
    snprintf(q, sizeof q, "%s/manifest.cknow", dst);
    if (copy_file(p, q) != 0) return -1;
    snprintf(p, sizeof p, "%s/%s", src, CNET_CAPSULE_ASSET_FILE);
    snprintf(q, sizeof q, "%s/%s", dst, CNET_CAPSULE_ASSET_FILE);
    return copy_file(p, q) == 0 ? 0 : 0;   /* absent asset is legitimate */
}

static int truncate_asset(const char *dir, long keep) {
    char p[512];
    int rc;
    snprintf(p, sizeof p, "%s/%s", dir, CNET_CAPSULE_ASSET_FILE);
    rc = truncate(p, keep);
    return rc;
}

static int flip_asset_byte(const char *dir, long off) {
    char p[512];
    FILE *fp;
    snprintf(p, sizeof p, "%s/%s", dir, CNET_CAPSULE_ASSET_FILE);
    fp = fopen(p, "r+b");
    if (!fp) return -1;
    if (fseek(fp, off, SEEK_SET) != 0) { fclose(fp); return -1; }
    fputc(0x5A, fp);
    return fclose(fp);
}

/* A refused import must not leave anything behind in the destination. */
static int dst_is_clean(const CnetBase *dst) { return dst->unit_count == 0; }

int main(void) {
    CnetBase src, dst;
    BinaryTransformNetwork btn;
    CnetCapsuleReport rep;
    unsigned char asset[2048];
    void *got = NULL;
    size_t got_len = 0;
    unsigned got_schema = 0;
    size_t i;
    const char *DIR = "/tmp/cnet_vca_pack";
    const char *DIR2 = "/tmp/cnet_vca_pack2";

    printf("== schema-2 capsule: bound sidecar asset ==\n");
    for (i = 0; i < sizeof asset; i++) asset[i] = (unsigned char)(i * 31u + 7u);

    rm_pack(DIR); rm_pack(DIR2);
    if (build_base(&src, &btn) != 0) { printf("VISION_CAPSULE_ASSET_FAIL setup\n"); return 9; }
    (void)mkdir(DIR, 0777);

    /* ---- export -------------------------------------------------------- */
    check(cnet_capsule_export_asset(&src, NULL, "vca_unit", DIR,
                                    asset, sizeof asset, 7u, &rep) == 0,
          "export: asset-bearing capsule exports");
    check(rep.schema == CNET_CAPSULE_SCHEMA_ASSET, "export: declares schema 2");
    check(rep.asset_bytes == sizeof asset, "export: records asset size");
    check(rep.asset_schema == 7u, "export: records asset schema");
    {
        char p[512];
        struct stat sb;
        snprintf(p, sizeof p, "%s/%s", DIR, CNET_CAPSULE_ASSET_FILE);
        check(stat(p, &sb) == 0 && sb.st_size == (off_t)sizeof asset,
              "export: asset file present at declared size");
    }

    /* ---- import: the asset must arrive byte-identical -------------------- */
    cnb_init(&dst);
    check(cnet_capsule_import_asset(&dst, NULL, DIR, &got, &got_len, &got_schema, &rep) == 0,
          "import: asset-bearing capsule imports");
    check(got != NULL && got_len == sizeof asset, "import: asset length preserved");
    check(got && memcmp(got, asset, sizeof asset) == 0, "import: asset bytes identical");
    check(got_schema == 7u, "import: asset schema preserved");
    check(cnb_has_unit(&dst, "vca_unit"), "import: unit materialised");
    free(got); got = NULL; got_len = 0;
    cnb_free(&dst);

    /* ---- a schema-1 caller must be refused, not silently given the head -- */
    cnb_init(&dst);
    check(cnet_capsule_import(&dst, NULL, DIR, &rep) != 0,
          "refuse: schema-1 import refuses an asset-bearing capsule");
    check(strcmp(rep.reject_reason, "asset_capsule_needs_asset_aware_import") == 0,
          "refuse: reason names the asset-aware requirement");
    check(dst_is_clean(&dst), "refuse: destination base unmutated");
    cnb_free(&dst);

    /* ---- missing asset file --------------------------------------------- */
    check(clone_pack(DIR, DIR2) == 0, "setup: package cloned");
    {
        char p[512];
        snprintf(p, sizeof p, "%s/%s", DIR2, CNET_CAPSULE_ASSET_FILE);
        (void)remove(p);
    }
    cnb_init(&dst);
    check(cnet_capsule_import_asset(&dst, NULL, DIR2, &got, &got_len, &got_schema, &rep) != 0,
          "refuse: missing asset file");
    check(strcmp(rep.reject_reason, "asset_missing_or_symlink") == 0,
          "refuse: reason names the missing asset");
    check(dst_is_clean(&dst), "refuse: destination unmutated (missing asset)");
    check(got == NULL, "refuse: no asset handed to the caller");
    cnb_free(&dst); rm_pack(DIR2);

    /* ---- truncated asset ------------------------------------------------ */
    check(clone_pack(DIR, DIR2) == 0, "setup: package cloned for truncation");
    check(truncate_asset(DIR2, (long)sizeof asset - 16) == 0, "setup: asset truncated");
    cnb_init(&dst);
    check(cnet_capsule_import_asset(&dst, NULL, DIR2, &got, &got_len, &got_schema, &rep) != 0,
          "refuse: truncated asset");
    check(strcmp(rep.reject_reason, "asset_integrity_mismatch") == 0,
          "refuse: reason names asset integrity");
    check(dst_is_clean(&dst), "refuse: destination unmutated (truncated asset)");
    cnb_free(&dst); rm_pack(DIR2);

    /* ---- corrupted asset ------------------------------------------------ */
    check(clone_pack(DIR, DIR2) == 0, "setup: package cloned for corruption");
    check(flip_asset_byte(DIR2, 100) == 0, "setup: asset byte flipped");
    cnb_init(&dst);
    check(cnet_capsule_import_asset(&dst, NULL, DIR2, &got, &got_len, &got_schema, &rep) != 0,
          "refuse: corrupted asset");
    check(dst_is_clean(&dst), "refuse: destination unmutated (corrupt asset)");
    cnb_free(&dst); rm_pack(DIR2);

    /* ---- mixed package: asset from a DIFFERENT export -------------------- */
    {
        unsigned char other[2048];
        char p[512], q[512];
        for (i = 0; i < sizeof other; i++) other[i] = (unsigned char)(i * 17u + 3u);
        (void)mkdir(DIR2, 0777);
        check(cnet_capsule_export_asset(&src, NULL, "vca_unit", DIR2,
                                        other, sizeof other, 7u, &rep) == 0,
              "setup: second export with a different asset");
        /* graft the other export's asset onto the first package's manifest */
        snprintf(p, sizeof p, "%s/%s", DIR2, CNET_CAPSULE_ASSET_FILE);
        snprintf(q, sizeof q, "%s/%s", DIR, CNET_CAPSULE_ASSET_FILE);
        {
            char keep[600];
            snprintf(keep, sizeof keep, "%s.keep", q);
            check(copy_file(q, keep) == 0, "setup: original asset saved");
            check(copy_file(p, q) == 0, "setup: foreign asset grafted");
            cnb_init(&dst);
            check(cnet_capsule_import_asset(&dst, NULL, DIR, &got, &got_len,
                                            &got_schema, &rep) != 0,
                  "refuse: mixed package (asset from another export)");
            check(dst_is_clean(&dst), "refuse: destination unmutated (mixed package)");
            cnb_free(&dst);
            check(copy_file(keep, q) == 0, "teardown: original asset restored");
            (void)remove(keep);
        }
        rm_pack(DIR2);
    }

    /* ---- schema-1 capsules still round-trip unchanged -------------------- */
    {
        (void)mkdir(DIR2, 0777);
        check(cnet_capsule_export(&src, NULL, "vca_unit", DIR2, &rep) == 0,
              "regression: schema-1 export still works");
        check(rep.schema == CNET_CAPSULE_SCHEMA, "regression: still declares schema 1");
        cnb_init(&dst);
        check(cnet_capsule_import(&dst, NULL, DIR2, &rep) == 0,
              "regression: schema-1 import still works");
        check(cnb_has_unit(&dst, "vca_unit"), "regression: unit materialised");
        cnb_free(&dst);
        /* and the asset-aware import accepts a schema-1 capsule with no asset */
        cnb_init(&dst);
        check(cnet_capsule_import_asset(&dst, NULL, DIR2, &got, &got_len,
                                        &got_schema, &rep) == 0,
              "regression: asset-aware import accepts a schema-1 capsule");
        check(got == NULL && got_len == 0, "regression: no asset reported for schema 1");
        cnb_free(&dst);
        rm_pack(DIR2);
    }

    rm_pack(DIR);
    btn_free(&btn);
    cnb_free(&src);

    printf("checks=%d failures=%d\n", checks, failures);
    if (failures == 0) {
        printf("VISION_CAPSULE_ASSET_PASS checks=%d\n", checks);
        return 0;
    }
    printf("VISION_CAPSULE_ASSET_FAIL failures=%d\n", failures);
    return 1;
}
