/* Portable isolated-knowledge capsule — export one certified capability from
 * one base and import it into a fresh one, with its gate intact.
 *
 * NOT an MTK/.tskill cartridge. Those (include/cce/cce_mtk.h) are CMSK/MTSK
 * weight deltas patched onto a specific host model's named tensor sites: no
 * typed contract, no certification, no coverage. A capsule is the certified
 * side of CNET — a CNU1-sealed BTN unit plus its Contract exemplars, ports,
 * provenance and certified coverage. The two are not interchangeable and this
 * gate deliberately does not pretend otherwise.
 *
 * cnb_export_subset already moves a unit between bases. What it does NOT move
 * is the coverage record, because coverage lives in a <base>.coverage sidecar
 * (S7). Transferring a mined unit without it silently un-gates it on the
 * target — exactly the confident-wrong failure coverage exists to stop. The
 * capsule binds the two together and refuses to import either half alone.
 *
 * make knowledge_capsule -> KNOWLEDGE_CAPSULE_PASS
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/cnet_capsule.h"
#include "../include/base.h"
#include "../include/hybrid_ai.h"
#include "../include/nn.h"
#include "../include/router.h"
#include "../include/contract/contract.h"
#include "../include/specialist.h"

#define SYM 8
#define COV_ROWS 5 /* certified on 5 of 8 symbols — 3 stay out of coverage */

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

/* Knowledge unit: rotate-by-3 over an 8-symbol alphabet. */
static int rot3(int x) { return (x + 3) % SYM; }

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
    if (btn_init(btn, SYM, SYM, 16, 64, 0.5, 7) != 0) return -1;
    btn_set_ports(btn, pin, pout);
    btn_train_dynamic(btn, (const double *)in, (const double *)tg, SYM, 20000,
                      200, 1e-6, 1e-8);
    btn_train(btn, (const double *)in, (const double *)tg, SYM, 4000);
    memset(&c, 0, sizeof c);
    if (contract_init_borrowed(&c, name, btn, (const double *)in,
                               (const double *)tg, SYM) != 0)
        return -1;
    memset(&s, 0, sizeof s);
    if (specialist_wrap_btn(&s, btn, name) != 0) {
        contract_free(&c);
        return -1;
    }
    if (cnb_add_unit(base, btn, &c, NULL) != 0) {
        contract_free(&c);
        return -1;
    }
    /* Certified over only the first COV_ROWS symbols: the rest must abstain. */
    if (hybrid_coverage_record(cov, pin, pout, name, (const double *)in,
                               (const double *)tg, COV_ROWS, SYM, SYM) != 0) {
        contract_free(&c);
        return -1;
    }
    contract_free(&c);
    return 0;
}

int main(void) {
    CnetBase src, dst;
    HybridAi cov_src, cov_dst;
    CnetCapsuleReport rep;
    Port pin = P("cap_in"), pout = P("cap_out");
    const char *dir = "tmp_capsule_pack";
    const char *unit = "cap_rot3";
    double probe[SYM];
    int rc;

    printf("== portable knowledge capsule (certified unit + its gate) ==\n");
    (void)system("rm -rf tmp_capsule_pack tmp_capsule_bad");

    cnb_init(&src);
    cnb_init(&dst);
    hybrid_ai_init(&cov_src);
    hybrid_ai_init(&cov_dst);

    check(build_unit(&src, &cov_src, unit) == 0, "source: unit built + sealed");
    check(cnb_has_unit(&src, unit) == 1, "source: unit in base");
    check(hybrid_coverage_rows(&cov_src, pin, pout) == COV_ROWS,
          "source: coverage recorded over a strict subset");

    /* ---- export ---------------------------------------------------------- */
    memset(&rep, 0, sizeof rep);
    rc = cnet_capsule_export(&src, &cov_src, unit, dir, &rep);
    check(rc == 0, "export: capsule written");
    check(rep.coverage_rows == COV_ROWS, "export: coverage travelled with it");
    check(rep.exemplars == SYM, "export: contract exemplars recorded");
    check(rep.payload_bytes > 0, "export: payload size reported");

    /* ---- import into a FRESH base ---------------------------------------- */
    memset(&rep, 0, sizeof rep);
    rc = cnet_capsule_import(&dst, &cov_dst, dir, &rep);
    check(rc == 0, "import: accepted into a fresh base");
    check(cnb_has_unit(&dst, unit) == 1, "import: unit present in target");
    check(hybrid_coverage_rows(&cov_dst, pin, pout) == COV_ROWS,
          "import: gate restored, not silently dropped");

    /* ---- behaviour is identical on held-out symbols ---------------------- */
    {
        BinaryTransformNetwork b2;
        Contract c2;
        int i, same = 0, covered_ok = 0;
        memset(&b2, 0, sizeof b2);
        memset(&c2, 0, sizeof c2);
        check(cnb_get_unit(&dst, unit, &b2, &c2) == 0,
              "import: unit materialises (CNU1 seal verified)");
        for (i = 0; i < SYM; i++) {
            const double *out;
            oh(probe, i);
            out = btn_forward(&b2, probe);
            if (!out) continue;
            if (argmax(out) == rot3(i)) same++;
            if (i < COV_ROWS && argmax(out) == rot3(i)) covered_ok++;
        }
        check(same == SYM, "round-trip: all 8 answers match the source");
        check(covered_ok == COV_ROWS, "round-trip: in-coverage answers correct");
        btn_free(&b2);
        contract_free(&c2);
    }

    /* ---- OOD: outside certified coverage the capsule must abstain -------- */
    {
        int refused = 0, admitted = 0, i;
        for (i = 0; i < SYM; i++) {
            oh(probe, i);
            if (hybrid_coverage_admits(&cov_dst, pin, pout, probe, SYM))
                admitted++;
            else
                refused++;
        }
        check(admitted == COV_ROWS, "OOD: only certified symbols admitted");
        check(refused == SYM - COV_ROWS, "OOD: uncovered symbols refused");
    }

    /* ---- negative control: corrupted payload must be rejected ------------ */
    {
        CnetBase bad;
        HybridAi bcov;
        FILE *fp;
        (void)system("cp -r tmp_capsule_pack tmp_capsule_bad");
        fp = fopen("tmp_capsule_bad/unit.cnb", "r+b");
        if (fp) {
            /* Flip a byte deep in the payload, past the container header. */
            fseek(fp, 64, SEEK_SET);
            fputc(0xA5, fp);
            fclose(fp);
        }
        cnb_init(&bad);
        hybrid_ai_init(&bcov);
        memset(&rep, 0, sizeof rep);
        rc = cnet_capsule_import(&bad, &bcov, "tmp_capsule_bad", &rep);
        check(rc != 0, "corruption: tampered payload REJECTED");
        check(rep.reject_reason[0] != '\0', "corruption: reason reported");
        printf("      reject_reason=%s\n", rep.reject_reason);
        check(cnb_has_unit(&bad, unit) == 0,
              "corruption: nothing partially imported");
        cnb_free(&bad);
        hybrid_ai_free(&bcov);
    }

    /* ---- negative control: incompatible format must be rejected ---------- */
    {
        CnetBase bad;
        HybridAi bcov;
        (void)system("rm -rf tmp_capsule_bad && cp -r tmp_capsule_pack tmp_capsule_bad");
        (void)system("sed -i 's/^cnb_version .*/cnb_version 99/' "
                     "tmp_capsule_bad/manifest.cknow");
        cnb_init(&bad);
        hybrid_ai_init(&bcov);
        memset(&rep, 0, sizeof rep);
        rc = cnet_capsule_import(&bad, &bcov, "tmp_capsule_bad", &rep);
        check(rc != 0, "incompatible: foreign cnb_version REJECTED");
        printf("      reject_reason=%s\n", rep.reject_reason);
        cnb_free(&bad);
        hybrid_ai_free(&bcov);
    }

    cnb_free(&src);
    cnb_free(&dst);
    hybrid_ai_free(&cov_src);
    hybrid_ai_free(&cov_dst);
    (void)system("rm -rf tmp_capsule_pack tmp_capsule_bad");

    printf("checks=%d failures=%d\n", checks, failures);
    if (failures == 0) {
        printf("KNOWLEDGE_CAPSULE_PASS checks=%d coverage_rows=%d\n", checks,
               COV_ROWS);
        return 0;
    }
    printf("KNOWLEDGE_CAPSULE_FAIL failures=%d\n", failures);
    return 1;
}
