/* Portable isolated-knowledge capsule — export one certified capability from
 * one base and import it into a fresh one, with its gate intact.
 *
 * NOT an MTK/.tskill cartridge. Those (include/cce/cce_mtk.h) are CMSK/MTSK
 * weight deltas patched onto a specific host model's named tensor sites: no
 * typed contract, no certification, no coverage. A capsule is the certified
 * side of CNET, and the two are not interchangeable.
 *
 * TRUST BOUNDARY: a capsule is a LOCAL transfer object. Every check here
 * detects accident — truncation, bit-rot, a partially written file, a mismatched
 * build. None of it detects a motivated forger: the checksums are unkeyed, so
 * anyone who can rewrite the payload can recompute them. Authenticity would
 * need signing, which is deliberately out of scope.
 *
 * make knowledge_capsule -> KNOWLEDGE_CAPSULE_PASS
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
#include "../include/router.h"
#include "../include/contract/contract.h"
#include "../include/specialist.h"

#define SYM 8
#define COV_ROWS 5 /* certified on 5 of 8 — 3 stay out of coverage */

static int failures, checks;

static void check(int ok, const char *name) {
    checks++;
    printf("  %-62s %s\n", name, ok ? "PASS" : "FAIL");
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

static int argmax(const double *v) {
    int i, b = 0;
    for (i = 1; i < SYM; i++)
        if (v[i] > v[b]) b = i;
    return b;
}

static int rot3(int x) { return (x + 3) % SYM; }

/* ---- C helpers: no system(), so the gate compiles warning-clean --------- */

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
    return copy_file(p, q);
}

static void rm_pack(const char *dir) {
    char p[512];
    snprintf(p, sizeof p, "%s/unit.cnb", dir);
    (void)remove(p);
    snprintf(p, sizeof p, "%s/manifest.cknow", dir);
    (void)remove(p);
    (void)rmdir(dir);
}

static int flip_payload_byte(const char *dir, long off) {
    char p[512];
    FILE *fp;
    snprintf(p, sizeof p, "%s/unit.cnb", dir);
    fp = fopen(p, "r+b");
    if (!fp) return -1;
    if (fseek(fp, off, SEEK_SET) != 0) { fclose(fp); return -1; }
    fputc(0xA5, fp);
    return fclose(fp);
}

/* Mirror of the manifest checksum: FNV-1a 64 (standard basis) over every byte
   before the trailing manifest_fnv line. Lets a test rewrite a field AND reseal,
   so a binding check is exercised instead of only the checksum. */
static unsigned long long manifest_fnv_of(const char *text, size_t upto) {
    unsigned long long h = 14695981039346656037ULL;
    size_t i;
    for (i = 0; i < upto; i++) {
        h ^= (unsigned char)text[i];
        h *= 1099511628211ULL;
    }
    return h;
}

/* Replace the line starting with `prefix` with `repl`. reseal=1 recomputes the
   trailing checksum (simulating an internally-consistent but wrong manifest);
   reseal=0 leaves it stale (simulating corruption). */
static int edit_manifest(const char *dir, const char *prefix, const char *repl,
                         int reseal) {
    char path[512], *buf;
    long len;
    FILE *fp;
    char out[16384];
    size_t olen = 0;
    char *line, *save;
    int replaced = 0;

    snprintf(path, sizeof path, "%s/manifest.cknow", dir);
    fp = fopen(path, "rb");
    if (!fp) return -1;
    fseek(fp, 0, SEEK_END);
    len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    buf = (char *)calloc((size_t)len + 1, 1);
    if (!buf) { fclose(fp); return -1; }
    if (fread(buf, 1, (size_t)len, fp) != (size_t)len) {
        fclose(fp); free(buf); return -1;
    }
    fclose(fp);

    for (line = strtok_r(buf, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        const char *emit = line;
        if (strncmp(line, "manifest_fnv", 12) == 0) continue; /* re-added below */
        if (strncmp(line, prefix, strlen(prefix)) == 0) {
            emit = repl;
            replaced = 1;
        }
        olen += (size_t)snprintf(out + olen, sizeof out - olen, "%s\n", emit);
    }
    free(buf);
    if (!replaced) return -2;

    fp = fopen(path, "wb");
    if (!fp) return -1;
    fwrite(out, 1, olen, fp);
    fprintf(fp, "manifest_fnv %llu\n",
            reseal ? manifest_fnv_of(out, olen) : 0ULL);
    return fclose(fp);
}

static int build_unit(CnetBase *base, HybridAi *cov, const char *name) {
    BinaryTransformNetwork *btn;
    double in[SYM][SYM], tg[SYM][SYM];
    Contract c;
    Specialist s;
    Port pin = P("cap_in"), pout = P("cap_out");
    int i;

    btn = (BinaryTransformNetwork *)calloc(1, sizeof *btn);
    if (!btn) return -1;
    for (i = 0; i < SYM; i++) {
        oh(in[i], i);
        oh(tg[i], rot3(i));
    }
    if (btn_init(btn, SYM, SYM, 16, 64, 0.5, 7) != 0) { free(btn); return -1; }
    btn_set_ports(btn, pin, pout);
    btn_train_dynamic(btn, (const double *)in, (const double *)tg, SYM, 20000,
                      200, 1e-6, 1e-8);
    btn_train(btn, (const double *)in, (const double *)tg, SYM, 4000);
    memset(&c, 0, sizeof c);
    if (contract_init_borrowed(&c, name, btn, (const double *)in,
                               (const double *)tg, SYM) != 0) {
        btn_free(btn); free(btn); return -1;
    }
    memset(&s, 0, sizeof s);
    if (specialist_wrap_btn(&s, btn, name) != 0) {
        contract_free(&c); btn_free(btn); free(btn); return -1;
    }
    if (cnb_add_unit(base, btn, &c, NULL) != 0) {
        contract_free(&c); btn_free(btn); free(btn); return -1;
    }
    if (hybrid_coverage_record(cov, pin, pout, name, (const double *)in,
                               (const double *)tg, COV_ROWS, SYM, SYM) != 0) {
        contract_free(&c);
        btn_free(btn);
        free(btn);
        return -1;
    }
    /* cnb_add_unit copies the serialized image, so the BTN stays caller-owned. */
    contract_free(&c);
    btn_free(btn);
    free(btn);
    return 0;
}

/* Import into a throwaway target and report only whether it was refused. */
static int refused(const char *dir, char *reason, size_t rcap) {
    CnetBase b;
    HybridAi h;
    CnetCapsuleReport rep;
    int rc, clean;
    cnb_init(&b);
    hybrid_ai_init(&h);
    memset(&rep, 0, sizeof rep);
    rc = cnet_capsule_import(&b, &h, dir, &rep);
    clean = (rc != 0) && !cnb_has_unit(&b, "cap_rot3") &&
            hybrid_coverage_count(&h) == 0;
    if (reason) snprintf(reason, rcap, "%s", rep.reject_reason);
    cnb_free(&b);
    hybrid_ai_free(&h);
    return clean;
}

int main(void) {
    CnetBase src, dst;
    HybridAi cov_src, cov_dst;
    CnetCapsuleReport rep;
    Port pin = P("cap_in"), pout = P("cap_out");
    const char *dir = "tmp_capsule_pack";
    const char *bad = "tmp_capsule_bad";
    const char *unit = "cap_rot3";
    char reason[CNET_CAPSULE_REASON_MAX];
    double probe[SYM];
    int rc;

    printf("== portable knowledge capsule (certified unit + its gate) ==\n");
    rm_pack(dir);
    rm_pack(bad);

    cnb_init(&src);
    cnb_init(&dst);
    hybrid_ai_init(&cov_src);
    hybrid_ai_init(&cov_dst);

    check(build_unit(&src, &cov_src, unit) == 0, "source: unit built + sealed");
    check(hybrid_coverage_rows(&cov_src, pin, pout) == COV_ROWS,
          "source: coverage over a strict subset");

    memset(&rep, 0, sizeof rep);
    rc = cnet_capsule_export(&src, &cov_src, unit, dir, &rep);
    check(rc == 0, "export: capsule written");
    check(rep.coverage_rows == COV_ROWS, "export: coverage travelled with it");
    check(rep.exemplars == SYM, "export: contract exemplars recorded");

    memset(&rep, 0, sizeof rep);
    rc = cnet_capsule_import(&dst, &cov_dst, dir, &rep);
    check(rc == 0, "import: accepted into a fresh base");
    check(cnb_has_unit(&dst, unit) == 1, "import: unit present in target");
    check(hybrid_coverage_rows(&cov_dst, pin, pout) == COV_ROWS,
          "import: gate restored, not silently dropped");

    /* ---- coverage round-trip compared against the SOURCE values ---------- */
    {
        const HybridCoverage *a = NULL, *b = NULL;
        size_t i;
        int rows_same = 1, tgts_same = 1;
        for (i = 0; i < cov_src.coverage_count; i++)
            if (cov_src.coverage[i].active &&
                strcmp(cov_src.coverage[i].unit, unit) == 0)
                a = &cov_src.coverage[i];
        for (i = 0; i < cov_dst.coverage_count; i++)
            if (cov_dst.coverage[i].active &&
                strcmp(cov_dst.coverage[i].unit, unit) == 0)
                b = &cov_dst.coverage[i];
        check(a && b, "round-trip: coverage record found on both sides");
        if (a && b) {
            check(a->n_rows == b->n_rows && a->in_dim == b->in_dim &&
                      a->out_dim == b->out_dim,
                  "round-trip: coverage dims identical");
            if (a->n_rows == b->n_rows && a->in_dim == b->in_dim) {
                if (memcmp(a->rows, b->rows,
                           a->n_rows * a->in_dim * sizeof(double)) != 0)
                    rows_same = 0;
                if (a->targets && b->targets && a->out_dim == b->out_dim &&
                    memcmp(a->targets, b->targets,
                           a->n_rows * a->out_dim * sizeof(double)) != 0)
                    tgts_same = 0;
            }
            check(rows_same, "round-trip: coverage INPUT rows bit-identical");
            check(tgts_same, "round-trip: coverage TARGETS bit-identical");
            check(strcmp(a->input_port.tag, b->input_port.tag) == 0 &&
                      strcmp(a->goal_port.tag, b->goal_port.tag) == 0,
                  "round-trip: coverage bound to the same port tags");
        }
    }

    /* ---- exhaustive certified-contract replay ---------------------------
       All 8 exemplars ARE the contract's training set. This is serialization
       fidelity, not generalisation, and is labelled as such everywhere. */
    {
        BinaryTransformNetwork b2;
        Contract c2;
        int i, same = 0;
        memset(&b2, 0, sizeof b2);
        memset(&c2, 0, sizeof c2);
        check(cnb_get_unit(&dst, unit, &b2, &c2) == 0,
              "import: unit materialises (CNU1 seal verified)");
        for (i = 0; i < SYM; i++) {
            const double *out;
            oh(probe, i);
            out = btn_forward(&b2, probe);
            if (out && argmax(out) == rot3(i)) same++;
        }
        check(same == SYM,
              "replay: all 8 contract exemplars reproduce (not held-out)");
        btn_free(&b2);
        contract_free(&c2);
    }

    /* ---- OOD ------------------------------------------------------------- */
    {
        int refused_n = 0, admitted = 0, i;
        for (i = 0; i < SYM; i++) {
            oh(probe, i);
            if (hybrid_coverage_admits(&cov_dst, pin, pout, probe, SYM))
                admitted++;
            else
                refused_n++;
        }
        check(admitted == COV_ROWS, "OOD: only certified symbols admitted");
        check(refused_n == SYM - COV_ROWS, "OOD: uncovered symbols refused");
    }

    /* ================= negative controls: all fail closed ================= */

    /* payload corruption */
    check(clone_pack(dir, bad) == 0 && flip_payload_byte(bad, 64) == 0,
          "neg: payload tampered");
    check(refused(bad, reason, sizeof reason), "neg: corrupt payload REJECTED");
    printf("      %s\n", reason);
    rm_pack(bad);

    /* incompatible container version */
    check(clone_pack(dir, bad) == 0 &&
              edit_manifest(bad, "cnb_version", "cnb_version 99", 1) == 0,
          "neg: version rewritten + resealed");
    check(refused(bad, reason, sizeof reason), "neg: foreign cnb_version REJECTED");
    printf("      %s\n", reason);
    rm_pack(bad);

    /* manifest metadata corruption: a coverage INPUT value */
    check(clone_pack(dir, bad) == 0 &&
              edit_manifest(bad, "COVIN", "COVIN 9 9 9 9 9 9 9 9", 0) == 0,
          "neg: coverage row tampered (checksum left stale)");
    check(refused(bad, reason, sizeof reason), "neg: tampered coverage row REJECTED");
    printf("      %s\n", reason);
    rm_pack(bad);

    /* manifest metadata corruption: a coverage TARGET value */
    check(clone_pack(dir, bad) == 0 &&
              edit_manifest(bad, "COVOUT", "COVOUT 9 9 9 9 9 9 9 9", 0) == 0,
          "neg: coverage target tampered");
    check(refused(bad, reason, sizeof reason), "neg: tampered coverage target REJECTED");
    printf("      %s\n", reason);
    rm_pack(bad);

    /* manifest tag rewritten AND resealed — checksum is consistent, so only a
       binding check against the payload's real ports can catch this */
    check(clone_pack(dir, bad) == 0 &&
              edit_manifest(bad, "in ", "in 1 8 1 not_cap_in", 1) == 0,
          "neg: manifest tag rewritten and resealed");
    check(refused(bad, reason, sizeof reason),
          "neg: manifest tag not matching payload REJECTED");
    printf("      %s\n", reason);
    rm_pack(bad);

    /* provenance rewritten and resealed — must not diverge from the payload */
    check(clone_pack(dir, bad) == 0 &&
              edit_manifest(bad, "provenance", "provenance forged_teacher", 1) == 0,
          "neg: provenance rewritten and resealed");
    check(refused(bad, reason, sizeof reason),
          "neg: provenance not matching payload REJECTED");
    printf("      %s\n", reason);
    rm_pack(bad);

    /* hostile manifest: absurd coverage dimensions must not allocate */
    check(clone_pack(dir, bad) == 0 &&
              edit_manifest(bad, "coverage ",
                            "coverage 4294967295 4294967295 4294967295", 1) == 0,
          "neg: hostile coverage rows/in written");
    check(refused(bad, reason, sizeof reason) &&
              strstr(reason, "coverage_bounds") != NULL,
          "neg: overflowing coverage rows/in REJECTED by BOUND");
    printf("      %s\n", reason);
    rm_pack(bad);

    /* cov_out was the unbounded one: rows and in_dim are legal here, so only a
       bound on the OUTPUT dimension (and checked multiplication) stops a
       ~68 GB calloc. */
    check(clone_pack(dir, bad) == 0 &&
              edit_manifest(bad, "coverage ", "coverage 2 8 4294967295", 1) == 0,
          "neg: hostile coverage OUT dim written");
    check(refused(bad, reason, sizeof reason) &&
              strstr(reason, "coverage_bounds") != NULL,
          "neg: overflowing coverage out_dim REJECTED by BOUND (not by oom)");
    printf("      %s\n", reason);
    rm_pack(bad);

    /* symlinked payload must be refused (local artifact hardening) */
    {
        char lnk[512], tgt[512];
        check(clone_pack(dir, bad) == 0, "neg: pack cloned for symlink test");
        snprintf(lnk, sizeof lnk, "%s/unit.cnb", bad);
        {
            char cwd[256];
            if (!getcwd(cwd, sizeof cwd)) cwd[0] = '\0';
            /* Absolute, so the link genuinely resolves to a VALID payload —
               a relative target would just dangle and be refused as missing,
               which would pass this test without testing anything. */
            snprintf(tgt, sizeof tgt, "%s/%s/unit.cnb", cwd, dir);
        }
        (void)remove(lnk);
        if (symlink(tgt, lnk) == 0) {
            check(refused(bad, reason, sizeof reason),
                  "neg: symlinked payload REJECTED");
            printf("      %s\n", reason);
        } else {
            check(0, "neg: could not create symlink for test");
        }
        rm_pack(bad);
    }

    /* capsule carries coverage but caller passes cov=NULL -> must NOT silently
       drop the gate */
    {
        CnetBase b;
        cnb_init(&b);
        memset(&rep, 0, sizeof rep);
        rc = cnet_capsule_import(&b, NULL, dir, &rep);
        check(rc != 0 && !cnb_has_unit(&b, unit),
              "neg: coverage-bearing capsule with cov=NULL REJECTED");
        printf("      %s\n", rep.reject_reason);
        cnb_free(&b);
    }

    /* R1: provenance longer than CNB_NAME_MAX must not overflow the parse
       buffer. Self-checksummed so the manifest is internally consistent and
       only the field bound can refuse it. */
    {
        char longprov[256];
        int i2;
        memset(longprov, 0, sizeof longprov);
        strcpy(longprov, "provenance ");
        for (i2 = 0; i2 < 200; i2++) longprov[11 + i2] = 'A';
        check(clone_pack(dir, bad) == 0 &&
                  edit_manifest(bad, "provenance", longprov, 1) == 0,
              "neg: overlong provenance written and resealed");
        check(refused(bad, reason, sizeof reason) &&
                  strstr(reason, "provenance") != NULL,
              "neg: overlong provenance REJECTED by FIELD BOUND (no overwrite)");
        printf("      %s\n", reason);
        rm_pack(bad);
    }

    /* R5: coverage output dimension must match the unit's OUTPUT port, not just
       generic caps. cov_out=7 is small and legal by caps, wrong for an 8-wide
       output port. */
    {
        char covline[64];
        snprintf(covline, sizeof covline, "coverage %d %d 7", COV_ROWS, SYM);
        check(clone_pack(dir, bad) == 0 &&
                  edit_manifest(bad, "coverage ", covline, 1) == 0,
              "neg: coverage out_dim mismatched to output port");
        check(refused(bad, reason, sizeof reason) &&
                  strstr(reason, "out_dim") != NULL,
              "neg: cov_out REJECTED by OUTPUT-PORT BINDING (not parse fallout)");
        printf("      %s\n", reason);
        rm_pack(bad);
    }

    /* R3: a DIFFERENT unit already owning this coverage port shape must not be
       displaced. Coverage is keyed by ports, so importing here would free the
       incumbent's rows and leave that older unit default-allow. */
    {
        CnetBase b;
        HybridAi occupied;
        double orow[SYM], otgt[SYM];
        const HybridCoverage *before_rec, *after_rec;
        double keep_rows[SYM * SYM], keep_tgts[SYM * SYM];
        size_t kn = 0, kin = 0, kout = 0;
        char keep_unit[96];
        cnb_init(&b);
        hybrid_ai_init(&occupied);
        oh(orow, 1);
        oh(otgt, 4);
        check(hybrid_coverage_record(&occupied, pin, pout, "incumbent_unit",
                                     orow, otgt, 1, SYM, SYM) == 0,
              "conflict: incumbent owns the coverage port shape");
        before_rec = NULL;
        {
            size_t z;
            for (z = 0; z < occupied.coverage_count; z++)
                if (occupied.coverage[z].active &&
                    strcmp(occupied.coverage[z].unit, "incumbent_unit") == 0)
                    before_rec = &occupied.coverage[z];
        }
        if (before_rec) {
            kn = before_rec->n_rows;
            kin = before_rec->in_dim;
            kout = before_rec->out_dim;
            memcpy(keep_rows, before_rec->rows, kn * kin * sizeof(double));
            if (before_rec->targets)
                memcpy(keep_tgts, before_rec->targets, kn * kout * sizeof(double));
            snprintf(keep_unit, sizeof keep_unit, "%s", before_rec->unit);
        }
        memset(&rep, 0, sizeof rep);
        rc = cnet_capsule_import(&b, &occupied, dir, &rep);
        check(rc != 0, "conflict: import REFUSED rather than displacing");
        printf("      %s\n", rep.reject_reason);
        check(!cnb_has_unit(&b, unit), "conflict: no unit admitted");
        after_rec = NULL;
        {
            size_t z;
            for (z = 0; z < occupied.coverage_count; z++)
                if (occupied.coverage[z].active &&
                    strcmp(occupied.coverage[z].unit, "incumbent_unit") == 0)
                    after_rec = &occupied.coverage[z];
        }
        check(after_rec != NULL, "conflict: incumbent record still present");
        check(after_rec && after_rec->n_rows == kn && after_rec->in_dim == kin &&
                  after_rec->out_dim == kout &&
                  memcmp(after_rec->rows, keep_rows, kn * kin * sizeof(double)) == 0 &&
                  after_rec->targets &&
                  memcmp(after_rec->targets, keep_tgts, kn * kout * sizeof(double)) == 0 &&
                  strcmp(after_rec->unit, keep_unit) == 0,
              "conflict: incumbent rows/targets/tags byte-identical");
        cnb_free(&b);
        hybrid_ai_free(&occupied);
    }

    /* R4: a failed cnb_add_unit must not leave the base partially mutated. The
       tag preflight compares each tag against the BASE, not against the unit's
       own other tags, so two mutually near-miss tags pass preflight, mint the
       first and fail on the second. */
    {
        CnetBase b;
        BinaryTransformNetwork *btn2;
        Contract c3;
        double in2[SYM][SYM], tg2[SYM][SYM];
        Port ain, aout;
        size_t tags_before, blobs_before, units_before;
        int i3, addrc;
        cnb_init(&b);
        ain = P("pair_aa");
        aout = P("pair_ab"); /* Levenshtein 1 from pair_aa */
        btn2 = (BinaryTransformNetwork *)calloc(1, sizeof *btn2);
        for (i3 = 0; i3 < SYM; i3++) { oh(in2[i3], i3); oh(tg2[i3], rot3(i3)); }
        btn_init(btn2, SYM, SYM, 16, 64, 0.5, 11);
        btn_set_ports(btn2, ain, aout);
        btn_train_dynamic(btn2, (const double *)in2, (const double *)tg2, SYM,
                          8000, 200, 1e-6, 1e-8);
        memset(&c3, 0, sizeof c3);
        if (contract_init_borrowed(&c3, "pair_unit", btn2, (const double *)in2,
                                   (const double *)tg2, SYM) == 0) {
            tags_before = b.tag_count;
            blobs_before = b.blob_count;
            units_before = b.unit_count;
            addrc = cnb_add_unit(&b, btn2, &c3, NULL);
            check(addrc != 0, "atomicity: mutually near-miss tags REFUSED");
            check(b.tag_count == tags_before,
                  "atomicity: tag_count unchanged after refusal");
            check(b.blob_count == blobs_before,
                  "atomicity: blob_count unchanged after refusal");
            check(b.unit_count == units_before,
                  "atomicity: unit_count unchanged after refusal");
            contract_free(&c3);
        }
        btn_free(btn2);
        free(btn2);
        cnb_free(&b);
    }

    /* R4-1: SAME owner, different incoming payload. The old gate must survive:
       replacing coverage then failing cnb_add_unit (same name, different
       content) used to delete the replacement and lose the original. */
    {
        CnetBase b;
        HybridAi h;
        BinaryTransformNetwork *other;
        Contract oc;
        double in2[SYM][SYM], tg2[SYM][SYM];
        double oldrow[SYM], oldtgt[SYM], keep_rows[SYM], keep_tgts[SYM];
        const HybridCoverage *rec;
        size_t tags0, blobs0, units0;
        int i4;
        cnb_init(&b);
        hybrid_ai_init(&h);
        /* a DIFFERENT unit body under the SAME name */
        other = (BinaryTransformNetwork *)calloc(1, sizeof *other);
        check(other != NULL, "same-owner: scratch btn allocated");
        for (i4 = 0; i4 < SYM; i4++) { oh(in2[i4], i4); oh(tg2[i4], (i4 + 5) % SYM); }
        if (other && btn_init(other, SYM, SYM, 16, 64, 0.5, 21) == 0) {
            btn_set_ports(other, pin, pout);
            btn_train_dynamic(other, (const double *)in2, (const double *)tg2,
                              SYM, 8000, 200, 1e-6, 1e-8);
            memset(&oc, 0, sizeof oc);
            if (contract_init_borrowed(&oc, unit, other, (const double *)in2,
                                       (const double *)tg2, SYM) == 0) {
                (void)cnb_add_unit(&b, other, &oc, NULL);
                contract_free(&oc);
            }
        }
        oh(oldrow, 2);
        oh(oldtgt, 6);
        check(hybrid_coverage_record(&h, pin, pout, unit, oldrow, oldtgt, 1,
                                     SYM, SYM) == 0,
              "same-owner: pre-existing gate for the same unit name");
        memcpy(keep_rows, oldrow, sizeof keep_rows);
        memcpy(keep_tgts, oldtgt, sizeof keep_tgts);
        tags0 = b.tag_count; blobs0 = b.blob_count; units0 = b.unit_count;
        memset(&rep, 0, sizeof rep);
        rc = cnet_capsule_import(&b, &h, dir, &rep);
        check(rc != 0, "same-owner: conflicting payload REJECTED");
        printf("      %s\n", rep.reject_reason);
        rec = NULL;
        { size_t z; for (z = 0; z < h.coverage_count; z++)
            if (h.coverage[z].active && strcmp(h.coverage[z].unit, unit) == 0)
                rec = &h.coverage[z]; }
        check(rec != NULL, "same-owner: old gate still present");
        check(rec && rec->n_rows == 1 && rec->in_dim == SYM &&
                  rec->out_dim == SYM &&
                  memcmp(rec->rows, keep_rows, sizeof keep_rows) == 0 &&
                  rec->targets &&
                  memcmp(rec->targets, keep_tgts, sizeof keep_tgts) == 0,
              "same-owner: old rows/targets byte-identical");
        check(b.tag_count == tags0 && b.blob_count == blobs0 &&
                  b.unit_count == units0,
              "same-owner: base counts unchanged");
        cnb_free(&b);
        hybrid_ai_free(&h);
        if (other) { btn_free(other); }
        free(other);
    }

    /* R4-2: repeated REJECTED imports must not consume registry slots. */
    {
        HybridAi h;
        size_t before, after, i5;
        hybrid_ai_init(&h);
        /* occupy the shape with a foreign owner so every import is rejected */
        {
            double row[SYM], tgt[SYM];
            oh(row, 0); oh(tgt, 3);
            (void)hybrid_coverage_record(&h, pin, pout, "foreign_owner", row,
                                         tgt, 1, SYM, SYM);
        }
        before = hybrid_coverage_count(&h);
        for (i5 = 0; i5 < HYBRID_COVERAGE_MAX + 8; i5++) {
            CnetBase b;
            cnb_init(&b);
            memset(&rep, 0, sizeof rep);
            (void)cnet_capsule_import(&b, &h, dir, &rep);
            cnb_free(&b);
        }
        after = hybrid_coverage_count(&h);
        check(after == before, "slots: rejected imports consume no registry slots");
        check(h.coverage_count <= HYBRID_COVERAGE_MAX,
              "slots: coverage_count stays within bound");
        /* registry must still work afterwards */
        {
            double row[SYM];
            oh(row, 1);
            check(hybrid_coverage_record(&h, P("fresh_in"), P("fresh_out"),
                                         "fresh_unit", row, NULL, 1, SYM,
                                         0) == 0,
                  "slots: registry still usable after the loop");
        }
        hybrid_ai_free(&h);
    }

    /* R4-3: forget must reclaim its slot, not strand it. */
    {
        HybridAi h;
        double row[SYM];
        size_t i6, c0;
        hybrid_ai_init(&h);
        oh(row, 0);
        for (i6 = 0; i6 < 4; i6++) {
            char t1[32], t2[32], un[32];
            snprintf(t1, sizeof t1, "rc_%c%c%zu_in", (char)('a' + i6),
                     (char)('m' + i6), i6);
            snprintf(t2, sizeof t2, "rc_%c%c%zu_out", (char)('a' + i6),
                     (char)('m' + i6), i6);
            snprintf(un, sizeof un, "rc_unit_%zu", i6);
            (void)hybrid_coverage_record(&h, P(t1), P(t2), un, row, NULL, 1,
                                         SYM, 0);
        }
        c0 = hybrid_coverage_count(&h);
        check(c0 == 4, "reclaim: four records stored");
        check(hybrid_coverage_forget_unit(&h, "rc_unit_1") == 1,
              "reclaim: middle record forgotten");
        check(hybrid_coverage_count(&h) == 3, "reclaim: count decreased");
        check(h.coverage_count == 3, "reclaim: array compacted, no stranded slot");
        check(hybrid_coverage_has_unit(&h, "rc_unit_0") &&
                  hybrid_coverage_has_unit(&h, "rc_unit_2") &&
                  hybrid_coverage_has_unit(&h, "rc_unit_3"),
              "reclaim: all remaining records preserved");
        hybrid_ai_free(&h);
    }

    /* R4-4: a pre-created payload temp SYMLINK must not clobber its target. */
    {
        char victim[512], tmplink[512];
        FILE *vf;
        char vbuf[64];
        rm_pack(bad);
        (void)mkdir(bad, 0777);
        snprintf(victim, sizeof victim, "%s/victim.txt", bad);
        vf = fopen(victim, "wb");
        if (vf) { fputs("DO_NOT_CLOBBER", vf); fclose(vf); }
        snprintf(tmplink, sizeof tmplink, "%s/unit.cnb.tmp", bad);
        (void)remove(tmplink);
        {
            char abs[600], cwd[256];
            if (!getcwd(cwd, sizeof cwd)) cwd[0] = '\0';
            snprintf(abs, sizeof abs, "%s/%s", cwd, victim);
            check(symlink(abs, tmplink) == 0, "symlink: payload temp planted");
        }
        memset(&rep, 0, sizeof rep);
        (void)cnet_capsule_export(&src, &cov_src, unit, bad, &rep);
        vf = fopen(victim, "rb");
        memset(vbuf, 0, sizeof vbuf);
        if (vf) { size_t vr = fread(vbuf, 1, sizeof vbuf - 1, vf); vbuf[vr] = 0; fclose(vf); }
        check(strcmp(vbuf, "DO_NOT_CLOBBER") == 0,
              "symlink: victim file byte-identical (temp not followed)");
        (void)remove(victim);
        (void)remove(tmplink);
        rm_pack(bad);
    }

    /* R4-5: cov_out==0 is a documented, permitted contract (targets optional). */
    {
        CnetBase b;
        HybridAi h, src2;
        CnetBase srcb;
        double row[SYM];
        cnb_init(&srcb);
        hybrid_ai_init(&src2);
        check(build_unit(&srcb, &src2, "cap_norows") == 0,
              "cov0: source unit built");
        oh(row, 0);
        /* replace with an inputs-only record, as the sidecar restore path makes */
        (void)hybrid_coverage_forget_unit(&src2, "cap_norows");
        check(hybrid_coverage_record(&src2, P("cap_in"), P("cap_out"),
                                     "cap_norows", row, NULL, 1, SYM, 0) == 0,
              "cov0: inputs-only coverage recorded");
        rm_pack("tmp_capsule_cov0");
        memset(&rep, 0, sizeof rep);
        check(cnet_capsule_export(&srcb, &src2, "cap_norows",
                                  "tmp_capsule_cov0", &rep) == 0,
              "cov0: exports with targets omitted");
        cnb_init(&b);
        hybrid_ai_init(&h);
        memset(&rep, 0, sizeof rep);
        check(cnet_capsule_import(&b, &h, "tmp_capsule_cov0", &rep) == 0,
              "cov0: imports (targets are optional by design)");
        check(hybrid_coverage_rows(&h, P("cap_in"), P("cap_out")) == 1,
              "cov0: inputs-only gate restored");
        cnb_free(&b); hybrid_ai_free(&h);
        cnb_free(&srcb); hybrid_ai_free(&src2);
        rm_pack("tmp_capsule_cov0");
    }

    /* R4-6: allocation failure at each reservation point must leave the base
       byte-identical. Without executable failure-path evidence, "all-or-nothing"
       is an assertion, not a property. */
    {
        CnetBase b;
        BinaryTransformNetwork *u1;
        Contract c1;
        double in3[SYM][SYM], tg3[SYM][SYM];
        size_t t0, b0, n0;
        int i7, fail_at, injected_refusals = 0;
        cnb_init(&b);
        u1 = (BinaryTransformNetwork *)calloc(1, sizeof *u1);
        check(u1 != NULL, "allocfail: scratch btn allocated");
        for (i7 = 0; i7 < SYM; i7++) { oh(in3[i7], i7); oh(tg3[i7], rot3(i7)); }
        if (u1 && btn_init(u1, SYM, SYM, 16, 64, 0.5, 31) == 0) {
            btn_set_ports(u1, P("af_alpha_in"), P("af_omega_out"));
            btn_train_dynamic(u1, (const double *)in3, (const double *)tg3, SYM,
                              8000, 200, 1e-6, 1e-8);
            memset(&c1, 0, sizeof c1);
            if (contract_init_borrowed(&c1, "af_unit", u1, (const double *)in3,
                                       (const double *)tg3, SYM) == 0) {
                t0 = b.tag_count; b0 = b.blob_count; n0 = b.unit_count;
                /* Three reservations happen: tags, blob, unit. */
                for (fail_at = 1; fail_at <= 3; fail_at++) {
                    int arc;
                    cnb_test_alloc_fail_in((size_t)fail_at);
                    arc = cnb_add_unit(&b, u1, &c1, NULL);
                    cnb_test_alloc_fail_in(0);
                    if (arc != 0) injected_refusals++;
                    check(b.tag_count == t0 && b.blob_count == b0 &&
                              b.unit_count == n0,
                          "allocfail: base unchanged after injected failure");
                }
                check(injected_refusals == 3,
                      "allocfail: every injected reservation failure refused");
                /* and the base still works with injection disabled */
                check(cnb_add_unit(&b, u1, &c1, NULL) == 0,
                      "allocfail: normal admit still succeeds afterwards");
                check(b.unit_count == n0 + 1, "allocfail: exactly one unit added");
                contract_free(&c1);
            }
        }
        cnb_free(&b);
        if (u1) { btn_free(u1); }
        free(u1);
    }

    /* transactional: coverage cannot be stored -> unit must not land either */
    {
        CnetBase b;
        HybridAi full;
        size_t i;
        double row[SYM];
        cnb_init(&b);
        hybrid_ai_init(&full);
        oh(row, 0);
        for (i = 0; i < HYBRID_COVERAGE_MAX; i++) {
            char t1[32], t2[32];
            snprintf(t1, sizeof t1, "sat_%c%c%zu_in", (char)('a' + i / 26),
                     (char)('a' + i % 26), i);
            snprintf(t2, sizeof t2, "sat_%c%c%zu_out", (char)('a' + i / 26),
                     (char)('a' + i % 26), i);
            if (hybrid_coverage_record(&full, P(t1), P(t2), t1, row, NULL, 1,
                                       SYM, 0) != 0)
                break;
        }
        check(hybrid_coverage_count(&full) == HYBRID_COVERAGE_MAX,
              "transactional: coverage table saturated");
        memset(&rep, 0, sizeof rep);
        rc = cnet_capsule_import(&b, &full, dir, &rep);
        check(rc != 0, "transactional: import refused when gate cannot be stored");
        check(!cnb_has_unit(&b, unit),
              "transactional: NO ungated unit left behind");
        printf("      %s\n", rep.reject_reason);
        cnb_free(&b);
        hybrid_ai_free(&full);
    }

    cnb_free(&src);
    cnb_free(&dst);
    hybrid_ai_free(&cov_src);
    hybrid_ai_free(&cov_dst);
    rm_pack(dir);
    rm_pack(bad);

    printf("checks=%d failures=%d\n", checks, failures);
    if (failures == 0) {
        printf("KNOWLEDGE_CAPSULE_PASS checks=%d coverage_rows=%d\n", checks,
               COV_ROWS);
        return 0;
    }
    printf("KNOWLEDGE_CAPSULE_FAIL failures=%d\n", failures);
    return 1;
}
