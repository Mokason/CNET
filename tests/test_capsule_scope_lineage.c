/* test_capsule_scope_lineage — a specialist and its boundary travel together,
 * and its lineage survives the trip.
 *
 * WHY THIS EXISTS. Three invariants the capsule claimed but did not enforce:
 *
 *  1. SCOPE. Coverage was optional and `coverage 0 0 0` was written whenever a
 *     caller passed no HybridAi. The header said `cov == NULL` was correct only
 *     for a whole-domain unit; nothing proved whole-domain. So a unit certified
 *     on a SAMPLE could be exported, imported, re-certified against that same
 *     sample, and then answer anywhere.
 *  2. LINEAGE. Import verified the payload's provenance against the manifest and
 *     then dropped it — `cnb_add_unit` zero-initialises the new unit ref — while
 *     the success report went on repeating the manifest's provenance. Callers
 *     got a report claiming lineage the destination did not carry.
 *  3. LEAST DISCLOSURE. `cnb_export_subset` copied EVERY oracle descriptor
 *     before filtering units, so a one-unit capsule disclosed the whole source
 *     registry.
 *
 * Plus the export/import parity bug: export accepted a zero-length asset that
 * import rejects.
 *
 * Manifest checksums here are recomputed after tampering on purpose. FNV is
 * unkeyed and the header says so: it detects accident, not authorship. These
 * cases prove the SEMANTIC checks refuse a well-formed lie, which is the only
 * thing that could have caught the defects above.
 *
 * Everything is written under a mkdtemp root. No real base, no real capsule.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../include/base.h"
#include "../include/cnet_capsule.h"
#include "../include/contract/contract.h"
#include "../include/hybrid_ai.h"
#include "../include/nn.h"

#define SYM 4
#define WIDE 8

static int failures;
static int checks;

static void check(int condition, const char *message) {
    checks++;
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
}

static char dir_template[] = "/tmp/cnet-capscope-XXXXXX";
static char *scratch;

static Port make_port(PortFamily family, size_t width, const char *tag) {
    Port p;
    memset(&p, 0, sizeof p);
    p.family = family;
    p.field_width = width;
    p.field_count = 1;
    snprintf(p.tag, sizeof p.tag, "%s", tag);
    return p;
}

static void one_hot(double *row, size_t width, size_t hot) {
    size_t i;
    for (i = 0; i < width; i++) row[i] = (i == hot) ? 1.0 : 0.0;
}

/* Build a unit whose exemplars cover `rows` of a `width`-point one-hot domain.
   rows == width is exhaustive; rows < width is a sample. */
static int build_unit(CnetBase *base, HybridAi *cov, const char *name,
                      const char *in_tag, const char *out_tag, size_t width,
                      size_t rows) {
    BinaryTransformNetwork btn;
    Contract contract;
    Port pin = make_port(PORT_ONEHOT, width, in_tag);
    Port pout = make_port(PORT_ONEHOT, width, out_tag);
    double *in = (double *)calloc(rows * width, sizeof(double));
    double *target = (double *)calloc(rows * width, sizeof(double));
    size_t i;
    int rc = -1;

    if (!in || !target) goto out;
    for (i = 0; i < rows; i++) {
        one_hot(in + i * width, width, i);
        one_hot(target + i * width, width, (i + 1) % width);
    }
    memset(&btn, 0, sizeof btn);
    memset(&contract, 0, sizeof contract);
    if (btn_init(&btn, width, width, 16, 64, 0.5, 20260730u) != 0) goto out;
    btn_set_ports(&btn, pin, pout);
    btn_train_dynamic(&btn, in, target, rows, 12000, 200, 1e-6, 1e-8);
    btn_train(&btn, in, target, rows, 3000);
    if (contract_init_borrowed(&contract, name, &btn, in, target, rows) != 0) {
        btn_free(&btn);
        goto out;
    }
    if (cnb_add_unit(base, &btn, &contract, NULL) == 0) {
        rc = 0;
        if (cov)
            (void)hybrid_coverage_record(cov, pin, pout, name, in, target, rows,
                                         width, width);
    }
    contract_free(&contract);
    btn_free(&btn);
out:
    free(in);
    free(target);
    return rc;
}

/* ---- manifest tampering -------------------------------------------------- */

static unsigned long long fnv(const unsigned char *p, size_t n) {
    unsigned long long h = 14695981039346656037ULL;
    size_t i;
    for (i = 0; i < n; i++) {
        h ^= (unsigned long long)p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

/* Copy dir/manifest.cknow with `from` replaced by `to`, then re-stamp
   manifest_fnv so only the SEMANTIC checks can refuse the result. */
static int retamper(const char *dir, const char *from, const char *to) {
    char path[600];
    char *text = NULL, *at, *tail;
    long size;
    FILE *fp;
    size_t head_len;

    snprintf(path, sizeof path, "%s/manifest.cknow", dir);
    fp = fopen(path, "rb");
    if (!fp) return -1;
    fseek(fp, 0, SEEK_END);
    size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (size <= 0) { fclose(fp); return -1; }
    text = (char *)malloc((size_t)size + strlen(to) + 64);
    if (!text) { fclose(fp); return -1; }
    if (fread(text, 1, (size_t)size, fp) != (size_t)size) {
        fclose(fp);
        free(text);
        return -1;
    }
    fclose(fp);
    text[size] = '\0';

    at = strstr(text, from);
    if (!at) { free(text); return -1; }
    {
        size_t from_len = strlen(from), to_len = strlen(to);
        size_t rest = strlen(at + from_len);
        memmove(at + to_len, at + from_len, rest + 1);
        memcpy(at, to, to_len);
    }

    tail = strstr(text, "\nmanifest_fnv ");
    if (!tail) { free(text); return -1; }
    head_len = (size_t)(tail - text) + 1;
    fp = fopen(path, "wb");
    if (!fp) { free(text); return -1; }
    fwrite(text, 1, head_len, fp);
    fprintf(fp, "manifest_fnv %llu\n",
            fnv((const unsigned char *)text, head_len));
    fclose(fp);
    free(text);
    return 0;
}

/* FNV-1a over a whole file: byte identity of the saved destination is a
   stronger statement than "the counts match". */
static unsigned long long file_fnv(const char *path) {
    FILE *fp = fopen(path, "rb");
    unsigned long long h = 14695981039346656037ULL;
    int ch;
    if (!fp) return 0;
    while ((ch = fgetc(fp)) != EOF) {
        h ^= (unsigned long long)(unsigned char)ch;
        h *= 1099511628211ULL;
    }
    fclose(fp);
    return h;
}

/* ---- destination non-mutation ------------------------------------------- */

typedef struct {
    size_t units;
    size_t oracles;
    size_t coverage;
} DstShape;

static DstShape shape_of(const CnetBase *b, const HybridAi *cov) {
    DstShape s;
    s.units = b->unit_count;
    s.oracles = b->oracle_count;
    s.coverage = hybrid_coverage_count(cov);
    return s;
}

static void check_unchanged(DstShape before, DstShape after, const char *what) {
    char message[256];
    snprintf(message, sizeof message, "%s: destination is not mutated", what);
    check(before.units == after.units && before.oracles == after.oracles &&
              before.coverage == after.coverage,
          message);
}

int main(void) {
    CnetBase src, dst;
    HybridAi src_cov, dst_cov;
    CnetCapsuleReport rep;
    char exh_dir[600], samp_dir[600], nocov_dir[600], asset_dir[600];
    Port oracle_in = make_port(PORT_ONEHOT, SYM, "capscope_source");
    Port oracle_out = make_port(PORT_ONEHOT, SYM, "capscope_answer");
    DstShape before;
    int rc;

    /* stdout is a pipe under make; unbuffer it so a crash cannot swallow the
       progress that says which case died. */
    setvbuf(stdout, NULL, _IONBF, 0);
    scratch = mkdtemp(dir_template);
    if (!scratch) {
        fprintf(stderr, "FAIL: cannot create scratch directory\n");
        return 1;
    }
    snprintf(exh_dir, sizeof exh_dir, "%s/exhaustive", scratch);
    snprintf(samp_dir, sizeof samp_dir, "%s/sampled", scratch);
    snprintf(nocov_dir, sizeof nocov_dir, "%s/sampled_nocov", scratch);
    snprintf(asset_dir, sizeof asset_dir, "%s/asset", scratch);

    cnb_init(&src);
    cnb_init(&dst);
    hybrid_ai_init(&src_cov);
    hybrid_ai_init(&dst_cov);

    /* Two oracle descriptors: one the exported unit references, one it does
       not. The second must never reach a destination. */
    check(cnb_add_oracle_desc(&src, "capscope_teacher", "builtin", oracle_in,
                              oracle_out) == 0,
          "source registers the referenced oracle descriptor");
    check(cnb_add_oracle_desc(&src, "capscope_unrelated", "builtin", oracle_in,
                              oracle_out) == 0,
          "source registers an unrelated oracle descriptor");

    check(build_unit(&src, NULL, "capscope_whole", "capscope_wholein",
                     "capscope_wholeout", SYM, SYM) == 0,
          "an exhaustively certified unit is built");
    check(cnb_set_unit_provenance(&src, "capscope_whole",
                                  "capscope_teacher") == 0,
          "the exhaustive unit records its provenance");
    check(build_unit(&src, &src_cov, "capscope_part", "capscope_partin",
                     "capscope_partout", WIDE, SYM) == 0,
          "a sampled unit is built (4 of 8 domain points)");

    /* --- 1. exhaustive scope may ship without coverage -------------------- */
    rc = cnet_capsule_export(&src, NULL, "capscope_whole", exh_dir, &rep);
    check(rc == 0, "an exhaustive unit exports without coverage");
    check(rep.reject_reason[0] == '\0',
          "the exhaustive export reports no rejection");

    before = shape_of(&dst, &dst_cov);
    rc = cnet_capsule_import(&dst, &dst_cov, exh_dir, &rep);
    check(rc == 0, "the exhaustive capsule imports");
#ifdef CNET_CAPSULE_REPORT_HAS_SCOPE
    check(strcmp(rep.scope, "exhaustive") == 0,
          "the import reports a verified exhaustive scope");
#endif

    /* --- 2. lineage is restored, not just reported ------------------------ */
    {
        const char *got = cnb_unit_provenance(&dst, "capscope_whole");
        check(got != NULL && strcmp(got, "capscope_teacher") == 0,
              "the destination unit carries the verified provenance");
        check(strcmp(rep.provenance, "capscope_teacher") == 0,
              "the report and the destination agree on provenance");
    }

    /* --- 3. least disclosure ----------------------------------------------
       Checked in the PAYLOAD as well as the destination: the leak was in
       cnb_export_subset, which copied every oracle descriptor before filtering
       units, so the shipped unit.cnb carried the whole source registry even
       when the destination never saw it. */
    {
        CnetBase payload;
        char payload_path[700];
        size_t i, leaked = 0, carried = 0;
        snprintf(payload_path, sizeof payload_path, "%s/unit.cnb", exh_dir);
        cnb_init(&payload);
        if (cnb_load(&payload, payload_path) != 0) {
            check(0, "the exported payload is loadable");
        } else {
            for (i = 0; i < payload.oracle_count; i++) {
                if (strcmp(payload.oracles[i].name, "capscope_unrelated") == 0)
                    leaked++;
                if (strcmp(payload.oracles[i].name, "capscope_teacher") == 0)
                    carried++;
            }
            check(carried == 1,
                  "the payload carries the referenced oracle descriptor");
            check(leaked == 0,
                  "the payload does not disclose an unreferenced descriptor");
        }
        cnb_free(&payload);
    }
    {
        size_t i, unrelated = 0, referenced = 0;
        for (i = 0; i < dst.oracle_count; i++) {
            if (strcmp(dst.oracles[i].name, "capscope_unrelated") == 0)
                unrelated++;
            if (strcmp(dst.oracles[i].name, "capscope_teacher") == 0)
                referenced++;
        }
        check(referenced == 1,
              "the referenced oracle descriptor travels with the unit");
        check(unrelated == 0,
              "an unreferenced oracle descriptor does not travel");
    }
    (void)before;

    /* --- 4. sampled scope MUST carry coverage ----------------------------- */
    rc = cnet_capsule_export(&src, NULL, "capscope_part", nocov_dir, &rep);
    check(rc != 0, "a sampled unit refuses to export without coverage");
    check(strcmp(rep.reject_reason, "sampled_scope_requires_coverage") == 0,
          "the refusal names the missing boundary");
    {
        char path[700];
        FILE *fp;
        snprintf(path, sizeof path, "%s/manifest.cknow", nocov_dir);
        fp = fopen(path, "rb");
        check(fp == NULL, "a refused export publishes no manifest");
        if (fp) fclose(fp);
    }

    rc = cnet_capsule_export(&src, &src_cov, "capscope_part", samp_dir, &rep);
    check(rc == 0, "a sampled unit exports with its coverage");

    before = shape_of(&dst, &dst_cov);
    rc = cnet_capsule_import(&dst, &dst_cov, samp_dir, &rep);
    check(rc == 0, "the sampled capsule imports");
#ifdef CNET_CAPSULE_REPORT_HAS_SCOPE
    check(strcmp(rep.scope, "sampled") == 0,
          "the import reports a sampled scope");
#endif
    check(rep.coverage_rows == SYM, "the sampled capsule carried its rows");

    /* --- 5. a well-formed lie about scope is refused ----------------------
       Guarded on the feature macro so this same file compiles against a build
       that predates the scope field: that build is the RED run, and there is no
       scope line in its manifests to tamper with. */
#ifdef CNET_CAPSULE_REPORT_HAS_SCOPE
    {
        char lie_dir[600];
        char command[1400];
        DstShape s0, s1;
        snprintf(lie_dir, sizeof lie_dir, "%s/lie_exhaustive", scratch);
        snprintf(command, sizeof command, "cp -r '%s' '%s'", samp_dir, lie_dir);
        if (system(command) != 0) {
            check(0, "scope-lie fixture could not be copied");
        } else if (retamper(lie_dir, "scope sampled", "scope exhaust") != 0) {
            check(0, "scope-lie fixture could not be tampered");
        } else {
            /* "scope exhaust" is not a known word: unscoped is refused. */
            s0 = shape_of(&dst, &dst_cov);
            rc = cnet_capsule_import(&dst, &dst_cov, lie_dir, &rep);
            s1 = shape_of(&dst, &dst_cov);
            check(rc != 0, "an unknown scope word is refused");
            check(strcmp(rep.reject_reason, "unknown_certification_scope") == 0,
                  "the refusal names the unknown scope");
            check_unchanged(s0, s1, "unknown scope");

            /* Now the real lie: claim exhaustive over a sampled payload. */
            if (retamper(lie_dir, "scope exhaust", "scope exhaustive") != 0) {
                check(0, "exhaustive-claim fixture could not be tampered");
            } else {
                s0 = shape_of(&dst, &dst_cov);
                rc = cnet_capsule_import(&dst, &dst_cov, lie_dir, &rep);
                s1 = shape_of(&dst, &dst_cov);
                check(rc != 0,
                      "an exhaustive claim over a sampled payload is refused");
                check(strcmp(rep.reject_reason,
                             "exhaustive_scope_claim_unproven") == 0,
                      "the refusal names the unproven claim");
                check_unchanged(s0, s1, "unproven exhaustive claim");
            }
        }
    }

#endif /* CNET_CAPSULE_REPORT_HAS_SCOPE */

    /* --- 6. sampled scope with the coverage stripped ---------------------- */
#ifdef CNET_CAPSULE_REPORT_HAS_SCOPE
    {
        char strip_dir[600];
        char command[1400];
        DstShape s0, s1;
        snprintf(strip_dir, sizeof strip_dir, "%s/lie_nocov", scratch);
        snprintf(command, sizeof command, "cp -r '%s' '%s'", samp_dir,
                 strip_dir);
        if (system(command) != 0) {
            check(0, "coverage-strip fixture could not be copied");
        } else {
            char want[64];
            snprintf(want, sizeof want, "coverage %d %d %d", SYM, WIDE, WIDE);
            if (retamper(strip_dir, want, "coverage 0 0 0") != 0) {
                check(0, "coverage-strip fixture could not be tampered");
            } else {
                s0 = shape_of(&dst, &dst_cov);
                rc = cnet_capsule_import(&dst, &dst_cov, strip_dir, &rep);
                s1 = shape_of(&dst, &dst_cov);
                check(rc != 0,
                      "a sampled capsule with its coverage stripped is refused");
                check(strcmp(rep.reject_reason,
                             "sampled_scope_without_coverage") == 0,
                      "the refusal names the missing coverage");
                check_unchanged(s0, s1, "stripped coverage");
            }
        }
    }

#endif /* CNET_CAPSULE_REPORT_HAS_SCOPE */

    /* --- 6b. atomic rollback after admission ------------------------------
       include/cnet_capsule.h promises the destination is not mutated when an
       import is refused, but cnb_add_unit has no removal counterpart, so a
       failure after admission left the unit in the base. The window is
       unreachable by construction -- which is not the same as recoverable, so
       CNET_CAPSULE_FAIL_AFTER_ADMIT makes the recovery path executable and this
       asserts BYTE identity of the saved destination across the refusal. */
    {
        CnetBase victim;
        HybridAi victim_cov;
        char before_path[700], after_path[700];
        unsigned long long before_fnv = 0, after_fnv = 0;
        size_t units_before, oracles_before, cov_before;
        int rc_inject;

        snprintf(before_path, sizeof before_path, "%s/rollback_before.cnb",
                 scratch);
        snprintf(after_path, sizeof after_path, "%s/rollback_after.cnb", scratch);

        cnb_init(&victim);
        hybrid_ai_init(&victim_cov);
        /* A destination that already holds something, so the assertion is not
           trivially about an empty base. */
        check(cnb_add_oracle_desc(&victim, "capscope_teacher", "builtin",
                                  oracle_in, oracle_out) == 0,
              "rollback victim registers a descriptor");
        check(build_unit(&victim, &victim_cov, "capscope_resident",
                         "capscope_residin", "capscope_residout", SYM,
                         SYM) == 0,
              "rollback victim holds a resident unit");

        units_before = victim.unit_count;
        oracles_before = victim.oracle_count;
        cov_before = hybrid_coverage_count(&victim_cov);
        check(cnb_save(&victim, before_path) == 0,
              "the destination saves before the refused import");
        before_fnv = file_fnv(before_path);

        setenv("CNET_CAPSULE_FAIL_AFTER_ADMIT", "1", 1);
        rc_inject = cnet_capsule_import(&victim, &victim_cov, exh_dir, &rep);
        unsetenv("CNET_CAPSULE_FAIL_AFTER_ADMIT");

        check(rc_inject != 0, "an injected failure after admission refuses");
        check(strcmp(rep.reject_reason, "injected_failure_after_admit") == 0,
              "the refusal names the injected failure");
        check(victim.unit_count == units_before,
              "the admitted unit is rolled back out of the base");
        check(victim.oracle_count == oracles_before,
              "the restored descriptor is rolled back too");
        check(hybrid_coverage_count(&victim_cov) == cov_before,
              "the stored coverage record is rolled back");
        check(cnb_save(&victim, after_path) == 0,
              "the destination saves after the refused import");
        after_fnv = file_fnv(after_path);
        check(before_fnv == after_fnv,
              "the destination is BYTE-identical across the refused import");

        /* And the same destination must still accept an honest import, so the
           rollback did not leave it subtly broken. */
        check(cnet_capsule_import(&victim, &victim_cov, exh_dir, &rep) == 0,
              "the rolled-back destination still accepts an honest import");
        check(victim.unit_count == units_before + 1,
              "the honest import lands after the rollback");

        hybrid_ai_free(&victim_cov);
        cnb_free(&victim);
    }

    /* --- 7. export/import schema parity ----------------------------------- */
    {
        static const unsigned char nothing[1] = {0};
        rc = cnet_capsule_export_asset(&src, NULL, "capscope_whole", asset_dir,
                                       nothing, 0, 2, &rep);
        check(rc != 0, "a zero-length asset refuses to export");
        check(strcmp(rep.reject_reason,
                     "zero_length_asset_would_not_import") == 0,
              "the refusal names the parity failure");
    }

    hybrid_ai_free(&src_cov);
    hybrid_ai_free(&dst_cov);
    cnb_free(&src);
    cnb_free(&dst);

    if (failures) {
        printf("CAPSULE_SCOPE_LINEAGE_FAIL checks=%d failures=%d\n", checks,
               failures);
        return 1;
    }
    printf("CAPSULE_SCOPE_LINEAGE_PASS checks=%d scope=machine_verified "
           "lineage=restored disclosure=referenced_only rollback=byte_identical\n", checks);
    return 0;
}
